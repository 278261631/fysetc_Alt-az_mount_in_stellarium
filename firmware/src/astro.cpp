// astro.cpp - time keeping, site coordinates and ALT/AZ <-> RA/DEC conversion
#include "astro.h"
#include "config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const double DEG2RAD = M_PI / 180.0;
static const double RAD2DEG = 180.0 / M_PI;

// ---------------------------------------------------------------------------
// Clock: UTC seconds since epoch, advancing with millis()
// ---------------------------------------------------------------------------
static double   s_utcBase    = 0.0;
static uint32_t s_baseMillis = 0;
static int      s_utcOffsetSec = 0;   // local = UTC + offset
static double   s_latDeg = SITE_LAT_DEG_DEFAULT;
static double   s_lonDeg = SITE_LON_DEG_DEFAULT;  // + east

static Preferences s_prefs;
static bool        s_prefsReady = false;

// Howard Hinnant's civil-date algorithms (proleptic Gregorian).
static long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);                       // [0, 399]
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return (long)era * 146097L + (long)doe - 719468L;
}

static void civilFromDays(long z, int* y, unsigned* m, unsigned* d) {
  z += 719468L;
  const long era = (z >= 0 ? z : z - 146096L) / 146097L;
  const unsigned doe = (unsigned)(z - era * 146097L);
  const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  const long yyy = (long)yoe + era * 400L;
  const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const unsigned mp = (5u * doy + 2u) / 153u;
  const unsigned dd = doy - (153u * mp + 2u) / 5u + 1u;
  const unsigned mm = mp + (mp < 10u ? 3u : (unsigned)-9);
  *y = (int)(yyy + (mm <= 2u ? 1 : 0));
  *m = mm;
  *d = dd;
}

// Start the clock from the firmware build timestamp.  __DATE__/__TIME__ are
// local time of the build host; the default UTC offset (config.h) turns it into
// UTC.  A client script can set the exact time later via :XW<unix>#.
static double buildLocalUnix() {
  static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* d = __DATE__;   // "Nov 18 2026"
  const char* t = __TIME__;   // "10:23:45"
  int mon = 1;
  for (int i = 0; i < 12; i++) {
    if (strncmp(d, kMonths + 3 * i, 3) == 0) { mon = i + 1; break; }
  }
  int day  = atoi(d + 4);
  int year = atoi(d + 7);
  int hh = atoi(t), mi = atoi(t + 3), ss = atoi(t + 6);
  const long days = daysFromCivil(year, (unsigned)mon, (unsigned)day);
  return (double)days * 86400.0 + hh * 3600.0 + mi * 60.0 + ss;
}

void astroBegin() {
  s_utcOffsetSec = DEFAULT_UTC_OFFSET_SEC;
  s_utcBase = buildLocalUnix() - (double)s_utcOffsetSec;
  s_baseMillis = millis();
  s_latDeg = SITE_LAT_DEG_DEFAULT;
  s_lonDeg = SITE_LON_DEG_DEFAULT;

  // Restore site/clock injected by a previous :XC / :XW (survives resets).
  s_prefsReady = s_prefs.begin("altaz", false);
  if (s_prefsReady) {
    s_latDeg = s_prefs.getDouble("lat", s_latDeg);
    s_lonDeg = s_prefs.getDouble("lon", s_lonDeg);
    s_utcOffsetSec = s_prefs.getInt("utcoff", s_utcOffsetSec);
    const double savedUtc = s_prefs.getDouble("utc", 0.0);
    if (savedUtc > 0.0) s_utcBase = savedUtc;
  }
}

void astroPersistSite() {
  if (!s_prefsReady) return;
  s_prefs.putDouble("lat", s_latDeg);
  s_prefs.putDouble("lon", s_lonDeg);
  s_prefs.putInt("utcoff", s_utcOffsetSec);
}

void astroPersistUtc() {
  if (!s_prefsReady) return;
  s_prefs.putDouble("utc", astroUtcUnix());
}

double astroUtcUnix() {
  return s_utcBase + (double)(millis() - s_baseMillis) / 1000.0;
}

void astroSetUtcUnix(double utcUnix) {
  s_utcBase = utcUnix;
  s_baseMillis = millis();
}

void astroSetSiteLatDeg(double latDeg) {
  if (!isfinite(latDeg)) return;
  if (latDeg > 90.0)  latDeg = 90.0;
  if (latDeg < -90.0) latDeg = -90.0;
  s_latDeg = latDeg;
}

void astroSetSiteLonDeg(double lonDegEast) {
  if (!isfinite(lonDegEast)) return;
  lonDegEast = fmod(lonDegEast, 360.0);
  if (lonDegEast > 180.0)  lonDegEast -= 360.0;
  if (lonDegEast < -180.0) lonDegEast += 360.0;
  s_lonDeg = lonDegEast;
}

double astroSiteLatDeg() { return s_latDeg; }
double astroSiteLonDeg() { return s_lonDeg; }

int  astroUtcOffsetSeconds() { return s_utcOffsetSec; }
void astroSetUtcOffsetSeconds(int secs) {
  if (secs < -14 * 3600) secs = -14 * 3600;
  if (secs >  14 * 3600) secs =  14 * 3600;
  s_utcOffsetSec = secs;
}

void astroGetLocalCivil(int* year, int* month, int* day, int* hour, int* minute, int* sec) {
  double local = astroUtcUnix() + (double)s_utcOffsetSec;
  long long li = (long long)floor(local);
  long days = (long)(li / 86400LL);
  int sod = (int)(li % 86400LL);
  if (sod < 0) { sod += 86400; days -= 1; }
  unsigned m, d;
  int y;
  civilFromDays(days, &y, &m, &d);
  if (year)   *year = y;
  if (month)  *month = (int)m;
  if (day)    *day = (int)d;
  if (hour)   *hour = sod / 3600;
  if (minute) *minute = (sod / 60) % 60;
  if (sec)    *sec = sod % 60;
}

void astroSetLocalTime(int hour, int minute, int sec) {
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || sec < 0 || sec > 59) return;
  int y = 0, mo = 1, dd = 1, h = 0, mi = 0, s = 0;
  astroGetLocalCivil(&y, &mo, &dd, &h, &mi, &s);
  const long localUnix = daysFromCivil(y, (unsigned)mo, (unsigned)dd) * 86400L
                       + hour * 3600 + minute * 60 + sec;
  astroSetUtcUnix((double)localUnix - (double)s_utcOffsetSec);
}

void astroSetLocalDate(int year, int month, int day) {
  if (month < 1 || month > 12 || day < 1 || day > 31) return;
  int y = 0, mo = 1, dd = 1, h = 0, mi = 0, s = 0;
  astroGetLocalCivil(&y, &mo, &dd, &h, &mi, &s);
  const long localUnix = daysFromCivil(year, (unsigned)month, (unsigned)day) * 86400L
                       + h * 3600 + mi * 60 + s;
  astroSetUtcUnix((double)localUnix - (double)s_utcOffsetSec);
}

double astroNormalizeHours(double h) {
  if (!isfinite(h)) return 0.0;
  h = fmod(h, 24.0);
  if (h < 0) h += 24.0;
  return h;
}

// ---------------------------------------------------------------------------
// Sidereal time
// ---------------------------------------------------------------------------
double astroLocalSiderealHours() {
  const double utc = astroUtcUnix();
  const double jd = utc / 86400.0 + 2440587.5;
  const double d = jd - 2451545.0;
  const double T = d / 36525.0;
  double gmst = 280.46061837 + 360.98564736629 * d
              + 0.000387933 * T * T - T * T * T / 38710000.0;
  double lstDeg = gmst + s_lonDeg;
  lstDeg = fmod(lstDeg, 360.0);
  if (lstDeg < 0) lstDeg += 360.0;
  return lstDeg / 15.0;   // hours
}

// ---------------------------------------------------------------------------
// Coordinate conversion (no refraction, epoch of date)
// ---------------------------------------------------------------------------
void astroAltAzToRaDec(double altDeg, double azDeg, double* raHours, double* decDeg) {
  if (!raHours || !decDeg) return;
  const double alt = altDeg * DEG2RAD;
  const double az  = azDeg  * DEG2RAD;
  const double lat = s_latDeg * DEG2RAD;

  double sinDec = sin(alt) * sin(lat) + cos(alt) * cos(lat) * cos(az);
  if (sinDec >  1.0) sinDec =  1.0;
  if (sinDec < -1.0) sinDec = -1.0;
  const double dec = asin(sinDec);

  // Hour angle:  H = atan2(-sin(Az)cos(Alt)cos(lat), sin(Alt) - sin(Dec)sin(lat))
  const double H = atan2(-sin(az) * cos(alt) * cos(lat),
                         sin(alt) - sin(dec) * sin(lat));

  const double lst = astroLocalSiderealHours();
  *raHours = astroNormalizeHours(lst - H * RAD2DEG / 15.0);
  *decDeg  = dec * RAD2DEG;
}

void astroRaDecToAltAz(double raHours, double decDeg, double* altDeg, double* azDeg) {
  if (!altDeg || !azDeg) return;
  const double lat = s_latDeg * DEG2RAD;
  const double dec = decDeg * DEG2RAD;
  const double H   = (astroLocalSiderealHours() - raHours) * 15.0 * DEG2RAD;

  double sinAlt = sin(dec) * sin(lat) + cos(dec) * cos(lat) * cos(H);
  if (sinAlt >  1.0) sinAlt =  1.0;
  if (sinAlt < -1.0) sinAlt = -1.0;
  const double alt = asin(sinAlt);
  const double az = atan2(-cos(dec) * sin(H),
                          sin(dec) * cos(lat) - cos(dec) * sin(lat) * cos(H));

  *altDeg = alt * RAD2DEG;
  double a = az * RAD2DEG;
  if (a < 0.0) a += 360.0;
  *azDeg = a;
}

// ---------------------------------------------------------------------------
// Precession J2000 <-> mean equator/equinox of date (IAU 1976, Meeus ch. 21)
// ---------------------------------------------------------------------------
static void precessionAngles(double* zeta, double* z, double* theta) {
  const double jd = astroUtcUnix() / 86400.0 + 2440587.5;
  const double T = (jd - 2451545.0) / 36525.0;
  const double T2 = T * T, T3 = T2 * T;
  *zeta  = (2306.2181 * T + 0.30188 * T2 + 0.017998 * T3) / 3600.0 * DEG2RAD;
  *z     = (2306.2181 * T + 1.09468 * T2 + 0.018203 * T3) / 3600.0 * DEG2RAD;
  *theta = (2004.3109 * T - 0.42665 * T2 - 0.041833 * T3) / 3600.0 * DEG2RAD;
}

void astroJ2000ToOfDate(double raJ2000, double decJ2000, double* raDate, double* decDate) {
  if (!raDate || !decDate) return;
  double zeta, z, theta;
  precessionAngles(&zeta, &z, &theta);

  const double a0 = raJ2000 * 15.0 * DEG2RAD;   // hours -> degrees -> rad
  const double d0 = decJ2000 * DEG2RAD;

  const double A = cos(d0) * sin(a0 + zeta);
  const double B = cos(theta) * cos(d0) * cos(a0 + zeta) - sin(theta) * sin(d0);
  const double C = sin(theta) * cos(d0) * cos(a0 + zeta) + cos(theta) * sin(d0);

  const double ra = atan2(A, B) + z;
  const double dec = asin((C > 1.0) ? 1.0 : ((C < -1.0) ? -1.0 : C));

  *raDate  = astroNormalizeHours(ra * RAD2DEG / 15.0);
  *decDate = dec * RAD2DEG;
}

void astroOfDateToJ2000(double raDate, double decDate, double* raJ2000, double* decJ2000) {
  if (!raJ2000 || !decJ2000) return;
  double zeta, z, theta;
  precessionAngles(&zeta, &z, &theta);

  const double a = raDate * 15.0 * DEG2RAD;
  const double d = decDate * DEG2RAD;

  // v0 = Rz(-zeta) * Ry(theta) * Rz(-z) * v_date
  const double a1 = a - z;
  const double x1 = cos(d) * cos(a1);
  const double y1 = cos(d) * sin(a1);
  const double z1 = sin(d);

  const double x2 = cos(theta) * x1 + sin(theta) * z1;
  const double y2 = y1;
  const double z2 = -sin(theta) * x1 + cos(theta) * z1;

  const double ra = atan2(y2, x2) - zeta;
  const double dec = asin((z2 > 1.0) ? 1.0 : ((z2 < -1.0) ? -1.0 : z2));

  *raJ2000  = astroNormalizeHours(ra * RAD2DEG / 15.0);
  *decJ2000 = dec * RAD2DEG;
}
