// 指纹鉴权模块接口：封装识别、录入流程和指纹元数据管理。
#pragma once

#include <Adafruit_Fingerprint.h>
#include <Audio.h>
#include <Arduino.h>
#include <Preferences.h>

#include "../audio_feedback/audio_feedback.h"

constexpr uint8_t kMaxFingerprintRecords = 127;
constexpr size_t kMaxFingerprintNameLength = 25;
constexpr uint8_t kFingerprintEnrollTotalSteps = 4;

enum class FingerprintEnrollSource : uint8_t {
  None,
  Serial,
  Web,
};

enum class FingerprintEnrollPhase : uint8_t {
  Idle,
  AwaitSample1,
  WaitLift1,
  AwaitSample2,
  WaitLift2,
  AwaitVerify3,
  WaitLift3,
  AwaitVerify4,
  WaitLift4,
  Finalizing,
};

struct FingerprintRecord {
  uint8_t id = 0;
  bool occupied = false;
  char name[kMaxFingerprintNameLength] = {0};
};

struct FingerprintAccessState {
  bool sensorReady = false;
  FingerprintEnrollSource source = FingerprintEnrollSource::None;
  FingerprintEnrollPhase phase = FingerprintEnrollPhase::Idle;
  uint8_t enrollId = 0;
  uint8_t step = 0;
  uint8_t retryCount = 0;
  bool pendingTemplateStored = false;
  unsigned long nextActionAt = 0;
  String enrollInput;
  String pendingName;
  String statusMessage;
  String lastError;
  uint8_t progress = 0;
  FingerprintRecord records[kMaxFingerprintRecords];
  uint8_t recordCount = 0;
};

void initializeFingerprintAccess(Preferences& prefs, FingerprintAccessState& state, HardwareSerial& serialPort, Adafruit_Fingerprint& finger, int rxPin, int txPin);
void handleFingerprintConsoleInput(FingerprintAccessState& state, Adafruit_Fingerprint& finger, Audio& audio, bool otaUpdating, bool provisioningPortalActive);
void tickFingerprintAccess(Preferences& prefs, FingerprintAccessState& state, Adafruit_Fingerprint& finger, Audio& audio, bool otaUpdating, bool provisioningPortalActive, unsigned long nowMs);
int pollFingerprintMatch(const FingerprintAccessState& state, Adafruit_Fingerprint& finger, bool otaUpdating);
bool fingerprintAccessBusy(const FingerprintAccessState& state);
int startFingerprintEnrollFromWeb(FingerprintAccessState& state, Adafruit_Fingerprint& finger, const String& name, bool otaUpdating, bool provisioningPortalActive, String& message);
int cancelFingerprintEnroll(FingerprintAccessState& state, Adafruit_Fingerprint& finger, String& message);
int renameFingerprintRecord(FingerprintAccessState& state, Preferences& prefs, int id, const String& name, String& message);
int deleteFingerprintRecord(FingerprintAccessState& state, Preferences& prefs, Adafruit_Fingerprint& finger, int id, String& message);
void buildFingerprintRecordsJson(const FingerprintAccessState& state, String& itemsJson);
void buildFingerprintEnrollStatusJson(const FingerprintAccessState& state, String& dataJson);
