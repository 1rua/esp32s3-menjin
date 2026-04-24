#pragma once

#include <Arduino.h>
#include <ESP32Servo.h>

struct DoorControllerConfig {
  uint8_t servoPin;
  int openUs;
  int closeUs;
  uint32_t pulseMs;
  uint32_t defaultOpenDurationMs;
  uint32_t startupPulseMs;
};

struct DoorControllerState {
  bool isOpen = false;
  unsigned long openedAt = 0;
  uint32_t activeOpenDurationMs = 0;
  bool pulseActive = false;
  unsigned long detachAt = 0;
};

void initializeDoorController(DoorControllerState& state, Servo& servo, const DoorControllerConfig& config);
void requestDoorOpen(DoorControllerState& state, Servo& servo, const DoorControllerConfig& config, unsigned long nowMs);
void closeDoorNow(DoorControllerState& state, Servo& servo, const DoorControllerConfig& config);
void tickDoorController(DoorControllerState& state, Servo& servo, const DoorControllerConfig& config, unsigned long nowMs);
