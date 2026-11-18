// astro.h - time keeping, site coordinates and ALT/AZ <-> RA/DEC conversion
#pragma once

// Initialise the clock from the firmware build timestamp (UTC) and the site
// from SITE_LAT_DEG_DEFAULT / SITE_LON_DEG_DEFAULT.
void astroBegin();

// Site, degrees.  Latitude +north, longitude +east (LX200 convention).
void   astroSetSiteLatDeg(double latDeg);
void   astroSetSiteLonDeg(double lonDegEast);
double astroSiteLatDeg();
double astroSiteLonDeg();

// UTC seconds since 1970, advancing with millis().
double astroUtcUnix();
void   astroSetUtcUnix(double utcUnix);

int  astroUtcOffsetSeconds();
void astroSetUtcOffsetSeconds(int secs);

// Persist site / clock in NVS so a board reset (e.g. DTR toggle when a serial
// client opens COM7) does not lose the values injected by :XW / :XC.
void astroPersistSite();
void astroPersistUtc();

// Local mean sidereal time, hours 0..24.
double astroLocalSiderealHours();

// Equatorial (RA hours, Dec deg, epoch of date, no refraction) <-> horizontal.
void astroAltAzToRaDec(double altDeg, double azDeg, double* raHours, double* decDeg);
void astroRaDecToAltAz(double raHours, double decDeg, double* altDeg, double* azDeg);

// IAU 1976 precession between J2000 and the mean equator/equinox of date.
void astroJ2000ToOfDate(double raJ2000, double decJ2000, double* raDate, double* decDate);
void astroOfDateToJ2000(double raDate, double decDate, double* raJ2000, double* decJ2000);

// Local civil date/time helpers for LX200 :GC/:GL/:SC/:SL.
void astroGetLocalCivil(int* year, int* month, int* day, int* hour, int* minute, int* sec);
void astroSetLocalTime(int hour, int minute, int sec);
void astroSetLocalDate(int year, int month, int day);

double astroNormalizeHours(double h);   // 0..24
