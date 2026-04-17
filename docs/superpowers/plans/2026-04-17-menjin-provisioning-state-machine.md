# Menjin Provisioning State Machine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor the ESP32-S3 door firmware so AP provisioning, Web control, and config storage are split into modules, and Wi‑Fi / MQTT failures no longer block local unlocking.

**Architecture:** Keep the Arduino sketch as the runtime coordinator, but move persistent network config into `device_config.*`, startup/AP lifecycle into `provisioning.*`, and HTTP pages/routes into `web_portal.*`. The sketch continues to own hardware setup, local auth flows, OTA/MQTT maintenance, and small adapter callbacks that bridge existing NFC / door functions into the new modules.

**Tech Stack:** Arduino ESP32 (`WiFi`, `WebServer`, `Preferences`, `ArduinoOTA`), PubSubClient, existing single-sketch firmware layout with added `.h/.cpp` files.

---

## File Structure

- `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino`
  - Keep hardware init, door/NFC/fingerprint/keypad runtime, OTA/MQTT maintenance, and thin adapter callbacks.
  - Remove direct AP portal HTML, direct network-config writes, direct `isAPMode` gating, and old `setupWiFi()` / `setupWebServer()` responsibilities.

- `esp32-s3-menjin-version3.0/device_config.h`
  - Declare `DeviceConfig` and config persistence helpers.

- `esp32-s3-menjin-version3.0/device_config.cpp`
  - Implement NVS load/save logic for `wifi_ssid`, `wifi_pass`, `wifi_configured`, `skip_auto_prov`, and `mqtt_uid`.

- `esp32-s3-menjin-version3.0/provisioning.h`
  - Declare `StartupState`, `ProvisioningState`, BOOT/AP decision helpers, and AP start/stop helpers.

- `esp32-s3-menjin-version3.0/provisioning.cpp`
  - Implement startup decision logic, BOOT long-press detection, and SoftAP lifecycle.

- `esp32-s3-menjin-version3.0/web_portal.h`
  - Declare `WebPortalContext` and `setupWebRoutes(...)`.

- `esp32-s3-menjin-version3.0/web_portal.cpp`
  - Implement route handlers for `/`, `/open`, `/add_nfc`, `/configure_network`, and `/skip_provision`.

- `.claude/compile-menjin/compile-menjin.ino`
  - Generated verification sketch name only; never commit it. Use it only because the current `.ino` filename does not match the sketch folder name.

---

### Task 1: Extract persistent network config into `device_config.*`

**Files:**
- Create: `esp32-s3-menjin-version3.0/device_config.h`
- Create: `esp32-s3-menjin-version3.0/device_config.cpp`
- Modify: `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino:10-38,85-105,266-322`
- Test: `.claude/compile-menjin/compile-menjin.ino` (generated during verification)

- [ ] **Step 1: Write the failing integration-first test in the sketch**

```cpp
#include "device_config.h"

DeviceConfig deviceConfig;

void setup() {
  Serial.begin(115200);
  initNVSAndNFC();
  loadDeviceConfig(prefs, deviceConfig);

  Serial.println(hasValidWiFiConfig(deviceConfig)
                   ? "[CFG] WiFi config loaded"
                   : "[CFG] WiFi config missing");
  Serial.println(isMqttConfigured(deviceConfig)
                   ? "[CFG] MQTT enabled"
                   : "[CFG] MQTT disabled");
}
```

- [ ] **Step 2: Run compile to verify it fails before the module exists**

Run:

```bash
mkdir -p .claude/compile-menjin && \
cp esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino .claude/compile-menjin/compile-menjin.ino && \
arduino-cli compile --fqbn esp32:esp32:esp32s3 .claude/compile-menjin
```

Expected: FAIL with `device_config.h: No such file or directory`.

- [ ] **Step 3: Write the minimal config module**

`esp32-s3-menjin-version3.0/device_config.h`

```cpp
#pragma once

#include <Arduino.h>
#include <Preferences.h>

struct DeviceConfig {
  String wifiSsid;
  String wifiPassword;
  bool wifiConfigured = false;
  bool skipAutoProvision = false;
  String mqttUid;
};

void loadDeviceConfig(Preferences& prefs, DeviceConfig& config);
bool hasValidWiFiConfig(const DeviceConfig& config);
bool isMqttConfigured(const DeviceConfig& config);
void saveWiFiConfig(Preferences& prefs, DeviceConfig& config, const String& ssid, const String& password);
void saveMqttUid(Preferences& prefs, DeviceConfig& config, const String& mqttUid);
void setSkipAutoProvision(Preferences& prefs, DeviceConfig& config, bool skipAutoProvision);
```

`esp32-s3-menjin-version3.0/device_config.cpp`

```cpp
#include "device_config.h"

namespace {
constexpr const char* KEY_WIFI_SSID = "wifi_ssid";
constexpr const char* KEY_WIFI_PASS = "wifi_pass";
constexpr const char* KEY_WIFI_CONFIGURED = "wifi_configured";
constexpr const char* KEY_SKIP_AUTO_PROV = "skip_auto_prov";
constexpr const char* KEY_MQTT_UID = "mqtt_uid";
}

void loadDeviceConfig(Preferences& prefs, DeviceConfig& config) {
  config.wifiSsid = prefs.getString(KEY_WIFI_SSID, "");
  config.wifiPassword = prefs.getString(KEY_WIFI_PASS, "");
  config.wifiConfigured = prefs.getBool(KEY_WIFI_CONFIGURED, false);
  config.skipAutoProvision = prefs.getBool(KEY_SKIP_AUTO_PROV, false);
  config.mqttUid = prefs.getString(KEY_MQTT_UID, "");
}

bool hasValidWiFiConfig(const DeviceConfig& config) {
  return config.wifiConfigured && config.wifiSsid.length() > 0;
}

bool isMqttConfigured(const DeviceConfig& config) {
  return config.mqttUid.length() > 0;
}

void saveWiFiConfig(Preferences& prefs, DeviceConfig& config, const String& ssid, const String& password) {
  prefs.putString(KEY_WIFI_SSID, ssid);
  prefs.putString(KEY_WIFI_PASS, password);
  prefs.putBool(KEY_WIFI_CONFIGURED, true);
  prefs.putBool(KEY_SKIP_AUTO_PROV, false);

  config.wifiSsid = ssid;
  config.wifiPassword = password;
  config.wifiConfigured = true;
  config.skipAutoProvision = false;
}

void saveMqttUid(Preferences& prefs, DeviceConfig& config, const String& mqttUid) {
  prefs.putString(KEY_MQTT_UID, mqttUid);
  config.mqttUid = mqttUid;
}

void setSkipAutoProvision(Preferences& prefs, DeviceConfig& config, bool skipAutoProvision) {
  prefs.putBool(KEY_SKIP_AUTO_PROV, skipAutoProvision);
  config.skipAutoProvision = skipAutoProvision;
}
```

- [ ] **Step 4: Run compile to verify the new module builds**

Run:

```bash
mkdir -p .claude/compile-menjin && \
cp esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino .claude/compile-menjin/compile-menjin.ino && \
cp esp32-s3-menjin-version3.0/device_config.h esp32-s3-menjin-version3.0/device_config.cpp .claude/compile-menjin/ && \
arduino-cli compile --fqbn esp32:esp32:esp32s3 .claude/compile-menjin
```

Expected: PASS, or fail only on the next still-missing module.

- [ ] **Step 5: Commit**

```bash
git add esp32-s3-menjin-version3.0/device_config.h \
        esp32-s3-menjin-version3.0/device_config.cpp \
        esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino && \
git commit -m "refactor(firmware): extract persistent network config"
```

### Task 2: Add explicit startup / provisioning state machine in `provisioning.*`

**Files:**
- Create: `esp32-s3-menjin-version3.0/provisioning.h`
- Create: `esp32-s3-menjin-version3.0/provisioning.cpp`
- Modify: `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino:40-49,96-105,266-322,462-503`
- Test: `.claude/compile-menjin/compile-menjin.ino` (generated during verification)

- [ ] **Step 1: Write the failing integration-first state-machine usage in the sketch**

```cpp
#include "provisioning.h"

const uint8_t BOOT_BUTTON_PIN = 0;
const uint32_t FORCE_PROVISION_HOLD_MS = 5000;
const char* AP_SSID = "esp32s3-menjin";
const IPAddress AP_IP(192, 168, 10, 10);
const IPAddress AP_GATEWAY(192, 168, 10, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

ProvisioningState provisioningState;

void setup() {
  Serial.begin(115200);
  initNVSAndNFC();
  loadDeviceConfig(prefs, deviceConfig);

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  provisioningState.bootForcedProvision =
      detectForcedProvisioningRequest(BOOT_BUTTON_PIN, FORCE_PROVISION_HOLD_MS);

  applyStartupDecision(
      provisioningState,
      decideStartupState(deviceConfig, provisioningState.bootForcedProvision));

  if (provisioningState.startupState == StartupState::AP_PORTAL) {
    startProvisioningPortal(provisioningState, AP_SSID, AP_IP, AP_GATEWAY, AP_SUBNET);
  }
}
```

- [ ] **Step 2: Run compile to verify it fails before the module exists**

Run:

```bash
mkdir -p .claude/compile-menjin && \
cp esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino .claude/compile-menjin/compile-menjin.ino && \
cp esp32-s3-menjin-version3.0/device_config.h esp32-s3-menjin-version3.0/device_config.cpp .claude/compile-menjin/ && \
arduino-cli compile --fqbn esp32:esp32:esp32s3 .claude/compile-menjin
```

Expected: FAIL with `provisioning.h: No such file or directory` or `ProvisioningState does not name a type`.

- [ ] **Step 3: Write the minimal provisioning module**

`esp32-s3-menjin-version3.0/provisioning.h`

```cpp
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include "device_config.h"

enum class StartupState {
  BOOT_CHECK,
  PROVISION_DECISION,
  AP_PORTAL,
  CONNECTING_WIFI,
  NORMAL_RUNTIME,
};

struct ProvisioningState {
  StartupState startupState = StartupState::BOOT_CHECK;
  bool portalActive = false;
  bool bootForcedProvision = false;
};

bool detectForcedProvisioningRequest(uint8_t bootPin, unsigned long holdMs);
StartupState decideStartupState(const DeviceConfig& config, bool bootForcedProvision);
void applyStartupDecision(ProvisioningState& state, StartupState nextState);
void startProvisioningPortal(
    ProvisioningState& state,
    const char* apSsid,
    const IPAddress& apIp,
    const IPAddress& gateway,
    const IPAddress& subnet);
void stopProvisioningPortal(ProvisioningState& state);
void handleProvisioningSaved(ProvisioningState& state);
void handleProvisioningSkipped(ProvisioningState& state);
bool isProvisioningPortalActive(const ProvisioningState& state);
bool shouldAttemptWiFi(const ProvisioningState& state, const DeviceConfig& config);
```

`esp32-s3-menjin-version3.0/provisioning.cpp`

```cpp
#include "provisioning.h"

bool detectForcedProvisioningRequest(uint8_t bootPin, unsigned long holdMs) {
  if (digitalRead(bootPin) != LOW) {
    return false;
  }

  const unsigned long pressedAt = millis();
  while (millis() - pressedAt < holdMs) {
    if (digitalRead(bootPin) != LOW) {
      return false;
    }
    delay(10);
  }
  return true;
}

StartupState decideStartupState(const DeviceConfig& config, bool bootForcedProvision) {
  if (bootForcedProvision) {
    return StartupState::AP_PORTAL;
  }
  if (!hasValidWiFiConfig(config) && !config.skipAutoProvision) {
    return StartupState::AP_PORTAL;
  }
  if (hasValidWiFiConfig(config)) {
    return StartupState::CONNECTING_WIFI;
  }
  return StartupState::NORMAL_RUNTIME;
}

void applyStartupDecision(ProvisioningState& state, StartupState nextState) {
  state.startupState = nextState;
  state.portalActive = nextState == StartupState::AP_PORTAL;
}

void startProvisioningPortal(
    ProvisioningState& state,
    const char* apSsid,
    const IPAddress& apIp,
    const IPAddress& gateway,
    const IPAddress& subnet) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIp, gateway, subnet);
  WiFi.softAP(apSsid);
  state.portalActive = true;
  state.startupState = StartupState::AP_PORTAL;
}

void stopProvisioningPortal(ProvisioningState& state) {
  WiFi.softAPdisconnect(true);
  state.portalActive = false;
}

void handleProvisioningSaved(ProvisioningState& state) {
  state.portalActive = false;
  state.startupState = StartupState::CONNECTING_WIFI;
}

void handleProvisioningSkipped(ProvisioningState& state) {
  state.portalActive = false;
  state.startupState = StartupState::NORMAL_RUNTIME;
}

bool isProvisioningPortalActive(const ProvisioningState& state) {
  return state.portalActive;
}

bool shouldAttemptWiFi(const ProvisioningState& state, const DeviceConfig& config) {
  return !state.portalActive && hasValidWiFiConfig(config);
}
```

- [ ] **Step 4: Run compile to verify the state machine builds**

Run:

```bash
mkdir -p .claude/compile-menjin && \
cp esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino .claude/compile-menjin/compile-menjin.ino && \
cp esp32-s3-menjin-version3.0/device_config.h esp32-s3-menjin-version3.0/device_config.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/provisioning.h esp32-s3-menjin-version3.0/provisioning.cpp .claude/compile-menjin/ && \
arduino-cli compile --fqbn esp32:esp32:esp32s3 .claude/compile-menjin
```

Expected: PASS, or fail only on the next still-missing module.

- [ ] **Step 5: Commit**

```bash
git add esp32-s3-menjin-version3.0/provisioning.h \
        esp32-s3-menjin-version3.0/provisioning.cpp \
        esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino && \
git commit -m "refactor(firmware): add provisioning startup state machine"
```

### Task 3: Move HTML and routes into `web_portal.*`

**Files:**
- Create: `esp32-s3-menjin-version3.0/web_portal.h`
- Create: `esp32-s3-menjin-version3.0/web_portal.cpp`
- Modify: `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino:117-263,505-510`
- Test: `.claude/compile-menjin/compile-menjin.ino` (generated during verification)

- [ ] **Step 1: Write the failing integration-first route hookup in the sketch**

Add these declarations near the existing global objects, right after `WebServer server(80);`:

```cpp
DeviceConfig deviceConfig;
ProvisioningState provisioningState;
WebPortalContext webPortalContext{server, prefs, deviceConfig, provisioningState,
                                  handleWebDoorOpenRequest, handleWebAddNfcRequest};
```

Add these adapter functions near the other forward declarations / small handlers:

```cpp
void handleWebDoorOpenRequest() {
  authorizeDoorOpen("Web-App");
}

bool handleWebAddNfcRequest(const String& uid, String& message) {
  NfcCard newCard = {};
  if (!parseUidHex(uid, newCard)) {
    message = "Invalid UID format";
    return false;
  }
  if (isDuplicateNfcCard(newCard)) {
    message = "UID already exists";
    return false;
  }
  if (whitelistCount >= MAX_NFC_CARDS) {
    message = "Whitelist Full!";
    return false;
  }

  nfcWhitelist[whitelistCount++] = newCard;
  persistNfcWhitelist();
  message = "NFC Added to NVS";
  return true;
}
```

Then replace the old `setupWebServer();` call in `setup()` with:

```cpp
setupWebRoutes(webPortalContext);
server.begin();
```

- [ ] **Step 2: Run compile to verify it fails before the module exists**

Run:

```bash
mkdir -p .claude/compile-menjin && \
cp esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino .claude/compile-menjin/compile-menjin.ino && \
cp esp32-s3-menjin-version3.0/device_config.h esp32-s3-menjin-version3.0/device_config.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/provisioning.h esp32-s3-menjin-version3.0/provisioning.cpp .claude/compile-menjin/ && \
arduino-cli compile --fqbn esp32:esp32:esp32s3 .claude/compile-menjin
```

Expected: FAIL with `web_portal.h: No such file or directory`.

- [ ] **Step 3: Write the minimal web portal module**

`esp32-s3-menjin-version3.0/web_portal.h`

```cpp
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include "device_config.h"
#include "provisioning.h"

typedef void (*OpenDoorHandler)();
typedef bool (*AddNfcHandler)(const String& uid, String& message);

struct WebPortalContext {
  WebServer& server;
  Preferences& prefs;
  DeviceConfig& deviceConfig;
  ProvisioningState& provisioningState;
  OpenDoorHandler openDoorHandler;
  AddNfcHandler addNfcHandler;
};

void setupWebRoutes(WebPortalContext& context);
```

`esp32-s3-menjin-version3.0/web_portal.cpp`

```cpp
#include "web_portal.h"

namespace {
WebPortalContext* gContext = nullptr;

const char* CONTROL_PAGE = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>ESP32 门禁控制</title></head>
<body>
  <h1>门禁控制页</h1>
  <button onclick="fetch('/open').then(() => alert('指令已发送'))">立即开门</button>
  <input type="text" id="nfcUid" placeholder="输入 Hex UID">
  <button onclick="addNfc()">写入 NVS 白名单</button>
  <script>
    function addNfc() {
      const uid = document.getElementById('nfcUid').value.trim();
      fetch('/add_nfc?uid=' + encodeURIComponent(uid)).then(r => r.text()).then(alert);
    }
  </script>
</body>
</html>
)rawliteral";

const char* PROVISION_PAGE = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>ESP32 门禁配网</title></head>
<body>
  <h1>门禁配网</h1>
  <form method="POST" action="/configure_network">
    <input name="ssid" placeholder="WiFi SSID">
    <input name="pass" type="password" placeholder="WiFi 密码">
    <input name="mqtt_uid" placeholder="巴法云 UID（可空）">
    <button type="submit">保存并连接</button>
  </form>
  <form method="POST" action="/skip_provision">
    <button type="submit">跳过配网</button>
  </form>
</body>
</html>
)rawliteral";

void handleRoot() {
  gContext->server.send(
      200,
      "text/html",
      isProvisioningPortalActive(gContext->provisioningState) ? PROVISION_PAGE : CONTROL_PAGE);
}

void handleOpen() {
  gContext->openDoorHandler();
  gContext->server.send(200, "text/plain", "Door Opened");
}

void handleAddNfc() {
  if (!gContext->server.hasArg("uid")) {
    gContext->server.send(400, "text/plain", "Missing UID");
    return;
  }

  String message;
  if (!gContext->addNfcHandler(gContext->server.arg("uid"), message)) {
    gContext->server.send(400, "text/plain", message);
    return;
  }

  gContext->server.send(200, "text/plain", message);
}

void handleConfigureNetwork() {
  if (!gContext->server.hasArg("ssid")) {
    gContext->server.send(400, "text/plain", "Missing ssid");
    return;
  }

  String ssid = gContext->server.arg("ssid");
  String pass = gContext->server.arg("pass");
  String mqttUid = gContext->server.arg("mqtt_uid");
  ssid.trim();
  mqttUid.trim();

  if (ssid.length() == 0) {
    gContext->server.send(400, "text/plain", "SSID cannot be empty");
    return;
  }

  saveWiFiConfig(gContext->prefs, gContext->deviceConfig, ssid, pass);
  if (mqttUid.length() > 0) {
    saveMqttUid(gContext->prefs, gContext->deviceConfig, mqttUid);
  }
  handleProvisioningSaved(gContext->provisioningState);
  stopProvisioningPortal(gContext->provisioningState);
  gContext->server.send(200, "text/plain", "Credentials saved. Connecting...");
}

void handleSkipProvision() {
  setSkipAutoProvision(gContext->prefs, gContext->deviceConfig, true);
  handleProvisioningSkipped(gContext->provisioningState);
  stopProvisioningPortal(gContext->provisioningState);
  gContext->server.send(200, "text/plain", "Provisioning skipped");
}
}  // namespace

void setupWebRoutes(WebPortalContext& context) {
  gContext = &context;
  context.server.on("/", HTTP_GET, handleRoot);
  context.server.on("/open", HTTP_GET, handleOpen);
  context.server.on("/add_nfc", HTTP_GET, handleAddNfc);
  context.server.on("/configure_network", HTTP_POST, handleConfigureNetwork);
  context.server.on("/skip_provision", HTTP_POST, handleSkipProvision);
}
```

- [ ] **Step 4: Run compile to verify the route module builds**

Run:

```bash
mkdir -p .claude/compile-menjin && \
cp esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino .claude/compile-menjin/compile-menjin.ino && \
cp esp32-s3-menjin-version3.0/device_config.h esp32-s3-menjin-version3.0/device_config.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/provisioning.h esp32-s3-menjin-version3.0/provisioning.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/web_portal.h esp32-s3-menjin-version3.0/web_portal.cpp .claude/compile-menjin/ && \
arduino-cli compile --fqbn esp32:esp32:esp32s3 .claude/compile-menjin
```

Expected: PASS, or fail only on remaining sketch integration errors.

- [ ] **Step 5: Commit**

```bash
git add esp32-s3-menjin-version3.0/web_portal.h \
        esp32-s3-menjin-version3.0/web_portal.cpp \
        esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino && \
git commit -m "refactor(firmware): move web provisioning routes into module"
```

### Task 4: Rewire the main sketch so local access keeps running without `isAPMode`

**Files:**
- Modify: `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino:29-49,85-105,176-201,246-263,266-384,462-689`
- Test: `.claude/compile-menjin/compile-menjin.ino` (generated during verification)

- [ ] **Step 1: Write the failing runtime-integration calls before the helpers exist**

```cpp
bool otaReady = false;
bool wifiConnectionAttemptActive = false;
unsigned long wifiConnectStartedAt = 0;
unsigned long lastWiFiReconnectAttempt = 0;
unsigned long lastMqttReconnectAttempt = 0;

void beginWiFiConnectionAttempt();
void maintainWiFiConnection();
void maintainMqttConnection();
void ensureOtaReady();

void setup() {
  // ...existing hardware init...
  loadDeviceConfig(prefs, deviceConfig);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  provisioningState.bootForcedProvision =
      detectForcedProvisioningRequest(BOOT_BUTTON_PIN, FORCE_PROVISION_HOLD_MS);
  applyStartupDecision(
      provisioningState,
      decideStartupState(deviceConfig, provisioningState.bootForcedProvision));

  if (provisioningState.startupState == StartupState::AP_PORTAL) {
    startProvisioningPortal(provisioningState, AP_SSID, AP_IP, AP_GATEWAY, AP_SUBNET);
  }

  WebPortalContext webPortalContext{server, prefs, deviceConfig, provisioningState,
                                    handleWebDoorOpenRequest, handleWebAddNfcRequest};
  setupWebRoutes(webPortalContext);
  server.begin();

  if (shouldAttemptWiFi(provisioningState, deviceConfig)) {
    beginWiFiConnectionAttempt();
  }
}

void loop() {
  audio.loop();
  server.handleClient();
  maintainWiFiConnection();
  maintainMqttConnection();
  if (WiFi.status() == WL_CONNECTED) {
    ensureOtaReady();
    ArduinoOTA.handle();
  }

  checkKeypad();
  int fpID = getFingerprintID();
  if (fpID != -1) authorizeDoorOpen("Fingerprint");
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) checkNFC();
}
```

- [ ] **Step 2: Run compile to verify it fails before the helper rewiring is complete**

Run:

```bash
mkdir -p .claude/compile-menjin && \
cp esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino .claude/compile-menjin/compile-menjin.ino && \
cp esp32-s3-menjin-version3.0/device_config.h esp32-s3-menjin-version3.0/device_config.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/provisioning.h esp32-s3-menjin-version3.0/provisioning.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/web_portal.h esp32-s3-menjin-version3.0/web_portal.cpp .claude/compile-menjin/ && \
arduino-cli compile --fqbn esp32:esp32:esp32s3 .claude/compile-menjin
```

Expected: FAIL with missing helper/function definitions or stale `isAPMode` references.

- [ ] **Step 3: Write the minimal sketch integration and delete stale AP-only flow**

Replace the old direct Wi‑Fi/AP logic with these helpers inside `menjin_esp32s3___official_version3.0.ino`:

```cpp
void beginWiFiConnectionAttempt() {
  if (!hasValidWiFiConfig(deviceConfig)) {
    return;
  }

  Serial.printf("[WIFI] Connecting to %s\n", deviceConfig.wifiSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(deviceConfig.wifiSsid.c_str(), deviceConfig.wifiPassword.c_str());
  wifiConnectionAttemptActive = true;
  wifiConnectStartedAt = millis();
  provisioningState.startupState = StartupState::CONNECTING_WIFI;
}

void ensureOtaReady() {
  if (otaReady || WiFi.status() != WL_CONNECTED) {
    return;
  }

  ArduinoOTA.setHostname("Mech-Master-S3");
  ArduinoOTA.setPassword(OTA_DEFAULT_PASSWORD);
  ArduinoOTA.onStart([]() {
    isOTAUpdating = true;
    audio.stopSong();
    doorServo.detach();
  });
  ArduinoOTA.onEnd([]() {
    isOTAUpdating = false;
    ESP.restart();
  });
  ArduinoOTA.begin();
  otaReady = true;
}

void maintainWiFiConnection() {
  if (!shouldAttemptWiFi(provisioningState, deviceConfig)) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectionAttemptActive = false;
    provisioningState.startupState = StartupState::NORMAL_RUNTIME;
    return;
  }

  if (wifiConnectionAttemptActive && millis() - wifiConnectStartedAt < 10000UL) {
    return;
  }

  if (millis() - lastWiFiReconnectAttempt < 30000UL) {
    return;
  }

  lastWiFiReconnectAttempt = millis();
  WiFi.disconnect();
  beginWiFiConnectionAttempt();
}

void maintainMqttConnection() {
  if (WiFi.status() != WL_CONNECTED || !isMqttConfigured(deviceConfig)) {
    return;
  }

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  if (client.connected()) {
    client.loop();
    return;
  }

  if (millis() - lastMqttReconnectAttempt < 5000UL) {
    return;
  }

  lastMqttReconnectAttempt = millis();
  if (client.connect(deviceConfig.mqttUid.c_str())) {
    client.subscribe(topic_door);
    client.publish(topic_door, "online");
    client.publish(topic_door, isDoorOpen ? "on" : "off");
  }
}
```

Then make these deletions/replacements in the sketch:

```cpp
// Delete these old declarations and implementations entirely:
// - bool isAPMode
// - unsigned long apModeStartTime
// - void setupWiFi()
// - void setupWebServer()
// - void handleRoot()
// - void handleSetWiFi()
// - AP timeout reboot logic in loop()

// Replace AP-dependent guards like these:
if (isOTAUpdating || isAPMode) return;
if (isAPMode) return;
if (!isAPMode) ArduinoOTA.handle();
if (!isAPMode && WiFi.status() == WL_CONNECTED && client.connected()) client.loop();

// With OTA-only or connectivity-only guards:
if (isOTAUpdating) return;
if (WiFi.status() == WL_CONNECTED) ArduinoOTA.handle();
if (WiFi.status() == WL_CONNECTED && client.connected()) client.loop();
```

Also replace hardcoded MQTT ID usage:

```cpp
// old
const char* mqtt_uid = "YOUR_BEMFA_UID";
if (client.connect(mqtt_uid)) { ... }

// new
if (client.connect(deviceConfig.mqttUid.c_str())) { ... }
```

- [ ] **Step 4: Run compile to verify the full refactor builds**

Run:

```bash
mkdir -p .claude/compile-menjin && \
cp esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino .claude/compile-menjin/compile-menjin.ino && \
cp esp32-s3-menjin-version3.0/device_config.h esp32-s3-menjin-version3.0/device_config.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/provisioning.h esp32-s3-menjin-version3.0/provisioning.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/web_portal.h esp32-s3-menjin-version3.0/web_portal.cpp .claude/compile-menjin/ && \
arduino-cli compile --fqbn esp32:esp32:esp32s3 .claude/compile-menjin
```

Expected: PASS with normal Arduino size output.

- [ ] **Step 5: Run the on-device verification matrix**

1. Fresh NVS / no Wi‑Fi config:
   - Flash the firmware.
   - Boot normally.
   - Expected: board creates `esp32s3-menjin`, `GET /` shows provisioning page.

2. Skip provisioning:
   - While on the AP page, click `跳过配网`.
   - Reboot.
   - Expected: no automatic AP on the next boot, keypad / fingerprint / NFC still work.

3. Save Wi‑Fi without MQTT UID:
   - On the AP page, submit SSID + password and leave UID blank.
   - Expected: old `mqtt_uid` is preserved if it existed; otherwise MQTT stays disabled.

4. Existing Wi‑Fi config, router unavailable:
   - Boot with saved Wi‑Fi credentials but the router turned off.
   - Expected: firmware does **not** reopen AP automatically, door unlock flows still work locally.

5. Existing Wi‑Fi config, MQTT unavailable:
   - Restore Wi‑Fi, block MQTT or set a bad UID.
   - Expected: Web/local door access still works, firmware retries MQTT in the background only.

6. Forced provisioning:
   - Hold BOOT for 5 seconds during power-up.
   - Expected: provisioning AP appears even if `skip_auto_prov == true`.

- [ ] **Step 6: Commit**

```bash
git add esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino \
        esp32-s3-menjin-version3.0/device_config.h \
        esp32-s3-menjin-version3.0/device_config.cpp \
        esp32-s3-menjin-version3.0/provisioning.h \
        esp32-s3-menjin-version3.0/provisioning.cpp \
        esp32-s3-menjin-version3.0/web_portal.h \
        esp32-s3-menjin-version3.0/web_portal.cpp && \
git commit -m "refactor(firmware): decouple provisioning flow from access runtime"
```

---

## Notes for the implementer

- Use `BOOT_BUTTON_PIN = 0` for the ESP32-S3 DevKitC BOOT key unless the actual board wiring differs.
- Do not add a reboot after `/configure_network` or `/skip_provision`; the spec explicitly wants AP exit and transition into runtime, not AP timeout/reboot semantics.
- Keep `authorizeDoorOpen(...)`, `openDoor()`, `closeDoor()`, NFC parsing, and fingerprint enrollment behavior unchanged except for removing AP-mode blocking.
- Do not keep `/set_wifi` unless you discover a real external dependency on it during implementation; the spec prefers `/configure_network` and `/skip_provision`.
- Do not reintroduce a single boolean with mixed semantics; `ProvisioningState.startupState` is the source of truth.
