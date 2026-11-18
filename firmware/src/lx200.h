// lx200.h - non-blocking LX200 serial protocol front end (UART0 / CH340 / COM7)
#pragma once

#include <stdint.h>

void lx200Begin();
void lx200Poll();   // call frequently from loop()

// Milliseconds since the last byte was received on the LX200 link (used for
// the "no command -> stop" safety timeout).
uint32_t lx200IdleMs();

// ---- selection (:Sr / :Sd) accessors, used by the NVS persistence layer ----
void lx200GetSelection(double* raHours, double* decDeg, bool* haveRa, bool* haveDec);
void lx200RestoreSelection(double raHours, double decDeg, bool haveRa, bool haveDec);
void lx200ClearSelection();

// ---- last accepted :MS# ---------------------------------------------------
uint32_t lx200LastMsMillis();           // 0 = no goto since boot
void     lx200RestoreLastMs(uint32_t ms);
void     lx200ResetLastMs();
