# Menjin Module Decoupling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decouple six firmware modules from `esp32-s3-menjin-version3.0.ino` while preserving existing door-control, local-auth, Web, MQTT, OTA, and provisioning behavior.

**Architecture:** Follow the existing codebase style: each new unit lives in its own folder under `esp32-s3-menjin-version3.0/` and exports a small state struct plus free functions. Phase 1 extracts `runtime_services` and introduces a sketch-local loop helper so the duplicated `loop()` / `safeDelay()` maintenance path is removed immediately; later phases replace each remaining in-sketch subsystem with one module at a time.

**Tech Stack:** Arduino ESP32 sketch, `WiFi`, `WebServer`, `Preferences`, `PubSubClient`, `ArduinoOTA`, `MFRC522`, `Adafruit_Fingerprint`, `ESP32Servo`, `ESP32-audioI2S`, `Keypad`.

---

## File Structure

- `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino`
  - Keep pin constants, hardware object construction, `setup()`, `loop()`, thin Web callback adapters, and final orchestration order.
  - Remove direct implementations of runtime services, audio feedback, door control, fingerprint logic, keypad logic, and NFC logic.

- `esp32-s3-menjin-version3.0/runtime_services/runtime_services.h`
  - Declare runtime state for OTA / WiFi / MQTT / Web / time-sync servicing and the public helper functions used by the sketch.

- `esp32-s3-menjin-version3.0/runtime_services/runtime_services.cpp`
  - Implement the code currently in the sketch for `processForcedProvisioningButton`, `beginWiFiConnectionAttempt`, `maintainWiFiConnection`, `maintainMqttConnection`, `ensureWebServerReady`, `ensureTimeSyncStarted`, `ensureOtaReady`, current-time helpers, and `serviceRuntimeFor(...)`.

- `esp32-s3-menjin-version3.0/audio_feedback/audio_feedback.h`
  - Declare named sound helpers and the audio tick/init helpers.

- `esp32-s3-menjin-version3.0/audio_feedback/audio_feedback.cpp`
  - Implement SPIFFS-backed sound playback for boot/open/error sounds.

- `esp32-s3-menjin-version3.0/door_controller/door_controller.h`
  - Declare `DoorControllerConfig`, `DoorControllerState`, and the servo-driven door APIs.

- `esp32-s3-menjin-version3.0/door_controller/door_controller.cpp`
  - Implement door open/close/pulse/auto-close behavior currently in the sketch.

- `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.h`
  - Declare `FingerprintEnrollPhase`, `FingerprintAccessState`, and the enrollment / match polling APIs.

- `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.cpp`
  - Implement fingerprint setup, serial-triggered enrollment, enrollment state machine, and match lookup.

- `esp32-s3-menjin-version3.0/keypad_access/keypad_access.h`
  - Declare `KeypadAccessState`, `KeypadAccessResult`, and keypad polling helpers.

- `esp32-s3-menjin-version3.0/keypad_access/keypad_access.cpp`
  - Implement keypad input buffering, timeout, lockout, and PIN verification bridging to `access_control`.

- `esp32-s3-menjin-version3.0/nfc_access/nfc_access.h`
  - Declare `NfcCard`, `NfcAccessState`, `NfcPollResult`, and whitelist persistence / polling helpers.

- `esp32-s3-menjin-version3.0/nfc_access/nfc_access.cpp`
  - Implement NVS whitelist loading/migration, UID parsing, duplicate detection, reader health checks, and card polling.

- `README.md`
  - Update the project-tree snippet and module descriptions after all refactors land.

---

## Common Verification Command

Use the existing saved board configuration from `esp32-s3-menjin-version3.0/build/esp32.esp32.esp32s3/build.options.json`:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=dfu,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default esp32-s3-menjin-version3.0
```

Expected success signal: the compile finishes with `Sketch uses ...` and no compiler errors.

---

### Task 1: Extract `runtime_services` without changing behavior

**Files:**
- Create: `esp32-s3-menjin-version3.0/runtime_services/runtime_services.h`
- Create: `esp32-s3-menjin-version3.0/runtime_services/runtime_services.cpp`
- Modify: `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino:109-122,174-371,422-439,452-467,823-844`
- Test: sketch compile via `arduino-cli compile`

- [ ] **Step 1: Write the failing integration-first usage in the sketch**

Add the new include and state declaration before the module exists:

```cpp
#include "runtime_services/runtime_services.h"

RuntimeServicesState runtimeServices;
```

Add a sketch-local accessor that the later runtime module will use:

```cpp
static void handleRuntimeOtaStartSideEffect() {
  audio.stopSong();
  doorServo.detach();
}

RuntimeServicesContext& getRuntimeServicesContext() {
  static RuntimeServicesContext context = {
    prefs,
    deviceConfig,
    provisioningState,
    accessControl,
    client,
    server,
    getWebPortalContext(),
    handleRuntimeOtaStartSideEffect,
  };
  return context;
}
```

- [ ] **Step 2: Run compile to verify it fails before the module exists**

Run:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=dfu,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default esp32-s3-menjin-version3.0
```

Expected: FAIL with `runtime_services/runtime_services.h: No such file or directory`.

- [ ] **Step 3: Create the runtime header with the exact state that currently lives in globals**

Create `esp32-s3-menjin-version3.0/runtime_services/runtime_services.h`:

```cpp
#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WebServer.h>

#include "../access_control/access_control.h"
#include "../device_config/device_config.h"
#include "../provisioning/provisioning.h"
#include "../web_portal/web_portal.h"

typedef void (*RuntimeOtaStartSideEffect)();
typedef void (*RuntimeMqttMessageCallback)(char* topic, byte* payload, unsigned int length);

struct RuntimeServicesState {
  bool isOtaUpdating = false;
  bool otaReady = false;
  bool wifiConnectionAttemptActive = false;
  bool webServerReady = false;
  bool timeSyncStarted = false;
  unsigned long mqttDisconnectTime = 0;
  unsigned long wifiConnectStartedAt = 0;
  unsigned long lastWiFiReconnectAttempt = 0;
  unsigned long lastMqttReconnectAttempt = 0;
};

struct RuntimeServicesContext {
  Preferences& prefs;
  DeviceConfig& deviceConfig;
  ProvisioningState& provisioningState;
  AccessControlState& accessControl;
  PubSubClient& mqttClient;
  WebServer& server;
  WebPortalContext& webPortalContext;
  RuntimeOtaStartSideEffect onOtaStartSideEffect;
};

bool shouldServiceWebServer(const RuntimeServicesContext& context);
bool shouldAttemptWiFiConnection(const RuntimeServicesContext& context);
void syncLegacyApStateFromProvisioningPortal(RuntimeServicesState& state, const ProvisioningState& provisioningState);
void beginWiFiConnectionAttempt(RuntimeServicesState& state, const RuntimeServicesContext& context);
void processForcedProvisioningButton(RuntimeServicesState& state, RuntimeServicesContext& context, uint8_t bootButtonPin, uint32_t holdMs, const char* apSsid, const IPAddress& apIp, const IPAddress& apGateway, const IPAddress& apSubnet);
void ensureWebServerReady(RuntimeServicesState& state, RuntimeServicesContext& context);
void maintainWiFiConnection(RuntimeServicesState& state, RuntimeServicesContext& context);
void maintainMqttConnection(RuntimeServicesState& state, RuntimeServicesContext& context, const char* mqttServer, int mqttPort, const char* topicDoor, RuntimeMqttMessageCallback mqttCallback);
void ensureTimeSyncStarted(RuntimeServicesState& state);
bool runtimeIsTimeSynced();
uint32_t runtimeCurrentEpochSeconds();
String runtimeCurrentLocalTimeString();
void ensureOtaReady(RuntimeServicesState& state, RuntimeServicesContext& context, const char* otaHostname, const char* otaPassword);
```

- [ ] **Step 4: Create the runtime implementation by moving the existing bodies out of the sketch**

Create `esp32-s3-menjin-version3.0/runtime_services/runtime_services.cpp`.

Move these sketch functions verbatim, changing only global reads/writes so they go through `RuntimeServicesState& state` or `RuntimeServicesContext& context`:

- `syncLegacyApStateFromProvisioningPortal` from sketch lines `174-180`
- `shouldServiceWebServer` from sketch lines `186-188`
- `shouldAttemptWiFiConnection` from sketch lines `194-196`
- `ensureTimeSyncStarted` from sketch lines `198-205`
- `isSystemTimeSynced` from sketch lines `207-209` and rename to `runtimeIsTimeSynced`
- `currentEpochSeconds` from sketch lines `211-217` and rename to `runtimeCurrentEpochSeconds`
- `currentLocalTimeString` from sketch lines `219-235` and rename to `runtimeCurrentLocalTimeString`
- `beginWiFiConnectionAttempt` from sketch lines `237-249`
- `processForcedProvisioningButton` from sketch lines `251-269`
- `ensureOtaReady` from sketch lines `271-282`
- `maintainWiFiConnection` from sketch lines `284-316`
- `maintainMqttConnection` from sketch lines `318-343`
- `ensureWebServerReady` from sketch lines `345-354`

Keep these exact behavior rules while moving them:

```cpp
ArduinoOTA.onStart([&state, &context]() {
  state.isOtaUpdating = true;
  if (context.onOtaStartSideEffect != nullptr) {
    context.onOtaStartSideEffect();
  }
});

ArduinoOTA.onEnd([&state]() {
  state.isOtaUpdating = false;
  ESP.restart();
});
```

Use `context.webPortalContext` when calling `setupWebRoutes(...)`.

- [ ] **Step 5: Rewire the sketch to use the runtime module**

In `esp32-s3-menjin-version3.0.ino`, remove these globals because they are now owned by `RuntimeServicesState`:

```cpp
unsigned long mqttDisconnectTime = 0;
bool isOTAUpdating = false;
bool otaReady = false;
bool wifiConnectionAttemptActive = false;
bool webServerReady = false;
bool timeSyncStarted = false;
unsigned long wifiConnectStartedAt = 0;
unsigned long lastWiFiReconnectAttempt = 0;
unsigned long lastMqttReconnectAttempt = 0;
```

Replace the setup and loop call sites with the runtime equivalents:

```cpp
if (provisioningState.startupState == StartupState::AP_PORTAL) {
  startProvisioningPortal(provisioningState, AP_SSID, AP_IP, AP_GATEWAY, AP_SUBNET);
  syncLegacyApStateFromProvisioningPortal(runtimeServices, provisioningState);
} else if (shouldAttemptWiFiConnection(getRuntimeServicesContext())) {
  beginWiFiConnectionAttempt(runtimeServices, getRuntimeServicesContext());
} else {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("[CFG] Startup decision: skip auto provisioning, staying in local runtime.");
}

ensureWebServerReady(runtimeServices, getRuntimeServicesContext());

if (WiFi.status() == WL_CONNECTED) {
  ensureTimeSyncStarted(runtimeServices);
  ensureOtaReady(runtimeServices, getRuntimeServicesContext(), "Mech-Master-S3", OTA_DEFAULT_PASSWORD);
  maintainMqttConnection(runtimeServices, getRuntimeServicesContext(), mqtt_server, mqtt_port, topic_door, mqttCallback);
}
```

- [ ] **Step 6: Run compile to verify the new module builds**

Run the common compile command.

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add esp32-s3-menjin-version3.0/runtime_services/runtime_services.h \
        esp32-s3-menjin-version3.0/runtime_services/runtime_services.cpp \
        esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino && \
git commit -m "refactor(firmware): extract runtime services"
```

### Task 2: Remove the duplicated `loop()` / `safeDelay()` maintenance path immediately

**Files:**
- Modify: `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino:445-508,823-844`
- Test: sketch compile via `arduino-cli compile`

- [ ] **Step 1: Write the failing refactor call sites before the helper exists**

Replace the duplicated bodies in `loop()` and `safeDelay()` with a single helper call:

```cpp
static void serviceCoreLoopSlice();

void loop() {
  if (runtimeServices.isOtaUpdating) {
    ArduinoOTA.handle();
    return;
  }

  serviceCoreLoopSlice();
  // keep the rest of the loop unchanged for now
}

void safeDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    if (runtimeServices.isOtaUpdating) {
      ArduinoOTA.handle();
      return;
    }
    serviceCoreLoopSlice();
  }
}
```

- [ ] **Step 2: Run compile to verify it fails before the helper is defined**

Run the common compile command.

Expected: FAIL with `serviceCoreLoopSlice was not declared`.

- [ ] **Step 3: Add the deduplication helper in the sketch**

Insert this exact helper above `setup()`:

```cpp
static void serviceCoreLoopSlice() {
  processForcedProvisioningButton(runtimeServices, getRuntimeServicesContext(), BOOT_BUTTON_PIN, FORCE_PROVISION_HOLD_MS, AP_SSID, AP_IP, AP_GATEWAY, AP_SUBNET);
  audio.loop();
  maintainDoorServoPulse();
  maintainFingerprintEnroll();
  ensureWebServerReady(runtimeServices, getRuntimeServicesContext());
  if (shouldServiceWebServer(getRuntimeServicesContext())) {
    server.handleClient();
  }
  maintainWiFiConnection(runtimeServices, getRuntimeServicesContext());
  maintainMqttConnection(runtimeServices, getRuntimeServicesContext(), mqtt_server, mqtt_port, topic_door, mqttCallback);
  if (WiFi.status() == WL_CONNECTED) {
    ensureTimeSyncStarted(runtimeServices);
    if (runtimeIsTimeSynced()) {
      pruneExpiredTemporaryPins(prefs, accessControl, runtimeCurrentEpochSeconds(), true);
    }
    ensureOtaReady(runtimeServices, getRuntimeServicesContext(), "Mech-Master-S3", OTA_DEFAULT_PASSWORD);
    ArduinoOTA.handle();
  }
}
```

This helper is intentionally temporary. Later tasks will replace `audio.loop()`, `maintainDoorServoPulse()`, and `maintainFingerprintEnroll()` with module calls.

- [ ] **Step 4: Run compile to verify the duplicate path is gone without behavior changes**

Run the common compile command.

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino && \
git commit -m "refactor(firmware): dedupe loop and safeDelay servicing"
```

### Task 3: Extract `audio_feedback`

**Files:**
- Create: `esp32-s3-menjin-version3.0/audio_feedback/audio_feedback.h`
- Create: `esp32-s3-menjin-version3.0/audio_feedback/audio_feedback.cpp`
- Modify: `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino:143,395-396,441-442,453,791,817,923-926`
- Test: sketch compile via `arduino-cli compile`

- [ ] **Step 1: Write the failing sketch usage first**

Add the include and replace direct sound calls:

```cpp
#include "audio_feedback/audio_feedback.h"
```

Replace these three call sites:

```cpp
initAudioFeedback(audio, I2S_BCLK, I2S_LRC, I2S_DOUT, 15);
playBootSound(audio);
playOpenSound(audio);
playErrorSound(audio);
tickAudioFeedback(audio);
```

- [ ] **Step 2: Run compile to verify it fails before the module exists**

Run the common compile command.

Expected: FAIL with `audio_feedback/audio_feedback.h: No such file or directory`.

- [ ] **Step 3: Create the audio module**

Create `esp32-s3-menjin-version3.0/audio_feedback/audio_feedback.h`:

```cpp
#pragma once

#include <Audio.h>
#include <SPIFFS.h>

void initAudioFeedback(Audio& audio, int bclkPin, int lrcPin, int doutPin, uint8_t volume);
void tickAudioFeedback(Audio& audio);
void playAudioFile(Audio& audio, const char* path);
void playBootSound(Audio& audio);
void playOpenSound(Audio& audio);
void playErrorSound(Audio& audio);
```

Create `esp32-s3-menjin-version3.0/audio_feedback/audio_feedback.cpp`:

```cpp
#include "audio_feedback.h"

namespace {
const char* kBootSound = "/boot.mp3";
const char* kOpenSound = "/open.mp3";
const char* kErrorSound = "/error.mp3";
}

void initAudioFeedback(Audio& audio, int bclkPin, int lrcPin, int doutPin, uint8_t volume) {
  audio.setPinout(bclkPin, lrcPin, doutPin);
  audio.setVolume(volume);
}

void tickAudioFeedback(Audio& audio) {
  audio.loop();
}

void playAudioFile(Audio& audio, const char* path) {
  if (SPIFFS.exists(path)) {
    audio.connecttoFS(SPIFFS, path);
  }
}

void playBootSound(Audio& audio) { playAudioFile(audio, kBootSound); }
void playOpenSound(Audio& audio) { playAudioFile(audio, kOpenSound); }
void playErrorSound(Audio& audio) { playAudioFile(audio, kErrorSound); }
```

- [ ] **Step 4: Remove the in-sketch audio helpers and wire the module in**

Delete the sketch-local `playLocalFile(...)` and `audio_eof_mp3(...)` functions.

Replace the old direct setup and loop code with:

```cpp
if (!SPIFFS.begin(true)) Serial.println("SPIFFS Fail");
initAudioFeedback(audio, I2S_BCLK, I2S_LRC, I2S_DOUT, 15);
```

```cpp
Serial.println(">>> System Ready. Type 'E' to enroll fingerprint. <<<");
playBootSound(audio);
```

```cpp
static void serviceCoreLoopSlice() {
  processForcedProvisioningButton(runtimeServices, getRuntimeServicesContext(), BOOT_BUTTON_PIN, FORCE_PROVISION_HOLD_MS, AP_SSID, AP_IP, AP_GATEWAY, AP_SUBNET);
  tickAudioFeedback(audio);
  maintainDoorServoPulse();
  maintainFingerprintEnroll();
  // keep the rest unchanged in this task
}
```

Replace the existing error/open calls with `playErrorSound(audio)` and `playOpenSound(audio)`.

- [ ] **Step 5: Run compile to verify the audio module builds**

Run the common compile command.

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add esp32-s3-menjin-version3.0/audio_feedback/audio_feedback.h \
        esp32-s3-menjin-version3.0/audio_feedback/audio_feedback.cpp \
        esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino && \
git commit -m "refactor(firmware): extract audio feedback"
```

### Task 4: Extract `door_controller`

**Files:**
- Create: `esp32-s3-menjin-version3.0/door_controller/door_controller.h`
- Create: `esp32-s3-menjin-version3.0/door_controller/door_controller.cpp`
- Modify: `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino:110-112,125-126,144-145,168-169,404-406,454,480-482,809-821,897-912`
- Test: sketch compile via `arduino-cli compile`

- [ ] **Step 1: Write the failing sketch usage first**

Add the new include and replace the existing door state globals with one config and one state object:

```cpp
#include "door_controller/door_controller.h"

const DoorControllerConfig kDoorControllerConfig = {
  SERVO_DOOR_PIN,
  DOOR_OPEN_US,
  DOOR_CLOSE_US,
  DOOR_SERVO_PULSE_MS,
  DOOR_OPEN_DURATION_MS,
  STARTUP_SERVO_PULSE_MS,
};

DoorControllerState doorState;
```

- [ ] **Step 2: Run compile to verify it fails before the module exists**

Run the common compile command.

Expected: FAIL with `door_controller/door_controller.h: No such file or directory`.

- [ ] **Step 3: Create the door-controller module**

Create `esp32-s3-menjin-version3.0/door_controller/door_controller.h`:

```cpp
#pragma once

#include <Arduino.h>
#include <ESP32Servo.h>

struct DoorControllerConfig {
  uint8_t servoPin;
  int openUs;
  int closeUs;
  uint32_t pulseMs;
  uint32_t defaultOpenDurationMs;
  uint32_t startupPulseMs;
};

struct DoorControllerState {
  bool isOpen = false;
  unsigned long openedAt = 0;
  uint32_t activeOpenDurationMs = 0;
  bool pulseActive = false;
  unsigned long detachAt = 0;
};

void initializeDoorController(DoorControllerState& state, Servo& servo, const DoorControllerConfig& config);
void requestDoorOpen(DoorControllerState& state, Servo& servo, const DoorControllerConfig& config, unsigned long nowMs);
void closeDoorNow(DoorControllerState& state, Servo& servo, const DoorControllerConfig& config);
void tickDoorController(DoorControllerState& state, Servo& servo, const DoorControllerConfig& config, unsigned long nowMs);
```

Create `esp32-s3-menjin-version3.0/door_controller/door_controller.cpp` by moving the current bodies from sketch lines `620-635` and `897-912` into these functions. Keep the behavior identical: attach the servo when driving it, write the configured microseconds, schedule detach using `pulseMs`, and auto-close when `nowMs - openedAt > activeOpenDurationMs`.

- [ ] **Step 4: Rewire the sketch to the door module**

In `setup()` replace the current startup servo code with:

```cpp
ESP32PWM::allocateTimer(0);
ESP32PWM::allocateTimer(1);
ESP32PWM::allocateTimer(2);
ESP32PWM::allocateTimer(3);
doorServo.setPeriodHertz(50);
initializeDoorController(doorState, doorServo, kDoorControllerConfig);
```

Keep `authorizeDoorOpen(...)` in the sketch, but change it to:

```cpp
customDoorDuration = DOOR_OPEN_DURATION_MS;
playOpenSound(audio);
requestDoorOpen(doorState, doorServo, kDoorControllerConfig, millis());
if (client.connected()) client.publish(topic_door, "on");
```

Replace the old loop checks with:

```cpp
tickDoorController(doorState, doorServo, kDoorControllerConfig, millis());
```

and

```cpp
if (!shouldRunLocalAccess() || doorState.isOpen || nfcErrorFeedbackUntil != 0 ||
    fingerprintEnrollPhase != FingerprintEnrollPhase::Idle) {
  return;
}
```

- [ ] **Step 5: Run compile to verify the door module builds**

Run the common compile command.

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add esp32-s3-menjin-version3.0/door_controller/door_controller.h \
        esp32-s3-menjin-version3.0/door_controller/door_controller.cpp \
        esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino && \
git commit -m "refactor(firmware): extract door controller"
```

### Task 5: Extract `fingerprint_access`

**Files:**
- Create: `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.h`
- Create: `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.cpp`
- Modify: `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino:51-58,128-131,151,170-172,413-420,455,473-478,484-485,491-492,637-763,914-920`
- Test: sketch compile via `arduino-cli compile`

- [ ] **Step 1: Write the failing sketch usage first**

Add the include and replace the enrollment globals with a dedicated state object:

```cpp
#include "fingerprint_access/fingerprint_access.h"

FingerprintAccessState fingerprintState;
```

Replace the direct enrollment/match call sites with:

```cpp
initializeFingerprintAccess(mySerial, finger, FP_RX_PIN, FP_TX_PIN);
handleFingerprintConsoleInput(fingerprintState, finger, audio);
tickFingerprintAccess(fingerprintState, finger, audio, isProvisioningPortalActive(provisioningState), millis());
int fpID = pollFingerprintMatch(fingerprintState, finger, runtimeServices.isOtaUpdating);
```

- [ ] **Step 2: Run compile to verify it fails before the module exists**

Run the common compile command.

Expected: FAIL with `fingerprint_access/fingerprint_access.h: No such file or directory`.

- [ ] **Step 3: Create the fingerprint module**

Create `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.h`:

```cpp
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
```

Create `esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.cpp` by moving these sketch bodies exactly:

- enrollment reset/start/tick logic from sketch lines `637-763`
- match lookup from sketch lines `914-920`
- setup serial initialization from sketch lines `413-420`

Keep the same messages, same `1..127` ID range, same remove-delay timing, and the same `playBootSound(audio)` success sound.

- [ ] **Step 4: Rewire the sketch to the fingerprint module**

Replace the current serial trigger block with:

```cpp
handleFingerprintConsoleInput(fingerprintState, finger, audio);
```

Replace the busy checks with:

```cpp
if (!shouldRunLocalAccess() || doorState.isOpen || nfcErrorFeedbackUntil != 0 ||
    fingerprintAccessBusy(fingerprintState)) {
  return;
}
```

Replace the old `maintainFingerprintEnroll()` call in `serviceCoreLoopSlice()` with:

```cpp
tickFingerprintAccess(fingerprintState, finger, audio, isProvisioningPortalActive(provisioningState), millis());
```

- [ ] **Step 5: Run compile to verify the fingerprint module builds**

Run the common compile command.

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.h \
        esp32-s3-menjin-version3.0/fingerprint_access/fingerprint_access.cpp \
        esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino && \
git commit -m "refactor(firmware): extract fingerprint access"
```

### Task 6: Extract `keypad_access`

**Files:**
- Create: `esp32-s3-menjin-version3.0/keypad_access/keypad_access.h`
- Create: `esp32-s3-menjin-version3.0/keypad_access/keypad_access.cpp`
- Modify: `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino:92-93,123-124,150,156-157,489,846-895,965-975`
- Test: sketch compile via `arduino-cli compile`

- [ ] **Step 1: Write the failing sketch usage first**

Add the include and replace the sketch keypad state with a dedicated module state object:

```cpp
#include "keypad_access/keypad_access.h"

KeypadAccessState keypadState;
```

Replace the direct `checkKeypad();` call with:

```cpp
KeypadAccessResult keypadResult = pollKeypadAccess(keypadState, keypad, prefs, accessControl, millis(), runtimeCurrentEpochSeconds(), runtimeIsTimeSynced());
```

- [ ] **Step 2: Run compile to verify it fails before the module exists**

Run the common compile command.

Expected: FAIL with `keypad_access/keypad_access.h: No such file or directory`.

- [ ] **Step 3: Create the keypad module**

Create `esp32-s3-menjin-version3.0/keypad_access/keypad_access.h`:

```cpp
#pragma once

#include <Arduino.h>
#include <Keypad.h>
#include <Preferences.h>

#include "../access_control/access_control.h"

struct KeypadAccessState {
  String inputCode;
  unsigned long lastKeyTime = 0;
  uint8_t failedAttempts = 0;
  unsigned long lockoutUntil = 0;
};

struct KeypadAccessResult {
  bool authorized = false;
  bool rejected = false;
  String matchedSource;
};

KeypadAccessResult pollKeypadAccess(KeypadAccessState& state, Keypad& keypad, Preferences& prefs, AccessControlState& accessControl, unsigned long nowMs, uint32_t nowEpoch, bool timeSynced);
bool keypadAccessLocked(const KeypadAccessState& state, unsigned long nowMs);
```

Create `esp32-s3-menjin-version3.0/keypad_access/keypad_access.cpp` by moving the current keypad logic from sketch lines `846-895` and `965-975`. Keep the exact timeout of `10000`, the exact lockout threshold of `5`, and the exact lockout duration of `30` seconds.

- [ ] **Step 4: Rewire the sketch to handle keypad results at the top level**

Use this exact sketch glue:

```cpp
KeypadAccessResult keypadResult = pollKeypadAccess(keypadState, keypad, prefs, accessControl, millis(), runtimeCurrentEpochSeconds(), runtimeIsTimeSynced());
if (keypadResult.authorized) {
  authorizeDoorOpen(keypadResult.matchedSource == "Temporary PIN" ? "Temporary PIN" : "Keypad Password");
} else if (keypadResult.rejected) {
  playErrorSound(audio);
}
```

Delete the old sketch-local keypad helpers and state globals.

- [ ] **Step 5: Run compile to verify the keypad module builds**

Run the common compile command.

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add esp32-s3-menjin-version3.0/keypad_access/keypad_access.h \
        esp32-s3-menjin-version3.0/keypad_access/keypad_access.cpp \
        esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino && \
git commit -m "refactor(firmware): extract keypad access"
```

### Task 7: Extract `nfc_access`

**Files:**
- Create: `esp32-s3-menjin-version3.0/nfc_access/nfc_access.h`
- Create: `esp32-s3-menjin-version3.0/nfc_access/nfc_access.cpp`
- Modify: `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino:41-42,48,114,127,133-140,148-159,385,494-507,511-607,765-796,928-963`
- Test: sketch compile via `arduino-cli compile`

- [ ] **Step 1: Write the failing sketch usage first**

Add the include and replace the sketch NFC globals with a single module state object:

```cpp
#include "nfc_access/nfc_access.h"

NfcAccessState nfcState;
```

Replace the in-sketch whitelist load, web add-card adapter, health check, and card poll sites with:

```cpp
initializeNfcAccess(prefs, nfcState);
int addStatus = addNfcCardFromHex(prefs, nfcState, uid, message);
maintainNfcReaderHealth(nfcState, mfrc522, millis(), handleRuntimeOtaStartSideEffect);
NfcPollResult nfcResult = pollNfcAccess(nfcState, mfrc522, runtimeServices.isOtaUpdating, millis());
```

- [ ] **Step 2: Run compile to verify it fails before the module exists**

Run the common compile command.

Expected: FAIL with `nfc_access/nfc_access.h: No such file or directory`.

- [ ] **Step 3: Create the NFC module**

Create `esp32-s3-menjin-version3.0/nfc_access/nfc_access.h`:

```cpp
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
```

Create `esp32-s3-menjin-version3.0/nfc_access/nfc_access.cpp` by moving these bodies exactly from the sketch:

- NVS whitelist init/migration from lines `511-581`
- web add-card path from lines `583-607`
- card-present polling from lines `765-796`
- UID parsing and persistence helpers from lines `928-963`

Keep the existing legacy-format migration, max whitelist size, min/max UID lengths, and the exact `NFC_ERROR_COOLDOWN_MS` behavior.

- [ ] **Step 4: Rewire the sketch and keep side effects at the top level**

Replace the old health check and read loop with this exact orchestration pattern:

```cpp
clearExpiredNfcErrorFeedback(nfcState, millis());

maintainNfcReaderHealth(nfcState, mfrc522, millis(), handleRuntimeOtaStartSideEffect);

if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
  NfcPollResult nfcResult = pollNfcAccess(nfcState, mfrc522, runtimeServices.isOtaUpdating, millis());
  if (nfcResult.authorized) {
    authorizeDoorOpen("NFC");
  } else if (nfcResult.rejected) {
    playErrorSound(audio);
  }
}
```

Keep the Web adapter in the sketch as a thin wrapper:

```cpp
int addNfcCardFromWeb(const String& uid, String& message) {
  return addNfcCardFromHex(prefs, nfcState, uid, message);
}
```

- [ ] **Step 5: Run compile to verify the NFC module builds**

Run the common compile command.

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add esp32-s3-menjin-version3.0/nfc_access/nfc_access.h \
        esp32-s3-menjin-version3.0/nfc_access/nfc_access.cpp \
        esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino && \
git commit -m "refactor(firmware): extract nfc access"
```

### Task 8: Final cleanup, top-level loop order, and docs

**Files:**
- Modify: `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino`
- Modify: `README.md:82-109`
- Test: sketch compile via `arduino-cli compile`

- [ ] **Step 1: Remove dead declarations and keep the sketch as the composer only**

Delete the old forward declarations that have moved into modules:

```cpp
void playLocalFile(const char *filename);
void openDoor();
void closeDoor();
void initNVSAndNFC();
void checkNFC();
void checkKeypad();
int getFingerprintID();
bool parseUidHex(const String& uidStr, NfcCard& outCard);
bool isDuplicateNfcCard(const NfcCard& card);
void clearNfcWhitelist();
void persistNfcWhitelist();
bool isKeypadLocked();
void resetKeypadLockIfExpired();
void scheduleDoorServoDetach(uint32_t pulseMs);
void maintainDoorServoPulse();
void startFingerprintEnrollMode();
void maintainFingerprintEnroll();
void resetFingerprintEnrollState();
```

- [ ] **Step 2: Flatten `loop()` into the final orchestration order**

Rewrite the top-level `loop()` into this shape:

```cpp
void loop() {
  if (runtimeServices.isOtaUpdating) {
    ArduinoOTA.handle();
    return;
  }

  serviceCoreLoopSlice();

  KeypadAccessResult keypadResult = pollKeypadAccess(keypadState, keypad, prefs, accessControl, millis(), runtimeCurrentEpochSeconds(), runtimeIsTimeSynced());
  if (keypadResult.authorized) {
    authorizeDoorOpen(keypadResult.matchedSource == "Temporary PIN" ? "Temporary PIN" : "Keypad Password");
  } else if (keypadResult.rejected) {
    playErrorSound(audio);
  }

  int fpID = pollFingerprintMatch(fingerprintState, finger, runtimeServices.isOtaUpdating);
  if (fpID != -1) authorizeDoorOpen("Fingerprint");

  maintainNfcReaderHealth(nfcState, mfrc522, millis(), handleRuntimeOtaStartSideEffect);
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    NfcPollResult nfcResult = pollNfcAccess(nfcState, mfrc522, runtimeServices.isOtaUpdating, millis());
    if (nfcResult.authorized) authorizeDoorOpen("NFC");
    else if (nfcResult.rejected) playErrorSound(audio);
  }
}
```

Keep `serviceCoreLoopSlice()` as the one place where runtime ticking, audio ticking, door ticking, and fingerprint enrollment ticking happen.

- [ ] **Step 3: Update the README tree so it matches the refactored firmware layout**

Replace the stale project tree in `README.md` with:

```text
esp32-s3-menjin-version3.0/
├─ esp32-s3-menjin-version3.0.ino
├─ access_control/
│  ├─ access_control.h
│  └─ access_control.cpp
├─ device_config/
│  ├─ device_config.h
│  └─ device_config.cpp
├─ provisioning/
│  ├─ provisioning.h
│  └─ provisioning.cpp
├─ web_portal/
│  ├─ web_portal.h
│  └─ web_portal.cpp
├─ runtime_services/
│  ├─ runtime_services.h
│  └─ runtime_services.cpp
├─ audio_feedback/
│  ├─ audio_feedback.h
│  └─ audio_feedback.cpp
├─ door_controller/
│  ├─ door_controller.h
│  └─ door_controller.cpp
├─ fingerprint_access/
│  ├─ fingerprint_access.h
│  └─ fingerprint_access.cpp
├─ keypad_access/
│  ├─ keypad_access.h
│  └─ keypad_access.cpp
├─ nfc_access/
│  ├─ nfc_access.h
│  └─ nfc_access.cpp
└─ partitions.csv
```

- [ ] **Step 4: Run the full compile check**

Run the common compile command.

Expected: PASS.

- [ ] **Step 5: Run the manual smoke checklist on hardware**

Verify these exact behaviors after flashing the build:

1. Boot the device and confirm the boot sound still plays.
2. Confirm the Web portal still loads and can open the door.
3. Enter a valid long-term PIN and confirm the door opens.
4. Enter five invalid PINs and confirm the keypad locks for 30 seconds.
5. Trigger fingerprint enrollment with `E`, complete enrollment, and confirm the stored finger matches.
6. Add an NFC UID from the Web portal and confirm the same card opens the door.
7. Long-press BOOT for 5 seconds and confirm the provisioning portal starts.
8. Connect WiFi, then confirm MQTT reconnects and OTA stays available.

- [ ] **Step 6: Commit**

```bash
git add esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino \
        README.md && \
git commit -m "refactor(firmware): finish modular door controller split"
```

---

## Spec Coverage Check

- `runtime_services`: covered by Tasks 1 and 2.
- `audio_feedback`: covered by Task 3.
- `door_controller`: covered by Task 4.
- `fingerprint_access`: covered by Task 5.
- `keypad_access`: covered by Task 6.
- `nfc_access`: covered by Task 7.
- Main sketch reduced to composition and top-level ordering: covered by Task 8.
- `safeDelay()` duplication removed in Phase 1: covered by Task 2.
- README structure update after the refactor: covered by Task 8.

## Self-Review Notes

- No `TODO`, `TBD`, or placeholder task text remains.
- All task code snippets use the same file names, module names, and public function names.
- The plan keeps the existing C-style module pattern already used by `device_config`, `provisioning`, `access_control`, and `web_portal`.
- The plan avoids adding extra framework layers beyond one temporary sketch helper, `serviceCoreLoopSlice()`, which is introduced only to remove the current duplicated `loop()` / `safeDelay()` path immediately.
