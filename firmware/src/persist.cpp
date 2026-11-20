// persist.cpp - NVS persistence: last pointing state + LX200 session command log
#include "persist.h"
#include "config.h"
#include "astro.h"
#include "motion.h"
#include "scope.h"
#include "lx200.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint32_t kStateMagic = 0x41415A31UL;  // "AAZ1"
constexpr uint32_t kLogMagic   = 0x41414C47UL;  // "AALG"
constexpr uint16_t kVersion    = 1;

constexpr uint8_t  kLogMax      = 24;
constexpr size_t   kLogTextLen  = 33;   // 32 chars + NUL
constexpr uint32_t kSaveIntervalMs = 2000;

// flags bits for PersistState
constexpr uint16_t kFlagHaveTarget = 0x0001;
constexpr uint16_t kFlagTracking   = 0x0002;
constexpr uint16_t kFlagHaveSelRa  = 0x0004;
constexpr uint16_t kFlagHaveSelDec = 0x0008;

struct PersistState {
  uint32_t magic;
  uint16_t version;
  uint16_t flags;
  int32_t  azSteps;
  int32_t  altSteps;
  double   tgtRa;       // scope target, client (equinox) frame, hours
  double   tgtDec;      // degrees
  double   selRa;       // :Sr selection, hours
  double   selDec;      // :Sd selection, degrees
  double   syncRaOffH;  // sync offsets (client frame)
  double   syncDecOffD;
  double   utc;         // UTC unix seconds
  uint32_t bootCount;
  uint32_t lastMs;      // millis() of the last accepted :MS#
  uint32_t checksum;
};

struct LogEntry {
  uint32_t ms;
  char     text[kLogTextLen];
};

struct PersistLog {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  LogEntry entries[kLogMax];
  uint32_t checksum;
};

Preferences s_prefs;
bool        s_ready = false;
uint32_t    s_bootCount = 0;
uint32_t    s_lastSaveMs = 0;

LogEntry s_ring[kLogMax];
uint8_t  s_ringCount = 0;
uint8_t  s_ringHead = 0;   // next slot to write

uint32_t fnv1a(const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint32_t)p[i];
    h *= 16777619UL;
  }
  return h;
}

constexpr size_t statePayloadLen() { return offsetof(PersistState, checksum); }
constexpr size_t logPayloadLen()   { return offsetof(PersistLog, checksum); }

// ---------------------------------------------------------------------------
// State snapshot
// ---------------------------------------------------------------------------
void writeState() {
  if (!s_ready) return;

  double tgtRa = 0.0, tgtDec = 0.0;
  bool haveTarget = false, tracking = false;
  scopeGetState(&tgtRa, &tgtDec, &haveTarget, &tracking);

  double selRa = 0.0, selDec = 0.0;
  bool haveSelRa = false, haveSelDec = false;
  lx200GetSelection(&selRa, &selDec, &haveSelRa, &haveSelDec);

  PersistState st;
  memset(&st, 0, sizeof(st));
  st.magic = kStateMagic;
  st.version = kVersion;
  st.azSteps = motionAzSteps();
  st.altSteps = motionAltSteps();
  st.tgtRa = tgtRa;
  st.tgtDec = tgtDec;
  st.selRa = selRa;
  st.selDec = selDec;
  // Sync offsets intentionally NOT persisted: they are only valid for the
  // current power session (v1.2.0).  The legacy struct fields stay zero.
  st.syncRaOffH = 0.0;
  st.syncDecOffD = 0.0;
  st.utc = astroUtcUnix();
  st.bootCount = s_bootCount;
  st.lastMs = lx200LastMsMillis();
  if (haveTarget) st.flags |= kFlagHaveTarget;
  if (tracking)   st.flags |= kFlagTracking;
  if (haveSelRa)  st.flags |= kFlagHaveSelRa;
  if (haveSelDec) st.flags |= kFlagHaveSelDec;
  st.checksum = fnv1a(&st, statePayloadLen());

  s_prefs.putBytes("state", &st, sizeof(st));
}

bool readState(PersistState* st) {
  if (!s_ready || !st) return false;
  if (s_prefs.getBytesLength("state") != sizeof(PersistState)) return false;
  if (s_prefs.getBytes("state", st, sizeof(*st)) != sizeof(*st)) return false;
  if (st->magic != kStateMagic || st->version != kVersion) return false;
  if (st->checksum != fnv1a(st, statePayloadLen())) return false;
  return true;
}

void restoreState(const PersistState& st) {
  // Order matters: positions/clock/target first, then the client selection.
  motionSetPositions(st.azSteps, st.altSteps);
  // Time never goes backwards: :XW writes the clock immediately, so keep the
  // newer of the persisted state and the astro (:XW) clock.
  if (isfinite(st.utc) && st.utc > 0.0 && st.utc > astroUtcUnix()) {
    astroSetUtcUnix(st.utc);
  }
  // Sync offsets (st.syncRaOffH/st.syncDecOffD) are deliberately ignored:
  // a fresh boot always starts with zero offsets.
  scopeRestoreState(st.tgtRa, st.tgtDec,
                    (st.flags & kFlagHaveTarget) != 0,
                    (st.flags & kFlagTracking) != 0);
  lx200RestoreSelection(st.selRa, st.selDec,
                        (st.flags & kFlagHaveSelRa) != 0,
                        (st.flags & kFlagHaveSelDec) != 0);
  lx200RestoreLastMs(st.lastMs);
}

// ---------------------------------------------------------------------------
// Session command log (24-entry RAM ring, persisted as a flat array)
// ---------------------------------------------------------------------------
void ringClear() {
  s_ringCount = 0;
  s_ringHead = 0;
}

void ringPush(const LogEntry& e) {
  s_ring[s_ringHead] = e;
  s_ringHead = (uint8_t)((s_ringHead + 1) % kLogMax);
  if (s_ringCount < kLogMax) s_ringCount++;
}

uint8_t ringStart() {
  return (uint8_t)((s_ringHead + kLogMax - s_ringCount) % kLogMax);
}

void writeLog() {
  if (!s_ready) return;
  PersistLog lg;
  memset(&lg, 0, sizeof(lg));
  lg.magic = kLogMagic;
  lg.version = kVersion;
  lg.count = s_ringCount;
  const uint8_t start = ringStart();
  for (uint8_t i = 0; i < s_ringCount; i++) {
    lg.entries[i] = s_ring[(start + i) % kLogMax];
  }
  lg.checksum = fnv1a(&lg, logPayloadLen());
  s_prefs.putBytes("log", &lg, sizeof(lg));
}

void loadLog() {
  ringClear();
  if (!s_ready) return;
  if (s_prefs.getBytesLength("log") != sizeof(PersistLog)) return;
  PersistLog lg;
  if (s_prefs.getBytes("log", &lg, sizeof(lg)) != sizeof(lg)) return;
  if (lg.magic != kLogMagic || lg.version != kVersion) return;
  if (lg.checksum != fnv1a(&lg, logPayloadLen())) return;
  uint16_t cnt = lg.count;
  if (cnt > kLogMax) cnt = kLogMax;
  for (uint16_t i = 0; i < cnt; i++) ringPush(lg.entries[i]);
}

// Routine position polls and identification/diagnostic queries would otherwise
// fill the 24-entry ring within a few seconds (Stellarium polls :GR/:GD every
// 500 ms).  Keep the log focused on goto/sync/stop and setup traffic.
bool isNoiseCommand(const char* cmd) {
  static const char* const kNoise[] = {
      ":GR", ":GD", ":GA", ":GZ",
      ":XG", ":XJ", ":XS",
      ":GVP", ":GVN", ":GVD", ":GVT",
  };
  for (const char* n : kNoise) {
    if (strncmp(cmd, n, strlen(n)) == 0) return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void persistBegin() {
  s_ready = s_prefs.begin("altazP", false);

  s_bootCount = 1;
  if (s_ready) {
    s_bootCount = s_prefs.getUInt("boot", 0) + 1;
    s_prefs.putUInt("boot", s_bootCount);
  }

  // Restore the previous session's log first, so a stale/invalid state blob
  // cannot wipe the evidence.
  loadLog();

  PersistState st;
  if (readState(&st)) restoreState(st);

  s_lastSaveMs = millis();
  // "boot 时写入": keep the previous session log and a fresh state snapshot.
  writeState();
  writeLog();
}

void persistTick() {
  const uint32_t now = millis();
  if ((uint32_t)(now - s_lastSaveMs) >= kSaveIntervalMs) {
    s_lastSaveMs = now;
    writeState();
  }
}

void persistFlushState() {
  s_lastSaveMs = millis();
  writeState();
}

void persistLogCommand(const char* cmd) {
  if (!cmd || cmd[0] != ':') return;
  if (isNoiseCommand(cmd)) return;
  LogEntry e;
  memset(&e, 0, sizeof(e));
  e.ms = millis();
  size_t n = strlen(cmd);
  if (n >= kLogTextLen) n = kLogTextLen - 1;
  memcpy(e.text, cmd, n);
  e.text[n] = '\0';
  ringPush(e);
}

void persistLogFlush() {
  writeLog();
}

void persistLogToString(char* out, size_t outLen) {
  if (!out || outLen == 0) return;
  out[0] = '\0';

  if (s_ringCount == 0) {
    strncpy(out, "NONE", outLen - 1);
    out[outLen - 1] = '\0';
    return;
  }

  const uint8_t start = ringStart();
  int len[kLogMax];
  for (int i = 0; i < (int)s_ringCount; i++) {
    const LogEntry& e = s_ring[(start + i) % kLogMax];
    char tmp[64];
    len[i] = snprintf(tmp, sizeof(tmp), "%lu:%s", (unsigned long)e.ms, e.text);
    if (len[i] < 0) len[i] = 0;
  }

  // Keep as many NEWEST entries as fit into outLen-1 bytes.
  int first = (int)s_ringCount;
  size_t total = 0;
  for (int i = (int)s_ringCount - 1; i >= 0; i--) {
    const size_t add = (size_t)len[i] + ((i == (int)s_ringCount - 1) ? 0u : 1u);
    if (total + add > outLen - 1) break;
    total += add;
    first = i;
  }
  if (first >= (int)s_ringCount) {
    strncpy(out, "NONE", outLen - 1);
    out[outLen - 1] = '\0';
    return;
  }

  size_t pos = 0;
  for (int i = first; i < (int)s_ringCount; i++) {
    const LogEntry& e = s_ring[(start + i) % kLogMax];
    const int n = snprintf(out + pos, outLen - pos, "%s%lu:%s",
                           (i == first) ? "" : "|",
                           (unsigned long)e.ms, e.text);
    if (n <= 0) break;
    pos += (size_t)n;
    if (pos >= outLen - 1) break;
  }
}

void persistResetState() {
  scopeStop();
  motionSetPositions(0, 0);
  scopeClearTarget();
  lx200ClearSelection();
  lx200ResetLastMs();
  if (s_ready) {
    s_prefs.remove("state");
    writeState();   // write the cleared state back (magic valid, positions 0)
  }
  s_lastSaveMs = millis();
}

uint32_t persistBootCount() {
  return s_bootCount;
}
