// tmc.cpp - TMC2209 UART configuration (Serial1, shared single-wire bus)
//
// FYSETC E4: the four TMC2209 share HardwareSerial1 (RX=21 / TX=22) and are
// distinguished by hardware strapping addresses.  The FYSETC E4 Marlin example
// uses  X=1  Y=3  Z=0  E0=2  and R_SENSE = 0.11 ohm (Marlin TMC2209 default).
// X is our AZ axis, Y is our ALT axis.
//
// If a driver does not answer on the UART the firmware keeps running with pure
// STEP/DIR pulses (stealthChop stand-alone mode) - the motors still turn.
#include "tmc.h"
#include "config.h"

#include <Arduino.h>
#include <TMCStepper.h>
#include <stdio.h>

static TMC2209Stepper* s_az  = nullptr;
static TMC2209Stepper* s_alt = nullptr;
static bool     s_okAz  = false;
static bool     s_okAlt = false;
static uint32_t s_ioinAz  = 0;
static uint32_t s_ioinAlt = 0;
static char     s_status[40] = "AZ=FAIL ALT=FAIL";

static bool configureDriver(TMC2209Stepper* d, uint32_t* ioinOut) {
  if (!d) return false;
  d->begin();
  // stealthChop, automatic current scaling, 650 mA RMS, 1/16 microsteps
  d->toff(4);
  d->blank_time(24);
  d->rms_current(TMC_RMS_CURRENT_MA);
  d->microsteps(TMC_MICROSTEPS);
  d->en_spreadCycle(false);
  d->pwm_autoscale(true);
  d->ihold(8);            // hold current 8/32 of run current
  delay(5);
  const uint32_t ioin = d->IOIN();
  if (ioinOut) *ioinOut = ioin;
  // test_connection() returns 0 when the driver answered with a sane DRV_STATUS.
  return d->test_connection() == 0;
}

void tmcBegin() {
  Serial1.begin(TMC_BAUD, SERIAL_8N1, PIN_TMC_RX, PIN_TMC_TX);
  delay(50);

  s_az  = new TMC2209Stepper(&Serial1, TMC_R_SENSE, TMC_ADDR_AZ);
  s_alt = new TMC2209Stepper(&Serial1, TMC_R_SENSE, TMC_ADDR_ALT);

  s_okAz  = configureDriver(s_az,  &s_ioinAz);
  s_okAlt = configureDriver(s_alt, &s_ioinAlt);

  snprintf(s_status, sizeof(s_status), "AZ=%s ALT=%s",
           s_okAz ? "OK" : "FAIL", s_okAlt ? "OK" : "FAIL");
}

uint8_t tmcOkMask() {
  return (uint8_t)((s_okAz ? 0x01 : 0x00) | (s_okAlt ? 0x02 : 0x00));
}
bool tmcAzOk()  { return s_okAz; }
bool tmcAltOk() { return s_okAlt; }

const char* tmcStatusText() { return s_status; }

uint32_t tmcIoinAz()  { return s_ioinAz; }
uint32_t tmcIoinAlt() { return s_ioinAlt; }
