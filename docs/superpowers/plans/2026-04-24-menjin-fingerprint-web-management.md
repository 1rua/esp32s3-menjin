# Menjin Fingerprint Web Management Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Web-triggered fingerprint enrollment, fingerprint list/rename/delete management, and a 4-sample validated enrollment flow with percentage progress reporting while keeping serial `E/e` as a fallback entry.

**Architecture:** Keep the existing project style: the fingerprint feature remains centered in `fingerprint_access/`, the main sketch keeps only thin glue callbacks, and `web_portal/` stays a Basic-Auth-protected route/UI layer. Implement the work as vertical slices: extend the fingerprint module first, then expose it through sketch wrappers, then add HTTP routes, then add the control-page UI.

**Tech Stack:** Arduino ESP32 sketch, `Adafruit_Fingerprint`, `Preferences`, `WebServer`, inline HTML/JS in `web_portal.cpp`, existing `Audio` feedback helpers.

---

## File Structure

- `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino`
  - Keep `setup()`, `loop()`, hardware construction, and thin Web glue callbacks only.
  - Update fingerprint initialization and loop call signatures to the new module API.
  - Add thin wrappers used by `WebPortalContext` for fingerprint list/start/status/cancel/rename/delete.

- `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.h`
  - Expand the public state struct for sensor readiness, metadata, progress, and unified enrollment session state.
  - Declare Web-facing management functions and JSON builders used by the sketch wrappers.

- `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.cpp`
  - Implement fingerprint metadata persistence in NVS.
  - Implement unified enrollment session creation/cancel/status.
  - Replace the 2-sample enrollment flow with a 4-sample validated flow.
  - Keep match polling isolated from door control.

- `esp32-s3-menjin-version3.0/web_portal/web_portal.h`
  - Add fingerprint management callback typedefs.
  - Extend `WebPortalContext` with fingerprint callbacks.

- `esp32-s3-menjin-version3.0/web_portal/web_portal.cpp`
  - Add HTTP routes for fingerprint list/start/status/cancel/rename/delete.
  - Extend `CONTROL_PAGE_HTML` with a fingerprint management card, progress bar, and polling JS.

---

## Common Verification Command

Use the existing saved board configuration from `esp32-s3-menjin-version3.0/build/esp32.esp32.esp32s3/build.options.json`:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=dfu,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default esp32-s3-menjin-version3.0
```

Expected success signal: the compile finishes with `Sketch uses ...` and no compiler errors.

---

### Task 1: Expand `fingerprint_access` state and persistence foundation

**Files:**
- Modify: `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.h:1-40`
- Modify: `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.cpp:1-181`
- Modify: `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino:174,227,263,285`
- Test: sketch compile via `arduino-cli compile`

- [ ] **Step 1: Write the failing integration-first call sites in the sketch**

Update the sketch call sites before the module signature exists:

```cpp
initializeFingerprintAccess(prefs, fingerprintState, mySerial, finger, FP_RX_PIN, FP_TX_PIN);
```

```cpp
tickFingerprintAccess(
  prefs,
  fingerprintState,
  finger,
  audio,
  runtimeServices.isOtaUpdating,
  isProvisioningPortalActive(provisioningState),
  millis()
);
```

```cpp
handleFingerprintConsoleInput(
  fingerprintState,
  finger,
  audio,
  runtimeServices.isOtaUpdating,
  isProvisioningPortalActive(provisioningState)
);
```

Keep the existing match poll for now:

```cpp
int fpID = pollFingerprintMatch(fingerprintState, finger, runtimeServices.isOtaUpdating);
```

- [ ] **Step 2: Run compile to verify it fails before the API exists**

Run:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=dfu,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default esp32-s3-menjin-version3.0
```

Expected: FAIL with signature mismatch errors for `initializeFingerprintAccess`, `tickFingerprintAccess`, and `handleFingerprintConsoleInput`.

- [ ] **Step 3: Expand the fingerprint public header with persistent records and session state**

Replace the current `fingerprint_access.h` contents with this public surface:

```cpp
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
int startFingerprintEnrollFromWeb(FingerprintAccessState& state, const String& name, bool otaUpdating, bool provisioningPortalActive, String& message);
int cancelFingerprintEnroll(FingerprintAccessState& state, Adafruit_Fingerprint& finger, String& message);
int renameFingerprintRecord(FingerprintAccessState& state, Preferences& prefs, int id, const String& name, String& message);
int deleteFingerprintRecord(FingerprintAccessState& state, Preferences& prefs, Adafruit_Fingerprint& finger, int id, String& message);
void buildFingerprintRecordsJson(const FingerprintAccessState& state, String& itemsJson);
void buildFingerprintEnrollStatusJson(const FingerprintAccessState& state, String& dataJson);
```

- [ ] **Step 4: Implement metadata loading and the new initializer in `fingerprint_access.cpp`**

At the top of `fingerprint_access.cpp`, add the persistence constants and helpers:

```cpp
namespace {
constexpr unsigned long kFingerprintEnrollRemoveDelayMs = 2000;
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
  state.enrollId = 0;
  state.step = 0;
  state.retryCount = 0;
  state.pendingTemplateStored = false;
  state.nextActionAt = 0;
  state.enrollInput = "";
  state.pendingName = "";
}
}
```

Then replace the initializer with:

```cpp
void initializeFingerprintAccess(Preferences& prefs, FingerprintAccessState& state, HardwareSerial& serialPort, Adafruit_Fingerprint& finger, int rxPin, int txPin) {
  memset(state.records, 0, sizeof(state.records));
  state.recordCount = 0;
  state.progress = 0;
  state.statusMessage = "当前无录入任务";
  state.lastError = "";
  resetFingerprintSessionRuntime(state);
  loadFingerprintRecords(prefs, state);

  serialPort.begin(57600, SERIAL_8N1, rxPin, txPin);
  finger.begin(57600);

  state.sensorReady = finger.verifyPassword();
  if (state.sensorReady) {
    Serial.println("Fingerprint Sensor Found!");
  } else {
    Serial.println("Fingerprint Sensor NOT FOUND :(");
  }
}
```

Keep the other functions compiling by updating only their signatures for now and preserving the old logic.

- [ ] **Step 5: Rewire the sketch to the new signatures**

Apply these exact `.ino` replacements:

```cpp
initializeFingerprintAccess(prefs, fingerprintState, mySerial, finger, FP_RX_PIN, FP_TX_PIN);
```

```cpp
handleFingerprintConsoleInput(
  fingerprintState,
  finger,
  audio,
  runtimeServices.isOtaUpdating,
  isProvisioningPortalActive(provisioningState)
);
```

```cpp
tickFingerprintAccess(
  prefs,
  fingerprintState,
  finger,
  audio,
  runtimeServices.isOtaUpdating,
  isProvisioningPortalActive(provisioningState),
  millis()
);
```

- [ ] **Step 6: Run compile to verify the foundation builds**

Run the common compile command.

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.h \
        esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.cpp \
        esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino && \
git commit -m "feat(fingerprint): add persistent state foundation"
```

### Task 2: Add sketch glue and Web-facing fingerprint management APIs

**Files:**
- Modify: `esp32-s3-menjin-version3.0/web_portal/web_portal.h:11-30`
- Modify: `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino:112-139,300-340`
- Modify: `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.h:1-60`
- Modify: `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.cpp:1-260`
- Test: sketch compile via `arduino-cli compile`

- [ ] **Step 1: Write the failing WebPortalContext glue in the sketch first**

Add these prototypes above `getWebPortalContext()`:

```cpp
int startFingerprintEnrollFromWebPortal(const String& name, String& message);
void getFingerprintEnrollStatusFromWebPortal(String& dataJson);
int cancelFingerprintEnrollFromWebPortal(String& message);
void listFingerprintsFromWebPortal(String& itemsJson);
int renameFingerprintFromWebPortal(int id, const String& name, String& message);
int deleteFingerprintFromWebPortal(int id, String& message);
```

Then extend the `WebPortalContext` initializer with the fingerprint callbacks before the struct supports them:

```cpp
WebPortalContext& getWebPortalContext() {
  static WebPortalContext context = {
    server,
    prefs,
    deviceConfig,
    provisioningState,
    accessControl,
    handleDoorOpenFromWeb,
    addNfcCardFromWeb,
    startFingerprintEnrollFromWebPortal,
    getFingerprintEnrollStatusFromWebPortal,
    cancelFingerprintEnrollFromWebPortal,
    listFingerprintsFromWebPortal,
    renameFingerprintFromWebPortal,
    deleteFingerprintFromWebPortal,
    handlePortalStateChanged,
    runtimeIsTimeSynced,
    runtimeCurrentEpochSeconds,
    runtimeCurrentLocalTimeString
  };
  return context;
}
```

- [ ] **Step 2: Run compile to verify it fails before `web_portal.h` is extended**

Run the common compile command.

Expected: FAIL with `too many initializers for 'WebPortalContext'`.

- [ ] **Step 3: Extend `web_portal.h` with fingerprint callback typedefs and context fields**

Replace the callback block in `web_portal.h` with:

```cpp
typedef void (*WebPortalDoorOpenCallback)();
typedef int (*WebPortalAddNfcCallback)(const String& uid, String& message);
typedef int (*WebPortalFingerprintEnrollStartCallback)(const String& name, String& message);
typedef void (*WebPortalFingerprintEnrollStatusCallback)(String& dataJson);
typedef int (*WebPortalFingerprintEnrollCancelCallback)(String& message);
typedef void (*WebPortalFingerprintListCallback)(String& itemsJson);
typedef int (*WebPortalFingerprintRenameCallback)(int id, const String& name, String& message);
typedef int (*WebPortalFingerprintDeleteCallback)(int id, String& message);
typedef void (*WebPortalStateChangedCallback)();
typedef bool (*WebPortalTimeSyncedCallback)();
typedef uint32_t (*WebPortalCurrentEpochCallback)();
typedef String (*WebPortalLocalTimeCallback)();
```

And update `WebPortalContext` to:

```cpp
struct WebPortalContext {
  WebServer& server;
  Preferences& prefs;
  DeviceConfig& deviceConfig;
  ProvisioningState& provisioningState;
  AccessControlState& accessControl;
  WebPortalDoorOpenCallback onDoorOpen;
  WebPortalAddNfcCallback onAddNfc;
  WebPortalFingerprintEnrollStartCallback onStartFingerprintEnroll;
  WebPortalFingerprintEnrollStatusCallback onFingerprintEnrollStatus;
  WebPortalFingerprintEnrollCancelCallback onCancelFingerprintEnroll;
  WebPortalFingerprintListCallback onListFingerprints;
  WebPortalFingerprintRenameCallback onRenameFingerprint;
  WebPortalFingerprintDeleteCallback onDeleteFingerprint;
  WebPortalStateChangedCallback onProvisioningStateChanged;
  WebPortalTimeSyncedCallback isTimeSynced;
  WebPortalCurrentEpochCallback currentEpochSeconds;
  WebPortalLocalTimeCallback currentLocalTimeString;
};
```

- [ ] **Step 4: Add the Web-facing management functions to `fingerprint_access.h`**

Ensure these declarations exist in the public header:

```cpp
int startFingerprintEnrollFromWeb(FingerprintAccessState& state, const String& name, bool otaUpdating, bool provisioningPortalActive, String& message);
int cancelFingerprintEnroll(FingerprintAccessState& state, Adafruit_Fingerprint& finger, String& message);
int renameFingerprintRecord(FingerprintAccessState& state, Preferences& prefs, int id, const String& name, String& message);
int deleteFingerprintRecord(FingerprintAccessState& state, Preferences& prefs, Adafruit_Fingerprint& finger, int id, String& message);
void buildFingerprintRecordsJson(const FingerprintAccessState& state, String& itemsJson);
void buildFingerprintEnrollStatusJson(const FingerprintAccessState& state, String& dataJson);
```

- [ ] **Step 5: Implement the management functions in `fingerprint_access.cpp`**

Add these helper functions near the top of `fingerprint_access.cpp`:

```cpp
namespace {
int findFingerprintRecordIndex(const FingerprintAccessState& state, int id) {
  for (uint8_t i = 0; i < state.recordCount; ++i) {
    if (state.records[i].occupied && state.records[i].id == id) {
      return i;
    }
  }
  return -1;
}

int findNextFingerprintId(const FingerprintAccessState& state) {
  for (int id = 1; id <= kMaxFingerprintRecords; ++id) {
    if (findFingerprintRecordIndex(state, id) == -1) {
      return id;
    }
  }
  return -1;
}

String normalizeFingerprintName(const String& value, int fallbackId) {
  String name = trimFingerprintName(value);
  if (name.length() == 0) {
    return defaultFingerprintName(static_cast<uint8_t>(fallbackId));
  }
  return name;
}

void appendFingerprintRecordJson(const FingerprintRecord& record, String& itemsJson) {
  if (itemsJson.length() > 0) {
    itemsJson += ",";
  }
  itemsJson += String("{\"id\":") + String(record.id) +
               ",\"name\":\"" + jsonEscape(String(record.name)) +
               "\",\"occupied\":" + (record.occupied ? "true" : "false") + "}";
}
}
```

Then add these public functions exactly:

```cpp
int startFingerprintEnrollFromWeb(FingerprintAccessState& state, const String& name, bool otaUpdating, bool provisioningPortalActive, String& message) {
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

  const int nextId = findNextFingerprintId(state);
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
  state.statusMessage = "请放置手指（1/4）";
  state.lastError = "";
  state.progress = 0;

  message = String("Fingerprint enrollment started for ID #") + String(nextId);
  return 200;
}
```

```cpp
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
```

```cpp
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
```

```cpp
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
```

```cpp
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
```

- [ ] **Step 6: Add the thin sketch wrappers in `.ino`**

Add these function bodies below the existing `addNfcCardFromWeb(...)` wrapper:

```cpp
int startFingerprintEnrollFromWebPortal(const String& name, String& message) {
  return startFingerprintEnrollFromWeb(
    fingerprintState,
    name,
    runtimeServices.isOtaUpdating,
    isProvisioningPortalActive(provisioningState),
    message
  );
}

void getFingerprintEnrollStatusFromWebPortal(String& dataJson) {
  buildFingerprintEnrollStatusJson(fingerprintState, dataJson);
}

int cancelFingerprintEnrollFromWebPortal(String& message) {
  return cancelFingerprintEnroll(fingerprintState, finger, message);
}

void listFingerprintsFromWebPortal(String& itemsJson) {
  buildFingerprintRecordsJson(fingerprintState, itemsJson);
}

int renameFingerprintFromWebPortal(int id, const String& name, String& message) {
  return renameFingerprintRecord(fingerprintState, prefs, id, name, message);
}

int deleteFingerprintFromWebPortal(int id, String& message) {
  return deleteFingerprintRecord(fingerprintState, prefs, finger, id, message);
}
```

- [ ] **Step 7: Run compile to verify the glue and management APIs build**

Run the common compile command.

Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add esp32-s3-menjin-version3.0/web_portal/web_portal.h \
        esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.h \
        esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.cpp \
        esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino && \
git commit -m "feat(fingerprint): add management callbacks and glue"
```

### Task 3: Replace the current enrollment logic with a 4-sample validated session

**Files:**
- Modify: `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.h:8-60`
- Modify: `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.cpp:1-420`
- Test: sketch compile via `arduino-cli compile`

- [ ] **Step 1: Write the failing enum-first refactor in the header**

Replace the old phase enum values in `fingerprint_access.h` with the new 4-step values only:

```cpp
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
```

Do not touch the old `switch` yet.

- [ ] **Step 2: Run compile to verify the old state machine now fails**

Run the common compile command.

Expected: FAIL because `AwaitId`, `CaptureFirst`, `WaitRemoveDelay`, `WaitRemove`, and `CaptureSecond` no longer exist.

- [ ] **Step 3: Add the session helpers used by the new flow**

Add these helpers near the top of `fingerprint_access.cpp`:

```cpp
namespace {
const char* kPhaseMessageSample1 = "请放置手指（1/4）";
const char* kPhaseMessageSample2 = "请再次按压同一手指（2/4）";
const char* kPhaseMessageVerify3 = "请换个角度再次按压（3/4）";
const char* kPhaseMessageVerify4 = "请再换个角度按压（4/4）";
const char* kPhaseMessageLift = "请移开手指";

void updateFingerprintProgress(FingerprintAccessState& state, uint8_t step, const String& message) {
  state.step = step;
  state.progress = step * 25;
  state.statusMessage = message;
  state.retryCount = 0;
}

void failFingerprintEnroll(FingerprintAccessState& state, Adafruit_Fingerprint& finger, const String& error) {
  if (state.pendingTemplateStored) {
    finger.deleteModel(state.enrollId);
  }
  state.lastError = error;
  state.statusMessage = error;
  state.pendingTemplateStored = false;
  resetFingerprintSessionRuntime(state);
}

void completeFingerprintEnroll(FingerprintAccessState& state, Preferences& prefs, Audio& audio) {
  int index = findFingerprintRecordIndex(state, state.enrollId);
  if (index < 0 && state.recordCount < kMaxFingerprintRecords) {
    index = state.recordCount++;
  }
  if (index >= 0) {
    state.records[index].id = state.enrollId;
    state.records[index].occupied = true;
    memset(state.records[index].name, 0, sizeof(state.records[index].name));
    state.pendingName.toCharArray(state.records[index].name, sizeof(state.records[index].name));
    persistFingerprintRecords(prefs, state);
  }
  state.progress = 100;
  state.step = 4;
  state.lastError = "";
  state.statusMessage = "录入完成";
  state.pendingTemplateStored = false;
  playBootSound(audio);
  resetFingerprintSessionRuntime(state);
}

bool verifyAgainstPendingTemplate(Adafruit_Fingerprint& finger, uint8_t expectedId) {
  int p = finger.getImage();
  if (p != FINGERPRINT_OK) {
    return false;
  }
  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    return false;
  }
  p = finger.fingerFastSearch();
  return p == FINGERPRINT_OK && finger.fingerID == expectedId;
}

int startFingerprintEnrollFromSerial(FingerprintAccessState& state, bool otaUpdating, bool provisioningPortalActive, String& message) {
  const int nextId = findNextFingerprintId(state);
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
}
```

- [ ] **Step 4: Replace `handleFingerprintConsoleInput(...)` with unified session start**

Use this exact implementation:

```cpp
void handleFingerprintConsoleInput(FingerprintAccessState& state, Adafruit_Fingerprint&, Audio& audio, bool otaUpdating, bool provisioningPortalActive) {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if ((c == 'E' || c == 'e') && !fingerprintAccessBusy(state)) {
      String message;
      const int statusCode = startFingerprintEnrollFromSerial(state, otaUpdating, provisioningPortalActive, message);
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
}
```

- [ ] **Step 5: Replace `tickFingerprintAccess(...)` with the 4-step validated state machine**

Replace the current `switch` body with:

```cpp
void tickFingerprintAccess(Preferences& prefs, FingerprintAccessState& state, Adafruit_Fingerprint& finger, Audio& audio, bool otaUpdating, bool provisioningPortalActive, unsigned long nowMs) {
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
      if (p != FINGERPRINT_OK || finger.image2Tz(1) != FINGERPRINT_OK) {
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
      if (p != FINGERPRINT_OK || finger.image2Tz(2) != FINGERPRINT_OK) {
        failFingerprintEnroll(state, finger, "录入失败：第 2 次采样无效");
        return;
      }
      if (finger.createModel() != FINGERPRINT_OK || finger.storeModel(state.enrollId) != FINGERPRINT_OK) {
        failFingerprintEnroll(state, finger, "录入失败：无法创建或写入模板");
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
    case FingerprintEnrollPhase::AwaitVerify3:
      if (verifyAgainstPendingTemplate(finger, state.enrollId)) {
        updateFingerprintProgress(state, 3, kPhaseMessageLift);
        state.phase = FingerprintEnrollPhase::WaitLift3;
        return;
      }
      if (finger.getImage() != FINGERPRINT_NOFINGER) {
        failFingerprintEnroll(state, finger, "录入失败：第 3 次采样与前序特征不一致");
      }
      return;
    case FingerprintEnrollPhase::WaitLift3:
      if (finger.getImage() == FINGERPRINT_NOFINGER) {
        state.phase = FingerprintEnrollPhase::AwaitVerify4;
        state.statusMessage = kPhaseMessageVerify4;
      }
      return;
    case FingerprintEnrollPhase::AwaitVerify4:
      if (verifyAgainstPendingTemplate(finger, state.enrollId)) {
        state.phase = FingerprintEnrollPhase::Finalizing;
        state.statusMessage = "正在验证并写入模板";
        return;
      }
      if (finger.getImage() != FINGERPRINT_NOFINGER) {
        failFingerprintEnroll(state, finger, "录入失败：第 4 次采样与前序特征不一致");
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
```

- [ ] **Step 6: Keep `pollFingerprintMatch(...)` paused during enrollment**

Make sure the guard remains exactly this:

```cpp
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
```

- [ ] **Step 7: Run compile to verify the new enrollment flow builds**

Run the common compile command.

Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.h \
        esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.cpp && \
git commit -m "feat(fingerprint): add four-step validated enrollment"
```

### Task 4: Add fingerprint management HTTP routes in `web_portal`

**Files:**
- Modify: `esp32-s3-menjin-version3.0/web_portal/web_portal.cpp:522-745`
- Test: sketch compile via `arduino-cli compile`

- [ ] **Step 1: Register the new routes before the handlers exist**

Add these route registrations in `setupWebRoutes(...)` first:

```cpp
gContext->server.on("/fingerprints", HTTP_GET, handleFingerprintsRoute);
gContext->server.on("/fingerprint_enroll_start", HTTP_POST, handleFingerprintEnrollStartRoute);
gContext->server.on("/fingerprint_enroll_status", HTTP_GET, handleFingerprintEnrollStatusRoute);
gContext->server.on("/fingerprint_enroll_cancel", HTTP_POST, handleFingerprintEnrollCancelRoute);
gContext->server.on("/fingerprint_rename", HTTP_POST, handleFingerprintRenameRoute);
gContext->server.on("/fingerprint_delete", HTTP_POST, handleFingerprintDeleteRoute);
```

- [ ] **Step 2: Run compile to verify it fails before the handlers exist**

Run the common compile command.

Expected: FAIL with undeclared handler errors such as `handleFingerprintsRoute was not declared`.

- [ ] **Step 3: Add the fingerprint route handlers above `setupWebRoutes(...)`**

Insert these handlers near the other `handle*Route()` functions:

```cpp
void handleFingerprintsRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  String itemsJson;
  if (gContext->onListFingerprints != nullptr) {
    gContext->onListFingerprints(itemsJson);
  }
  sendJsonData(200, "ok", "Fingerprints loaded", String("{\"items\":[") + itemsJson + "]}");
}
```

```cpp
void handleFingerprintEnrollStartRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  const String name = readRequestArg("name");
  String message;
  const int statusCode = gContext->onStartFingerprintEnroll != nullptr
    ? gContext->onStartFingerprintEnroll(name, message)
    : 500;
  String dataJson = "{}";
  if (gContext->onFingerprintEnrollStatus != nullptr) {
    gContext->onFingerprintEnrollStatus(dataJson);
  }
  sendJsonData(statusCode, statusCode == 200 ? "ok" : "error", message, dataJson);
}
```

```cpp
void handleFingerprintEnrollStatusRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  String dataJson = "{}";
  if (gContext->onFingerprintEnrollStatus != nullptr) {
    gContext->onFingerprintEnrollStatus(dataJson);
  }
  sendJsonData(200, "ok", "Fingerprint enrollment status loaded", dataJson);
}
```

```cpp
void handleFingerprintEnrollCancelRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  String message;
  const int statusCode = gContext->onCancelFingerprintEnroll != nullptr
    ? gContext->onCancelFingerprintEnroll(message)
    : 500;
  sendJson(statusCode, statusCode == 200 ? "ok" : "error", message.length() > 0 ? message : "Fingerprint enroll cancel callback unavailable");
}
```

```cpp
void handleFingerprintRenameRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  const String idValue = readRequestArg("id");
  const String name = readRequestArg("name");
  if (idValue.length() == 0) {
    sendJson(400, "error", "Missing fingerprint id");
    return;
  }
  String message;
  const int statusCode = gContext->onRenameFingerprint != nullptr
    ? gContext->onRenameFingerprint(idValue.toInt(), name, message)
    : 500;
  sendJson(statusCode, statusCode == 200 ? "ok" : "error", message.length() > 0 ? message : "Fingerprint rename callback unavailable");
}
```

```cpp
void handleFingerprintDeleteRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  const String idValue = readRequestArg("id");
  if (idValue.length() == 0) {
    sendJson(400, "error", "Missing fingerprint id");
    return;
  }
  String message;
  const int statusCode = gContext->onDeleteFingerprint != nullptr
    ? gContext->onDeleteFingerprint(idValue.toInt(), message)
    : 500;
  sendJson(statusCode, statusCode == 200 ? "ok" : "error", message.length() > 0 ? message : "Fingerprint delete callback unavailable");
}
```

- [ ] **Step 4: Run compile to verify the new routes build**

Run the common compile command.

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add esp32-s3-menjin-version3.0/web_portal/web_portal.cpp && \
git commit -m "feat(web): add fingerprint management routes"
```

### Task 5: Add the fingerprint management card and polling UI

**Files:**
- Modify: `esp32-s3-menjin-version3.0/web_portal/web_portal.cpp:9-324`
- Test: sketch compile via `arduino-cli compile`

- [ ] **Step 1: Add the CSS needed for a simple percentage progress bar**

In the `CONTROL_PAGE_HTML` `<style>` block, add:

```css
.progress { width: 100%; height: 10px; background: #2c2c2c; border-radius: 999px; overflow: hidden; margin-top: 10px; }
.progress-bar { height: 100%; width: 0%; background: #4caf50; transition: width 0.2s ease; }
.inline-input { display: flex; gap: 10px; }
.inline-input input { flex: 1; }
```

- [ ] **Step 2: Add the fingerprint management card markup**

Insert this card in `CONTROL_PAGE_HTML` below the NFC card and above the network card:

```html
<div class="card">
  <h3>指纹管理</h3>
  <div class="inline-input">
    <input type="text" id="fingerprintName" maxlength="24" placeholder="新指纹名称，例如：右手拇指">
    <button id="fingerprintEnrollStart" onclick="startFingerprintEnroll()">开始录入</button>
  </div>
  <button id="fingerprintEnrollCancel" class="btn-secondary" onclick="cancelFingerprintEnroll()" disabled>取消当前录入</button>
  <div id="fingerprintEnrollStatus" class="status">当前无录入任务</div>
  <div class="progress"><div id="fingerprintEnrollBar" class="progress-bar"></div></div>
  <div id="fingerprintEnrollPercent" class="hint">0%</div>
  <div id="fingerprintList" class="list"></div>
</div>
```

- [ ] **Step 3: Add the JS renderers and polling helpers**

Insert these functions inside the control-page `<script>` block:

```javascript
let fingerprintPollTimer = 0;

function stopFingerprintPolling() {
  if (fingerprintPollTimer) {
    clearInterval(fingerprintPollTimer);
    fingerprintPollTimer = 0;
  }
}

function updateFingerprintStatus(status) {
  const progress = Number(status.progress || 0);
  document.getElementById('fingerprintEnrollBar').style.width = progress + '%';
  document.getElementById('fingerprintEnrollPercent').textContent = progress + '%';
  document.getElementById('fingerprintEnrollStatus').innerHTML = `${escapeHtml(status.message || '当前无录入任务')}<br><span class="hint mono">ID: ${status.id || '-'} · ${escapeHtml(status.name || '-')}</span>${status.error ? `<br><span class="warning">${escapeHtml(status.error)}</span>` : ''}`;
  document.getElementById('fingerprintEnrollCancel').disabled = !status.active;
  document.getElementById('fingerprintEnrollStart').disabled = !!status.active;
}

function renderFingerprints(items) {
  const container = document.getElementById('fingerprintList');
  if (!items.length) {
    container.innerHTML = '<div class="hint">暂无已录入指纹</div>';
    return;
  }
  container.innerHTML = items.map(item => `
    <div class="row">
      <div>
        <div>${escapeHtml(item.name)}</div>
        <div class="hint mono">ID: ${item.id}</div>
      </div>
      <div class="row-actions">
        <button class="btn-secondary" onclick="renameFingerprint(${item.id}, '${escapeJsString(item.name)}')">重命名</button>
        <button class="btn-danger" onclick="deleteFingerprint(${item.id})">删除</button>
      </div>
    </div>`).join('');
}

async function loadFingerprints() {
  const data = await requestJson('/fingerprints');
  renderFingerprints((data.data && data.data.items) || []);
}

async function loadFingerprintEnrollStatus() {
  const data = await requestJson('/fingerprint_enroll_status');
  const status = data.data || {};
  updateFingerprintStatus(status);
  if (!status.active) {
    stopFingerprintPolling();
    await loadFingerprints();
  }
}

function startFingerprintPolling() {
  stopFingerprintPolling();
  fingerprintPollTimer = setInterval(() => {
    loadFingerprintEnrollStatus().catch(error => {
      stopFingerprintPolling();
      alert(error.message || '指纹进度读取失败');
    });
  }, 800);
}

async function startFingerprintEnroll() {
  const name = document.getElementById('fingerprintName').value.trim();
  if (!name) return alert('必须输入指纹名称');
  const data = await requestJson('/fingerprint_enroll_start', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: encodeForm({name})
  });
  showMessage(data, '指纹录入已开始');
  updateFingerprintStatus(data.data || {});
  startFingerprintPolling();
}

async function cancelFingerprintEnroll() {
  const data = await requestJson('/fingerprint_enroll_cancel', {method: 'POST'});
  showMessage(data, '指纹录入已取消');
  stopFingerprintPolling();
  await loadFingerprintEnrollStatus();
}

async function renameFingerprint(id, currentName) {
  const name = prompt('输入新的指纹名称', currentName || '');
  if (name === null) return;
  const data = await requestJson('/fingerprint_rename', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: encodeForm({id, name})
  });
  showMessage(data, '指纹名称已更新');
  await loadFingerprints();
}

async function deleteFingerprint(id) {
  if (!confirm(`确认删除指纹 ID ${id} 吗？`)) return;
  const data = await requestJson('/fingerprint_delete', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: encodeForm({id})
  });
  showMessage(data, '指纹已删除');
  await loadFingerprints();
}
```

- [ ] **Step 4: Load fingerprint state during page initialization**

Extend `initializePage()` to:

```javascript
async function initializePage() {
  try {
    await loadPinUsers();
    await loadTimeStatus();
    await loadTempPins();
    await loadAdminStatus();
    await loadFingerprints();
    await loadFingerprintEnrollStatus();
  } catch (error) {
    alert(error.message || '页面初始化失败');
  }
}
```

- [ ] **Step 5: Run compile to verify the embedded page still builds**

Run the common compile command.

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add esp32-s3-menjin-version3.0/web_portal/web_portal.cpp && \
git commit -m "feat(web): add fingerprint management UI"
```

### Task 6: Final verification and smoke checklist

**Files:**
- Modify: `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.cpp` (only if smoke finds issues)
- Modify: `esp32-s3-menjin-version3.0/web_portal/web_portal.cpp` (only if smoke finds issues)
- Modify: `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino` (only if smoke finds issues)
- Test: sketch compile via `arduino-cli compile`, hardware smoke on the device

- [ ] **Step 1: Run the full compile check**

Run the common compile command.

Expected: PASS.

- [ ] **Step 2: Run the hardware smoke checklist**

Verify these exact behaviors after flashing the build:

1. Open the Web control page and confirm the new “指纹管理” card renders.
2. Confirm `GET /fingerprints` shows the existing fingerprint list.
3. Start a Web enrollment and confirm the progress bar moves `0% -> 25% -> 50% -> 75% -> 100%`.
4. During enrollment, confirm ordinary fingerprint matching no longer opens the door.
5. Complete enrollment successfully and confirm the new fingerprint immediately matches and opens the door.
6. Start a Web enrollment, deliberately use a different finger on the 3rd or 4th sample, and confirm the session fails with an error message.
7. Delete an existing fingerprint from the Web page and confirm it can no longer open the door.
8. Rename an existing fingerprint from the Web page and confirm the refreshed list shows the new name.
9. Trigger serial `E` enrollment and confirm it enters the same unified session flow with an auto-generated name.
10. While the provisioning portal is active or OTA is running, confirm a new Web enrollment is rejected.

- [ ] **Step 3: Fix only the issues found during smoke**

If any smoke issue appears, fix only that issue before the final commit. Do not broaden the scope.

- [ ] **Step 4: Commit**

```bash
git add esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.cpp \
        esp32-s3-menjin-version3.0/web_portal/web_portal.cpp \
        esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino && \
git commit -m "feat(firmware): finish fingerprint web management"
```

---

## Spec Coverage Check

- Web-triggered fingerprint enrollment: covered by Tasks 2, 4, and 5.
- Fingerprint list / rename / delete management: covered by Tasks 2, 4, and 5.
- 4-sample validated enrollment flow: covered by Task 3.
- Percentage progress reporting: covered by Tasks 2, 3, 4, and 5.
- Keep serial `E/e` as fallback: covered by Task 3.
- Keep the sketch as thin glue only: covered by Tasks 1 and 2.
- Short-polling Web UI instead of push: covered by Task 5.
- Hardware verification of the full user flow: covered by Task 6.

## Self-Review Notes

- No `TODO`, `TBD`, or placeholder task text remains.
- Public function names are consistent across all tasks.
- The plan stays within the existing module pattern and does not introduce new framework layers.
- Automated verification is compile-based because this firmware project has no existing host-side unit-test harness for the fingerprint sensor path.
