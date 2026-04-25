#include "keypad_access.h"

namespace {
constexpr unsigned long kKeypadTimeoutMs = 10000;
constexpr uint8_t kKeypadMaxFailedAttempts = 5;
constexpr unsigned long kKeypadLockoutMs = 30UL * 1000UL;

void resetKeypadLockIfExpired(KeypadAccessState& state, unsigned long nowMs) {
  if (state.lockoutUntil != 0 && static_cast<long>(nowMs - state.lockoutUntil) >= 0) {
    state.lockoutUntil = 0;
  }
}
}

KeypadAccessResult pollKeypadAccess(KeypadAccessState& state, Keypad& keypad, Preferences& prefs, AccessControlState& accessControl, unsigned long nowMs, uint32_t nowEpoch, bool timeSynced) {
  KeypadAccessResult result;

  char key = keypad.getKey();
  if (key) {
    resetKeypadLockIfExpired(state, nowMs);
    if (keypadAccessLocked(state, nowMs)) {
      Serial.println("Keypad Locked. Please wait.");
      state.inputCode = "";
      return result;
    }

    state.lastKeyTime = nowMs;
    Serial.print("Key Pressed: ");
    Serial.println(key);
    if (key == '*') {
      state.inputCode = "";
      Serial.println("Input Cleared");
    } else if (key == '#') {
      String matchedSource;
      if (timeSynced) {
        pruneExpiredTemporaryPins(prefs, accessControl, nowEpoch, true);
      }
      if (verifyKeypadCode(accessControl, state.inputCode, nowEpoch, timeSynced, matchedSource)) {
        Serial.println("Password Correct!");
        state.failedAttempts = 0;
        state.inputCode = "";
        result.authorized = true;
        result.matchedSource = matchedSource;
      } else {
        Serial.println("Password Wrong!");
        state.failedAttempts++;
        if (state.failedAttempts >= kKeypadMaxFailedAttempts) {
          state.lockoutUntil = nowMs + kKeypadLockoutMs;
          state.failedAttempts = 0;
          Serial.println("Too many failed attempts. Keypad locked for 30s.");
        }
        state.inputCode = "";
        result.rejected = true;
      }
    } else if (key >= '0' && key <= '9') {
      state.inputCode += key;
      if (state.inputCode.length() > kPinMaxLength) {
        state.inputCode = "";
        Serial.println("Input Overflow");
      }
    }
  }

  if (state.inputCode.length() > 0 && (nowMs - state.lastKeyTime > kKeypadTimeoutMs)) {
    state.inputCode = "";
    Serial.println("Keypad Timeout");
  }

  return result;
}

bool keypadAccessLocked(const KeypadAccessState& state, unsigned long nowMs) {
  return state.lockoutUntil != 0 && static_cast<long>(nowMs - state.lockoutUntil) < 0;
}
