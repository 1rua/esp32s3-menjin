/**
 * ESP32-S3 Mech Master (Core Access Version)
 * * Base: Version 2.03 Final
 * * Retained: Web UI, AP Provisioning, Dynamic NFC (NVS)
 * * Removed: Weather, scene logic, persistent door open, light-servo control
 * * Servo Logic: Reverted to V2.02 Safe Guard (Attach -> Write -> Delay -> Detach)
 * * Author: Grey Goo & Fourth Crisis
 */

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

// ================= 🌐 默认/回退配置区 (Fallback Config) =================

const char* default_ssid     = "深圳湾一号尊享-5G";
const char* default_password = "qzsfb2-210";
const String DOOR_PASSWORD = "11451";

// ☁️ MQTT (巴法云 Bemfa)
const char* mqtt_server = "mqtt.bemfa.com";
const int   mqtt_port   = 9501;
const char* mqtt_uid    = "YOUR_BEMFA_UID";
const char* topic_door  = "homedoor006";
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

// ================= 🤖 硬件引脚 (Hardware Pins) =================
#define SERVO_DOOR_PIN  9

// I2S 音频 (I2S Audio)
#define I2S_DOUT        6
#define I2S_BCLK        5
#define I2S_LRC         4

// NFC
#define NFC_SDA_PIN     10
#define NFC_SCK_PIN     12
#define NFC_MOSI_PIN    11
#define NFC_MISO_PIN    13
#define NFC_RST_PIN     40

// 指纹 (Fingerprint)
#define FP_RX_PIN       18
#define FP_TX_PIN       17

// 矩阵键盘配置 (Matrix Keypad Config)
const byte ROWS = 4; // 四行 (4 rows)
const byte COLS = 4; // 四列 (4 cols)
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {8, 15, 16, 21};
byte colPins[COLS] = {1, 2, 3, 7};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);
String inputCode = "";
unsigned long lastKeyTime = 0;

// ================= 固件全局对象 (Global Objects) =================
WiFiClient espClient;
PubSubClient client(espClient);
Audio audio;
MFRC522 mfrc522(NFC_SDA_PIN, NFC_RST_PIN);
HardwareSerial mySerial(1);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Servo doorServo;
Preferences prefs;
WebServer server(80);

// 状态变量 (State Variables)
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

// 动态 NFC 白名单系统 (Dynamic NFC Whitelist System)
#define MAX_NFC_CARDS 30
struct NfcCard {
  uint8_t uid[MAX_NFC_UID_LENGTH];
  uint8_t size;
};
NfcCard nfcWhitelist[MAX_NFC_CARDS];
int whitelistCount = 0;

// ================= Web 界面 HTML 模板 (Web UI HTML Template - Contains Chinese UI) =================
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

// ================= 函数声明 (Function Declarations) =================
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

// ================= Web 服务器路由处理 (Web Server Route Handlers) =================

void handleRoot() {
  // 返回主控页面 (Return main control page)
  server.send(200, "text/html", WEB_HTML);
}

void handleOpen() {
  authorizeDoorOpen("Web-App");
  server.send(200, "text/plain", "Door Opened");
}

void handleAddNFC() {
  // Web添加NFC白名单 (Web add NFC whitelist)
  if (!server.hasArg("uid")) {
    server.send(400, "text/plain", "Missing UID"); // 缺少UID参数
    return;
  }

  String uidStr = server.arg("uid");
  uidStr.trim();
  NfcCard newCard = {};
  if (!parseUidHex(uidStr, newCard)) {
    server.send(400, "text/plain", "Invalid UID format");
    return;
  }

  if (isDuplicateNfcCard(newCard)) {
    server.send(409, "text/plain", "UID already exists");
    return;
  }

  if (whitelistCount >= MAX_NFC_CARDS) {
    server.send(507, "text/plain", "Whitelist Full!");
    return;
  }

  nfcWhitelist[whitelistCount++] = newCard;
  persistNfcWhitelist();
  Serial.printf("[NVS] Added New NFC (len=%d)\n", newCard.size);
  server.send(200, "text/plain", "NFC Added to NVS");
}

void handleSetWiFi() {
  // Web配网并重启 (Web configure WiFi and restart)
  if (server.hasArg("ssid")) {
    String newSsid = server.arg("ssid");
    String newPass = server.hasArg("pass") ? server.arg("pass") : "";

    prefs.putString("wifi_ssid", newSsid);
    prefs.putString("wifi_pass", newPass);

    Serial.println("[WIFI] New credentials saved. Rebooting..."); // 保存成功准备重启
    server.send(200, "text/plain", "Credentials Saved. Rebooting...");

    delay(1000);
    ESP.restart(); // 重启系统以应用新网络配置
    return;
  }
  server.send(400, "text/plain", "Missing ssid"); // 缺少SSID参数
}

// ================= 初始化 (Setup) =================
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

// ================= 主循环 (Main Loop) =================
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

// ================= 核心子系统实现 (Core Subsystem Implementations) =================

void initNVSAndNFC() {
  prefs.begin("mech_master", false); // 打开 mech_master 命名空间，可读写 (Open namespace, read/write)

  whitelistCount = prefs.getInt("nfc_cnt", -1);

  // 如果是首次运行，将硬编码数组迁入 NVS (Migrate hardcoded array to NVS on first run)
  if (whitelistCount == -1) {
    Serial.println("[NVS] First boot detected. Migrating hardcoded NFC list..."); // 检测到首次启动，迁移默认白名单
    const byte defaultWhitelist[][4] = {
      {0xF7, 0x6D, 0x16, 0x3F},
      {0xE5, 0x6B, 0x1A, 0x06},
      {0x1D, 0x8E, 0x39, 0x68},
      {0xAD, 0xE9, 0x31, 0x55},
      {0x01, 0x62, 0xAD, 0x1C}
    };
    const int defCount = sizeof(defaultWhitelist) / sizeof(defaultWhitelist[0]);
    whitelistCount = 0;
    for (int i = 0; i < defCount && whitelistCount < MAX_NFC_CARDS; i++) {
      NfcCard card = {};
      card.size = 4;
      memcpy(card.uid, defaultWhitelist[i], 4);
      if (!isDuplicateNfcCard(card)) {
        nfcWhitelist[whitelistCount++] = card;
      }
    }
    persistNfcWhitelist();
  } else {
    // 读取已有的白名单 (Read existing whitelist)
    if (whitelistCount < 0 || whitelistCount > MAX_NFC_CARDS) {
      Serial.printf("[NVS] Invalid nfc_cnt=%d, resetting whitelist.\n", whitelistCount);
      clearNfcWhitelist();
      persistNfcWhitelist();
      return;
    }

    const size_t bytesLen = prefs.getBytesLength("nfc_list");
    const size_t expectedStructLen = whitelistCount * sizeof(NfcCard);
    const size_t expectedLegacyLen = whitelistCount * sizeof(uint32_t);

    if (bytesLen == expectedStructLen) {
      prefs.getBytes("nfc_list", nfcWhitelist, expectedStructLen);
      Serial.printf("[NVS] Loaded %d NFC cards from storage.\n", whitelistCount);
      return;
    }

    if (bytesLen == expectedLegacyLen) {
      Serial.println("[NVS] Legacy NFC format detected. Migrating...");
      uint32_t legacyList[MAX_NFC_CARDS] = {0};
      const int legacyCount = whitelistCount;
      prefs.getBytes("nfc_list", legacyList, expectedLegacyLen);
      clearNfcWhitelist();
      for (int i = 0; i < MAX_NFC_CARDS && i < legacyCount; i++) {
        NfcCard card = {};
        card.size = 4;
        card.uid[0] = (legacyList[i] >> 24) & 0xFF;
        card.uid[1] = (legacyList[i] >> 16) & 0xFF;
        card.uid[2] = (legacyList[i] >> 8) & 0xFF;
        card.uid[3] = legacyList[i] & 0xFF;
        if (!isDuplicateNfcCard(card) && whitelistCount < MAX_NFC_CARDS) {
          nfcWhitelist[whitelistCount++] = card;
        }
      }
      persistNfcWhitelist();
      Serial.printf("[NVS] Migrated legacy NFC list, count=%d.\n", whitelistCount);
      return;
    }

    Serial.printf("[NVS] Invalid nfc_list size=%u, expected=%u. Resetting whitelist.\n",
                  static_cast<unsigned int>(bytesLen), static_cast<unsigned int>(expectedStructLen));
    clearNfcWhitelist();
    persistNfcWhitelist();
  }
}

void setupWiFi() {
  // 从 NVS 读取 WiFi 凭证 (Read WiFi credentials from NVS)
  String s = prefs.getString("wifi_ssid", default_ssid);
  String p = prefs.getString("wifi_pass", default_password);

  WiFi.begin(s.c_str(), p.c_str());
  int t = 0;
  Serial.printf("Connecting WiFi to %s ", s.c_str()); // 正在连接设定的WiFi

  // 尝试连接 20 次 (Try 20 times -> 10 seconds)
  while (WiFi.status() != WL_CONNECTED && t < 20) {
    delay(500); Serial.print("."); t++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected OK!"); // 网络连接成功
    isAPMode = false;

    // 初始化 OTA (Initialize OTA)
    ArduinoOTA.setHostname("Mech-Master-S3");
    ArduinoOTA.setPassword(OTA_DEFAULT_PASSWORD);
    ArduinoOTA.onStart([]() { isOTAUpdating = true; audio.stopSong(); doorServo.detach(); });
    ArduinoOTA.onEnd([]() { isOTAUpdating = false; ESP.restart(); });
    ArduinoOTA.begin();
    apModeStartTime = 0;
    Serial.print("IP address: "); Serial.println(WiFi.localIP());

  } else {
    Serial.println("\n[WIFI] Failed! Starting AP Provisioning Mode."); // 连接失败，启动配网模式
    isAPMode = true;

    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    // 配置固定 IP: 192.168.10.10 (Config static IP)
    WiFi.softAPConfig(IPAddress(192,168,10,10), IPAddress(192,168,10,1), IPAddress(255,255,255,0));
    WiFi.softAP("esp32s3-menjin"); // 配网热点名称 (AP SSID)
    apModeStartTime = millis();

    Serial.println("[AP] Access Point started: esp32s3-menjin"); // 热点已启动
    Serial.println("[AP] IP Address: 192.168.10.10");
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/open", HTTP_GET, handleOpen);
  server.on("/add_nfc", HTTP_GET, handleAddNFC);
  server.on("/set_wifi", HTTP_GET, handleSetWiFi);
  server.begin();
  Serial.println("[WEB] Server Engine Started on port 80.");
}

void checkNFC() {
  if (isOTAUpdating || isAPMode) return;

  NfcCard currentCard = {};
  currentCard.size = mfrc522.uid.size;
  if (currentCard.size > MAX_NFC_UID_LENGTH) {
    Serial.printf("[NFC] UID length %d too long, rejected.\n", currentCard.size);
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    return;
  }

  Serial.print("UID:");
  for (byte i = 0; i < currentCard.size; i++) {
    Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(mfrc522.uid.uidByte[i], HEX);
    currentCard.uid[i] = mfrc522.uid.uidByte[i];
  }
  Serial.println();

  const bool match = isDuplicateNfcCard(currentCard);

  if (match) {
    authorizeDoorOpen("NFC");
  } else {
    Serial.println("Unknown Card");
    playLocalFile("/error.mp3");
    safeDelay(1000);
  }
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (isOTAUpdating) return;

  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  // 继续沿用既有的 "on" 远程开门指令；本地先执行再发布状态，避免 MQTT 回环造成重入。
  if (String(topic) == topic_door && msg == "on") {
    authorizeDoorOpen("Remote-MQTT");
  }
}

void authorizeDoorOpen(const char* source) {
  if (isOTAUpdating) return;
  if (isDoorOpen) return;

  Serial.print("Access granted via: ");
  Serial.println(source);

  customDoorDuration = DOOR_OPEN_DURATION_MS;
  playLocalFile("/open.mp3");

  // 发布状态前先完成本地执行，避免在 safeDelay() 期间经由 MQTT 回环触发重入。
  openDoor();

  if (client.connected()) client.publish(topic_door, "on");
}

// ================= 其他遗留函数保持不变 (Legacy Functions Remain Unchanged) =================

void safeDelay(unsigned long ms) {
  unsigned long start = millis();
  while(millis() - start < ms) {
    if(isOTAUpdating) { ArduinoOTA.handle(); return; }
    audio.loop();
    server.handleClient();
    if(!isAPMode) ArduinoOTA.handle();
    if (!isAPMode && WiFi.status() == WL_CONNECTED && client.connected()) client.loop();
  }
}

void checkKeypad() {
  char key = keypad.getKey();
  if (key) {
    resetKeypadLockIfExpired();
    if (isKeypadLocked()) {
      Serial.println("Keypad Locked. Please wait.");
      inputCode = "";
      return;
    }

    lastKeyTime = millis();
    Serial.print("Key Pressed: "); Serial.println(key);
    if (key == '*') {
      inputCode = ""; Serial.println("Input Cleared");
    }
    else if (key == '#') {
      if (inputCode == DOOR_PASSWORD) {
        Serial.println("Password Correct!");
        keypadFailedAttempts = 0;
        inputCode = "";
        authorizeDoorOpen("Keypad Password");
      } else {
        Serial.println("Password Wrong!");
        keypadFailedAttempts++;
        if (keypadFailedAttempts >= KEYPAD_MAX_FAILED_ATTEMPTS) {
          keypadLockoutUntil = millis() + KEYPAD_LOCKOUT_MS;
          keypadFailedAttempts = 0;
          Serial.println("Too many failed attempts. Keypad locked for 30s.");
        }
        inputCode = "";
        playLocalFile("/error.mp3");
      }
    }
    else {
      inputCode += key;
      if (inputCode.length() > 10) { inputCode = ""; Serial.println("Input Overflow"); }
    }
  }
  if (inputCode.length() > 0 && (millis() - lastKeyTime > 10000)) {
    inputCode = ""; Serial.println("Keypad Timeout");
  }
}

void reconnectMQTT() {
  if (isOTAUpdating || isAPMode) return;

  if (WiFi.status() != WL_CONNECTED) {
      if (mqttDisconnectTime == 0) mqttDisconnectTime = millis();
      if (millis() - mqttDisconnectTime > 30000) {
         Serial.println("[Watchdog] WiFi Lost for 30s. Restarting WiFi...");
         WiFi.disconnect(); WiFi.reconnect(); mqttDisconnectTime = millis();
      }
      return;
  }

  static unsigned long lastRec = 0;
  if (millis() - lastRec > 5000) {
    lastRec = millis();
    if (mqttDisconnectTime == 0) mqttDisconnectTime = millis();
    if (millis() - mqttDisconnectTime > 60000) {
        Serial.println("[Watchdog] MQTT Dead for 60s. Forcing WiFi Reset...");
        WiFi.disconnect(); delay(100); WiFi.reconnect(); mqttDisconnectTime = millis();
        return;
    }
    Serial.print("Attempting MQTT connection...");
    if (client.connect(mqtt_uid)) {
      Serial.println("[Reconnected]");
      client.subscribe(topic_door);
      client.publish(topic_door, "online");
      client.publish(topic_door, isDoorOpen ? "on" : "off");
      mqttDisconnectTime = 0;
    }
  }
}

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

int getFingerprintID() {
  if (isOTAUpdating || isAPMode) return -1;
  uint8_t p = finger.getImage(); if (p != FINGERPRINT_OK) return -1;
  p = finger.image2Tz(); if (p != FINGERPRINT_OK) return -1;
  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) return finger.fingerID;
  return -1;
}

void enterEnrollMode() {
  Serial.println("\n=== ENTERING ENROLL MODE ==="); // 进入指纹录入模式
  audio.stopSong();
  int id = 0;
  while (true) {
    if (Serial.available()) { id = Serial.parseInt(); if (id > 0 && id <= 127) break; }
    delay(100);
  }
  Serial.print("Enrolling ID #"); Serial.println(id);
  while (!getFingerprintEnroll(id));
  Serial.println("=== ENROLLMENT FINISHED ==="); // 录入结束
  playLocalFile("/boot.mp3");
}

uint8_t getFingerprintEnroll(int id) {
  int p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) Serial.print("."); else if (p == FINGERPRINT_OK) Serial.println("\nImage taken"); // 获取图像成功
    delay(100);
  }
  p = finger.image2Tz(1); if (p != FINGERPRINT_OK) return false;
  Serial.println("Remove finger"); // 移开手指
  delay(2000); p = 0;
  while (p != FINGERPRINT_NOFINGER) { p = finger.getImage(); delay(50); }
  Serial.println("Place same finger again"); // 再次放置同一手指
  p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) Serial.print("."); else if (p == FINGERPRINT_OK) Serial.println("\nImage taken");
    delay(100);
  }
  p = finger.image2Tz(2); if (p != FINGERPRINT_OK) return false;
  p = finger.createModel();
  if (p == FINGERPRINT_OK) { Serial.println("Prints matched!"); } else return false; // 特征匹配成功
  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) { Serial.println("Stored!"); return true; } else return false; // 存储成功
}

void playLocalFile(const char *filename) {
  if (SPIFFS.exists(filename)) audio.connecttoFS(SPIFFS, filename);
}
void audio_eof_mp3(const char *info){;}

bool parseUidHex(const String& uidStr, NfcCard& outCard) {
  if (uidStr.length() == 0 || (uidStr.length() % 2) != 0) return false;
  const uint8_t uidBytes = uidStr.length() / 2;
  if (uidBytes < MIN_NFC_UID_LENGTH || uidBytes > MAX_NFC_UID_LENGTH) return false;

  memset(outCard.uid, 0, sizeof(outCard.uid));
  outCard.size = uidBytes;
  for (uint8_t i = 0; i < uidBytes; i++) {
    const char hi = uidStr.charAt(i * 2);
    const char lo = uidStr.charAt(i * 2 + 1);
    if (!isxdigit(hi) || !isxdigit(lo)) return false;
    const char buf[3] = {hi, lo, '\0'};
    outCard.uid[i] = static_cast<uint8_t>(strtoul(buf, NULL, 16));
  }
  return true;
}

bool isDuplicateNfcCard(const NfcCard& card) {
  for (int i = 0; i < whitelistCount; i++) {
    if (nfcWhitelist[i].size == card.size &&
        memcmp(nfcWhitelist[i].uid, card.uid, card.size) == 0) {
      return true;
    }
  }
  return false;
}

void clearNfcWhitelist() {
  whitelistCount = 0;
  memset(nfcWhitelist, 0, sizeof(nfcWhitelist));
}

void persistNfcWhitelist() {
  prefs.putBytes("nfc_list", nfcWhitelist, whitelistCount * sizeof(NfcCard));
  prefs.putInt("nfc_cnt", whitelistCount);
}

bool isKeypadLocked() {
  return keypadLockoutUntil != 0 &&
         static_cast<long>(millis() - keypadLockoutUntil) < 0;
}

void resetKeypadLockIfExpired() {
  if (keypadLockoutUntil != 0 &&
      static_cast<long>(millis() - keypadLockoutUntil) >= 0) {
    keypadLockoutUntil = 0;
  }
}
