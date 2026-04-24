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
#include <time.h>
#include <esp32-hal-psram.h>
#include "access_control/access_control.h"
#include "audio_feedback/audio_feedback.h"
#include "device_config/device_config.h"
#include "door_controller/door_controller.h"
#include "fingerprint_access/fingerprint_access.h"
#include "keypad_access/keypad_access.h"
#include "nfc_access/nfc_access.h"
#include "provisioning/provisioning.h"
#include "runtime_services/runtime_services.h"
#include "web_portal/web_portal.h"

// ☁️ MQTT (巴法云 Bemfa)
const char* mqtt_server = "mqtt.bemfa.com";
const int   mqtt_port   = 9501;
const char* topic_door  = "homedoor006";
const char* OTA_DEFAULT_PASSWORD = "esp32s3-menjin";

const int DOOR_OPEN_US = 3000;
const int DOOR_CLOSE_US = 500;
const uint32_t DOOR_OPEN_DURATION_MS = 4000;
const uint8_t BOOT_BUTTON_PIN = 0;
const uint32_t FORCE_PROVISION_HOLD_MS = 5000;
const char* AP_SSID = "esp32s3-menjin";
const IPAddress AP_IP(192, 168, 10, 10);
const IPAddress AP_GATEWAY(192, 168, 10, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);
const uint32_t DOOR_SERVO_PULSE_MS = 500;
const uint32_t STARTUP_SERVO_PULSE_MS = 600;

// ================= 🤖 硬件引脚 (Hardware Pins) =================
#define SERVO_DOOR_PIN  9

const DoorControllerConfig kDoorControllerConfig = {
  SERVO_DOOR_PIN,
  DOOR_OPEN_US,
  DOOR_CLOSE_US,
  DOOR_SERVO_PULSE_MS,
  DOOR_OPEN_DURATION_MS,
  STARTUP_SERVO_PULSE_MS,
};

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
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {8, 15, 16, 21};
byte colPins[COLS] = {1, 2, 3, 7};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

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
AccessControlState accessControl;
WebServer server(80);
RuntimeServicesState runtimeServices;
DoorControllerState doorState;
FingerprintAccessState fingerprintState;
KeypadAccessState keypadState;
NfcAccessState nfcState;

// 状态变量 (State Variables)
unsigned long customDoorDuration = DOOR_OPEN_DURATION_MS;

// ================= 函数声明 (Function Declarations) =================
void authorizeDoorOpen(const char* source);
void mqttCallback(char* topic, byte* payload, unsigned int length);
int addNfcCardFromWeb(const String& uid, String& message);
void handleDoorOpenFromWeb();
void handlePortalStateChanged();
WebPortalContext& getWebPortalContext();
RuntimeServicesContext& getRuntimeServicesContext();

bool shouldRunLocalAccess() {
  return !runtimeServices.isOtaUpdating;
}

WebPortalContext& getWebPortalContext() {
  static WebPortalContext context = {
    server,
    prefs,
    deviceConfig,
    provisioningState,
    accessControl,
    handleDoorOpenFromWeb,
    addNfcCardFromWeb,
    handlePortalStateChanged,
    runtimeIsTimeSynced,
    runtimeCurrentEpochSeconds,
    runtimeCurrentLocalTimeString
  };
  return context;
}

static void handleRuntimeOtaStartSideEffect() {
  audio.stopSong();
  doorServo.detach();
}

static bool runtimeDoorIsOpen() {
  return doorState.isOpen;
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
    runtimeDoorIsOpen,
  };
  return context;
}

static void serviceCoreLoopSlice() {
  processForcedProvisioningButton(runtimeServices, getRuntimeServicesContext(), BOOT_BUTTON_PIN, FORCE_PROVISION_HOLD_MS, AP_SSID, AP_IP, AP_GATEWAY, AP_SUBNET);
  tickAudioFeedback(audio);
  const bool wasDoorOpen = doorState.isOpen;
  tickDoorController(doorState, doorServo, kDoorControllerConfig, millis());
  if (wasDoorOpen && !doorState.isOpen && client.connected()) {
    client.publish(topic_door, "off");
  }
  tickFingerprintAccess(fingerprintState, finger, audio, isProvisioningPortalActive(provisioningState), millis());
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

// ================= 初始化 (Setup) =================
void setup() {
  Serial.begin(115200);

  Serial.printf("[HW] ESP32-S3 flash=%uMB, PSRAM=%u bytes\n",
                ESP.getFlashChipSize() / (1024 * 1024), ESP.getPsramSize());
  if (psramFound()) {
    Serial.println("[HW] PSRAM detected and ready.");
  } else {
    Serial.println("[HW] PSRAM not detected.");
  }

  initializeNfcAccess(prefs, nfcState);
  loadDeviceConfig(prefs, deviceConfig);
  loadAccessControl(prefs, accessControl);
  Serial.println(hasValidWiFiConfig(deviceConfig) ? "[CFG] WiFi config loaded" : "[CFG] WiFi config missing");
  Serial.println(isMqttConfigured(deviceConfig) ? "[CFG] MQTT enabled" : "[CFG] MQTT disabled");
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  resetForcedProvisioningButtonState(provisioningState);
  applyStartupDecision(provisioningState, decideStartupState(deviceConfig, false));

  if (!SPIFFS.begin(true)) Serial.println("SPIFFS Fail");
  initAudioFeedback(audio, I2S_BCLK, I2S_LRC, I2S_DOUT, 15);

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  doorServo.setPeriodHertz(50);
  initializeDoorController(doorState, doorServo, kDoorControllerConfig);

  SPI.begin(NFC_SCK_PIN, NFC_MISO_PIN, NFC_MOSI_PIN, NFC_SDA_PIN);
  mfrc522.PCD_Init();
  delay(10);
  mfrc522.PCD_DumpVersionToSerial();

  initializeFingerprintAccess(mySerial, finger, FP_RX_PIN, FP_TX_PIN);

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

  Serial.println(">>> System Ready. Type 'E' to enroll fingerprint. <<<");
  playBootSound(audio);
}

// ================= 主循环 (Main Loop) =================
void loop() {
  if (runtimeServices.isOtaUpdating) {
    ArduinoOTA.handle();
    return;
  }

  serviceCoreLoopSlice();

  clearExpiredNfcErrorFeedback(nfcState, millis());

  handleFingerprintConsoleInput(fingerprintState, finger, audio);

  if (!shouldRunLocalAccess() || doorState.isOpen || nfcState.errorFeedbackUntil != 0 ||
      fingerprintAccessBusy(fingerprintState)) {
    return;
  }

  KeypadAccessResult keypadResult = pollKeypadAccess(
    keypadState,
    keypad,
    prefs,
    accessControl,
    millis(),
    runtimeCurrentEpochSeconds(),
    runtimeIsTimeSynced()
  );
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
    if (nfcResult.authorized) {
      authorizeDoorOpen("NFC");
    } else if (nfcResult.rejected) {
      playErrorSound(audio);
    }
  }
}

// ================= 核心子系统实现 (Core Subsystem Implementations) =================
int addNfcCardFromWeb(const String& uid, String& message) {
  return addNfcCardFromHex(prefs, nfcState, uid, message);
}

void handleDoorOpenFromWeb() {
  authorizeDoorOpen("Web-App");
}

void handlePortalStateChanged() {
  syncLegacyApStateFromProvisioningPortal(runtimeServices, provisioningState);
  if (!isProvisioningPortalActive(provisioningState) && shouldAttemptWiFiConnection(getRuntimeServicesContext())) {
    beginWiFiConnectionAttempt(runtimeServices, getRuntimeServicesContext());
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (runtimeServices.isOtaUpdating) return;

  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  if (String(topic) == topic_door && msg == "on") {
    authorizeDoorOpen("Remote-MQTT");
  }
}

void authorizeDoorOpen(const char* source) {
  if (runtimeServices.isOtaUpdating) return;
  if (doorState.isOpen) return;

  Serial.print("Access granted via: ");
  Serial.println(source);

  customDoorDuration = DOOR_OPEN_DURATION_MS;
  playOpenSound(audio);
  doorState.activeOpenDurationMs = customDoorDuration;
  requestDoorOpen(doorState, doorServo, kDoorControllerConfig, millis());

  if (client.connected()) client.publish(topic_door, "on");
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
