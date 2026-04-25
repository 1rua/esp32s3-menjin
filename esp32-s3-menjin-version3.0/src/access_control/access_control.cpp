// 门禁访问控制实现：包含哈希校验、PIN 持久化及临时 PIN 生命周期维护。
#include "access_control.h"

#include <mbedtls/sha256.h>
#include <string.h>
#include <time.h>

namespace {
constexpr uint32_t kPinStoreVersion = 1;
constexpr char kDefaultAdminUsername[] = "admin";
constexpr char kDefaultAdminPassword[] = "esp32s3-menjin";
constexpr char kDefaultPermanentPinLabel[] = "Default PIN";
constexpr char kDefaultPermanentPin[] = "11451";
constexpr uint32_t kTemporaryPinGenerationAttempts = 64;

const char* kPinStoreVersionKey = "pin_store_ver";
const char* kAdminUserKey = "admin_user";
const char* kAdminSaltKey = "admin_salt";
const char* kAdminHashKey = "admin_hash";
const char* kAdminDefaultKey = "admin_default";
const char* kPermanentPinCountKey = "perm_pin_cnt";
const char* kPermanentPinListKey = "perm_pin_list";
const char* kPermanentPinNextIdKey = "perm_pin_next_id";
const char* kTemporaryPinCountKey = "temp_pin_cnt";
const char* kTemporaryPinListKey = "temp_pin_list";
const char* kTemporaryPinNextIdKey = "temp_pin_next_id";


void clearState(AccessControlState& state) {
  memset(&state, 0, sizeof(state));
  state.nextPermanentPinId = 1;
  state.nextTemporaryPinId = 1;
}

bool isAllZero(const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (data[i] != 0) {
      return false;
    }
  }
  return true;
}

void fillRandomBytes(uint8_t* buffer, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    buffer[i] = static_cast<uint8_t>(esp_random() & 0xFF);
  }
}

bool constantTimeEquals(const uint8_t* left, const uint8_t* right, size_t length) {
  uint8_t diff = 0;
  for (size_t i = 0; i < length; ++i) {
    diff |= left[i] ^ right[i];
  }
  return diff == 0;
}

void computeHash(const uint8_t* salt, const String& plaintext, uint8_t* outputHash) {
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  mbedtls_sha256_starts(&context, 0);
  mbedtls_sha256_update(&context, salt, kSaltLength);
  const uint8_t* plaintextBytes = reinterpret_cast<const uint8_t*>(plaintext.c_str());
  mbedtls_sha256_update(&context, plaintextBytes, plaintext.length());
  mbedtls_sha256_finish(&context, outputHash);
  mbedtls_sha256_free(&context);
}

void setCredential(char* usernameBuffer, size_t usernameBufferLength, uint8_t* salt, uint8_t* hash,
                   const String& username, const String& password) {
  memset(usernameBuffer, 0, usernameBufferLength);
  username.substring(0, usernameBufferLength - 1).toCharArray(usernameBuffer, usernameBufferLength);
  fillRandomBytes(salt, kSaltLength);
  computeHash(salt, password, hash);
}

bool isDigitsOnly(const String& value) {
  if (value.length() == 0) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value.charAt(i);
    if (ch < '0' || ch > '9') {
      return false;
    }
  }
  return true;
}

String trimCopy(const String& value) {
  String copy = value;
  copy.trim();
  return copy;
}

String jsonEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value.charAt(i);
    if (ch == '\\' || ch == '"') {
      escaped += '\\';
      escaped += ch;
    } else if (ch == '\n') {
      escaped += "\\n";
    } else if (ch == '\r') {
      escaped += "\\r";
    } else {
      escaped += ch;
    }
  }
  return escaped;
}

bool verifyHash(const uint8_t* salt, const uint8_t* hash, const String& plaintext) {
  uint8_t computedHash[kHashLength] = {0};
  computeHash(salt, plaintext, computedHash);
  return constantTimeEquals(hash, computedHash, kHashLength);
}

uint32_t maxPermanentPinId(const AccessControlState& state) {
  uint32_t maxId = 0;
  for (uint8_t i = 0; i < state.permanentPinCount; ++i) {
    if (state.permanentPins[i].id > maxId) {
      maxId = state.permanentPins[i].id;
    }
  }
  return maxId;
}

uint32_t maxTemporaryPinId(const AccessControlState& state) {
  uint32_t maxId = 0;
  for (uint8_t i = 0; i < state.temporaryPinCount; ++i) {
    if (state.temporaryPins[i].id > maxId) {
      maxId = state.temporaryPins[i].id;
    }
  }
  return maxId;
}

bool loadFixedBytes(Preferences& prefs, const char* key, void* destination, size_t expectedLength) {
  if (prefs.getBytesLength(key) != expectedLength) {
    return false;
  }
  return prefs.getBytes(key, destination, expectedLength) == expectedLength;
}

void saveListBytes(Preferences& prefs, const char* key, const void* value, size_t length) {
  if (length == 0) {
    prefs.remove(key);
    return;
  }
  prefs.putBytes(key, value, length);
}

String buildTemporaryPinCandidate() {
  char pin[kTemporaryPinLength + 1] = {0};
  for (uint8_t i = 0; i < kTemporaryPinLength; ++i) {
    pin[i] = static_cast<char>('0' + (esp_random() % 10));
  }
  return String(pin);
}

bool permanentPinMatches(const AccessControlState& state, const String& pin, int32_t skipId = -1) {
  for (uint8_t i = 0; i < state.permanentPinCount; ++i) {
    const PermanentPinRecord& record = state.permanentPins[i];
    if (!record.enabled || static_cast<int32_t>(record.id) == skipId) {
      continue;
    }
    if (verifyHash(record.salt, record.hash, pin)) {
      return true;
    }
  }
  return false;
}

bool temporaryPinMatches(const AccessControlState& state, const String& pin, int32_t skipId = -1) {
  for (uint8_t i = 0; i < state.temporaryPinCount; ++i) {
    const TemporaryPinRecord& record = state.temporaryPins[i];
    if (!record.enabled || static_cast<int32_t>(record.id) == skipId) {
      continue;
    }
    if (verifyHash(record.salt, record.hash, pin)) {
      return true;
    }
  }
  return false;
}

String temporaryPinLocalString(uint32_t epoch) {
  if (epoch < kMinValidEpoch) {
    return "Unavailable";
  }

  time_t rawTime = static_cast<time_t>(epoch);
  struct tm localTimeInfo = {};
  if (localtime_r(&rawTime, &localTimeInfo) == nullptr) {
    return "Unavailable";
  }

  char buffer[24] = {0};
  if (strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &localTimeInfo) == 0) {
    return "Unavailable";
  }
  return String(buffer);
}

void createPermanentPinRecord(PermanentPinRecord& record, uint32_t id, const String& label, const String& pin) {
  memset(&record, 0, sizeof(record));
  record.id = id;
  record.enabled = true;
  trimCopy(label).substring(0, kPinLabelMaxLength - 1).toCharArray(record.label, kPinLabelMaxLength);
  fillRandomBytes(record.salt, kSaltLength);
  computeHash(record.salt, pin, record.hash);
}

void createTemporaryPinRecord(TemporaryPinRecord& record, uint32_t id, uint32_t expiresAtEpoch, const String& pin) {
  memset(&record, 0, sizeof(record));
  record.id = id;
  record.enabled = true;
  record.expiresAtEpoch = expiresAtEpoch;
  fillRandomBytes(record.salt, kSaltLength);
  computeHash(record.salt, pin, record.hash);
}

bool isValidAdminUsername(const String& username) {
  if (username.length() == 0 || username.length() >= kAdminUsernameMaxLength) {
    return false;
  }
  return username.indexOf(':') == -1;
}

String jsonBool(bool value) {
  return value ? "true" : "false";
}

}  // namespace

void loadAccessControl(Preferences& prefs, AccessControlState& state) {
  clearState(state);

  const String adminUsername = prefs.getString(kAdminUserKey, "");
  if (adminUsername.length() > 0 && adminUsername.length() < kAdminUsernameMaxLength) {
    adminUsername.toCharArray(state.adminUsername, kAdminUsernameMaxLength);
  }

  const bool adminSaltLoaded = loadFixedBytes(prefs, kAdminSaltKey, state.adminSalt, kSaltLength);
  const bool adminHashLoaded = loadFixedBytes(prefs, kAdminHashKey, state.adminHash, kHashLength);
  if (!adminSaltLoaded || !adminHashLoaded) {
    memset(state.adminSalt, 0, sizeof(state.adminSalt));
    memset(state.adminHash, 0, sizeof(state.adminHash));
  }
  state.adminUsesDefaultCredentials = prefs.getBool(kAdminDefaultKey, true);

  const uint32_t permanentPinCount = prefs.getUInt(kPermanentPinCountKey, 0);
  const size_t permanentPinListLength = prefs.getBytesLength(kPermanentPinListKey);
  if (permanentPinCount <= kMaxPermanentPinUsers &&
      permanentPinListLength == permanentPinCount * sizeof(PermanentPinRecord)) {
    if (permanentPinCount > 0) {
      prefs.getBytes(kPermanentPinListKey, state.permanentPins, permanentPinListLength);
    }
    state.permanentPinCount = static_cast<uint8_t>(permanentPinCount);
  }

  const uint32_t temporaryPinCount = prefs.getUInt(kTemporaryPinCountKey, 0);
  const size_t temporaryPinListLength = prefs.getBytesLength(kTemporaryPinListKey);
  if (temporaryPinCount <= kMaxTemporaryPins &&
      temporaryPinListLength == temporaryPinCount * sizeof(TemporaryPinRecord)) {
    if (temporaryPinCount > 0) {
      prefs.getBytes(kTemporaryPinListKey, state.temporaryPins, temporaryPinListLength);
    }
    state.temporaryPinCount = static_cast<uint8_t>(temporaryPinCount);
  }

  state.nextPermanentPinId = prefs.getUInt(kPermanentPinNextIdKey, maxPermanentPinId(state) + 1);
  if (state.nextPermanentPinId == 0) {
    state.nextPermanentPinId = maxPermanentPinId(state) + 1;
  }

  state.nextTemporaryPinId = prefs.getUInt(kTemporaryPinNextIdKey, maxTemporaryPinId(state) + 1);
  if (state.nextTemporaryPinId == 0) {
    state.nextTemporaryPinId = maxTemporaryPinId(state) + 1;
  }

  ensureAccessControlInitialized(prefs, state);
}

void saveAccessControl(Preferences& prefs, const AccessControlState& state) {
  prefs.putUInt(kPinStoreVersionKey, kPinStoreVersion);
  prefs.putString(kAdminUserKey, String(state.adminUsername));
  prefs.putBytes(kAdminSaltKey, state.adminSalt, kSaltLength);
  prefs.putBytes(kAdminHashKey, state.adminHash, kHashLength);
  prefs.putBool(kAdminDefaultKey, state.adminUsesDefaultCredentials);
  prefs.putUInt(kPermanentPinCountKey, state.permanentPinCount);
  saveListBytes(prefs, kPermanentPinListKey, state.permanentPins,
                state.permanentPinCount * sizeof(PermanentPinRecord));
  prefs.putUInt(kPermanentPinNextIdKey, state.nextPermanentPinId);
  prefs.putUInt(kTemporaryPinCountKey, state.temporaryPinCount);
  saveListBytes(prefs, kTemporaryPinListKey, state.temporaryPins,
                state.temporaryPinCount * sizeof(TemporaryPinRecord));
  prefs.putUInt(kTemporaryPinNextIdKey, state.nextTemporaryPinId);
}

void ensureAccessControlInitialized(Preferences& prefs, AccessControlState& state) {
  const bool needsInitialMigration = prefs.getUInt(kPinStoreVersionKey, 0) == 0;
  bool changed = false;

  if (!isValidAdminUsername(String(state.adminUsername)) ||
      isAllZero(state.adminSalt, kSaltLength) ||
      isAllZero(state.adminHash, kHashLength)) {
    setCredential(state.adminUsername, sizeof(state.adminUsername), state.adminSalt, state.adminHash,
                  String(kDefaultAdminUsername), String(kDefaultAdminPassword));
    state.adminUsesDefaultCredentials = true;
    changed = true;
  }

  if (needsInitialMigration && state.permanentPinCount == 0) {
    createPermanentPinRecord(state.permanentPins[0], state.nextPermanentPinId++,
                             String(kDefaultPermanentPinLabel), String(kDefaultPermanentPin));
    state.permanentPinCount = 1;
    changed = true;
  }

  if (state.nextPermanentPinId == 0) {
    state.nextPermanentPinId = maxPermanentPinId(state) + 1;
    changed = true;
  }
  if (state.nextTemporaryPinId == 0) {
    state.nextTemporaryPinId = maxTemporaryPinId(state) + 1;
    changed = true;
  }

  if (changed || needsInitialMigration) {
    saveAccessControl(prefs, state);
  }
}

bool isAdminUsingDefaultCredentials(const AccessControlState& state) {
  return state.adminUsesDefaultCredentials;
}

bool verifyAdminCredentials(const AccessControlState& state, const String& username, const String& password) {
  const String normalizedUsername = trimCopy(username);
  if (normalizedUsername.length() == 0 || password.length() == 0) {
    return false;
  }
  if (normalizedUsername != String(state.adminUsername)) {
    return false;
  }
  return verifyHash(state.adminSalt, state.adminHash, password);
}

bool updateAdminPassword(Preferences& prefs, AccessControlState& state, const String& username,
                         const String& newPassword, String& message) {
  const String normalizedUsername = trimCopy(username);
  if (!isValidAdminUsername(normalizedUsername)) {
    message = "Invalid admin username";
    return false;
  }
  if (newPassword.length() == 0) {
    message = "Admin password cannot be empty";
    return false;
  }

  setCredential(state.adminUsername, sizeof(state.adminUsername), state.adminSalt, state.adminHash,
                normalizedUsername, newPassword);
  state.adminUsesDefaultCredentials = false;
  saveAccessControl(prefs, state);
  message = "Admin credentials updated";
  return true;
}

bool isValidPermanentPinFormat(const String& pin) {
  return pin.length() >= kPinMinLength && pin.length() <= kPinMaxLength && isDigitsOnly(pin);
}

bool isValidTemporaryPinFormat(const String& pin) {
  return pin.length() == kTemporaryPinLength && isDigitsOnly(pin);
}

bool verifyKeypadCode(AccessControlState& state, const String& pin, uint32_t nowEpoch,
                      bool timeSynced, String& matchedSource) {
  matchedSource = "";

  if (!isValidPermanentPinFormat(pin) && !isValidTemporaryPinFormat(pin)) {
    return false;
  }

  for (uint8_t i = 0; i < state.permanentPinCount; ++i) {
    const PermanentPinRecord& record = state.permanentPins[i];
    if (!record.enabled) {
      continue;
    }
    if (verifyHash(record.salt, record.hash, pin)) {
      matchedSource = "Permanent PIN";
      return true;
    }
  }

  if (!timeSynced || nowEpoch < kMinValidEpoch) {
    return false;
  }

  for (uint8_t i = 0; i < state.temporaryPinCount; ++i) {
    const TemporaryPinRecord& record = state.temporaryPins[i];
    if (!record.enabled || record.expiresAtEpoch <= nowEpoch) {
      continue;
    }
    if (verifyHash(record.salt, record.hash, pin)) {
      matchedSource = "Temporary PIN";
      return true;
    }
  }

  return false;
}

int listPermanentPinsJson(const AccessControlState& state, String& json) {
  json = "[";
  for (uint8_t i = 0; i < state.permanentPinCount; ++i) {
    const PermanentPinRecord& record = state.permanentPins[i];
    if (i > 0) {
      json += ',';
    }
    json += "{\"id\":" + String(record.id) +
            ",\"label\":\"" + jsonEscape(String(record.label)) +
            "\",\"enabled\":" + jsonBool(record.enabled) + "}";
  }
  json += "]";
  return 200;
}

int upsertPermanentPin(Preferences& prefs, AccessControlState& state, int32_t id,
                       const String& label, const String& pin, String& message) {
  const String normalizedLabel = trimCopy(label);
  if (normalizedLabel.length() == 0 || normalizedLabel.length() >= kPinLabelMaxLength) {
    message = "Invalid label";
    return 400;
  }
  if (!isValidPermanentPinFormat(pin)) {
    message = "Permanent PIN must be 4-10 digits";
    return 400;
  }
  if (permanentPinMatches(state, pin, id)) {
    message = "Permanent PIN already exists";
    return 409;
  }
  if (temporaryPinMatches(state, pin)) {
    message = "PIN conflicts with an active temporary PIN";
    return 409;
  }

  if (id <= 0) {
    if (state.permanentPinCount >= kMaxPermanentPinUsers) {
      message = "Permanent PIN storage is full";
      return 507;
    }
    createPermanentPinRecord(state.permanentPins[state.permanentPinCount],
                             state.nextPermanentPinId++, normalizedLabel, pin);
    state.permanentPinCount++;
    saveAccessControl(prefs, state);
    message = "Permanent PIN saved";
    return 200;
  }

  for (uint8_t i = 0; i < state.permanentPinCount; ++i) {
    if (static_cast<int32_t>(state.permanentPins[i].id) != id) {
      continue;
    }
    createPermanentPinRecord(state.permanentPins[i], state.permanentPins[i].id,
                             normalizedLabel, pin);
    saveAccessControl(prefs, state);
    message = "Permanent PIN updated";
    return 200;
  }

  message = "Permanent PIN not found";
  return 404;
}

int deletePermanentPin(Preferences& prefs, AccessControlState& state, int32_t id, String& message) {
  if (id <= 0) {
    message = "Missing PIN id";
    return 400;
  }

  for (uint8_t i = 0; i < state.permanentPinCount; ++i) {
    if (static_cast<int32_t>(state.permanentPins[i].id) != id) {
      continue;
    }
    for (uint8_t j = i; j + 1 < state.permanentPinCount; ++j) {
      state.permanentPins[j] = state.permanentPins[j + 1];
    }
    memset(&state.permanentPins[state.permanentPinCount - 1], 0, sizeof(PermanentPinRecord));
    state.permanentPinCount--;
    saveAccessControl(prefs, state);
    message = "Permanent PIN deleted";
    return 200;
  }

  message = "Permanent PIN not found";
  return 404;
}

int listTemporaryPinsJson(AccessControlState& state, uint32_t nowEpoch, bool timeSynced, String& json) {
  json = "[";
  for (uint8_t i = 0; i < state.temporaryPinCount; ++i) {
    const TemporaryPinRecord& record = state.temporaryPins[i];
    if (i > 0) {
      json += ',';
    }
    const bool enabled = record.enabled && timeSynced && record.expiresAtEpoch > nowEpoch;
    json += "{\"id\":" + String(record.id) +
            ",\"enabled\":" + jsonBool(enabled) +
            ",\"expiresAtEpoch\":" + String(record.expiresAtEpoch) +
            ",\"expiresAtLocal\":\"" + jsonEscape(temporaryPinLocalString(record.expiresAtEpoch)) +
            "\",\"timeSynced\":" + jsonBool(timeSynced) +
            "}";
  }
  json += "]";
  return 200;
}

int generateTemporaryPin(Preferences& prefs, AccessControlState& state, uint32_t expiresAtEpoch,
                         uint32_t nowEpoch, bool timeSynced, String& generatedPin, String& message) {
  generatedPin = "";
  if (!timeSynced || nowEpoch < kMinValidEpoch) {
    message = "Device time is not synced";
    return 400;
  }
  if (expiresAtEpoch <= nowEpoch) {
    message = "Expiration time must be in the future";
    return 400;
  }

  pruneExpiredTemporaryPins(prefs, state, nowEpoch, timeSynced);
  if (state.temporaryPinCount >= kMaxTemporaryPins) {
    message = "Temporary PIN storage is full";
    return 507;
  }

  for (uint32_t attempt = 0; attempt < kTemporaryPinGenerationAttempts; ++attempt) {
    const String candidate = buildTemporaryPinCandidate();
    if (permanentPinMatches(state, candidate) || temporaryPinMatches(state, candidate)) {
      continue;
    }

    createTemporaryPinRecord(state.temporaryPins[state.temporaryPinCount],
                             state.nextTemporaryPinId++, expiresAtEpoch, candidate);
    state.temporaryPinCount++;
    saveAccessControl(prefs, state);
    generatedPin = candidate;
    message = "Temporary PIN generated";
    return 200;
  }

  message = "Unable to generate a unique temporary PIN";
  return 500;
}

int revokeTemporaryPin(Preferences& prefs, AccessControlState& state, int32_t id, String& message) {
  if (id <= 0) {
    message = "Missing PIN id";
    return 400;
  }

  for (uint8_t i = 0; i < state.temporaryPinCount; ++i) {
    if (static_cast<int32_t>(state.temporaryPins[i].id) != id) {
      continue;
    }
    for (uint8_t j = i; j + 1 < state.temporaryPinCount; ++j) {
      state.temporaryPins[j] = state.temporaryPins[j + 1];
    }
    memset(&state.temporaryPins[state.temporaryPinCount - 1], 0, sizeof(TemporaryPinRecord));
    state.temporaryPinCount--;
    saveAccessControl(prefs, state);
    message = "Temporary PIN revoked";
    return 200;
  }

  message = "Temporary PIN not found";
  return 404;
}

void pruneExpiredTemporaryPins(Preferences& prefs, AccessControlState& state, uint32_t nowEpoch, bool timeSynced) {
  if (!timeSynced || nowEpoch < kMinValidEpoch || state.temporaryPinCount == 0) {
    return;
  }

  uint8_t writeIndex = 0;
  bool changed = false;
  for (uint8_t i = 0; i < state.temporaryPinCount; ++i) {
    const TemporaryPinRecord& record = state.temporaryPins[i];
    if (record.enabled && record.expiresAtEpoch > nowEpoch) {
      if (writeIndex != i) {
        state.temporaryPins[writeIndex] = record;
      }
      writeIndex++;
    } else {
      changed = true;
    }
  }

  if (!changed) {
    return;
  }

  for (uint8_t i = writeIndex; i < state.temporaryPinCount; ++i) {
    memset(&state.temporaryPins[i], 0, sizeof(TemporaryPinRecord));
  }
  state.temporaryPinCount = writeIndex;
  saveAccessControl(prefs, state);
}
