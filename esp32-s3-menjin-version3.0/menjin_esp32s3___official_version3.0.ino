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
#include "device_config.h"
#include "provisioning.h"
#include "web_portal.h"

// ================= 🌐 默认/回退配置区 (Fallback Config) =================

const char* default_ssid     = "深圳湾一号尊享-5G";
const char* default_password = "qzsfb2-210";
const String DOOR_PASSWORD = "11451";

// ☁️ MQTT (巴法云 Bemfa)
const char* mqtt_server = "mqtt.bemfa.com";
const int   mqtt_port   = 9501;
const char* topic_door  = "homedoor006";
const char* OTA_DEFAULT_PASSWORD = "esp32s3-menjin";

#define ANGLE_NEUTRAL   90
const int DOOR_OPEN_US = 3000;
const int DOOR_CLOSE_US = 500;
const uint32_t DOOR_OPEN_DURATION_MS = 4000;
const uint32_t AP_MODE_TIMEOUT_MS = 10UL * 60UL * 1000UL;
const uint8_t BOOT_BUTTON_PIN = 0;
const uint32_t FORCE_PROVISION_HOLD_MS = 5000;
const char* AP_SSID = "esp32s3-menjin";
const IPAddress AP_IP(192, 168, 10, 10);
const IPAddress AP_GATEWAY(192, 168, 10, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);
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
DeviceConfig deviceConfig;
ProvisioningState provisioningState;
WebServer server(80);

// 状态变量 (State Variables)
unsigned long doorOpenTime = 0;
bool isDoorOpen = false;
unsigned long customDoorDuration = DOOR_OPEN_DURATION_MS;
unsigned long mqttDisconnectTime = 0;
unsigned long lastNFCHealthCheck = 0;
bool isOTAUpdating = false;
bool otaReady = false;
bool wifiConnectionAttemptActive = false;
bool webServerReady = false;
unsigned long wifiConnectStartedAt = 0;
unsigned long lastWiFiReconnectAttempt = 0;
unsigned long lastMqttReconnectAttempt = 0;
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

// ================= 函数声明 (Function Declarations) =================
void playLocalFile(const char *filename);
void safeDelay(unsigned long ms);
void authorizeDoorOpen(const char* source);
void openDoor();
void closeDoor();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void initNVSAndNFC();
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
int addNfcCardFromWeb(const String& uid, String& message);
void handleDoorOpenFromWeb();
void handlePortalStateChanged();
void beginWiFiConnectionAttempt();
bool shouldAttemptWiFiConnection();
void maintainWiFiConnection();
void maintainMqttConnection();
void ensureOtaReady();
void ensureWebServerReady();
WebPortalContext& getWebPortalContext();

void syncLegacyApStateFromProvisioningPortal() {
  if (!isProvisioningPortalActive(provisioningState)) {
    return;
  }

  wifiConnectionAttemptActive = false;
}

bool shouldRunNetworking() {
  return provisioningState.startupState == StartupState::CONNECTING_WIFI || WiFi.status() == WL_CONNECTED;
}

bool shouldServiceWebServer() {
  return isProvisioningPortalActive(provisioningState) || hasValidWiFiConfig(deviceConfig);
}

bool shouldRunLocalAccess() {
  return !isOTAUpdating;
}

bool shouldAttemptWiFiConnection() {
  return hasValidWiFiConfig(deviceConfig) && !isProvisioningPortalActive(provisioningState);
}

void beginWiFiConnectionAttempt() {
  if (!hasValidWiFiConfig(deviceConfig)) {
    return;
  }

  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
  }
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
  ArduinoOTA.onStart([]() { isOTAUpdating = true; audio.stopSong(); doorServo.detach(); });
  ArduinoOTA.onEnd([]() { isOTAUpdating = false; ESP.restart(); });
  ArduinoOTA.begin();
  otaReady = true;
}

void maintainWiFiConnection() {
  if (!shouldAttemptWiFiConnection()) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectionAttemptActive = false;
    if (provisioningState.startupState == StartupState::CONNECTING_WIFI) {
      provisioningState.startupState = StartupState::NORMAL_RUNTIME;
    }
    mqttDisconnectTime = 0;
    return;
  }

  if (wifiConnectionAttemptActive) {
    if (millis() - wifiConnectStartedAt < 10000UL) {
      return;
    }
    wifiConnectionAttemptActive = false;
    if (provisioningState.startupState == StartupState::CONNECTING_WIFI) {
      provisioningState.startupState = StartupState::NORMAL_RUNTIME;
    }
  }

  if (millis() - lastWiFiReconnectAttempt < 30000UL) {
    return;
  }

  lastWiFiReconnectAttempt = millis();
  WiFi.disconnect();
  beginWiFiConnectionAttempt();
}

void maintainMqttConnection() {
  if (!isMqttConfigured(deviceConfig) || WiFi.status() != WL_CONNECTED) {
    return;
  }

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  if (client.connected()) {
    client.loop();
    mqttDisconnectTime = 0;
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
    mqttDisconnectTime = 0;
  }
}

void ensureWebServerReady() {
  if (webServerReady || !shouldServiceWebServer()) {
    return;
  }

  setupWebRoutes(getWebPortalContext());
  server.begin();
  webServerReady = true;
  Serial.println("[WEB] Server Engine Started on port 80.");
}

WebPortalContext& getWebPortalContext() {
  static WebPortalContext context = {
    server,
    prefs,
    deviceConfig,
    provisioningState,
    handleDoorOpenFromWeb,
    addNfcCardFromWeb,
    handlePortalStateChanged
  };
  return context;
}

// ================= 初始化 (Setup) =================
void setup() {
  Serial.begin(115200);

  initNVSAndNFC();
  loadDeviceConfig(prefs, deviceConfig);
  Serial.println(hasValidWiFiConfig(deviceConfig) ? "[CFG] WiFi config loaded" : "[CFG] WiFi config missing");
  Serial.println(isMqttConfigured(deviceConfig) ? "[CFG] MQTT enabled" : "[CFG] MQTT disabled");
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  provisioningState.bootForcedProvision = detectForcedProvisioningRequest(BOOT_BUTTON_PIN, FORCE_PROVISION_HOLD_MS);
  applyStartupDecision(provisioningState, decideStartupState(deviceConfig, provisioningState.bootForcedProvision));

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

  if (provisioningState.startupState == StartupState::AP_PORTAL) {
    startProvisioningPortal(provisioningState, AP_SSID, AP_IP, AP_GATEWAY, AP_SUBNET);
    syncLegacyApStateFromProvisioningPortal();
  } else if (shouldAttemptWiFiConnection()) {
    beginWiFiConnectionAttempt();
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[CFG] Startup decision: skip auto provisioning, staying in local runtime.");
  }

  ensureWebServerReady();

  if (WiFi.status() == WL_CONNECTED) {
    ensureOtaReady();
    maintainMqttConnection();
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
  ensureWebServerReady();
  if (shouldServiceWebServer()) server.handleClient();
  maintainWiFiConnection();
  maintainMqttConnection();
  if (WiFi.status() == WL_CONNECTED) {
    ensureOtaReady();
    ArduinoOTA.handle();
  }

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'E' || c == 'e') {
      enterEnrollMode();
    }
  }

  if (isDoorOpen && (millis() - doorOpenTime > customDoorDuration)) {
    closeDoor();
  }

  if (!shouldRunLocalAccess() || isDoorOpen) {
    return;
  }

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


int addNfcCardFromWeb(const String& uid, String& message) {
  String uidStr = uid;
  uidStr.trim();
  NfcCard newCard = {};
  if (!parseUidHex(uidStr, newCard)) {
    message = "Invalid UID format";
    return 400;
  }

  if (isDuplicateNfcCard(newCard)) {
    message = "UID already exists";
    return 409;
  }

  if (whitelistCount >= MAX_NFC_CARDS) {
    message = "Whitelist Full!";
    return 507;
  }

  nfcWhitelist[whitelistCount++] = newCard;
  persistNfcWhitelist();
  Serial.printf("[NVS] Added New NFC (len=%d)\n", newCard.size);
  message = "NFC Added to NVS";
  return 200;
}

void handleDoorOpenFromWeb() {
  authorizeDoorOpen("Web-App");
}

void handlePortalStateChanged() {
  syncLegacyApStateFromProvisioningPortal();
  if (!isProvisioningPortalActive(provisioningState) && shouldAttemptWiFiConnection()) {
    beginWiFiConnectionAttempt();
  }
}

void checkNFC() {
  if (isOTAUpdating) return;

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
    ensureWebServerReady();
    if (shouldServiceWebServer()) server.handleClient();
    maintainWiFiConnection();
    maintainMqttConnection();
    if (WiFi.status() == WL_CONNECTED) {
      ensureOtaReady();
      ArduinoOTA.handle();
    }
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
  if (isOTAUpdating) return -1;
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
