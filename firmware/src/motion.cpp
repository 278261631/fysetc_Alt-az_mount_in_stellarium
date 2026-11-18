// motion.cpp - AZ/ALT step generation with FastAccelStepper (RMT backend on ESP32)
#include "motion.h"
#include "config.h"

#include <Arduino.h>
#include <FastAccelStepper.h>
#include <math.h>

static FastAccelStepperEngine s_engine;
static FastAccelStepper*      s_az  = nullptr;   // X motor = azimuth
static FastAccelStepper*      s_alt = nullptr;   // Y motor = altitude

static inline double wrap360(double d) {
  if (!isfinite(d)) return 0.0;
  d = fmod(d, 360.0);
  if (d < 0.0) d += 360.0;
  return d;
}

static inline double wrap180(double d) {
  d = wrap360(d);
  if (d >= 180.0) d -= 360.0;
  return d;
}

static inline int32_t clampToInt32(double v) {
  if (v > 2147483000.0)  v = 2147483000.0;
  if (v < -2147483000.0) v = -2147483000.0;
  return (int32_t)lround(v);
}

void motionBegin() {
  s_engine.init();

  s_az = s_engine.stepperConnectToPin(PIN_AZ_STEP);
  if (s_az) {
    s_az->setDirectionPin(PIN_AZ_DIR);
    s_az->setSpeedInHz(AXIS_MAX_SPEED_HZ);
    s_az->setAcceleration(AXIS_ACCEL_HZ2);
    s_az->setCurrentPosition(0);
  }

  s_alt = s_engine.stepperConnectToPin(PIN_ALT_STEP);
  if (s_alt) {
    s_alt->setDirectionPin(PIN_ALT_DIR);
    s_alt->setSpeedInHz(AXIS_MAX_SPEED_HZ);
    s_alt->setAcceleration(AXIS_ACCEL_HZ2);
    s_alt->setCurrentPosition(0);
  }
}

void motionSetSpeed(uint32_t maxHz, uint32_t accelHz2) {
  if (s_az)  { s_az->setSpeedInHz(maxHz);  s_az->setAcceleration(accelHz2); }
  if (s_alt) { s_alt->setSpeedInHz(maxHz); s_alt->setAcceleration(accelHz2); }
}

void motionSetPositions(int32_t azSteps, int32_t altSteps) {
  if (s_az) s_az->setCurrentPosition(azSteps);
  if (s_alt) {
    const int32_t maxAlt =
        clampToInt32((ALT_MAX_DEG - ALT_MIN_DEG) * (double)ALT_STEPS_PER_DEG);
    if (altSteps < 0) altSteps = 0;
    if (altSteps > maxAlt) altSteps = maxAlt;
    s_alt->setCurrentPosition(altSteps);
  }
}

double motionAzDeg() {
  if (!s_az) return 0.0;
  return wrap360((double)s_az->getCurrentPosition() / (double)AZ_STEPS_PER_DEG);
}

double motionAltDeg() {
  if (!s_alt) return ALT_MIN_DEG;
  double a = ALT_MIN_DEG + (double)s_alt->getCurrentPosition() / (double)ALT_STEPS_PER_DEG;
  if (a < ALT_MIN_DEG) a = ALT_MIN_DEG;
  if (a > ALT_MAX_DEG) a = ALT_MAX_DEG;
  return a;
}

int32_t motionAzSteps()      { return s_az  ? s_az->getCurrentPosition() : 0; }
int32_t motionAltSteps()     { return s_alt ? s_alt->getCurrentPosition() : 0; }
int32_t motionAzTargetSteps()  { return s_az  ? s_az->targetPos() : 0; }
int32_t motionAltTargetSteps() { return s_alt ? s_alt->targetPos() : 0; }

bool motionAzRunning()  { return s_az  ? s_az->isRunning() : false; }
bool motionAltRunning() { return s_alt ? s_alt->isRunning() : false; }

void motionAzMoveToDeg(double azDeg) {
  if (!s_az || !isfinite(azDeg)) return;
  const int32_t cur = s_az->getCurrentPosition();
  const double  curDeg = wrap360((double)cur / (double)AZ_STEPS_PER_DEG);
  const double  deltaDeg = wrap180(azDeg - curDeg);
  const int32_t target = clampToInt32((double)cur + deltaDeg * (double)AZ_STEPS_PER_DEG);
  if (target == cur) return;
  s_az->moveTo(target);
}

void motionAltMoveToDeg(double altDeg) {
  if (!s_alt || !isfinite(altDeg)) return;
  if (altDeg < ALT_MIN_DEG) altDeg = ALT_MIN_DEG;
  if (altDeg > ALT_MAX_DEG) altDeg = ALT_MAX_DEG;
  const int32_t target =
      clampToInt32((altDeg - ALT_MIN_DEG) * (double)ALT_STEPS_PER_DEG);
  if (s_alt->targetPos() == target && s_alt->isRunning()) return;
  s_alt->moveTo(target);
}

void motionStopAll() {
  if (s_az)  s_az->forceStop();
  if (s_alt) s_alt->forceStop();
}

void motionStopAz() {
  if (s_az) s_az->forceStop();
}

void motionStopAlt() {
  if (s_alt) s_alt->forceStop();
}
