// motion.h - two-axis step pulse generation (FastAccelStepper)
#pragma once

#include <stdint.h>

void motionBegin();
void motionSetSpeed(uint32_t maxHz, uint32_t accelHz2);

// Overwrite the current step positions (used to restore the last known pointing
// from NVS after a reset - no motion is generated).
void motionSetPositions(int32_t azSteps, int32_t altSteps);

// Absolute AZ target in degrees; the shortest way round is used and the axis
// may rotate continuously (position is not wrapped).
void motionAzMoveToDeg(double azDeg);
// Absolute ALT target in degrees, clamped to ALT_MIN_DEG..ALT_MAX_DEG.
void motionAltMoveToDeg(double altDeg);

void motionStopAll();
void motionStopAz();
void motionStopAlt();

double  motionAzDeg();        // current, 0..360
double  motionAltDeg();       // current, ALT_MIN..ALT_MAX
int32_t motionAzSteps();
int32_t motionAltSteps();
int32_t motionAzTargetSteps();
int32_t motionAltTargetSteps();
bool    motionAzRunning();
bool    motionAltRunning();
