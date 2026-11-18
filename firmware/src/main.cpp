// main.cpp - FYSETC E4 Alt-Az mount controller
//
//   * two TMC2209 (X = azimuth, Y = altitude) on the FYSETC E4 board
//   * FastAccelStepper step pulse generation (RMT backend)
//   * LX200 serial protocol on UART0 (CH340 / COM7), 9600 8N1 (Stellarium 23.4
//     hard-codes 9600 for its direct LX200 serial connection)
//
// UART0 is the LX200 channel: at runtime NOTHING is printed on it except the
// exact LX200 replies (and the custom :XG# diagnostics).  The one-time boot
// banner is compiled out by default (LX200_BOOT_BANNER=0) because a Stellarium
// session would otherwise read those bytes as a malformed :GR# answer.
#include <Arduino.h>

#include "config.h"
#include "astro.h"
#include "motion.h"
#include "tmc.h"
#include "scope.h"
#include "lx200.h"
#include "persist.h"

#if LX200_BOOT_BANNER
static void printBanner() {
  Serial.println();
  Serial.println("==============================================================");
  Serial.print("FYSETC E4 AltAz LX200 ready | firmware ");
  Serial.println(FIRMWARE_VERSION);
  Serial.print("Product: ");
  Serial.println(PRODUCT_NAME);
  Serial.println("Link: UART0/CH340 9600 8N1, commands '#'-terminated");
  Serial.print("TMC2209 UART (Serial1 21/22): ");
  Serial.println(tmcStatusText());
  Serial.printf("Mechanics: AZ %.2f steps/deg, ALT %.2f steps/deg, ALT limits %.0f..%.0f deg\n",
                (double)AZ_STEPS_PER_DEG, (double)ALT_STEPS_PER_DEG,
                (double)ALT_MIN_DEG, (double)ALT_MAX_DEG);
  Serial.printf("Motion: max %lu steps/s, accel %lu steps/s^2, microsteps %d\n",
                (unsigned long)AXIS_MAX_SPEED_HZ, (unsigned long)AXIS_ACCEL_HZ2,
                TMC_MICROSTEPS);
  Serial.printf("Default site: lat %.4f lon %.4f (east+) | clock = build time (UTC)\n",
                astroSiteLatDeg(), astroSiteLonDeg());
  Serial.println("Diag: :XG#   Set UTC: :XW<unix>#   Set site: :XC<lat>,<lon>#");
  Serial.println("==============================================================");
}
#endif

void setup() {
  // UART0: GPIO3 RX / GPIO1 TX -> CH340 -> COM7, LX200 channel @ 9600 8N1.
  Serial.begin(LX200_BAUD);
  delay(50);

  // Shared stepper enable, active LOW.  Keep the drivers disabled while the
  // TMC2209 registers are being configured.
  pinMode(PIN_EN, OUTPUT);
  digitalWrite(PIN_EN, HIGH);

  astroBegin();
  motionBegin();
  tmcBegin();       // degrades gracefully to pure STEP/DIR when UART is absent
  scopeBegin();
  lx200Begin();

  // Restore last pointing state + previous session log from NVS.  This runs
  // after motion/scope/lx200 init and has no side effects on the motors.
  persistBegin();

  digitalWrite(PIN_EN, LOW);   // enable drivers: motors hold current
#if LX200_BOOT_BANNER
  printBanner();
#endif
}

void loop() {
  lx200Poll();
  scopeUpdate();
  persistTick();   // save pointing state every ~2 s

  // Safety: if the LX200 link goes quiet (client closed/crashed), stop the
  // motors instead of slewing/tracking forever.
  if (lx200IdleMs() > LX200_IDLE_STOP_MS && (scopeTracking() || scopeSlewing())) {
    scopeStop();
  }

  delay(1);
}
