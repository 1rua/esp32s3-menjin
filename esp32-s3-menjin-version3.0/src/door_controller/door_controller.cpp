// 门锁执行器实现：通过舵机脉冲与状态机控制开关门时序。
#include "door_controller.h"

namespace {
void scheduleDetach(DoorControllerState& state, uint32_t pulseMs) {
  state.pulseActive = true;
  state.detachAt = millis() + pulseMs;
}
}

void initializeDoorController(DoorControllerState& state, Servo& servo, const DoorControllerConfig& config) {
  servo.attach(config.servoPin, 500, 3000);
  servo.writeMicroseconds(config.closeUs);
  scheduleDetach(state, config.startupPulseMs);
  state.isOpen = false;
  state.openedAt = 0;
  state.activeOpenDurationMs = config.defaultOpenDurationMs;
}

void requestDoorOpen(DoorControllerState& state, Servo& servo, const DoorControllerConfig& config, unsigned long nowMs) {
  servo.attach(config.servoPin, 500, 3000);
  servo.writeMicroseconds(config.openUs);
  scheduleDetach(state, config.pulseMs);
  state.isOpen = true;
  state.openedAt = nowMs;
  if (state.activeOpenDurationMs == 0) {
    state.activeOpenDurationMs = config.defaultOpenDurationMs;
  }
}

void closeDoorNow(DoorControllerState& state, Servo& servo, const DoorControllerConfig& config) {
  servo.attach(config.servoPin, 500, 3000);
  servo.writeMicroseconds(config.closeUs);
  scheduleDetach(state, config.pulseMs);
  state.isOpen = false;
  state.activeOpenDurationMs = config.defaultOpenDurationMs;
}

void tickDoorController(DoorControllerState& state, Servo& servo, const DoorControllerConfig& config, unsigned long nowMs) {
  if (state.pulseActive && static_cast<long>(nowMs - state.detachAt) >= 0) {
    servo.detach();
    state.pulseActive = false;
    state.detachAt = 0;
  }

  if (state.isOpen && state.activeOpenDurationMs != 0 && nowMs - state.openedAt > state.activeOpenDurationMs) {
    closeDoorNow(state, servo, config);
  }
}
