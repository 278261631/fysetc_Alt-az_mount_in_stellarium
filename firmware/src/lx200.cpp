// lx200.cpp - LX200 serial protocol implementation for the FYSETC E4 Alt-Az mount
//
// Wire format: every command starts with ':' and ends with '#'.  Replies are
// '#'-terminated and are sent immediately.
//
// The subset implemented here is exactly what Stellarium 23.4's direct LX200
// telescope client needs (plugins/TelescopeControl/src/Lx200/*), plus the
// usual LX200 getters/setters and a custom :XG# diagnostics command.
//
// Notes on Stellarium 23.4 (verified against the v23.4 sources):
//  * It polls  :GR# / :GD#  every 500 ms and prepends a stray '#'.  Bytes
//    before ':' are therefore ignored by this parser.
//  * Goto   = :Q#  :Sr<hh:mm:ss>#  :Sd<+dd*mm:ss>#  :MS#
//  * Sync   = :Q#  :Sr...#  :Sd...#  :CM#
//  * Long format (with seconds) is used for RA and Dec.  The degree sign it
//    parses is 0xDF (not '*'); '*' is tolerated by Stellarium too.
//  * If the first RA/Dec answer was short-format it sends  :U#  (no answer
//    wanted) - we always answer long format, :U# is a no-op.
#include "lx200.h"
#include "config.h"
#include "astro.h"
#include "motion.h"
#include "scope.h"
#include "tmc.h"
#include "persist.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------
static char    s_cmd[96];
static uint8_t s_len = 0;

static void sendBytes(const char* s) {
  if (s && *s) Serial.write((const uint8_t*)s, strlen(s));
}

// ---------------------------------------------------------------------------
// Response formatting
// ---------------------------------------------------------------------------
static void fmtRa(char* b, size_t n, double raHours) {
  long t = (long)lround(astroNormalizeHours(raHours) * 3600.0) % 86400L;
  if (t < 0) t += 86400L;
  snprintf(b, n, "%02ld:%02ld:%02ld#", t / 3600L, (t / 60L) % 60L, t % 60L);
}

// Signed angle with 0xDF degree sign, DD*MM:SS
static void fmtAngleSS(char* b, size_t n, double deg) {
  if (!isfinite(deg)) deg = 0.0;
  if (deg >  90.0) deg =  90.0;
  if (deg < -90.0) deg = -90.0;
  long t = (long)lround(fabs(deg) * 3600.0);
  if (t > 90L * 3600L) t = 90L * 3600L;
  snprintf(b, n, "%c%02ld%c%02ld:%02ld#", deg < 0.0 ? '-' : '+',
           t / 3600L, (char)0xDF, (t / 60L) % 60L, t % 60L);
}

// Unsigned azimuth, DDD*MM:SS
static void fmtAzSS(char* b, size_t n, double deg) {
  if (!isfinite(deg)) deg = 0.0;
  double d = fmod(deg, 360.0);
  if (d < 0.0) d += 360.0;
  long t = (long)lround(d * 3600.0);
  if (t >= 360L * 3600L) t = 0;
  snprintf(b, n, "%03ld%c%02ld:%02ld#", t / 3600L, (char)0xDF,
           (t / 60L) % 60L, t % 60L);
}

static void fmtSiteLat(char* b, size_t n, double latDeg) {
  if (!isfinite(latDeg)) latDeg = 0.0;
  if (latDeg > 90.0) latDeg = 90.0;
  if (latDeg < -90.0) latDeg = -90.0;
  long t = (long)lround(fabs(latDeg) * 60.0);
  snprintf(b, n, "%c%02ld%c%02ld#", latDeg < 0.0 ? '-' : '+',
           t / 60L, (char)0xDF, t % 60L);
}

static void fmtSiteLon(char* b, size_t n, double lonDegEast) {
  if (!isfinite(lonDegEast)) lonDegEast = 0.0;
  lonDegEast = fmod(lonDegEast, 360.0);
  if (lonDegEast > 180.0) lonDegEast -= 360.0;
  if (lonDegEast < -180.0) lonDegEast += 360.0;
  long t = (long)lround(fabs(lonDegEast) * 60.0);
  snprintf(b, n, "%c%03ld%c%02ld#", lonDegEast < 0.0 ? '-' : '+',
           t / 60L, (char)0xDF, t % 60L);
}

static void fmtHms(char* b, size_t n, int h, int m, int s) {
  snprintf(b, n, "%02d:%02d:%02d#", h, m, s);
}

// ---------------------------------------------------------------------------
// Parsing helpers
// ---------------------------------------------------------------------------
static const char* skipSpaces(const char* p) {
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

static bool readUInt(const char*& p, int* v, int maxDigits) {
  int n = 0, cnt = 0;
  while (*p >= '0' && *p <= '9' && cnt < maxDigits) {
    n = n * 10 + (*p - '0');
    p++;
    cnt++;
  }
  if (cnt == 0) return false;
  *v = n;
  return true;
}

static bool parseRa(const char* p, double* outHours) {
  p = skipSpaces(p);
  int h = 0, m = 0, s = 0;
  if (!readUInt(p, &h, 2)) return false;
  if (*p == ':') {
    p++;
    if (!readUInt(p, &m, 2)) return false;
  }
  if (*p == ':' || *p == '.') {
    p++;
    if (!readUInt(p, &s, 2)) return false;
  }
  if (h > 23 || m > 59 || s > 59) return false;
  *outHours = h + m / 60.0 + s / 3600.0;
  return true;
}

static bool parseDec(const char* p, double* outDeg) {
  p = skipSpaces(p);
  int sign = 1;
  if (*p == '+') { p++; }
  else if (*p == '-') { sign = -1; p++; }
  int d = 0, m = 0, s = 0;
  if (!readUInt(p, &d, 2)) return false;
  bool haveMin = false;
  if (*p == (char)0xDF || *p == '*') {
    p++;
    if (!readUInt(p, &m, 2)) return false;
    haveMin = true;
  } else if (*p == ':') {
    p++;
    if (!readUInt(p, &m, 2)) return false;
    haveMin = true;
  }
  if (haveMin && (*p == ':' || *p == '.')) {
    p++;
    if (!readUInt(p, &s, 2)) return false;
  }
  if (d > 90 || m > 59 || s > 59) return false;
  double v = d + m / 60.0 + s / 3600.0;
  *outDeg = sign * v;
  return true;
}

// Longitude, DDD signed, + east / - west.
static bool parseLon(const char* p, double* outDegEast) {
  p = skipSpaces(p);
  int sign = 1;
  if (*p == '+') { p++; }
  else if (*p == '-') { sign = -1; p++; }
  int d = 0, m = 0;
  if (!readUInt(p, &d, 3)) return false;
  if (*p == (char)0xDF || *p == '*' || *p == ':') {
    p++;
    if (!readUInt(p, &m, 2)) return false;
  }
  if (d > 360 || m > 59) return false;
  *outDegEast = sign * (d + m / 60.0);
  return true;
}

static bool parseHms(const char* p, int* hour, int* minute, int* sec) {
  p = skipSpaces(p);
  int h = 0, m = 0, s = 0;
  if (!readUInt(p, &h, 2)) return false;
  if (*p == ':') {
    p++;
    if (!readUInt(p, &m, 2)) return false;
    if (*p == ':') {
      p++;
      if (!readUInt(p, &s, 2)) return false;
    }
  } else if (*p == '.') {
    // HH.MM (some clients use a decimal separator)
    p++;
    if (!readUInt(p, &m, 2)) return false;
  }
  if (h > 23 || m > 59 || s > 59) return false;
  *hour = h; *minute = m; *sec = s;
  return true;
}

static bool parseDate(const char* p, int* year, int* month, int* day) {
  p = skipSpaces(p);
  int mo = 0, dd = 0, yy = 0;
  if (!readUInt(p, &mo, 2)) return false;
  if (*p != '/' && *p != '-' && *p != ':') return false;
  p++;
  if (!readUInt(p, &dd, 2)) return false;
  if (*p != '/' && *p != '-' && *p != ':') return false;
  p++;
  if (!readUInt(p, &yy, 4)) return false;
  if (mo < 1 || mo > 12 || dd < 1 || dd > 31) return false;
  if (yy < 100) yy += 2000;
  if (yy < 1900 || yy > 2200) return false;
  *year = yy; *month = mo; *day = dd;
  return true;
}

static bool parseUtcOffset(const char* p, int* seconds) {
  p = skipSpaces(p);
  int sign = 1;
  if (*p == '+') { p++; }
  else if (*p == '-') { sign = -1; p++; }
  int h = 0;
  if (!readUInt(p, &h, 2)) return false;
  int m = 0;
  if (*p == ':') {
    p++;
    if (!readUInt(p, &m, 2)) return false;
  } else if (*p == '.') {
    // Decimal hours, e.g. "+5.5" -> 5 h 30 min
    p++;
    int frac = 0, digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 3) {
      frac = frac * 10 + (*p - '0');
      p++;
      digits++;
    }
    if (digits == 0) return false;
    double fh = (double)frac / pow(10.0, digits) * 60.0;
    m = (int)lround(fh);
  }
  if (h > 14 || m > 59) return false;
  *seconds = sign * (h * 3600 + m * 60);
  return true;
}

// :XW<unixSeconds># - set the UTC clock (integer seconds, fraction tolerated).
static bool parseUnixSeconds(const char* p, double* outUnix) {
  p = skipSpaces(p);
  char* end = nullptr;
  const double v = strtod(p, &end);
  if (end == p || !isfinite(v)) return false;
  *outUnix = v;
  return true;
}

// :XC<lat>,<lon># - decimal degrees, north/east positive.
static bool parseLatLonPair(const char* p, double* lat, double* lon) {
  p = skipSpaces(p);
  char* end = nullptr;
  const double a = strtod(p, &end);
  if (end == p || !isfinite(a)) return false;
  p = end;
  while (*p == ' ' || *p == ',' || *p == ';' || *p == '\t') p++;
  char* end2 = nullptr;
  const double b = strtod(p, &end2);
  if (end2 == p || !isfinite(b)) return false;
  *lat = a;
  *lon = b;
  return true;
}

// ---------------------------------------------------------------------------
// Command state
// ---------------------------------------------------------------------------
static bool   s_haveSelRa = false;
static bool   s_haveSelDec = false;
static double s_selRa = 0.0;
static double s_selDec = 0.0;
static uint32_t s_lastRxMs = 0;
static uint32_t s_lastMsMs = 0;   // millis() of the last accepted :MS#

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
static void handleGet(const char* cmd) {
  const char c2 = cmd[2];
  if (c2 == 'V') {
    const char c3 = cmd[3];
    if (c3 == 'P')      sendBytes(PRODUCT_NAME "#");
    else if (c3 == 'N') sendBytes(FIRMWARE_VERSION "#");
    else if (c3 == 'D') sendBytes(__DATE__ "#");
    else if (c3 == 'T') sendBytes(__TIME__ "#");
    return;
  }

  char buf[40];
  switch (c2) {
    case 'R': {
      double ra = 0.0, dec = 0.0;
      scopePointingRaDec(&ra, &dec);
      fmtRa(buf, sizeof(buf), ra);
      sendBytes(buf);
      break;
    }
    case 'D': {
      double ra = 0.0, dec = 0.0;
      scopePointingRaDec(&ra, &dec);
      fmtAngleSS(buf, sizeof(buf), dec);
      sendBytes(buf);
      break;
    }
    case 'A':
      fmtAngleSS(buf, sizeof(buf), scopeAltDeg());
      sendBytes(buf);
      break;
    case 'Z':
      fmtAzSS(buf, sizeof(buf), scopeAzDeg());
      sendBytes(buf);
      break;
    case 'G': {
      const int off = astroUtcOffsetSeconds();
      const int h = abs(off) / 3600;
      snprintf(buf, sizeof(buf), "%c%02d#", off < 0 ? '-' : '+', h);
      sendBytes(buf);
      break;
    }
    case 'L': {
      int y, mo, d, h, mi, s;
      astroGetLocalCivil(&y, &mo, &d, &h, &mi, &s);
      fmtHms(buf, sizeof(buf), h, mi, s);
      sendBytes(buf);
      break;
    }
    case 'C': {
      int y, mo, d, h, mi, s;
      astroGetLocalCivil(&y, &mo, &d, &h, &mi, &s);
      snprintf(buf, sizeof(buf), "%02d/%02d/%02d#", mo, d, y % 100);
      sendBytes(buf);
      break;
    }
    case 't':
      fmtSiteLat(buf, sizeof(buf), astroSiteLatDeg());
      sendBytes(buf);
      break;
    case 'g':
      fmtSiteLon(buf, sizeof(buf), astroSiteLonDeg());
      sendBytes(buf);
      break;
    case 'S': {
      // Sidereal time
      const double lst = astroLocalSiderealHours();
      long t = (long)lround(lst * 3600.0) % 86400L;
      fmtHms(buf, sizeof(buf), (int)(t / 3600), (int)((t / 60) % 60), (int)(t % 60));
      sendBytes(buf);
      break;
    }
    default:
      // Unknown getter: stay silent so the client just times out on it.
      break;
  }
}

static void handleSet(const char* cmd) {
  const char c2 = cmd[2];
  const char* data = cmd + 3;

  double ra = 0.0, dec = 0.0, lon = 0.0;

  switch (c2) {
    case 'r':
      if (parseRa(data, &ra) && isfinite(ra)) {
        s_selRa = astroNormalizeHours(ra);
        s_haveSelRa = true;
        // Stellarium consumes exactly ONE byte for :Sr / :Sd answers.
        sendBytes("1");
      } else {
        sendBytes("0");
      }
      persistLogFlush();
      break;

    case 'd':
      if (parseDec(data, &dec) && isfinite(dec)) {
        s_selDec = dec;
        s_haveSelDec = true;
        sendBytes("1");
      } else {
        sendBytes("0");
      }
      persistLogFlush();
      break;

    case 't':
      if (parseDec(data, &dec) && isfinite(dec)) {
        astroSetSiteLatDeg(dec);
        sendBytes("1#");
      } else {
        sendBytes("0#");
      }
      break;

    case 'g':
      if (parseLon(data, &lon) && isfinite(lon)) {
        astroSetSiteLonDeg(lon);
        sendBytes("1#");
      } else {
        sendBytes("0#");
      }
      break;

    case 'L': {
      int h = 0, m = 0, s = 0;
      if (parseHms(data, &h, &m, &s)) {
        astroSetLocalTime(h, m, s);
        sendBytes("1#");
      } else {
        sendBytes("0#");
      }
      break;
    }

    case 'C': {
      int y = 0, mo = 0, d = 0;
      if (parseDate(data, &y, &mo, &d)) {
        astroSetLocalDate(y, mo, d);
        sendBytes("1#");
      } else {
        sendBytes("0#");
      }
      break;
    }

    case 'G': {
      int off = 0;
      if (parseUtcOffset(data, &off)) {
        astroSetUtcOffsetSeconds(off);
        sendBytes("1#");
      } else {
        sendBytes("0#");
      }
      break;
    }

    default:
      break;
  }
}

static void handleCommand(const char* cmd) {
  if (!cmd || cmd[0] != ':' || cmd[1] == '\0') return;
  const char c1 = cmd[1];

  if (c1 == 'G') {
    handleGet(cmd);
    return;
  }

  if (c1 == 'S') {
    handleSet(cmd);
    return;
  }

  if (c1 == 'C') {
    if (cmd[2] == 'M') {  // :CM# sync - reply is consumed one byte at a time
      if (s_haveSelRa && s_haveSelDec && scopeSyncRaDec(s_selRa, s_selDec)) {
        sendBytes("0");
      } else {
        sendBytes("1#");
      }
      persistFlushState();
      persistLogFlush();
    }
    return;
  }

  if (c1 == 'M') {
    // :MS# - goto selected object: answer '0' immediately, then slew asynchronously
    double tgtRa = 0.0, tgtDec = 0.0;
    if (s_haveSelRa && s_haveSelDec) {
      tgtRa = s_selRa;
      tgtDec = s_selDec;
    } else {
      scopePointingRaDec(&tgtRa, &tgtDec);
      if (s_haveSelRa) tgtRa = s_selRa;
      if (s_haveSelDec) tgtDec = s_selDec;
    }
    const bool ok = scopeGotoRaDec(tgtRa, tgtDec);
    if (ok) {
      sendBytes("0");
      s_lastMsMs = millis();
    } else {
      sendBytes("1#");
    }
    // Persist target + positions immediately, and flush the command log.
    persistFlushState();
    persistLogFlush();
    return;
  }

  if (c1 == 'Q') {
    // :Q# (and :Qe# / :Qw# etc.) - abort slew, no answer expected
    scopeStop();
    persistLogFlush();
    return;
  }

  if (c1 == 'U') {
    // :U# toggle short/long format - we always answer long format, no answer
    return;
  }

  if (c1 == 'D') {
    // :D# distance bar - empty bar
    sendBytes("#");
    return;
  }

  if (c1 == 'X') {
    switch (cmd[2]) {
      case 'G': {
        // :XG# - custom one-line diagnostics (no '#' inside, '#' terminated).
        char diag[200];
        scopeDiag(diag, sizeof(diag));
        char line[240];
        snprintf(line, sizeof(line), "%s BOOT=%lu LASTMS=%lu",
                 diag, (unsigned long)persistBootCount(),
                 (unsigned long)lx200LastMsMillis());
        sendBytes(line);
        sendBytes("#");
        break;
      }
      case 'J': {
        // :XJ# - last 24 interesting LX200 commands ("ms:cmd|ms:cmd|...")
        char logline[780];
        persistLogToString(logline, sizeof(logline));
        sendBytes(logline);
        sendBytes("#");
        break;
      }
      case 'R': {
        // :XR# - clear persisted pointing state and zero both axes
        persistResetState();
        sendBytes("1#");
        break;
      }
      case 'S': {
        // :XS# - sync offset + requested target (no '#' inside, <200 bytes)
        char sync[96];
        scopeSyncDiag(sync, sizeof(sync));
        sendBytes(sync);
        sendBytes("#");
        break;
      }
      case 'W': {  // :XW<unixSeconds># - set UTC clock (persisted in NVS)
        double unixSec = 0.0;
        if (parseUnixSeconds(cmd + 3, &unixSec)) {
          astroSetUtcUnix(unixSec);
          astroPersistUtc();
          // Time changed -> any existing sync alignment is invalid.
          scopeClearSyncOffsets();
          persistFlushState();   // keep the state blob in sync with the new clock
          persistLogFlush();
          sendBytes("1#");
        } else {
          sendBytes("0#");
        }
        break;
      }
      case 'C': {  // :XC<lat>,<lon># - set site, decimal degrees east/north positive
        double lat = 0.0, lon = 0.0;
        if (parseLatLonPair(cmd + 3, &lat, &lon)) {
          astroSetSiteLatDeg(lat);
          astroSetSiteLonDeg(lon);
          astroPersistSite();
          // Site changed -> any existing sync alignment is invalid.
          scopeClearSyncOffsets();
          persistLogFlush();
          sendBytes("1#");
        } else {
          sendBytes("0#");
        }
        break;
      }
      default:
        break;
    }
    return;
  }

  // Unknown command: stay silent.
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
void lx200Begin() {
  s_len = 0;
  s_lastRxMs = millis();
  s_lastMsMs = 0;
}

uint32_t lx200IdleMs() {
  return (uint32_t)(millis() - s_lastRxMs);
}

void lx200GetSelection(double* raHours, double* decDeg, bool* haveRa, bool* haveDec) {
  if (raHours) *raHours = s_selRa;
  if (decDeg)  *decDeg = s_selDec;
  if (haveRa)  *haveRa = s_haveSelRa;
  if (haveDec) *haveDec = s_haveSelDec;
}

void lx200RestoreSelection(double raHours, double decDeg, bool haveRa, bool haveDec) {
  if (isfinite(raHours) && isfinite(decDeg) && decDeg >= -90.0 && decDeg <= 90.0) {
    s_selRa = astroNormalizeHours(raHours);
    s_selDec = decDeg;
    s_haveSelRa = haveRa;
    s_haveSelDec = haveDec;
  } else {
    s_haveSelRa = false;
    s_haveSelDec = false;
  }
}

void lx200ClearSelection() {
  s_haveSelRa = false;
  s_haveSelDec = false;
  s_selRa = 0.0;
  s_selDec = 0.0;
}

uint32_t lx200LastMsMillis() { return s_lastMsMs; }
void     lx200RestoreLastMs(uint32_t ms) { s_lastMsMs = ms; }
void     lx200ResetLastMs() { s_lastMsMs = 0; }

void lx200Poll() {
  if (Serial.available() > 0) s_lastRxMs = millis();
  while (Serial.available() > 0) {
    const int ci = Serial.read();
    if (ci < 0) break;
    const char c = (char)ci;

    if (c == ':') {
      // A ':' starts a new command only when no command is being assembled:
      // LX200 setters such as :SrHH:MM:SS# / :Sd+DD*MM:SS# carry ':' inside
      // their arguments, so an in-progress command must keep them as data.
      if (s_len == 0) {
        s_cmd[0] = ':';
        s_len = 1;
      } else if (s_len < (uint8_t)(sizeof(s_cmd) - 1)) {
        s_cmd[s_len++] = c;
      }
    } else if (c == '#') {
      if (s_len > 0) {
        s_cmd[s_len] = '\0';
        persistLogCommand(s_cmd);   // ring buffer (routine polls filtered out)
        handleCommand(s_cmd);
        s_len = 0;
      }
      // stray '#' (Stellarium sends one before :GR#/:GD#) is ignored
    } else if (s_len > 0) {
      if (s_len < (uint8_t)(sizeof(s_cmd) - 1)) {
        s_cmd[s_len++] = c;
      } else {
        s_len = 0;  // overflow: drop the command
      }
    }
    // bytes before ':' are ignored
  }
}
