#include "fingerprint_access.h"

namespace {
constexpr unsigned long kFingerprintEnrollRemoveDelayMs = 2000;

void resetFingerprintEnrollState(FingerprintAccessState& state) {
  state.phase = FingerprintEnrollPhase::Idle;
  state.enrollId = 0;
  state.nextActionAt = 0;
  state.enrollInput = "";
}

void startFingerprintEnrollMode(FingerprintAccessState& state, Audio& audio) {
  Serial.println("\n=== ENTERING ENROLL MODE ===");
  audio.stopSong();
  state.phase = FingerprintEnrollPhase::AwaitId;
  state.enrollId = 0;
  state.nextActionAt = 0;
}
}

void initializeFingerprintAccess(HardwareSerial& serialPort, Adafruit_Fingerprint& finger, int rxPin, int txPin) {
  serialPort.begin(57600, SERIAL_8N1, rxPin, txPin);
  finger.begin(57600);

  if (finger.verifyPassword()) {
    Serial.println("Fingerprint Sensor Found!");
  } else {
    Serial.println("Fingerprint Sensor NOT FOUND :(");
  }
}

void handleFingerprintConsoleInput(FingerprintAccessState& state, Adafruit_Fingerprint&, Audio& audio) {
  if (!Serial.available()) {
    return;
  }

  const char c = static_cast<char>(Serial.read());
  if ((c == 'E' || c == 'e') && state.phase == FingerprintEnrollPhase::Idle) {
    startFingerprintEnrollMode(state, audio);
  }
}

void tickFingerprintAccess(FingerprintAccessState& state, Adafruit_Fingerprint& finger, Audio& audio, bool provisioningPortalActive, unsigned long nowMs) {
  if (state.phase == FingerprintEnrollPhase::Idle) {
    return;
  }

  if (provisioningPortalActive) {
    Serial.println("[ENROLL] Provisioning portal active, enrollment cancelled.");
    resetFingerprintEnrollState(state);
    return;
  }

  switch (state.phase) {
    case FingerprintEnrollPhase::AwaitId: {
      while (Serial.available()) {
        const char ch = static_cast<char>(Serial.read());
        if (ch == '\r') {
          continue;
        }
        if (ch == '\n') {
          break;
        }
        if (ch >= '0' && ch <= '9' && state.enrollInput.length() < 3) {
          state.enrollInput += ch;
        }
      }
      if (state.enrollInput.length() == 0) {
        return;
      }
      const int id = state.enrollInput.toInt();
      state.enrollInput = "";
      if (id <= 0 || id > 127) {
        Serial.println("[ENROLL] Enter a fingerprint ID from 1 to 127.");
        return;
      }
      state.enrollId = id;
      state.phase = FingerprintEnrollPhase::CaptureFirst;
      Serial.print("Enrolling ID #");
      Serial.println(state.enrollId);
      Serial.println("Place finger");
      return;
    }
    case FingerprintEnrollPhase::CaptureFirst: {
      const int p = finger.getImage();
      if (p == FINGERPRINT_NOFINGER) {
        return;
      }
      if (p != FINGERPRINT_OK) {
        Serial.println("[ENROLL] Failed to capture first image.");
        resetFingerprintEnrollState(state);
        return;
      }
      if (finger.image2Tz(1) != FINGERPRINT_OK) {
        Serial.println("[ENROLL] Failed to process first image.");
        resetFingerprintEnrollState(state);
        return;
      }
      Serial.println("Image taken");
      Serial.println("Remove finger");
      state.phase = FingerprintEnrollPhase::WaitRemoveDelay;
      state.nextActionAt = nowMs + kFingerprintEnrollRemoveDelayMs;
      return;
    }
    case FingerprintEnrollPhase::WaitRemoveDelay:
      if (static_cast<long>(nowMs - state.nextActionAt) < 0) {
        return;
      }
      state.phase = FingerprintEnrollPhase::WaitRemove;
      return;
    case FingerprintEnrollPhase::WaitRemove: {
      const int p = finger.getImage();
      if (p == FINGERPRINT_NOFINGER) {
        Serial.println("Place same finger again");
        state.phase = FingerprintEnrollPhase::CaptureSecond;
      }
      return;
    }
    case FingerprintEnrollPhase::CaptureSecond: {
      const int p = finger.getImage();
      if (p == FINGERPRINT_NOFINGER) {
        return;
      }
      if (p != FINGERPRINT_OK) {
        Serial.println("[ENROLL] Failed to capture second image.");
        resetFingerprintEnrollState(state);
        return;
      }
      if (finger.image2Tz(2) != FINGERPRINT_OK) {
        Serial.println("[ENROLL] Failed to process second image.");
        resetFingerprintEnrollState(state);
        return;
      }
      if (finger.createModel() != FINGERPRINT_OK) {
        Serial.println("[ENROLL] Fingerprints did not match.");
        resetFingerprintEnrollState(state);
        return;
      }
      if (finger.storeModel(state.enrollId) != FINGERPRINT_OK) {
        Serial.println("[ENROLL] Failed to store fingerprint.");
        resetFingerprintEnrollState(state);
        return;
      }
      Serial.println("Stored!");
      Serial.println("=== ENROLLMENT FINISHED ===");
      playBootSound(audio);
      resetFingerprintEnrollState(state);
      return;
    }
    case FingerprintEnrollPhase::Idle:
    default:
      return;
  }
}

int pollFingerprintMatch(const FingerprintAccessState& state, Adafruit_Fingerprint& finger, bool otaUpdating) {
  if (otaUpdating || fingerprintAccessBusy(state)) {
    return -1;
  }

  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) {
    return -1;
  }

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    return -1;
  }

  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) {
    return finger.fingerID;
  }

  return -1;
}

bool fingerprintAccessBusy(const FingerprintAccessState& state) {
  return state.phase != FingerprintEnrollPhase::Idle;
}
