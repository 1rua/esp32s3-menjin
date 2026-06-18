// 指纹鉴权模块实现：包括模板录入分步状态机、识别与记录维护。
#include "fingerprint_access.h"

#include <string.h>

namespace {
const char* kPhaseMessageSample1 = "请放置手指（1/4）";
const char* kPhaseMessageSample2 = "请再次按压同一手指（2/4）";
const char* kPhaseMessageVerify3 = "请换个角度再次按压（3/4）";
const char* kPhaseMessageVerify4 = "请再换个角度按压（4/4）";
const char* kPhaseMessageLift = "请移开手指";
constexpr char kFingerprintMetaCountKey[] = "fp_meta_cnt";
constexpr char kFingerprintMetaListKey[] = "fp_meta_list";

String trimFingerprintName(const String& value) {
  String copy = value;
  copy.trim();
  return copy;
}

String defaultFingerprintName(uint8_t id) {
  return String("Fingerprint ") + String(id);
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

int findFingerprintRecordIndex(const FingerprintAccessState& state, int id) {
  for (uint8_t i = 0; i < state.recordCount; ++i) {
    if (state.records[i].occupied && state.records[i].id == id) {
      return i;
    }
  }
  return -1;
}

int findNextFingerprintId(const FingerprintAccessState& state, Adafruit_Fingerprint& finger) {
  (void)finger;
  bool occupied[kMaxFingerprintRecords + 1] = {false};
  for (uint8_t i = 0; i < state.recordCount; ++i) {
    const int id = state.records[i].id;
    if (!state.records[i].occupied || id < 1 || id > kMaxFingerprintRecords) {
      continue;
    }
    occupied[id] = true;
  }

  for (int id = 1; id <= kMaxFingerprintRecords; ++id) {
    if (!occupied[id]) {
      return id;
    }
  }
  return -1;
}

void appendFingerprintRecordJson(const FingerprintRecord& record, String& itemsJson) {
  if (itemsJson.length() > 0) {
    itemsJson += ",";
  }
  itemsJson += String("{\"id\":") + String(record.id) +
               ",\"name\":\"" + jsonEscape(String(record.name)) +
               "\",\"occupied\":" + (record.occupied ? "true" : "false") + "}";
}

void persistFingerprintRecords(Preferences& prefs, const FingerprintAccessState& state) {
  prefs.putUChar(kFingerprintMetaCountKey, state.recordCount);
  prefs.putBytes(kFingerprintMetaListKey, state.records, sizeof(FingerprintRecord) * state.recordCount);
}

void loadFingerprintRecords(Preferences& prefs, FingerprintAccessState& state) {
  state.recordCount = prefs.getUChar(kFingerprintMetaCountKey, 0);
  if (state.recordCount > kMaxFingerprintRecords) {
    state.recordCount = 0;
    prefs.putUChar(kFingerprintMetaCountKey, 0);
    prefs.remove(kFingerprintMetaListKey);
    return;
  }
  if (state.recordCount == 0) {
    return;
  }

  const size_t bytesExpected = sizeof(FingerprintRecord) * state.recordCount;
  if (prefs.getBytesLength(kFingerprintMetaListKey) != bytesExpected) {
    state.recordCount = 0;
    prefs.putUChar(kFingerprintMetaCountKey, 0);
    prefs.remove(kFingerprintMetaListKey);
    return;
  }
  prefs.getBytes(kFingerprintMetaListKey, state.records, bytesExpected);
}

void resetFingerprintSessionRuntime(FingerprintAccessState& state) {
  state.source = FingerprintEnrollSource::None;
  state.phase = FingerprintEnrollPhase::Idle;
  state.retryCount = 0;
  state.pendingTemplateStored = false;
  state.nextActionAt = 0;
  state.enrollInput = "";
}

enum class FingerprintVerifyResult : uint8_t {
  NoFinger,
  CaptureError,
  Mismatch,
  Match,
};

bool retryFingerprintStage(FingerprintAccessState& state, const String& retryMessage) {
  if (state.retryCount == 0) {
    state.retryCount = 1;
    state.statusMessage = retryMessage;
    return true;
  }
  return false;
}

void updateFingerprintProgress(FingerprintAccessState& state, uint8_t step, const String& message) {
  state.step = step;
  state.progress = step * 25;
  state.statusMessage = message;
  state.retryCount = 0;
}

void failFingerprintEnroll(FingerprintAccessState& state, Adafruit_Fingerprint& finger, const String& error) {
  const uint8_t failedId = state.enrollId;
  const String failedName = state.pendingName;
  if (state.pendingTemplateStored) {
    finger.deleteModel(state.enrollId);
  }
  state.lastError = error;
  state.statusMessage = error;
  state.progress = 0;
  state.pendingTemplateStored = false;
  resetFingerprintSessionRuntime(state);
  state.enrollId = failedId;
  state.pendingName = failedName;
}

void completeFingerprintEnroll(FingerprintAccessState& state, Preferences& prefs, Audio& audio) {
  const uint8_t enrolledId = state.enrollId;
  const String enrolledName = state.pendingName;
  int index = findFingerprintRecordIndex(state, enrolledId);
  if (index < 0 && state.recordCount < kMaxFingerprintRecords) {
    index = state.recordCount++;
  }
  if (index >= 0) {
    state.records[index].id = enrolledId;
    state.records[index].occupied = true;
    memset(state.records[index].name, 0, sizeof(state.records[index].name));
    enrolledName.toCharArray(state.records[index].name, sizeof(state.records[index].name));
    persistFingerprintRecords(prefs, state);
  }
  state.progress = 100;
  state.step = 4;
  state.lastError = "";
  state.statusMessage = "录入完成";
  state.pendingTemplateStored = false;
  resetFingerprintSessionRuntime(state);
  state.enrollId = enrolledId;
  state.pendingName = enrolledName;
  playBootSound(audio);
}

FingerprintVerifyResult verifyAgainstPendingTemplate(Adafruit_Fingerprint& finger, uint8_t expectedId) {
  int p = finger.getImage();
  if (p == FINGERPRINT_NOFINGER) {
    return FingerprintVerifyResult::NoFinger;
  }
  if (p != FINGERPRINT_OK) {
    return FingerprintVerifyResult::CaptureError;
  }
  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    return FingerprintVerifyResult::CaptureError;
  }
  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) {
    return FingerprintVerifyResult::Mismatch;
  }
  return finger.fingerID == expectedId ? FingerprintVerifyResult::Match : FingerprintVerifyResult::Mismatch;
}

int startFingerprintEnrollFromSerial(FingerprintAccessState& state, Adafruit_Fingerprint& finger, bool otaUpdating, bool provisioningPortalActive, String& message) {
  if (!state.sensorReady) {
    message = "Fingerprint sensor unavailable";
    return 503;
  }
  if (otaUpdating) {
    message = "OTA in progress";
    return 409;
  }
  if (provisioningPortalActive) {
    message = "Provisioning portal active";
    return 409;
  }
  if (fingerprintAccessBusy(state)) {
    message = "Fingerprint enrollment already in progress";
    return 409;
  }

  const int nextId = findNextFingerprintId(state, finger);
  if (nextId == -2) {
    message = "Failed to inspect fingerprint sensor slots";
    return 503;
  }
  if (nextId < 0) {
    message = "Fingerprint storage full";
    return 507;
  }

  state.source = FingerprintEnrollSource::Serial;
  state.phase = FingerprintEnrollPhase::AwaitSample1;
  state.enrollId = static_cast<uint8_t>(nextId);
  state.step = 0;
  state.retryCount = 0;
  state.pendingTemplateStored = false;
  state.nextActionAt = 0;
  state.enrollInput = "";
  state.pendingName = defaultFingerprintName(state.enrollId);
  state.statusMessage = kPhaseMessageSample1;
  state.lastError = "";
  state.progress = 0;
  message = String("Enrolling ID #") + String(nextId);
  return 200;
}
}  // namespace

void initializeFingerprintAccess(Preferences& prefs, FingerprintAccessState& state, HardwareSerial& serialPort, Adafruit_Fingerprint& finger, int rxPin, int txPin) {
  memset(state.records, 0, sizeof(state.records));
  state.recordCount = 0;
  state.progress = 0;
  state.statusMessage = "当前无录入任务";
  state.lastError = "";
  state.pendingName = "";
  state.enrollId = 0;
  state.step = 0;
  loadFingerprintRecords(prefs, state);
  resetFingerprintSessionRuntime(state);

  serialPort.begin(57600, SERIAL_8N1, rxPin, txPin);
  finger.begin(57600);

  state.sensorReady = finger.verifyPassword();
  if (state.sensorReady) {
    Serial.println("Fingerprint Sensor Found!");
  } else {
    Serial.println("Fingerprint Sensor NOT FOUND :(");
  }
}

void handleFingerprintConsoleInput(FingerprintAccessState& state, Adafruit_Fingerprint& finger, Audio& audio, bool otaUpdating, bool provisioningPortalActive) {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c != 'E' && c != 'e') {
      continue;
    }
    if (fingerprintAccessBusy(state)) {
      Serial.println("[ENROLL] Fingerprint enrollment already in progress");
      break;
    }
    String message;
    const int statusCode = startFingerprintEnrollFromSerial(state, finger, otaUpdating, provisioningPortalActive, message);
    if (statusCode == 200) {
      Serial.println("\n=== ENTERING ENROLL MODE ===");
      Serial.println(message);
      audio.stopSong();
    } else {
      Serial.println(String("[ENROLL] ") + message);
    }
    break;
  }
}

void tickFingerprintAccess(Preferences& prefs, FingerprintAccessState& state, Adafruit_Fingerprint& finger, Audio& audio, bool otaUpdating, bool provisioningPortalActive, unsigned long) {
  if (!fingerprintAccessBusy(state)) {
    return;
  }
  if (otaUpdating) {
    failFingerprintEnroll(state, finger, "Enrollment cancelled because OTA started");
    return;
  }
  if (provisioningPortalActive) {
    failFingerprintEnroll(state, finger, "Enrollment cancelled because provisioning portal is active");
    return;
  }

  switch (state.phase) {
    case FingerprintEnrollPhase::AwaitSample1: {
      const int p = finger.getImage();
      if (p == FINGERPRINT_NOFINGER) {
        return;
      }
      if (p != FINGERPRINT_OK) {
        if (retryFingerprintStage(state, "请重新放置手指（1/4）")) {
          return;
        }
        failFingerprintEnroll(state, finger, "录入失败：第 1 次采样无效");
        return;
      }
      if (finger.image2Tz(1) != FINGERPRINT_OK) {
        if (retryFingerprintStage(state, "请重新放置手指（1/4）")) {
          return;
        }
        failFingerprintEnroll(state, finger, "录入失败：第 1 次采样无效");
        return;
      }
      updateFingerprintProgress(state, 1, kPhaseMessageLift);
      state.phase = FingerprintEnrollPhase::WaitLift1;
      return;
    }
    case FingerprintEnrollPhase::WaitLift1:
      if (finger.getImage() == FINGERPRINT_NOFINGER) {
        state.phase = FingerprintEnrollPhase::AwaitSample2;
        state.statusMessage = kPhaseMessageSample2;
      }
      return;
    case FingerprintEnrollPhase::AwaitSample2: {
      const int p = finger.getImage();
      if (p == FINGERPRINT_NOFINGER) {
        return;
      }
      if (p != FINGERPRINT_OK) {
        if (retryFingerprintStage(state, "请再次按压同一手指（2/4）")) {
          return;
        }
        failFingerprintEnroll(state, finger, "录入失败：第 2 次采样无效");
        return;
      }
      if (finger.image2Tz(2) != FINGERPRINT_OK) {
        if (retryFingerprintStage(state, "请再次按压同一手指（2/4）")) {
          return;
        }
        failFingerprintEnroll(state, finger, "录入失败：第 2 次采样无效");
        return;
      }
      const int createStatus = finger.createModel();
      if (createStatus != FINGERPRINT_OK) {
        failFingerprintEnroll(state, finger, "录入失败：第 2 次采样与前序特征不一致");
        return;
      }
      const int storeStatus = finger.storeModel(state.enrollId);
      if (storeStatus != FINGERPRINT_OK) {
        failFingerprintEnroll(state, finger, "录入失败：无法写入模板");
        return;
      }
      state.pendingTemplateStored = true;
      updateFingerprintProgress(state, 2, kPhaseMessageLift);
      state.phase = FingerprintEnrollPhase::WaitLift2;
      return;
    }
    case FingerprintEnrollPhase::WaitLift2:
      if (finger.getImage() == FINGERPRINT_NOFINGER) {
        state.phase = FingerprintEnrollPhase::AwaitVerify3;
        state.statusMessage = kPhaseMessageVerify3;
      }
      return;
    case FingerprintEnrollPhase::AwaitVerify3: {
      const FingerprintVerifyResult verify = verifyAgainstPendingTemplate(finger, state.enrollId);
      if (verify == FingerprintVerifyResult::NoFinger) {
        return;
      }
      if (verify == FingerprintVerifyResult::CaptureError) {
        if (retryFingerprintStage(state, "请换个角度再次按压（3/4）")) {
          return;
        }
        failFingerprintEnroll(state, finger, "录入失败：第 3 次采样无效");
        return;
      }
      if (verify == FingerprintVerifyResult::Match) {
        updateFingerprintProgress(state, 3, kPhaseMessageLift);
        state.phase = FingerprintEnrollPhase::WaitLift3;
        return;
      }
      failFingerprintEnroll(state, finger, "录入失败：第 3 次采样与前序特征不一致");
      return;
    }
    case FingerprintEnrollPhase::WaitLift3:
      if (finger.getImage() == FINGERPRINT_NOFINGER) {
        state.phase = FingerprintEnrollPhase::AwaitVerify4;
        state.statusMessage = kPhaseMessageVerify4;
      }
      return;
    case FingerprintEnrollPhase::AwaitVerify4: {
      const FingerprintVerifyResult verify = verifyAgainstPendingTemplate(finger, state.enrollId);
      if (verify == FingerprintVerifyResult::NoFinger) {
        return;
      }
      if (verify == FingerprintVerifyResult::CaptureError) {
        if (retryFingerprintStage(state, "请再换个角度按压（4/4）")) {
          return;
        }
        failFingerprintEnroll(state, finger, "录入失败：第 4 次采样无效");
        return;
      }
      if (verify == FingerprintVerifyResult::Match) {
        state.phase = FingerprintEnrollPhase::WaitLift4;
        state.statusMessage = kPhaseMessageLift;
        return;
      }
      failFingerprintEnroll(state, finger, "录入失败：第 4 次采样与前序特征不一致");
      return;
    }
    case FingerprintEnrollPhase::WaitLift4:
      if (finger.getImage() == FINGERPRINT_NOFINGER) {
        state.phase = FingerprintEnrollPhase::Finalizing;
        state.statusMessage = "正在验证并写入模板";
      }
      return;
    case FingerprintEnrollPhase::Finalizing:
      completeFingerprintEnroll(state, prefs, audio);
      return;
    case FingerprintEnrollPhase::Idle:
    default:
      return;
  }
}

int pollFingerprintMatch(const FingerprintAccessState& state, Adafruit_Fingerprint& finger, bool otaUpdating) {
  static unsigned long lastPollMs = 0;
  const unsigned long nowMs = millis();
  if (!state.sensorReady || otaUpdating || fingerprintAccessBusy(state)) {
    return -1;
  }
  if (nowMs - lastPollMs < 120UL) {
    return -1;
  }
  lastPollMs = nowMs;
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

int startFingerprintEnrollFromWeb(FingerprintAccessState& state, Adafruit_Fingerprint& finger, const String& name, bool otaUpdating, bool provisioningPortalActive, String& message) {
  if (!state.sensorReady) {
    message = "Fingerprint sensor unavailable";
    return 503;
  }
  if (otaUpdating) {
    message = "OTA in progress";
    return 409;
  }
  if (provisioningPortalActive) {
    message = "Provisioning portal active";
    return 409;
  }
  if (fingerprintAccessBusy(state)) {
    message = "Fingerprint enrollment already in progress";
    return 409;
  }

  String normalized = trimFingerprintName(name);
  if (normalized.length() == 0) {
    message = "Missing fingerprint name";
    return 400;
  }
  if (normalized.length() > 24) {
    message = "Fingerprint name too long";
    return 400;
  }

  const int nextId = findNextFingerprintId(state, finger);
  if (nextId == -2) {
    message = "Failed to inspect fingerprint sensor slots";
    return 503;
  }
  if (nextId < 0) {
    message = "Fingerprint storage full";
    return 507;
  }

  state.source = FingerprintEnrollSource::Web;
  state.phase = FingerprintEnrollPhase::AwaitSample1;
  state.enrollId = static_cast<uint8_t>(nextId);
  state.step = 0;
  state.retryCount = 0;
  state.pendingTemplateStored = false;
  state.nextActionAt = 0;
  state.enrollInput = "";
  state.pendingName = normalized;
  state.statusMessage = kPhaseMessageSample1;
  state.lastError = "";
  state.progress = 0;

  message = String("Fingerprint enrollment started for ID #") + String(nextId);
  return 200;
}

int cancelFingerprintEnroll(FingerprintAccessState& state, Adafruit_Fingerprint& finger, String& message) {
  if (!fingerprintAccessBusy(state)) {
    message = "No fingerprint enrollment in progress";
    return 409;
  }
  if (state.pendingTemplateStored) {
    finger.deleteModel(state.enrollId);
  }
  state.lastError = "Enrollment cancelled";
  state.statusMessage = "录入已取消";
  state.progress = 0;
  resetFingerprintSessionRuntime(state);
  message = "Fingerprint enrollment cancelled";
  return 200;
}

int renameFingerprintRecord(FingerprintAccessState& state, Preferences& prefs, int id, const String& name, String& message) {
  if (fingerprintAccessBusy(state)) {
    message = "Fingerprint enrollment in progress";
    return 409;
  }
  const int index = findFingerprintRecordIndex(state, id);
  if (index < 0) {
    message = "Fingerprint not found";
    return 404;
  }

  String normalized = trimFingerprintName(name);
  if (normalized.length() == 0) {
    message = "Missing fingerprint name";
    return 400;
  }
  if (normalized.length() > 24) {
    message = "Fingerprint name too long";
    return 400;
  }

  memset(state.records[index].name, 0, sizeof(state.records[index].name));
  normalized.toCharArray(state.records[index].name, sizeof(state.records[index].name));
  persistFingerprintRecords(prefs, state);
  message = "Fingerprint renamed";
  return 200;
}

int deleteFingerprintRecord(FingerprintAccessState& state, Preferences& prefs, Adafruit_Fingerprint& finger, int id, String& message) {
  if (fingerprintAccessBusy(state)) {
    message = "Fingerprint enrollment in progress";
    return 409;
  }
  const int index = findFingerprintRecordIndex(state, id);
  if (index < 0) {
    message = "Fingerprint not found";
    return 404;
  }
  if (finger.deleteModel(id) != FINGERPRINT_OK) {
    message = "Failed to delete fingerprint template";
    return 500;
  }
  for (uint8_t i = index; i + 1 < state.recordCount; ++i) {
    state.records[i] = state.records[i + 1];
  }
  if (state.recordCount > 0) {
    state.recordCount--;
    memset(&state.records[state.recordCount], 0, sizeof(FingerprintRecord));
  }
  persistFingerprintRecords(prefs, state);
  message = "Fingerprint deleted";
  return 200;
}

void buildFingerprintRecordsJson(const FingerprintAccessState& state, String& itemsJson) {
  itemsJson = "";
  for (uint8_t i = 0; i < state.recordCount; ++i) {
    if (!state.records[i].occupied) {
      continue;
    }
    appendFingerprintRecordJson(state.records[i], itemsJson);
  }
}

void buildFingerprintEnrollStatusJson(const FingerprintAccessState& state, String& dataJson) {
  const bool active = fingerprintAccessBusy(state);
  dataJson = String("{\"active\":") + (active ? "true" : "false") +
             ",\"id\":" + String(state.enrollId) +
             ",\"name\":\"" + jsonEscape(state.pendingName) +
             "\",\"progress\":" + String(state.progress) +
             ",\"step\":" + String(state.step) +
             ",\"totalSteps\":" + String(kFingerprintEnrollTotalSteps) +
             ",\"phase\":\"" + String(static_cast<int>(state.phase)) +
             "\",\"message\":\"" + jsonEscape(state.statusMessage) +
             "\",\"error\":\"" + jsonEscape(state.lastError) + "\"}";
}
