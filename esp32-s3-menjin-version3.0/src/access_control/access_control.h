// 门禁访问控制接口：管理员账号、常驻 PIN 与临时 PIN 数据模型与操作。
#ifndef ACCESS_CONTROL_H
#define ACCESS_CONTROL_H

#include <Arduino.h>
#include <Preferences.h>

constexpr uint8_t kMaxPermanentPinUsers = 10;
constexpr uint8_t kMaxTemporaryPins = 10;
constexpr uint8_t kPinMinLength = 4;
constexpr uint8_t kPinMaxLength = 10;
constexpr uint8_t kTemporaryPinLength = 4;
constexpr uint8_t kAdminUsernameMaxLength = 16;
constexpr uint8_t kPinLabelMaxLength = 24;
constexpr uint8_t kSaltLength = 16;
constexpr uint8_t kHashLength = 32;
constexpr uint32_t kMinValidEpoch = 1704067200UL;

struct PermanentPinRecord {
  uint32_t id;
  bool enabled;
  char label[kPinLabelMaxLength];
  uint8_t salt[kSaltLength];
  uint8_t hash[kHashLength];
};

struct TemporaryPinRecord {
  uint32_t id;
  bool enabled;
  uint32_t expiresAtEpoch;
  uint8_t salt[kSaltLength];
  uint8_t hash[kHashLength];
};

struct AccessControlState {
  char adminUsername[kAdminUsernameMaxLength];
  uint8_t adminSalt[kSaltLength];
  uint8_t adminHash[kHashLength];
  bool adminUsesDefaultCredentials;

  PermanentPinRecord permanentPins[kMaxPermanentPinUsers];
  uint8_t permanentPinCount;

  TemporaryPinRecord temporaryPins[kMaxTemporaryPins];
  uint8_t temporaryPinCount;

  uint32_t nextPermanentPinId;
  uint32_t nextTemporaryPinId;
};

void loadAccessControl(Preferences& prefs, AccessControlState& state);
void saveAccessControl(Preferences& prefs, const AccessControlState& state);
void ensureAccessControlInitialized(Preferences& prefs, AccessControlState& state);

bool isAdminUsingDefaultCredentials(const AccessControlState& state);
bool verifyAdminCredentials(const AccessControlState& state, const String& username, const String& password);
bool updateAdminPassword(Preferences& prefs, AccessControlState& state, const String& username, const String& newPassword, String& message);

bool isValidPermanentPinFormat(const String& pin);
bool isValidTemporaryPinFormat(const String& pin);

bool verifyKeypadCode(AccessControlState& state, const String& pin, uint32_t nowEpoch, bool timeSynced, String& matchedSource);

int listPermanentPinsJson(const AccessControlState& state, String& json);
int upsertPermanentPin(Preferences& prefs, AccessControlState& state, int32_t id, const String& label, const String& pin, String& message);
int deletePermanentPin(Preferences& prefs, AccessControlState& state, int32_t id, String& message);

int listTemporaryPinsJson(AccessControlState& state, uint32_t nowEpoch, bool timeSynced, String& json);
int generateTemporaryPin(Preferences& prefs, AccessControlState& state, uint32_t expiresAtEpoch, uint32_t nowEpoch, bool timeSynced, String& generatedPin, String& message);
int revokeTemporaryPin(Preferences& prefs, AccessControlState& state, int32_t id, String& message);

void pruneExpiredTemporaryPins(Preferences& prefs, AccessControlState& state, uint32_t nowEpoch, bool timeSynced);

#endif
