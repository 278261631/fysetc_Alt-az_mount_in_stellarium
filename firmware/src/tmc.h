// tmc.h - TMC2209 UART configuration (shared single-wire bus on Serial1)
#pragma once

#include <stdint.h>

void tmcBegin();

// Bit 0 = AZ driver answered on the UART bus, bit 1 = ALT driver answered.
uint8_t tmcOkMask();
bool    tmcAzOk();
bool    tmcAltOk();

// Short ASCII description for the boot banner / :XG# diagnostics.
const char* tmcStatusText();

// Raw IOIN values (0 when the driver did not answer).
uint32_t tmcIoinAz();
uint32_t tmcIoinAlt();
