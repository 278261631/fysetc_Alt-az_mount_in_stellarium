// persist.h - NVS persistence: last pointing state + LX200 session command log
//
// Why: opening COM7 makes the CH340 assert DTR/RTS and reset the ESP32, so the
// RAM state (axis positions, target, tracking) and any evidence of what the
// client sent would otherwise be lost.  The last known state and the last 24
// interesting LX200 commands are kept in NVS (namespace "altazP").
#pragma once

#include <stddef.h>
#include <stdint.h>

// Call once in setup(), after astro/motion/scope/lx200 init.  Restores the last
// saved pointing state and the previous session's command log, increments the
// boot counter and saves everything back to NVS.
void persistBegin();

// Call from loop(): saves the pointing state every ~2 s.
void persistTick();

// Force an immediate pointing-state save (used right after :MS#).
void persistFlushState();

// Record one received LX200 command (string without the trailing '#').  Routine
// position polls (:GR/:GD) and the diagnostics themselves (:XG/:XJ) are filtered
// out so the 24-entry ring holds the interesting goto/sync/stop traffic.
void persistLogCommand(const char* cmd);

// Write the session command log to NVS (on :Sr/:Sd/:CM#/:Q#/:MS# and at boot).
void persistLogFlush();

// Build the ":XJ#" payload: "time:cmd|time:cmd|...#" - style, WITHOUT the final
// '#'.  Returns "NONE" when the ring is empty.  Newest entries are preserved.
void persistLogToString(char* out, size_t outLen);

// :XR# - clear the persisted pointing state and zero both axis positions.
void persistResetState();

// Number of boots since NVS was first initialised (shown in :XG#).
uint32_t persistBootCount();
