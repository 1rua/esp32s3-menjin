# Menjin Firmware Simplification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove weather, light-servo, leave-home/arrive-home scene logic, and keep-open mode from the ESP32-S3 firmware while preserving the core door-lock flow for NFC, fingerprint, keypad, MQTT, Web, OTA, and WiFi/NVS.

**Architecture:** Keep the existing single-file Arduino sketch structure in `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino`. Simplify by deleting scene-specific config, state, routes, and callbacks, then converge all remaining successful unlock entry points into one unified `authorizeDoorOpen(...)` path that only sets the standard open duration and calls `openDoor()`. Verification relies on targeted symbol/route searches plus a final sketch compile.

**Tech Stack:** Arduino `.ino` sketch, ESP32-S3 Arduino core, WiFi, PubSubClient MQTT, MFRC522 NFC, Adafruit Fingerprint, Keypad, WebServer, Preferences/NVS, ArduinoOTA, ESP32Servo.

---

## File Structure

- Modify: `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino`
  - Responsibility: entire firmware implementation, including config, global state, Web UI, setup/loop, MQTT callback, NFC/fingerprint/keypad flows, and door servo actions.
- Create: `docs/superpowers/plans/2026-04-16-menjin-firmware-simplification.md`
  - Responsibility: implementation instructions for the simplification.

No code files should be added. No file split should be introduced.

### Task 1: Prune config, globals, and Web keep-open UI

**Files:**

- Modify: `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino:8-223`

- [ ] **Step 1: Write the failing structural checks**

Create a local checklist file or scratch notes with these expected removals from `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino`:

```text
OWM_API_KEY
CITY
OWM_URL
topic_keep
KEEP_OPEN_MIN_MINUTES
KEEP_OPEN_MAX_MINUTES
SERVO_LIGHT1
SERVO_LIGHT2
TOUCH_PIN
lightServo1
lightServo2
acIsOn
sunsetTime
outdoorTemp
lastWeatherUpdate
lastTouchTime
handleKeepOpen
triggerLeaveHome
processArriveHome
physicallySwitchLight
updateWeather
```

- [ ] **Step 2: Run searches to verify the sketch still contains removable symbols before editing**

Run:

```bash
grep -nE "OWM_API_KEY|topic_keep|SERVO_LIGHT1|TOUCH_PIN|handleKeepOpen|triggerLeaveHome|processArriveHome|physicallySwitchLight|updateWeather" esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
```

Expected: multiple matches showing weather, keep-open, light-servo, and leave-home symbols still exist.

- [ ] **Step 3: Replace the config/global declarations with the simplified core-only version**

Update the top-of-file declarations so they match this shape:

```cpp
#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_Fingerprint.h>
#include <FS.h>
#include <SPIFFS.h>
#include <Audio.h>
#include <ESP32Servo.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <Keypad.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ctype.h>

const char* default_ssid     = "深圳湾一号尊享-5G";
const char* default_password = "qzsfb2-210";
const String DOOR_PASSWORD = "11451";

const char* mqtt_server = "mqtt.bemfa.com";
const int   mqtt_port   = 9501;
const char* mqtt_uid    = "YOUR_BEMFA_UID";
const char* topic_door  = "homedoor006";
const char* topic_cmd   = "homecmd006";
const char* OTA_DEFAULT_PASSWORD = "esp32s3-menjin";

#define ANGLE_NEUTRAL   90
const int DOOR_OPEN_US = 3000;
const int DOOR_CLOSE_US = 500;
const uint32_t DOOR_OPEN_DURATION_MS = 4000;
const uint32_t AP_MODE_TIMEOUT_MS = 10UL * 60UL * 1000UL;
const uint8_t MAX_NFC_UID_LENGTH = 10;
const uint8_t MIN_NFC_UID_LENGTH = 4;
const uint8_t KEYPAD_MAX_FAILED_ATTEMPTS = 5;
const uint32_t KEYPAD_LOCKOUT_MS = 30UL * 1000UL;

#define SERVO_DOOR_PIN  9
#define I2S_DOUT        6
#define I2S_BCLK        5
#define I2S_LRC         4
#define NFC_SDA_PIN     10
#define NFC_SCK_PIN     12
#define NFC_MOSI_PIN    11
#define NFC_MISO_PIN    13
#define NFC_RST_PIN     40
#define FP_RX_PIN       18
#define FP_TX_PIN       17

WiFiClient espClient;
PubSubClient client(espClient);
Audio audio;
MFRC522 mfrc522(NFC_SDA_PIN, NFC_RST_PIN);
HardwareSerial mySerial(1);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Servo doorServo;
Preferences prefs;
WebServer server(80);

unsigned long doorOpenTime = 0;
bool isDoorOpen = false;
unsigned long customDoorDuration = DOOR_OPEN_DURATION_MS;
unsigned long mqttDisconnectTime = 0;
unsigned long lastNFCHealthCheck = 0;
bool isOTAUpdating = false;
bool isAPMode = false;
unsigned long apModeStartTime = 0;
uint8_t keypadFailedAttempts = 0;
unsigned long keypadLockoutUntil = 0;
```

- [ ] **Step 4: Shrink the Web HTML to only open / NFC add / WiFi config**

Replace the current `WEB_HTML` body with this exact structure:

```cpp
const char* WEB_HTML = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 Mech Master 控制台</title>
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #121212; color: #ffffff; text-align: center; margin: 0; padding: 20px; }
    h1 { color: #00bcd4; }
    .card { background: #1e1e1e; border-radius: 10px; padding: 20px; margin: 15px auto; max-width: 400px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }
    button { background: #00bcd4; color: #000; border: none; padding: 10px 20px; font-size: 16px; border-radius: 5px; cursor: pointer; margin-top: 10px; font-weight: bold; width: 100%; }
    button:hover { background: #0097a7; }
    .btn-danger { background: #ff4081; }
    .btn-danger:hover { background: #c2185b; }
    input { width: calc(100% - 20px); padding: 10px; margin: 10px 0; border-radius: 5px; border: 1px solid #333; background: #2c2c2c; color: white; }
  </style>
</head>
<body>
  <h1>盾级权限 | 灰风控制中枢</h1>

  <div class="card">
    <h3>🚪 基础门禁</h3>
    <button onclick="fetch('/open').then(() => alert('指令已发送'))">立即开门</button>
  </div>

  <div class="card">
    <h3>💳 NFC 录入管理</h3>
    <input type="text" id="nfcUid" placeholder="输入 Hex UID (8~20位，如: F76D163F 或 046A12AB9C7D80)">
    <button onclick="addNfc()">写入 NVS 白名单</button>
  </div>

  <div class="card">
    <h3>🌐 网络终端配置</h3>
    <input type="text" id="ssid" placeholder="WiFi 名称 (SSID)">
    <input type="password" id="pwd" placeholder="WiFi 密码 (Password)">
    <button class="btn-danger" onclick="setWifi()">重写网络并重启系统</button>
  </div>

  <script>
    function addNfc() {
      let uid = document.getElementById('nfcUid').value.trim();
      if (!uid || uid.length % 2 !== 0) return alert('请输入有效的偶数位十六进制 UID');
      if (!/^[0-9a-fA-F]+$/.test(uid)) return alert('UID 只能包含十六进制字符 0-9/A-F');
      fetch('/add_nfc?uid=' + uid).then(() => alert('UID: ' + uid + ' 已并入核心白名单'));
    }
    function setWifi() {
      let s = document.getElementById('ssid').value;
      let p = document.getElementById('pwd').value;
      if (!s) return alert('必须输入SSID');
      fetch('/set_wifi?ssid=' + encodeURIComponent(s) + '&pass=' + encodeURIComponent(p))
        .then(() => alert('配置已覆写，系统正在重启...'));
    }
  </script>
</body>
</html>
)rawliteral";
```

- [ ] **Step 5: Replace the function declaration block with core-only declarations**

Use this exact declaration block:

```cpp
void playLocalFile(const char *filename);
void safeDelay(unsigned long ms);
void authorizeDoorOpen(const char* source);
void openDoor();
void closeDoor();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
void setupWiFi();
void initNVSAndNFC();
void setupWebServer();
void checkNFC();
void checkKeypad();
int getFingerprintID();
void enterEnrollMode();
uint8_t getFingerprintEnroll(int id);
bool parsePositiveInt(const String& value, uint32_t& outValue);
bool parseUidHex(const String& uidStr, NfcCard& outCard);
bool isDuplicateNfcCard(const NfcCard& card);
void clearNfcWhitelist();
void persistNfcWhitelist();
bool isKeypadLocked();
void resetKeypadLockIfExpired();
void handleRoot();
void handleOpen();
void handleAddNFC();
void handleSetWiFi();
```

- [ ] **Step 6: Delete the keep-open route handler**

Delete `handleKeepOpen()` entirely. `handleOpen()` must remain and should temporarily still compile, even if its implementation is updated in the next task.

- [ ] **Step 7: Run structural searches to verify the top-level cleanup succeeded**

Run:

```bash
grep -nE "OWM_API_KEY|topic_keep|SERVO_LIGHT1|SERVO_LIGHT2|TOUCH_PIN|handleKeepOpen|KEEP_OPEN_MIN_MINUTES|KEEP_OPEN_MAX_MINUTES|lightServo1|lightServo2|acIsOn|sunsetTime|outdoorTemp|lastWeatherUpdate|lastTouchTime" esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
```

Expected: no output.

- [ ] **Step 8: Commit the cleanup of declarations and Web UI**

```bash
git add esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
git commit -m "refactor: remove scene-specific firmware config"
```

### Task 2: Converge all unlock entry points into one authorized door-open path

**Files:**

- Modify: `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino:244-879`

- [ ] **Step 1: Write the failing structural checks for old scene entry points**

The sketch should stop referencing these scene functions after this task:

```text
processArriveHome(
triggerLeaveHome(
physicallySwitchLight(
```

- [ ] **Step 2: Run searches to confirm the old scene entry points still exist before editing**

Run:

```bash
grep -nE "processArriveHome\(|triggerLeaveHome\(|physicallySwitchLight\(" esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
```

Expected: matches in `handleOpen`, `checkNFC`, `mqttCallback`, `checkKeypad`, and the legacy function definitions.

- [ ] **Step 3: Add the new unified access function**

Insert this exact function above `openDoor()`:

```cpp
void authorizeDoorOpen(const char* source) {
  if (isOTAUpdating) return;
  if (isDoorOpen) return;

  Serial.print("Access granted via: ");
  Serial.println(source);

  customDoorDuration = DOOR_OPEN_DURATION_MS;
  playLocalFile("/open.mp3");
  if (client.connected()) client.publish(topic_door, "on");
  openDoor();
}
```

- [ ] **Step 4: Rewrite the remaining successful entry points to use `authorizeDoorOpen(...)`**

Apply these exact replacements:

```cpp
void handleOpen() {
  authorizeDoorOpen("Web-App");
  server.send(200, "text/plain", "Door Opened");
}
```

```cpp
if (match) {
  authorizeDoorOpen("NFC");
} else {
  Serial.println("Unknown Card");
  playLocalFile("/error.mp3");
  safeDelay(1000);
}
```

```cpp
if (String(topic) == topic_door && msg == "on") {
  authorizeDoorOpen("Remote-MQTT");
}
```

```cpp
if (inputCode == DOOR_PASSWORD) {
  Serial.println("Password Correct!");
  keypadFailedAttempts = 0;
  inputCode = "";
  authorizeDoorOpen("Keypad Password");
}
```

```cpp
int fpID = getFingerprintID();
if (fpID != -1) authorizeDoorOpen("Fingerprint");
```

- [ ] **Step 5: Delete the old scene-specific function implementations**

Delete these functions completely:

```cpp
void processArriveHome(String method, unsigned long customDurationMs) { ... }
void triggerLeaveHome() { ... }
void physicallySwitchLight(int id, bool state) { ... }
void updateWeather() { ... }
```

- [ ] **Step 6: Keep `openDoor()` / `closeDoor()` pure door-servo functions**

Make sure the final door functions look like this:

```cpp
void openDoor() {
  doorServo.attach(SERVO_DOOR_PIN, 500, 3000);
  doorServo.writeMicroseconds(DOOR_OPEN_US);
  safeDelay(500);
  doorServo.detach();
  isDoorOpen = true;
  doorOpenTime = millis();
}

void closeDoor() {
  doorServo.attach(SERVO_DOOR_PIN, 500, 3000);
  doorServo.writeMicroseconds(DOOR_CLOSE_US);
  safeDelay(500);
  doorServo.detach();
  isDoorOpen = false;
  if (client.connected()) client.publish(topic_door, "off");
  customDoorDuration = DOOR_OPEN_DURATION_MS;
}
```

- [ ] **Step 7: Run searches to verify all retained entry points now converge to the new function**

Run:

```bash
grep -n "authorizeDoorOpen" esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
grep -nE "processArriveHome\(|triggerLeaveHome\(|physicallySwitchLight\(|updateWeather\(" esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
```

Expected:
- first command: matches for declaration/definition plus calls from Web, NFC, MQTT, keypad, and fingerprint paths
- second command: no output

- [ ] **Step 8: Commit the unified door access refactor**

```bash
git add esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
git commit -m "refactor: unify firmware door access flow"
```

### Task 3: Remove setup/loop/MQTT references to deleted features

**Files:**

- Modify: `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino:321-481`

- [ ] **Step 1: Write the failing structural checks for setup/loop leftovers**

This task removes all remaining references to:

```text
pinMode(TOUCH_PIN, INPUT)
lightServo1.setPeriodHertz(50)
lightServo2.setPeriodHertz(50)
lightServo1.attach(
lightServo2.attach(
updateWeather()
client.subscribe(topic_keep)
digitalRead(TOUCH_PIN)
triggerLeaveHome()
```

- [ ] **Step 2: Run searches to verify those leftovers still exist before editing**

Run:

```bash
grep -nE "pinMode\(TOUCH_PIN|lightServo1|lightServo2|updateWeather\(|topic_keep|digitalRead\(TOUCH_PIN|triggerLeaveHome\(" esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
```

Expected: matches in `setup()`, `loop()`, `mqttCallback()`, and `reconnectMQTT()`.

- [ ] **Step 3: Simplify `setup()` to initialize only the door servo and retained services**

Rewrite the hardware/network part of `setup()` to this shape:

```cpp
void setup() {
  Serial.begin(115200);

  initNVSAndNFC();

  if (!SPIFFS.begin(true)) Serial.println("SPIFFS Fail");
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(15);

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  doorServo.setPeriodHertz(50);

  doorServo.attach(SERVO_DOOR_PIN, 500, 3000);
  doorServo.writeMicroseconds(DOOR_CLOSE_US);
  delay(600);
  doorServo.detach();

  SPI.begin(NFC_SCK_PIN, NFC_MISO_PIN, NFC_MOSI_PIN, NFC_SDA_PIN);
  mfrc522.PCD_Init();
  delay(10);
  mfrc522.PCD_DumpVersionToSerial();

  mySerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(57600);

  if (finger.verifyPassword()) {
    Serial.println("Fingerprint Sensor Found!");
  } else {
    Serial.println("Fingerprint Sensor NOT FOUND :(");
  }

  setupWiFi();
  setupWebServer();

  if (!isAPMode) {
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(mqttCallback);

    Serial.print("Connecting to Bemfa MQTT...");
    if (client.connect(mqtt_uid)) {
      Serial.println("\n[SUCCESS] MQTT Connected!");
      client.subscribe(topic_cmd);
      client.subscribe(topic_door);
      client.publish(topic_door, "online");
      mqttDisconnectTime = 0;
    } else {
      Serial.print(" Failed! rc=");
      Serial.println(client.state());
      mqttDisconnectTime = millis();
    }
  }

  Serial.println(">>> System Ready. Type 'E' to enroll fingerprint. <<<");
  playLocalFile("/boot.mp3");
}
```

- [ ] **Step 4: Simplify `loop()` to only retained runtime behavior**

Rewrite the middle of `loop()` to this exact structure:

```cpp
void loop() {
  if (isOTAUpdating) {
    ArduinoOTA.handle();
    return;
  }

  audio.loop();
  if (!isAPMode) ArduinoOTA.handle();
  server.handleClient();

  if (isAPMode && apModeStartTime > 0 && (millis() - apModeStartTime >= AP_MODE_TIMEOUT_MS)) {
    Serial.println("[AP] Provision timeout reached (10 min), shutting down AP and rebooting.");
    WiFi.softAPdisconnect(true);
    delay(100);
    ESP.restart();
  }

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'E' || c == 'e') {
      enterEnrollMode();
    }
  }

  if (isAPMode) return;

  if (!client.connected()) {
    reconnectMQTT();
  } else {
    client.loop();
    mqttDisconnectTime = 0;
  }

  if (isDoorOpen && (millis() - doorOpenTime > customDoorDuration)) {
    closeDoor();
  }

  if (!isDoorOpen) {
    checkKeypad();

    int fpID = getFingerprintID();
    if (fpID != -1) authorizeDoorOpen("Fingerprint");

    if (millis() - lastNFCHealthCheck > 3000) {
      byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
      if (v == 0x00 || v == 0xFF) {
        Serial.println("[Watchdog] NFC Dead. Resetting...");
        if (audio.isRunning()) audio.stopSong();
        mfrc522.PCD_Init();
        delay(50);
        Serial.println("[Watchdog] Reset Done.");
      }
      lastNFCHealthCheck = millis();
    }

    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      checkNFC();
    }
  }
}
```

- [ ] **Step 5: Remove keep-open from Web and MQTT setup paths**

Make these exact route/subscription changes:

```cpp
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/open", HTTP_GET, handleOpen);
  server.on("/add_nfc", HTTP_GET, handleAddNFC);
  server.on("/set_wifi", HTTP_GET, handleSetWiFi);
  server.begin();
  Serial.println("[WEB] Server Engine Started on port 80.");
}
```

```cpp
if (client.connect(mqtt_uid)) {
  Serial.println("[Reconnected]");
  client.subscribe(topic_cmd);
  client.subscribe(topic_door);
  client.publish(topic_door, "online");
  client.publish(topic_door, isDoorOpen ? "on" : "off");
  mqttDisconnectTime = 0;
}
```

- [ ] **Step 6: Simplify `mqttCallback()` to retained commands only**

Use this final callback body:

```cpp
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (isOTAUpdating) return;

  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  if (String(topic) == topic_door && msg == "on") {
    authorizeDoorOpen("Remote-MQTT");
  }
}
```

- [ ] **Step 7: Run structural searches to verify the runtime cleanup succeeded**

Run:

```bash
grep -nE "pinMode\(TOUCH_PIN|lightServo1|lightServo2|updateWeather\(|topic_keep|digitalRead\(TOUCH_PIN|triggerLeaveHome\(|/keep" esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
```

Expected: no output.

- [ ] **Step 8: Commit the runtime cleanup**

```bash
git add esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
git commit -m "refactor: remove non-core firmware runtime paths"
```

### Task 4: Verify core endpoints, compile integrity, and final cleanup

**Files:**

- Modify: `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino` (only if verification exposes leftovers)

- [ ] **Step 1: Verify the retained Web endpoints still exist**

Run:

```bash
grep -nE 'server\.on\("/(open|add_nfc|set_wifi)"' esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
```

Expected: exactly three route registrations for `/open`, `/add_nfc`, and `/set_wifi`.

- [ ] **Step 2: Verify all five retained entry categories still reach door access**

Run:

```bash
grep -nE 'authorizeDoorOpen\("(Web-App|NFC|Remote-MQTT|Keypad Password|Fingerprint)"\)' esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
```

Expected: exactly five matches, one per retained entry type.

- [ ] **Step 3: Verify the deleted features are fully gone**

Run:

```bash
grep -nE 'OWM_|updateWeather|SERVO_LIGHT|lightServo|triggerLeaveHome|processArriveHome|physicallySwitchLight|topic_keep|/keep|TOUCH_PIN|leave_home|MQTT-KeepOpen|Web-Persistent' esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
```

Expected: no output.

- [ ] **Step 4: Compile the sketch**

Run:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 esp32-s3-menjin-version3.0
```

Expected: `Sketch uses ... bytes` and `Compilation successful`.

If `arduino-cli` is not installed yet, install/configure it first, then rerun the same compile command. Do not skip compilation.

- [ ] **Step 5: Inspect the final diff for scope control**

Run:

```bash
git diff -- esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
```

Expected: only deletions/refactors related to weather, light servo, leave-home/arrive-home, keep-open, and the new unified `authorizeDoorOpen(...)` path.

- [ ] **Step 6: Run the simplify review workflow on the finished code**

Invoke the `simplify` skill and point it at the final firmware diff.

Expected: either no changes needed, or small cleanup fixes around reuse/quality/efficiency.

- [ ] **Step 7: Commit the verified simplification**

```bash
git add esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino
git commit -m "refactor: simplify menjin firmware to core access flow"
```

## Self-Review Checklist

- Spec coverage:
  - weather removal → Task 1, Task 2, Task 3, Task 4
  - light-servo removal → Task 1, Task 2, Task 3, Task 4
  - leave-home/arrive-home removal → Task 2, Task 3, Task 4
  - keep-open removal → Task 1, Task 3, Task 4
  - unified authorized open path → Task 2, Task 4
  - retain `/open`, `/add_nfc`, `/set_wifi` → Task 1, Task 3, Task 4
  - retain NFC / fingerprint / keypad / MQTT / Web unlock → Task 2, Task 4
  - compile integrity → Task 4
- Placeholder scan: no TBD/TODO placeholders remain in the plan.
- Type consistency:
  - unified function name is consistently `authorizeDoorOpen(const char* source)`
  - default open duration is consistently `DOOR_OPEN_DURATION_MS`
  - retained MQTT topic is consistently `topic_door`
