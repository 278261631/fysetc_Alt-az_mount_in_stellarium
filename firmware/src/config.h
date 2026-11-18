// config.h - board / mechanics / defaults for the FYSETC E4 Alt-Az controller
//
// Board: FYSETC E4 (ESP32-WROOM-32, Marlin env "fysetc_e4")
//   pin names taken from MarlinFirmware/Marlin pins_FYSETC_E4.h + pins_ESPA_common.h
//   TMC2209 hardware addresses taken from FYSETC/FYSETC-E4
//   examples/Ender-3/Configuration_adv.h:  X=1  Y=3  Z=0  E0=2
//   (R_SENSE follows the Marlin TMC2209 default 0.11 ohm)
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Stepper pins (FYSETC E4)
// ---------------------------------------------------------------------------
#define PIN_AZ_STEP   27   // X_STEP_PIN  -> azimuth axis
#define PIN_AZ_DIR    26   // X_DIR_PIN
#define PIN_ALT_STEP  33   // Y_STEP_PIN  -> altitude axis
#define PIN_ALT_DIR   32   // Y_DIR_PIN
#define PIN_EN        25   // X/Y/Z/E0 shared ENABLE, active LOW
#define PIN_TMC_RX    21   // HARDWARE_SERIAL1_RX
#define PIN_TMC_TX    22   // HARDWARE_SERIAL1_TX

// ---------------------------------------------------------------------------
// TMC2209 (shared single-wire UART bus on Serial1)
// ---------------------------------------------------------------------------
#define TMC_BAUD            115200
#define TMC_R_SENSE         0.11f
#define TMC_ADDR_AZ         1     // FYSETC E4: X driver
#define TMC_ADDR_ALT        3     // FYSETC E4: Y driver
#define TMC_RMS_CURRENT_MA  650   // same value the stock Marlin firmware reports via M906
#define TMC_MICROSTEPS      16
// If the UART link cannot be established the drivers stay in stand-alone mode;
// we then assume this microstep resolution for the degree <-> step conversion.
#define TMC_FALLBACK_MICROSTEPS 16

// ---------------------------------------------------------------------------
// Mechanics
// ---------------------------------------------------------------------------
// The mount is mechanically unknown/unloaded.  These constants only define how
// "degrees" map to step counts; the motor spins either way.
//   200 full steps/rev * 16 microsteps * 5 (virtual reduction) / 360 = 44.44 steps/deg
#define MOTOR_FULL_STEPS    200
#define AZ_GEAR_RATIO       5.0f
#define ALT_GEAR_RATIO      5.0f

#define AZ_STEPS_PER_DEG    ((float)MOTOR_FULL_STEPS * (float)TMC_MICROSTEPS * AZ_GEAR_RATIO / 360.0f)
#define ALT_STEPS_PER_DEG   ((float)MOTOR_FULL_STEPS * (float)TMC_MICROSTEPS * ALT_GEAR_RATIO / 360.0f)

// ---------------------------------------------------------------------------
// Motion limits
// ---------------------------------------------------------------------------
#define AXIS_MAX_SPEED_HZ   2000  // steps/s
#define AXIS_ACCEL_HZ2      800   // steps/s^2
#define ALT_MIN_DEG         0.0f
#define ALT_MAX_DEG         90.0f // use 180.0f for a 0..180 deg altitude range

// ---------------------------------------------------------------------------
// Tracking / state machine
// ---------------------------------------------------------------------------
#define TRACK_INTERVAL_MS   200
#define GOTO_SETTLE_MS      300

// ---------------------------------------------------------------------------
// Default site (overridden at runtime by :XC / LX200 :St / :Sg)
// ---------------------------------------------------------------------------
#define SITE_LAT_DEG_DEFAULT   39.9042   // Beijing, + north
#define SITE_LON_DEG_DEFAULT   116.4074  // + east
#define DEFAULT_UTC_OFFSET_SEC (8 * 3600) // local = UTC + offset (build host is +08:00)

// ---------------------------------------------------------------------------
// LX200 link
// ---------------------------------------------------------------------------
// Stellarium 23.4's direct LX200 serial client is hard-coded to 9600 8N1
// (plugins/TelescopeControl/src/common/SerialPort.cpp).  Flashing still uses
// the ESP32 ROM bootloader baud (upload_speed), unrelated to this value.
#define LX200_BAUD          9600
#define PRODUCT_NAME        "FYSETC E4 AltAz"
#define FIRMWARE_VERSION    "1.1.0"

// Client (Stellarium) coordinate frame.  The recommended telescope configuration
// uses "equinox": "JNow" (see docs/stellarium-config.md), i.e. the client sends
// and expects coordinates of the mean equator/equinox of date -> set 0.
// Set to 1 if the client is configured for J2000 (firmware then applies IAU 1976
// precession between J2000 and of-date).
#define CLIENT_EQUINOX_J2000 0

// No-command safety: if nothing has been received on the LX200 link for this
// long, stop the motors / tracking (protects against a closed serial port).
#define LX200_IDLE_STOP_MS  6000

// UART0 is the LX200 channel and must stay silent at runtime.  The one-time
// boot banner is therefore disabled by default; enable for bench diagnosis.
#if !defined(LX200_BOOT_BANNER)
  #define LX200_BOOT_BANNER 0
#endif
