#pragma once

#include <Arduino.h>
#include <MFRC522.h>
#include <Preferences.h>

typedef void (*NfcResetSideEffect)();

constexpr uint8_t kMaxNfcCards = 30;

struct NfcCard {
  uint8_t uid[10];
  uint8_t size;
};

struct NfcAccessState {
  NfcCard whitelist[kMaxNfcCards];
  int whitelistCount = 0;
  unsigned long lastHealthCheck = 0;
  unsigned long errorFeedbackUntil = 0;
};

struct NfcPollResult {
  bool authorized = false;
  bool rejected = false;
};

void initializeNfcAccess(Preferences& prefs, NfcAccessState& state);
int addNfcCardFromHex(Preferences& prefs, NfcAccessState& state, const String& uid, String& message);
void clearExpiredNfcErrorFeedback(NfcAccessState& state, unsigned long nowMs);
void maintainNfcReaderHealth(NfcAccessState& state, MFRC522& reader, unsigned long nowMs, NfcResetSideEffect beforeReset);
NfcPollResult pollNfcAccess(NfcAccessState& state, MFRC522& reader, bool otaUpdating, unsigned long nowMs);
