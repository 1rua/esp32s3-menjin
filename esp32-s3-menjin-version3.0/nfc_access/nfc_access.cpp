#include "nfc_access.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace {
constexpr uint8_t kMinNfcUidLength = 4;
constexpr uint8_t kMaxNfcUidLength = 10;
constexpr unsigned long kNfcHealthCheckIntervalMs = 3000;
constexpr unsigned long kNfcErrorCooldownMs = 1000;

void clearNfcWhitelist(NfcAccessState& state) {
  state.whitelistCount = 0;
  memset(state.whitelist, 0, sizeof(state.whitelist));
}

void persistNfcWhitelist(Preferences& prefs, const NfcAccessState& state) {
  prefs.putBytes("nfc_list", state.whitelist, state.whitelistCount * sizeof(NfcCard));
  prefs.putInt("nfc_cnt", state.whitelistCount);
}

bool isDuplicateNfcCard(const NfcAccessState& state, const NfcCard& card) {
  for (int i = 0; i < state.whitelistCount; i++) {
    if (state.whitelist[i].size == card.size && memcmp(state.whitelist[i].uid, card.uid, card.size) == 0) {
      return true;
    }
  }
  return false;
}

bool parseUidHex(const String& uidStr, NfcCard& outCard) {
  if (uidStr.length() == 0 || (uidStr.length() % 2) != 0) {
    return false;
  }

  const uint8_t uidBytes = uidStr.length() / 2;
  if (uidBytes < kMinNfcUidLength || uidBytes > kMaxNfcUidLength) {
    return false;
  }

  memset(outCard.uid, 0, sizeof(outCard.uid));
  outCard.size = uidBytes;
  for (uint8_t i = 0; i < uidBytes; i++) {
    const char hi = uidStr.charAt(i * 2);
    const char lo = uidStr.charAt(i * 2 + 1);
    if (!isxdigit(hi) || !isxdigit(lo)) {
      return false;
    }
    const char buf[3] = {hi, lo, '\0'};
    outCard.uid[i] = static_cast<uint8_t>(strtoul(buf, NULL, 16));
  }
  return true;
}
}

void initializeNfcAccess(Preferences& prefs, NfcAccessState& state) {
  prefs.begin("mech_master", false);

  state.whitelistCount = prefs.getInt("nfc_cnt", -1);

  if (state.whitelistCount == -1) {
    Serial.println("[NVS] First boot detected. Migrating hardcoded NFC list...");
    const byte defaultWhitelist[][4] = {
      {0xF7, 0x6D, 0x16, 0x3F},
      {0xE5, 0x6B, 0x1A, 0x06},
      {0x1D, 0x8E, 0x39, 0x68},
      {0xAD, 0xE9, 0x31, 0x55},
      {0x01, 0x62, 0xAD, 0x1C}
    };
    const int defCount = sizeof(defaultWhitelist) / sizeof(defaultWhitelist[0]);
    state.whitelistCount = 0;
    for (int i = 0; i < defCount && state.whitelistCount < kMaxNfcCards; i++) {
      NfcCard card = {};
      card.size = 4;
      memcpy(card.uid, defaultWhitelist[i], 4);
      if (!isDuplicateNfcCard(state, card)) {
        state.whitelist[state.whitelistCount++] = card;
      }
    }
    persistNfcWhitelist(prefs, state);
    return;
  }

  if (state.whitelistCount < 0 || state.whitelistCount > kMaxNfcCards) {
    Serial.printf("[NVS] Invalid nfc_cnt=%d, resetting whitelist.\n", state.whitelistCount);
    clearNfcWhitelist(state);
    persistNfcWhitelist(prefs, state);
    return;
  }

  const size_t bytesLen = prefs.getBytesLength("nfc_list");
  const size_t expectedStructLen = state.whitelistCount * sizeof(NfcCard);
  const size_t expectedLegacyLen = state.whitelistCount * sizeof(uint32_t);

  if (bytesLen == expectedStructLen) {
    prefs.getBytes("nfc_list", state.whitelist, expectedStructLen);
    Serial.printf("[NVS] Loaded %d NFC cards from storage.\n", state.whitelistCount);
    return;
  }

  if (bytesLen == expectedLegacyLen) {
    Serial.println("[NVS] Legacy NFC format detected. Migrating...");
    uint32_t legacyList[kMaxNfcCards] = {0};
    const int legacyCount = state.whitelistCount;
    prefs.getBytes("nfc_list", legacyList, expectedLegacyLen);
    clearNfcWhitelist(state);
    for (int i = 0; i < kMaxNfcCards && i < legacyCount; i++) {
      NfcCard card = {};
      card.size = 4;
      card.uid[0] = (legacyList[i] >> 24) & 0xFF;
      card.uid[1] = (legacyList[i] >> 16) & 0xFF;
      card.uid[2] = (legacyList[i] >> 8) & 0xFF;
      card.uid[3] = legacyList[i] & 0xFF;
      if (!isDuplicateNfcCard(state, card) && state.whitelistCount < kMaxNfcCards) {
        state.whitelist[state.whitelistCount++] = card;
      }
    }
    persistNfcWhitelist(prefs, state);
    Serial.printf("[NVS] Migrated legacy NFC list, count=%d.\n", state.whitelistCount);
    return;
  }

  Serial.printf("[NVS] Invalid nfc_list size=%u, expected=%u. Resetting whitelist.\n",
                static_cast<unsigned int>(bytesLen), static_cast<unsigned int>(expectedStructLen));
  clearNfcWhitelist(state);
  persistNfcWhitelist(prefs, state);
}

int addNfcCardFromHex(Preferences& prefs, NfcAccessState& state, const String& uid, String& message) {
  String uidStr = uid;
  uidStr.trim();
  NfcCard newCard = {};
  if (!parseUidHex(uidStr, newCard)) {
    message = "Invalid UID format";
    return 400;
  }

  if (isDuplicateNfcCard(state, newCard)) {
    message = "UID already exists";
    return 409;
  }

  if (state.whitelistCount >= kMaxNfcCards) {
    message = "Whitelist Full!";
    return 507;
  }

  state.whitelist[state.whitelistCount++] = newCard;
  persistNfcWhitelist(prefs, state);
  Serial.printf("[NVS] Added New NFC (len=%d)\n", newCard.size);
  message = "NFC Added to NVS";
  return 200;
}

void clearExpiredNfcErrorFeedback(NfcAccessState& state, unsigned long nowMs) {
  if (state.errorFeedbackUntil != 0 && static_cast<long>(nowMs - state.errorFeedbackUntil) >= 0) {
    state.errorFeedbackUntil = 0;
  }
}

void maintainNfcReaderHealth(NfcAccessState& state, MFRC522& reader, unsigned long nowMs, NfcResetSideEffect beforeReset) {
  if (nowMs - state.lastHealthCheck <= kNfcHealthCheckIntervalMs) {
    return;
  }

  byte v = reader.PCD_ReadRegister(reader.VersionReg);
  if (v == 0x00 || v == 0xFF) {
    Serial.println("[Watchdog] NFC Dead. Resetting...");
    if (beforeReset != nullptr) {
      beforeReset();
    }
    reader.PCD_Init();
    Serial.println("[Watchdog] Reset Done.");
  }
  state.lastHealthCheck = nowMs;
}

NfcPollResult pollNfcAccess(NfcAccessState& state, MFRC522& reader, bool otaUpdating, unsigned long nowMs) {
  NfcPollResult result;
  if (otaUpdating) {
    return result;
  }

  NfcCard currentCard = {};
  currentCard.size = reader.uid.size;
  if (currentCard.size > kMaxNfcUidLength) {
    Serial.printf("[NFC] UID length %d too long, rejected.\n", currentCard.size);
    reader.PICC_HaltA();
    reader.PCD_StopCrypto1();
    return result;
  }

  Serial.print("UID:");
  for (byte i = 0; i < currentCard.size; i++) {
    Serial.print(reader.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(reader.uid.uidByte[i], HEX);
    currentCard.uid[i] = reader.uid.uidByte[i];
  }
  Serial.println();

  if (isDuplicateNfcCard(state, currentCard)) {
    result.authorized = true;
  } else {
    Serial.println("Unknown Card");
    state.errorFeedbackUntil = nowMs + kNfcErrorCooldownMs;
    result.rejected = true;
  }

  reader.PICC_HaltA();
  reader.PCD_StopCrypto1();
  return result;
}
