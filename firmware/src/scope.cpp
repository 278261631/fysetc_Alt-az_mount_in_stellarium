// scope.cpp - high level mount state machine: goto / stop / sync / tracking
#include "scope.h"
#include "config.h"
#include "astro.h"
#include "motion.h"
#include "tmc.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>

enum ScopeState { ST_IDLE = 0, ST_SLEW = 1 };

static ScopeState s_state = ST_IDLE;
static bool       s_haveTarget = false;
static double     s_tgtRa = 0.0;      // hours
static double     s_tgtDec = 0.0;     // degrees
static bool       s_tracking = false;
static double     s_syncRaOffH = 0.0; // hours added to the reported RA
static double     s_syncDecOffD = 0.0;
static uint32_t   s_slewStartMs = 0;
static uint32_t   s_lastTrackMs = 0;

static bool validCoords(double raHours, double decDeg) {
  if (!isfinite(raHours) || !isfinite(decDeg)) return false;
  if (decDeg < -90.0 || decDeg > 90.0) return false;
  return true;
}

// Mount pointing derived from the actual step positions (no sync offsets),
// in the internal equator/equinox-of-date frame.
static void rawPointing(double* raHours, double* decDeg) {
  astroAltAzToRaDec(motionAltDeg(), motionAzDeg(), raHours, decDeg);
}

// Client (Stellarium) coordinate frame <-> internal of-date frame.
static void dateToClient(double raDate, double decDate, double* raClient, double* decClient) {
#if CLIENT_EQUINOX_J2000
  astroOfDateToJ2000(raDate, decDate, raClient, decClient);
#else
  *raClient = raDate;
  *decClient = decDate;
#endif
}

static void clientToDate(double raClient, double decClient, double* raDate, double* decDate) {
#if CLIENT_EQUINOX_J2000
  astroJ2000ToOfDate(raClient, decClient, raDate, decDate);
#else
  *raDate = raClient;
  *decDate = decClient;
#endif
}

void scopeBegin() {
  s_state = ST_IDLE;
  s_haveTarget = false;
  s_tracking = false;
  s_syncRaOffH = 0.0;
  s_syncDecOffD = 0.0;
  s_tgtRa = 0.0;
  s_tgtDec = 0.0;
  s_slewStartMs = millis();
  s_lastTrackMs = millis();
}

bool scopeGotoRaDec(double raHours, double decDeg) {
  if (!validCoords(raHours, decDeg)) return false;

  // Stellarium target (J2000 by default) -> local equatorial of date -> alt/az.
  double raDate = 0.0, decDate = 0.0;
  clientToDate(astroNormalizeHours(raHours), decDeg, &raDate, &decDate);
  double alt = 0.0, az = 0.0;
  astroRaDecToAltAz(raDate, decDate, &alt, &az);
  if (!isfinite(alt) || !isfinite(az)) return false;

  s_tgtRa = astroNormalizeHours(raHours);
  s_tgtDec = decDeg;
  s_haveTarget = true;

  motionAzMoveToDeg(az);
  motionAltMoveToDeg(alt);   // clamped to ALT_MIN..ALT_MAX

  s_state = ST_SLEW;
  s_tracking = false;
  s_slewStartMs = millis();
  return true;
}

bool scopeSyncRaDec(double raHours, double decDeg) {
  if (!validCoords(raHours, decDeg)) return false;

  // Current pointing expressed in the client frame, then shift the reported
  // coordinates so that the mount reports the synced position.
  double rawRa = 0.0, rawDec = 0.0;
  rawPointing(&rawRa, &rawDec);
  double cliRa = 0.0, cliDec = 0.0;
  dateToClient(rawRa, rawDec, &cliRa, &cliDec);
  if (!isfinite(cliRa) || !isfinite(cliDec)) return false;

  s_syncRaOffH  = astroNormalizeHours(astroNormalizeHours(raHours) - cliRa);
  s_syncDecOffD = decDeg - cliDec;

  s_tgtRa = astroNormalizeHours(raHours);
  s_tgtDec = decDeg;
  s_haveTarget = true;
  s_tracking = false;
  if (s_state == ST_SLEW) {
    motionStopAll();
    s_state = ST_IDLE;
  }
  return true;
}

void scopeStop() {
  motionStopAll();
  s_tracking = false;
  s_state = ST_IDLE;
}

void scopeGetState(double* tgtRa, double* tgtDec, bool* haveTarget,
                   double* syncRaOffH, double* syncDecOffD, bool* tracking) {
  if (tgtRa)       *tgtRa = s_tgtRa;
  if (tgtDec)      *tgtDec = s_tgtDec;
  if (haveTarget)  *haveTarget = s_haveTarget;
  if (syncRaOffH)  *syncRaOffH = s_syncRaOffH;
  if (syncDecOffD) *syncDecOffD = s_syncDecOffD;
  if (tracking)    *tracking = s_tracking;
}

void scopeRestoreState(double tgtRa, double tgtDec, bool haveTarget,
                       double syncRaOffH, double syncDecOffD, bool tracking) {
  if (!validCoords(tgtRa, tgtDec) || !isfinite(syncRaOffH) || !isfinite(syncDecOffD)) {
    return;
  }
  s_tgtRa = astroNormalizeHours(tgtRa);
  s_tgtDec = tgtDec;
  s_haveTarget = haveTarget;
  s_syncRaOffH = syncRaOffH;
  s_syncDecOffD = syncDecOffD;
  s_tracking = tracking && haveTarget;
  s_state = ST_IDLE;
  s_slewStartMs = millis();
  s_lastTrackMs = millis();
}

void scopeClearTarget() {
  s_haveTarget = false;
  s_tracking = false;
  s_syncRaOffH = 0.0;
  s_syncDecOffD = 0.0;
  s_tgtRa = 0.0;
  s_tgtDec = 0.0;
  s_state = ST_IDLE;
}

void scopeUpdate() {
  const uint32_t now = millis();

  if (s_state == ST_SLEW) {
    if ((uint32_t)(now - s_slewStartMs) >= GOTO_SETTLE_MS &&
        !motionAzRunning() && !motionAltRunning()) {
      s_state = ST_IDLE;
      s_tracking = s_haveTarget;
      s_lastTrackMs = now;
    }
    return;
  }

  if (s_tracking && s_haveTarget &&
      (uint32_t)(now - s_lastTrackMs) >= TRACK_INTERVAL_MS) {
    s_lastTrackMs = now;
    double raDate = 0.0, decDate = 0.0;
    clientToDate(s_tgtRa, s_tgtDec, &raDate, &decDate);
    double alt = 0.0, az = 0.0;
    astroRaDecToAltAz(raDate, decDate, &alt, &az);
    if (isfinite(alt) && isfinite(az)) {
      motionAzMoveToDeg(az);
      motionAltMoveToDeg(alt);
    }
  }
}

void scopePointingRaDec(double* raHours, double* decDeg) {
  double ra = 0.0, dec = 0.0;
  rawPointing(&ra, &dec);
  if (!isfinite(ra) || !isfinite(dec)) { ra = 0.0; dec = 0.0; }
  double cliRa = 0.0, cliDec = 0.0;
  dateToClient(ra, dec, &cliRa, &cliDec);
  ra = astroNormalizeHours(cliRa + s_syncRaOffH);
  dec = cliDec + s_syncDecOffD;
  if (dec > 90.0)  dec = 90.0;
  if (dec < -90.0) dec = -90.0;
  if (raHours) *raHours = ra;
  if (decDeg)  *decDeg = dec;
}

double scopeAzDeg()  { return motionAzDeg(); }
double scopeAltDeg() { return motionAltDeg(); }

bool scopeTracking() { return s_tracking; }
bool scopeSlewing()  { return s_state == ST_SLEW; }

// Steps are clamped for display so the whole :XG# line stays below 200 bytes.
static long diagSteps(long v) {
  if (v > 999999L) return 999999L;
  if (v < -999999L) return -999999L;
  return v;
}

void scopeDiag(char* out, size_t outLen) {
  if (!out || outLen == 0) return;
  double ra = 0.0, dec = 0.0;
  scopePointingRaDec(&ra, &dec);
  // Compact single line; the caller appends BOOT=/LASTMS=.
  snprintf(out, outLen,
           "S=%s AZ=%.2f ALT=%.2f AZS=%ld/%ld ALTS=%ld/%ld TRK=%d "
           "RA=%.4f DEC=%+.2f TGT=%.4f/%+.2f TMC=%s UTC=%.0f LAT=%.2f LON=%+.2f",
           scopeSlewing() ? "SLEW" : "IDLE",
           scopeAzDeg(), scopeAltDeg(),
           diagSteps((long)motionAzSteps()), diagSteps((long)motionAzTargetSteps()),
           diagSteps((long)motionAltSteps()), diagSteps((long)motionAltTargetSteps()),
           s_tracking ? 1 : 0,
           ra, dec, s_tgtRa, s_tgtDec,
           tmcOkMask() == 0x03 ? "OK" : (tmcOkMask() == 0x01 ? "AZ" :
                                        (tmcOkMask() == 0x02 ? "ALT" : "FAIL")),
           astroUtcUnix(),
           astroSiteLatDeg(), astroSiteLonDeg());
}
