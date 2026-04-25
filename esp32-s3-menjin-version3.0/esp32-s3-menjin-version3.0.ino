// 主程序入口：负责初始化硬件、加载配置，并在 loop 中协调各门禁子模块。
#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_Fingerprint.h>
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
#include "src/access_control/access_control.h"
#include "src/device_config/device_config.h"
#include "src/door_controller/door_controller.h"
#include "src/fingerprint_access/fingerprint_access.h"
#include "src/keypad_access/keypad_access.h"
#include "src/nfc_access/nfc_access.h"
#include "src/provisioning/provisioning.h"
#include "src/runtime_services/runtime_services.h"
#include "src/web_portal/web_portal.h"

const char* mqtt_server = "mqtt.bemfa.com";
const int   mqtt_port   = 9501;
const char* topic_door  = "homedoor006";
const char* OTA_DEFAULT_PASSWORD = "esp32s3-menjin";

// 门锁舵机与强制配网按键相关的硬件时序参数。
const int DOOR_OPEN_US = 3000;
const int DOOR_CLOSE_US = 500;
const uint32_t DOOR_OPEN_DURATION_MS = 2000;
const uint8_t BOOT_BUTTON_PIN = 0;
const uint32_t FORCE_PROVISION_HOLD_MS = 5000;
// AP 配网门户默认网络参数。
const char* AP_SSID = "esp32s3-menjin";
const IPAddress AP_IP(192, 168, 10, 10);
const IPAddress AP_GATEWAY(192, 168, 10, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);
const uint32_t DOOR_SERVO_PULSE_MS = 1500;
const uint32_t STARTUP_SERVO_PULSE_MS = 600;

#define ENABLE_LOOP_PROFILING 1
#if ENABLE_LOOP_PROFILING
#define PROFILE_TASK(name, expr) do { \
  unsigned long __profileStart = millis(); \
  expr; \
  unsigned long __profileCost = millis() - __profileStart; \
  if (__profileCost > 50UL) { \
    Serial.printf("[BLOCK] %s took %lu ms\n", name, __profileCost); \
  } \
} while (0)
#else
#define PROFILE_TASK(name, expr) do { expr; } while (0)
#endif

#define SERVO_DOOR_PIN  9

const DoorControllerConfig kDoorControllerConfig = {
  SERVO_DOOR_PIN,
  DOOR_OPEN_US,
  DOOR_CLOSE_US,
  DOOR_SERVO_PULSE_MS,
  DOOR_OPEN_DURATION_MS,
  STARTUP_SERVO_PULSE_MS,
};

// NFC（MFRC522）SPI 引脚定义。
#define NFC_SDA_PIN     10
#define NFC_SCK_PIN     12
#define NFC_MOSI_PIN    11
#define NFC_MISO_PIN    13
#define NFC_RST_PIN     40

// 指纹模块串口引脚定义。
#define FP_RX_PIN       18
#define FP_TX_PIN       17

// 4x4 矩阵键盘布局与引脚映射。
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

WiFiClient espClient;
PubSubClient client(espClient);
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

// 支持后续按场景扩展为自定义开门时长（当前默认使用固定值）。
unsigned long customDoorDuration = DOOR_OPEN_DURATION_MS;
static bool pendingMqttDoorOn = false;
static bool pendingMqttDoorOff = false;
static unsigned long lastLocalAccessActivityMs = 0;
static unsigned long lastPinPruneAt = 0;
static unsigned long lastFingerprintPollAt = 0;
static unsigned long lastNfcHealthCheckTaskAt = 0;

void authorizeDoorOpen(const char* source);
void mqttCallback(char* topic, byte* payload, unsigned int length);
int addNfcCardFromWeb(const String& uid, String& message);
void handleDoorOpenFromWeb();
int startFingerprintEnrollFromWebPortal(const String& name, String& message);
void getFingerprintEnrollStatusFromWebPortal(String& dataJson);
int cancelFingerprintEnrollFromWebPortal(String& message);
void listFingerprintsFromWebPortal(String& itemsJson);
int renameFingerprintFromWebPortal(int id, const String& name, String& message);
int deleteFingerprintFromWebPortal(int id, String& message);
void handlePortalStateChanged();
WebPortalContext& getWebPortalContext();
RuntimeServicesContext& getRuntimeServicesContext();
static bool pollLocalAccessFirst();
static void queueDoorMqttState(bool opened);
static void servicePostUnlockEvents();

bool shouldRunLocalAccess() {
  return !runtimeServices.isOtaUpdating;
}

WebPortalContext& getWebPortalContext() {
  // 使用静态上下文对象，避免重复构造并确保回调引用稳定。
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

static void handleRuntimeOtaStartSideEffect() {
  // OTA 开始时释放外设资源，降低升级过程中的冲突风险。
  doorServo.detach();
}

static bool runtimeDoorIsOpen() {
  return doorState.isOpen;
}

RuntimeServicesContext& getRuntimeServicesContext() {
  // 运行时服务共享依赖的统一上下文。
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
  // 统一处理核心后台任务：配网按钮、门控、指纹、Web、Wi-Fi、MQTT、NTP、OTA。
  PROFILE_TASK("processForcedProvisioningButton", processForcedProvisioningButton(runtimeServices, getRuntimeServicesContext(), BOOT_BUTTON_PIN, FORCE_PROVISION_HOLD_MS, AP_SSID, AP_IP, AP_GATEWAY, AP_SUBNET));
  PROFILE_TASK("tickFingerprintAccess", tickFingerprintAccess(
    prefs,
    fingerprintState,
    finger,
    runtimeServices.isOtaUpdating,
    isProvisioningPortalActive(provisioningState),
    millis()
  ));
  ensureWebServerReady(runtimeServices, getRuntimeServicesContext());
  if (shouldServiceWebServer(getRuntimeServicesContext())) {
    PROFILE_TASK("server.handleClient", server.handleClient());
  }
  PROFILE_TASK("maintainWiFiConnection", maintainWiFiConnection(runtimeServices, getRuntimeServicesContext()));
  const bool recentlyLocalActive = millis() - lastLocalAccessActivityMs < 3000UL;
  const bool fingerprintBusy = fingerprintAccessBusy(fingerprintState);
  if (!doorState.isOpen && !recentlyLocalActive && !fingerprintBusy) {
    PROFILE_TASK("maintainMqttConnection", maintainMqttConnection(runtimeServices, getRuntimeServicesContext(), mqtt_server, mqtt_port, topic_door, mqttCallback));
  } else {
    serviceMqttLoopOnly(getRuntimeServicesContext());
  }
  if (WiFi.status() == WL_CONNECTED) {
    ensureTimeSyncStarted(runtimeServices);
    if (runtimeIsTimeSynced() && millis() - lastPinPruneAt >= 60000UL) {
      lastPinPruneAt = millis();
      PROFILE_TASK("pruneExpiredTemporaryPins", pruneExpiredTemporaryPins(prefs, accessControl, runtimeCurrentEpochSeconds(), true));
    }
    ensureOtaReady(runtimeServices, getRuntimeServicesContext(), "Mech-Master-S3", OTA_DEFAULT_PASSWORD);
    PROFILE_TASK("ArduinoOTA.handle", ArduinoOTA.handle());
  }
  if (millis() - lastNfcHealthCheckTaskAt >= 1000UL) {
    lastNfcHealthCheckTaskAt = millis();
    PROFILE_TASK("maintainNfcReaderHealth", maintainNfcReaderHealth(nfcState, mfrc522, millis(), handleRuntimeOtaStartSideEffect));
  }
  servicePostUnlockEvents();
}

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

  // 分配舵机 PWM 定时器并完成门锁控制器初始化。
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

  initializeFingerprintAccess(prefs, fingerprintState, mySerial, finger, FP_RX_PIN, FP_TX_PIN);

  // 按启动决策进入 AP 配网门户或尝试连接已保存的 Wi-Fi。
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
}

void loop() {
  // OTA 进行中时，仅维持 OTA 循环，暂停本地门禁逻辑。
  if (runtimeServices.isOtaUpdating) {
    PROFILE_TASK("ArduinoOTA.handle", ArduinoOTA.handle());
    return;
  }
  const bool wasDoorOpen = doorState.isOpen;
  PROFILE_TASK("tickDoorController", tickDoorController(doorState, doorServo, kDoorControllerConfig, millis()));
  if (wasDoorOpen && !doorState.isOpen) {
    queueDoorMqttState(false);
  }

  handleFingerprintConsoleInput(
    fingerprintState,
    finger,
    runtimeServices.isOtaUpdating,
    isProvisioningPortalActive(provisioningState)
  );

  if (pollLocalAccessFirst()) {
    servicePostUnlockEvents();
    return;
  }

  serviceCoreLoopSlice();
}

static bool pollLocalAccessFirst() {
  clearExpiredNfcErrorFeedback(nfcState, millis());
  if (!shouldRunLocalAccess() || doorState.isOpen || nfcState.errorFeedbackUntil != 0) {
    return false;
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
    lastLocalAccessActivityMs = millis();
    authorizeDoorOpen(keypadResult.matchedSource == "Temporary PIN" ? "Temporary PIN" : "Keypad Password");
    return true;
  }
  if (keypadResult.rejected) {
    lastLocalAccessActivityMs = millis();
    return false;
  }

  if (millis() - lastFingerprintPollAt >= 80UL) {
    lastFingerprintPollAt = millis();
    lastLocalAccessActivityMs = millis();
    int fpID = -1;
    PROFILE_TASK("pollFingerprintMatch", fpID = pollFingerprintMatch(fingerprintState, finger, runtimeServices.isOtaUpdating));
    if (fpID != -1) {
      authorizeDoorOpen("Fingerprint");
      return true;
    }
  }

  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    lastLocalAccessActivityMs = millis();
    NfcPollResult nfcResult = pollNfcAccess(nfcState, mfrc522, runtimeServices.isOtaUpdating, millis());
    if (nfcResult.authorized) {
      authorizeDoorOpen("NFC");
      return true;
    }
  }
  return false;
}

static void queueDoorMqttState(bool opened) {
  if (opened) {
    pendingMqttDoorOn = true;
    pendingMqttDoorOff = false;
  } else {
    pendingMqttDoorOff = true;
    pendingMqttDoorOn = false;
  }
}

static void servicePostUnlockEvents() {
  if (client.connected()) {
    if (pendingMqttDoorOn) {
      pendingMqttDoorOn = false;
      client.publish(topic_door, "on");
    }

    if (pendingMqttDoorOff) {
      pendingMqttDoorOff = false;
      client.publish(topic_door, "off");
    }
  }
}

int addNfcCardFromWeb(const String& uid, String& message) {
  return addNfcCardFromHex(prefs, nfcState, uid, message);
}

void handleDoorOpenFromWeb() {
  authorizeDoorOpen("Web-App");
}

int startFingerprintEnrollFromWebPortal(const String& name, String& message) {
  return startFingerprintEnrollFromWeb(
    fingerprintState,
    finger,
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

void handlePortalStateChanged() {
  syncLegacyApStateFromProvisioningPortal(runtimeServices, provisioningState);
  if (!isProvisioningPortalActive(provisioningState) && shouldAttemptWiFiConnection(getRuntimeServicesContext())) {
    beginWiFiConnectionAttempt(runtimeServices, getRuntimeServicesContext());
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (runtimeServices.isOtaUpdating) return;

  // 将 MQTT 二进制载荷转换为字符串命令。
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
  doorState.activeOpenDurationMs = customDoorDuration;
  unsigned long t0 = millis();
  requestDoorOpen(doorState, doorServo, kDoorControllerConfig, t0);
  unsigned long cost = millis() - t0;
  if (cost > 20UL) {
    Serial.printf("[OPEN] requestDoorOpen took %lu ms\n", cost);
  }
  lastLocalAccessActivityMs = millis();
  queueDoorMqttState(true);
}

void safeDelay(unsigned long ms) {
  // 可中断延时：在等待期间持续驱动核心循环，并允许 OTA 抢占。
  unsigned long start = millis();
  while (millis() - start < ms) {
    if (runtimeServices.isOtaUpdating) {
      ArduinoOTA.handle();
      return;
    }
    const bool wasDoorOpen = doorState.isOpen;
    tickDoorController(doorState, doorServo, kDoorControllerConfig, millis());
    if (wasDoorOpen && !doorState.isOpen) {
      queueDoorMqttState(false);
    }
    servicePostUnlockEvents();
    serviceCoreLoopSlice();
  }
}
