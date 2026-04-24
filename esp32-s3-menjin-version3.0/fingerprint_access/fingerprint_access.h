#pragma once

#include <Adafruit_Fingerprint.h>
#include <Audio.h>
#include <Arduino.h>

#include "../audio_feedback/audio_feedback.h"

enum class FingerprintEnrollPhase : uint8_t {
  Idle,
  AwaitId,
  CaptureFirst,
  WaitRemoveDelay,
  WaitRemove,
  CaptureSecond,
};

struct FingerprintAccessState {
  FingerprintEnrollPhase phase = FingerprintEnrollPhase::Idle;
  int enrollId = 0;
  unsigned long nextActionAt = 0;
  String enrollInput;
};

void initializeFingerprintAccess(HardwareSerial& serialPort, Adafruit_Fingerprint& finger, int rxPin, int txPin);
void handleFingerprintConsoleInput(FingerprintAccessState& state, Adafruit_Fingerprint& finger, Audio& audio);
void tickFingerprintAccess(FingerprintAccessState& state, Adafruit_Fingerprint& finger, Audio& audio, bool provisioningPortalActive, unsigned long nowMs);
int pollFingerprintMatch(const FingerprintAccessState& state, Adafruit_Fingerprint& finger, bool otaUpdating);
bool fingerprintAccessBusy(const FingerprintAccessState& state);
