// scope.h - high level mount state machine: goto / stop / sync / tracking
#pragma once

#include <stddef.h>
#include <stdint.h>

void scopeBegin();

// Slew to equatorial coordinates (RA hours 0..24, Dec degrees).
// Returns false when the target is not finite.
bool scopeGotoRaDec(double raHours, double decDeg);

// Re-define the current pointing as the given equatorial coordinates.
bool scopeSyncRaDec(double raHours, double decDeg);

// Abort all motion (LX200 :Q#).
void scopeStop();

// Called from loop(): goto completion detection and sidereal tracking.
void scopeUpdate();

// Pointing as reported to the client: equatorial of date (sync offsets applied).
void scopePointingRaDec(double* raHours, double* decDeg);

double scopeAzDeg();
double scopeAltDeg();

bool scopeTracking();
bool scopeSlewing();

// Persisted state (NVS restore after a reset).  Coordinates are in the client
// (equinox) frame, identical to the :Sr/:Sd arguments.  No motion is started.
// NOTE: sync offsets are deliberately NOT part of the persisted state; they are
// only valid for the current power session (cleared on :XW / :XC / :XR / boot).
void scopeGetState(double* tgtRa, double* tgtDec, bool* haveTarget, bool* tracking);
void scopeRestoreState(double tgtRa, double tgtDec, bool haveTarget, bool tracking);

// Forget target / tracking / sync offsets (pointing reset).  Does not move axes.
void scopeClearTarget();

// Drop the sync offsets (time/site changed or explicit reset) and recompute the
// motion target for the (still selected) requested target.
void scopeClearSyncOffsets();

// Sync offset + requested target (for :XS# diagnostics).
void scopeGetSyncInfo(double* syncRaOffH, double* syncDecOffD,
                      double* reqRa, double* reqDec, bool* haveTarget);

// One-line :XS# payload (no '#'), e.g. "SYN=+00.0000/+00.00 TGTREQ=19.8464/+8.87"
void scopeSyncDiag(char* out, size_t outLen);

// One-line ASCII diagnostics, no '#' inside, for the :XG# command.
void scopeDiag(char* out, size_t outLen);
