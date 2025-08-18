// -----------------------------------------------------------------------------------
// Servo Tracking Velocity Calibration - Overview
//
// This routine performs a 3-phase, bidirectional calibration to determine the
// PWM duty cycles needed to reliably track at sidereal velocity with a DC servo motor.
// The process measures the minimum and maximum duty cycles needed to overcome
// static friction (stiction) and identifies the lowest sustained PWM values
// required for continuous motion.
//
// Calibration logic (per direction) proceeds in 3 main steps:
//
// 1) Stiction Ceiling (stictionCeiling):
//    - Use exponential search (doubling the PWM value starting from
//      SERVO_CALIBRATION_START_DUTY_CYCLE) to find a PWM that initiates
//      any movement.
//    - This gives the upper bound for the motor's stiction threshold.
//    - Result is stored in stictionCeilingFwd or stictionCeilingRev.
//
// 2) Stiction Floor (stictionFloor):
//    - Refine the result using a binary search between
//      SERVO_CALIBRATION_VELOCITY_SEARCH_MIN_FACTOR * stictionCeiling
//      and stictionCeiling.
//    - This identifies the lowest PWM that reliably causes movement.
//    - Between each test step, the motor is stopped for
//      SERVO_CALIBRATION_MOTOR_SETTLE_TIME to ensure clean restarts
//      and avoid carry-over motion.
//    - The final result is stored in stictionFloorFwd or stictionFloorRev.
//    - This PWM will also serve as a kickstarter during the next phase.
//
// 3) Velocity Tracking Calibration:
//    - This was an experiment and might be removed in the future. The idea was to check
//      whether if we kick-start the motor with the min breaking stiction PWM if we can lower gradually
//      the needed PWM duty cycle to keep the motor going. If this was the case we could have the motor
//      rotating closer to the sidereal rotation.
//    - Starting from stictionFloor, a binary search is used to find the lowest
//      PWM that maintains continuous rotation after kickstarting the motor.
//    - The motor is kicked using stictionFloor before attempting a lower PWM.
//    - If the motor stalls or fails to rotate, PWM is increased again.
//    - The result is the trackingPwmFwd or trackingPwmRev, representing the duty
//      cycle needed to maintain sidereal tracking velocity with minimal power.
//
//    At least in my mesu-200 maxon 353611 motors this completely fails.
//
// After both directions are calibrated, an imbalance check is performed.
// If the forward/reverse duty cycles differ significantly, a warning is logged.
// This can indicate mount imbalance or frictional asymmetry.
//
// The result is a robust and safe calibration of tracking PWM duty cycles
// customized per motor and mechanical setup and telescope load.

#include "TrackingVelocity.h"

#if defined(SERVO_MOTOR_PRESENT) && defined(CALIBRATE_SERVO_DC)

ServoCalibrateTrackingVelocity::ServoCalibrateTrackingVelocity(uint8_t axisNumber) {
  this->axisNumber = axisNumber;
  snprintf(axisPrefix, sizeof(axisPrefix), "Axis%d VelCal", axisNumber);
}

void ServoCalibrateTrackingVelocity::init() {
  experimentPwm = 0.0f;
  experimentMode = false;
  calibrationState = CALIBRATION_IDLE;
  motorState = MOTOR_STOPPED;

  // Reset calibration values
  stictionCeilingFwd = SERVO_CALIBRATION_STOP_DUTY_CYCLE;
  stictionFloorFwd = 0.0f;
  trackingPwmFwd = 0.0f;
  stictionCeilingRev = SERVO_CALIBRATION_STOP_DUTY_CYCLE;
  stictionFloorRev = 0.0f;
  trackingPwmRev = 0.0f;

  lastStateChangeTime = 0;
  calibrationStepStartTime = 0;
  calibrationStepStartTicks = 0;
  lastVelocityMeasurement = 0;
  lastVelocityTime = 0;

  lastTicks = 0;
  lastCheckTime = 0;
  enabled = false;
  everMovedFwd = false;
  everMovedRev = false;
}

void ServoCalibrateTrackingVelocity::start(float trackingFrequency, long instrumentCoordinateSteps) {
  if (!(CALIBRATE_SERVO_AXIS_SELECT & (1 << (axisNumber - 1)))) {
    VF("MSG: "); V(axisPrefix); VLF(" Calibration skipped for this axis");
    return;
  }

  VF("MSG: "); V(axisPrefix); VL(" Starting 3-phase bidirectional calibration");

  // Initialize state machine
  enabled = true;
  experimentMode = true;
  calibrationDirectionIsForward = true;
  calibrationState = CALIBRATION_STICTION_CEILING;
  motorState = MOTOR_STOPPED;
  calibrationPwm = SERVO_CALIBRATION_START_DUTY_CYCLE;
  targetVelocity = trackingFrequency;
  lastStateChangeTime = millis();

  // Start by stopping motor and waiting for settle
  setPwm(0);
  startSettling();
}

void ServoCalibrateTrackingVelocity::updateState(long instrumentCoordinateSteps) {
  if(!enabled || !experimentMode) return;

  currentTime = millis();
  currentTicks = instrumentCoordinateSteps;

  // Handle motor state transitions
  handleMotorState();

  // State timeout check (safety feature)
  if (currentTime - lastStateChangeTime > SERVO_CALIBRATION_TIMEOUT ) {
    VF("WARN: "); V(axisPrefix); VLF(" State timeout, resetting calibration");
    handleCalibrationFailure();
    return;
  }

  // Only process state machine when motor is in steady state or stopped
  if (motorState != MOTOR_RUNNING_STEADY && motorState != MOTOR_STOPPED) return;

  // Main state machine processing
  switch (calibrationState) {
    case CALIBRATION_IDLE:
      break;

    case CALIBRATION_STICTION_CEILING:
      processStictionCeiling();
      break;

    case CALIBRATION_STICTION_FLOOR:
      processStictionFloor();
      break;

    case CALIBRATION_CHECK_IMBALANCE:
      processImbalanceCheck();
      break;

    default:
      break;
  }
}

// Private helper methods //////////////////////////////////////////////////////

void ServoCalibrateTrackingVelocity::handleMotorState() {
  float currentVelocity = 0.0;
  switch (motorState) {
    case MOTOR_SETTLING:
      // check also the velocity
      currentVelocity = calculateInstantaneousVelocity();
      if (currentTime - settleStartTime >= SERVO_CALIBRATION_MOTOR_SETTLE_TIME) {
        motorState = MOTOR_STOPPED;
        V(axisPrefix); VF(": Motor settled to STOPPED_STATE"); VF(" Vel="); V(currentVelocity); VL(" steps/s");
      }
      lastVelocityMeasurement = currentVelocity;
      lastVelocityTime = currentTime;
      break;

    case MOTOR_ACCELERATING:
      // Check if motor has reached steady state
      if (currentTime - lastVelocityTime >= SERVO_CALIBRATION_VELOCITY_SETTLE_CHECK_INTERVAL) {
        currentVelocity = calculateInstantaneousVelocity();
        float velocityChange = fabs(currentVelocity - lastVelocityMeasurement);

        if (fabsf(currentVelocity) > SERVO_CALIBRATION_MIN_DETECTABLE_VELOCITY && fabsf(velocityChange) < SERVO_CALIBRATION_VELOCITY_STABILITY_THRESHOLD) {
          motorState = MOTOR_RUNNING_STEADY;
          calibrationStepStartTime = currentTime;
          calibrationStepStartTicks = currentTicks;
          V(axisPrefix); VF(": Motor reached STEADY_STATE"); VF(" Vel="); V(currentVelocity); VL(" steps/s");
        } else if (fabsf(currentVelocity) <= SERVO_CALIBRATION_MIN_DETECTABLE_VELOCITY) {
          // The  motor never managed to
          motorState = MOTOR_STOPPED;
          calibrationStepStartTime = currentTime;
          calibrationStepStartTicks = currentTicks;
          V(axisPrefix); VF(": Motor never moved due to stiction! Set it in STOPPED_STATE"); VF(" Vel="); V(currentVelocity); VL(" steps/s");
        }

        lastVelocityMeasurement = currentVelocity;
        lastVelocityTime = currentTime;
      }
      break;

    case MOTOR_RUNNING_STEADY:
      // Motor is running steady, nothing to do
      break;

    case MOTOR_STOPPED:
      // Nothing to do here, waiting for state machine to start next test
      break;
  }
}

// compute current speed
float ServoCalibrateTrackingVelocity::calculateInstantaneousVelocity() {
  if (lastCheckTime == 0) {
    lastCheckTime = currentTime;
    lastTicks = currentTicks;
    return 0.0f;
  }


  float elapsed = (currentTime - lastCheckTime) / 1000.0f;
  if (elapsed <= 0) return 0.0f;
  float velocity = (currentTicks - lastTicks) / elapsed;

#ifdef SERVO_CAL_DEBUG
  VF("DBG: "); V(axisPrefix);VF(" PWM="); V(calibrationPwm);
  VF(" dTicks="); V(currentTicks - lastTicks);
  VF(" dt(ms)=");V(currentTime - lastCheckTime);
  VF(" v="); V(velocity);VL(" steps/s");
#endif

  lastTicks = currentTicks;
  lastCheckTime = currentTime;

  return velocity;
}

// This function checks whether stiction is broken. Else it does an exponential search over the PWM space
// clamped to SERVO_CALIBRATION_STOP_DUTY_CYCLE
void ServoCalibrateTrackingVelocity::processStictionCeiling() {
  // sample at a short cadence
  if (currentTime - lastVelocityTime < SERVO_CALIBRATION_STICTION_SAMPLE_INTERVAL_MS) return;

  float v = calculateInstantaneousVelocity();
  lastVelocityTime = currentTime;

  if (fabsf(v) > SERVO_CALIBRATION_MIN_DETECTABLE_VELOCITY) {
    // motion detected at this PWM
    float absPwm = fabsf(calibrationPwm);
    float& stictionMax = calibrationDirectionIsForward ? stictionCeilingFwd : stictionCeilingRev;
    if (calibrationDirectionIsForward) everMovedFwd = true; else everMovedRev = true;
    stictionMax = absPwm;

    setPwm(0);
    // non-blocking settle:
    settleStartTime = currentTime;
    motorState = MOTOR_SETTLING;  // your existing settling handler will time this out

    transitionToRefine();
    return;
  }

  // no motion → exponential step up (with clamp)
  calibrationPwm = copysignf(
      fminf(fabsf(calibrationPwm) * 2.0f, SERVO_CALIBRATION_STOP_DUTY_CYCLE),
      calibrationPwm
  );

  // We explored the space and found no movement up to max
  if (fabsf(calibrationPwm) >= SERVO_CALIBRATION_STOP_DUTY_CYCLE) {
    // give up for this direction, record max
    float& stictionMax = calibrationDirectionIsForward ? stictionCeilingFwd : stictionCeilingRev;
    stictionMax = SERVO_CALIBRATION_STOP_DUTY_CYCLE;
    bool ever = calibrationDirectionIsForward ? everMovedFwd : everMovedRev;
    if (!ever) {
      // Direction failed: report + switch/finish
      V(axisPrefix); VF(calibrationDirectionIsForward ? " FWD" : " REV");
      VLF(" no motion up to PWM_MAX – skipping refine");
      if (calibrationDirectionIsForward) {
        calibrationDirectionIsForward = false;
        calibrationState = CALIBRATION_STICTION_CEILING;
        calibrationPwm = -SERVO_CALIBRATION_START_DUTY_CYCLE;
        startTest(calibrationPwm);
      } else {
        calibrationState = CALIBRATION_CHECK_IMBALANCE;
        processImbalanceCheck();
      }
      return;
    }
    transitionToRefine();
  } else {
    startTest(calibrationPwm);  // re-apply PWM and continue probing
  }
}

void ServoCalibrateTrackingVelocity::transitionToRefine() {
  // Enter the stiction floor (refine) phase
  calibrationState = CALIBRATION_STICTION_FLOOR;
  lastStateChangeTime = currentTime;

  const float stictionMax = calibrationDirectionIsForward
                              ? stictionCeilingFwd
                              : stictionCeilingRev;

  // Initial bounds for binary search
  calibrationMinPwm = SERVO_CALIBRATION_VELOCITY_SEARCH_MIN_FACTOR * stictionMax;
  if (calibrationMinPwm < 0.0f) calibrationMinPwm = 0.0f;   // safety clamp
  calibrationMaxPwm = stictionMax;

  float mid = 0.5f * (calibrationMinPwm + calibrationMaxPwm);
  calibrationPwm = calibrationDirectionIsForward ? mid : -mid;

  // Start the first refine test at the midpoint PWM
  startTest(calibrationPwm);
}

void ServoCalibrateTrackingVelocity::processStictionFloor() {
  // Up to REFINE_MAX_ITERATIONS
  if (++refineIters > SERVO_CALIBRATION_REFINE_MAX_ITERATIONS) {
    VLF("WARN: refine iteration cap reached; using current bounds");
    calibrationMaxPwm = fmaxf(calibrationMaxPwm, calibrationMinPwm);
     // fall through to convergence
  }

  // We only evaluate after a test has resolved to a steady state or stop.
  // (updateState() already guards for this; keeping a safety check here too.)
  if (motorState != MOTOR_RUNNING_STEADY && motorState != MOTOR_STOPPED) return;

  const bool moved = (motorState == MOTOR_RUNNING_STEADY);

  if (!moved) {
    // No movement → raise the lower bound
    calibrationMinPwm = fabsf(calibrationPwm);

    V(axisPrefix);
    if (calibrationDirectionIsForward) { VF(" FWD"); } else { VF(" REV"); }
    VF(" refine: no move @ PWM="); V(calibrationPwm); VLF("%");
  } else {
    // Movement → lower the upper bound
    calibrationMaxPwm = fabsf(calibrationPwm);

    V(axisPrefix);
    if (calibrationDirectionIsForward) { VF(" FWD"); } else { VF(" REV"); }
    VF(" refine: MOVE  @ PWM="); V(calibrationPwm);
    VF("%, v≈"); V(lastVelocityMeasurement); VL(" steps/s");
  }

  // Convergence check
  if ((calibrationMaxPwm - calibrationMinPwm) <= SERVO_CALIBRATION_STICTION_REFINE_STEP) {
    // Conservative pick: the smallest PWM known to MOVE
    const float resultMin = calibrationMaxPwm;

    float& stictionMin = calibrationDirectionIsForward ? stictionFloorFwd : stictionFloorRev;
    float& trackingPwm  = calibrationDirectionIsForward ? trackingPwmFwd   : trackingPwmRev;

    stictionMin = resultMin;
    trackingPwm = resultMin * SERVO_CALIBRATION_KICKSTART_DROP_FACTOR;

    V(axisPrefix);
    if (calibrationDirectionIsForward) { VF(" FWD"); } else { VF(" REV"); }
    VF(" MinStiction="); V(resultMin);
    VF("% → Tracking PWM="); V(trackingPwm); VL("%");

    // Stop & settle before switching direction / next phase
    setPwm(0);
    startSettling();

    if (calibrationDirectionIsForward) {
      // Switch to reverse direction and restart at ceiling search
      calibrationDirectionIsForward = false;
      calibrationState = CALIBRATION_STICTION_CEILING;
      calibrationPwm = -SERVO_CALIBRATION_START_DUTY_CYCLE;
      startTest(calibrationPwm);
    } else {
      // Proceed to imbalance check
      calibrationState = CALIBRATION_CHECK_IMBALANCE;
      processImbalanceCheck();
    }
    return;
  }

  // Continue binary search: midpoint of [min, max]
  float mid = 0.5f * (calibrationMinPwm + calibrationMaxPwm);
  calibrationPwm = calibrationDirectionIsForward ? mid : -mid;

  // Start the next test; handleMotorState() will advance to RUNNING_STEADY or STOPPED
  startTest(calibrationPwm);
}

void ServoCalibrateTrackingVelocity::processImbalanceCheck() {
  // Print calibration report
  printReport();

  // Check for significant imbalance
  if (trackingPwmFwd > 0 && trackingPwmRev > 0) {
    float avgTracking = (trackingPwmFwd + trackingPwmRev) / 2.0f;
    float imbalance = fabs(trackingPwmFwd - trackingPwmRev) / avgTracking * 100.0f;

    if (imbalance > SERVO_CALIBRATION_IMBALANCE_ERROR_THRESHOLD) {
      VF("WARN: "); V(axisPrefix);
      VF(" Significant imbalance: "); V(imbalance); VL("%");
    }
  }

  // Calibration complete
  experimentMode = false;
  calibrationState = CALIBRATION_IDLE;
  setPwm(0);
  VF("MSG: "); V(axisPrefix); VL(" complete");
}

void ServoCalibrateTrackingVelocity::handleCalibrationFailure() {
  VF("ERR: "); V(axisPrefix); VL("Calibration failed, resetting");

  // Reset to safe state
  experimentMode = false;
  calibrationState = CALIBRATION_IDLE;
  setPwm(0);

  // Set default values
  stictionCeilingFwd = SERVO_CALIBRATION_STOP_DUTY_CYCLE;
  stictionFloorFwd = SERVO_CALIBRATION_STOP_DUTY_CYCLE;
  trackingPwmFwd = SERVO_CALIBRATION_STOP_DUTY_CYCLE;
  stictionCeilingRev = SERVO_CALIBRATION_STOP_DUTY_CYCLE;
  stictionFloorRev = SERVO_CALIBRATION_STOP_DUTY_CYCLE;
  trackingPwmRev = SERVO_CALIBRATION_STOP_DUTY_CYCLE;
}

void ServoCalibrateTrackingVelocity::startSettling() {
  settleStartTime = currentTime;
  motorState = MOTOR_SETTLING;
  VF("DBG: "); V(axisPrefix); VF(" Starting settling for ");
  V(SERVO_CALIBRATION_MOTOR_SETTLE_TIME); VL("ms");
}

void ServoCalibrateTrackingVelocity::startTest(float pwm) {
  setPwm(pwm);
  lastVelocityTime = currentTime;
  lastVelocityMeasurement = 0;
  motorState = MOTOR_ACCELERATING;
  lastStateChangeTime = currentTime;

  V(axisPrefix);
  if (calibrationDirectionIsForward) {VF(" FWD");} else {VF(" REV");}
  VF(" Starting test at PWM="); V(pwm); VL("%");
}

void ServoCalibrateTrackingVelocity::setPwm(float pwm) {
  // Apply PWM with constraints
  experimentPwm = constrain(pwm, -SERVO_CALIBRATION_STOP_DUTY_CYCLE, SERVO_CALIBRATION_STOP_DUTY_CYCLE);
}

// Getters ////////////////////////////////////////////////////////////////////
float ServoCalibrateTrackingVelocity::getStictionCeiling(bool forward) {
  return forward ? stictionCeilingFwd : stictionCeilingRev;
}

float ServoCalibrateTrackingVelocity::getStictionFloor(bool forward) {
  return forward ? stictionFloorFwd : stictionFloorRev;
}

float ServoCalibrateTrackingVelocity::getTrackingPwm(bool forward) {
  return forward ? trackingPwmFwd : trackingPwmRev;
}

void ServoCalibrateTrackingVelocity::printReport() {
  VF("\n=== Calibration Report: "); V(axisPrefix); VLF(" ===");

  VF("Stiction Ceiling at FWD: Max="); V(stictionCeilingFwd);
  VF("%, Floor="); V(stictionFloorFwd); VL("%");

  VF("Tracking PWM FWD: "); V(trackingPwmFwd); VL("%\n");

  VF("Stiction Ceiling at REV: Max="); V(stictionCeilingRev);
  VF("%, Floor="); V(stictionFloorRev); VL("%");

  VF("Tracking PWM REV: "); V(trackingPwmRev); VL("%\n");

  float stictionImbalance = fabs(stictionFloorFwd - stictionFloorRev);
  float trackingImbalance = fabs(trackingPwmFwd - trackingPwmRev);

  VF("Stiction Min Imbalance: "); V(stictionImbalance); VL("%");
  VF("Tracking PWM Imbalance: "); V(trackingImbalance); VL("%\n");

  if (trackingPwmFwd >= SERVO_CALIBRATION_STOP_DUTY_CYCLE - 0.1f) {
    VLF("WARN: FWD PWM at calibration limit");
  }
  if (trackingPwmRev >= SERVO_CALIBRATION_STOP_DUTY_CYCLE - 0.1f) {
    VLF("WARN: REV PWM at calibration limit");
  }

  VF("=== End Report ===\n");
}

#endif