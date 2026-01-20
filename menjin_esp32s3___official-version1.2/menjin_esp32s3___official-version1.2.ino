/**
 * ESP32-S3 Mech Master (Ultimate Edition)
 * * Base: Official Version 1.1 (Auto-Heal Network)
 * * Features Integrated:
 * 1. NFC Booster: Max Gain (48dB), Soft Reset, Firmware Check.
 * 2. Fingerprint Pro: Robust Enrollment (Loop until success), Better Matching.
 * 3. Servo Custom: 500-3000us Pulse Width, Write 300 for Open.
 * 4. Removed: Audio, Light Servos.
 * Author: Grey Goo (For Fourth Crisis)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_Fingerprint.h>
#include <time.h> 
#include <ESP32Servo.h> 

// ================= 🌐 用户配置区 (USER CONFIG) =================

const char* ssid        = "";      // WiFi名称
const char* password    = "";  // WiFi密码

// 🌤️ 天气 API
String OWM_API_KEY      = "";    // API Key
String CITY             = ",CN";           // 城市
String OWM_URL          = "http://api.openweathermap.org/data/2.5/weather?units=metric&q=";

// ☁️ MQTT (巴法云)
const char* mqtt_server = "mqtt.bemfa.com";
const int   mqtt_port   = 9501;
const char* mqtt_uid    = "";      // 私钥
const char* topic_door  = "homedoor006";           // 门锁主题
const char* topic_cmd   = "homecmd006";            // 指令主题

// ⚙️ 舵机关键参数 (你的定制参数)
#define SERVO_PIN       9
#define PULSE_MIN       500
#define PULSE_MAX       3000  // 大舵机专用
#define ANGLE_OPEN      300   // 大角度
#define ANGLE_CLOSE     0

// ================= 🤖 硬件引脚 =================
#define TOUCH_PIN       14 

// NFC
#define NFC_SDA_PIN     10
#define NFC_SCK_PIN     12
#define NFC_MOSI_PIN    11
#define NFC_MISO_PIN    13
#define NFC_RST_PIN     40 

// 指纹
#define FP_RX_PIN       18 
#define FP_TX_PIN       17 

// ================= NFC 白名单 =================
byte Whitelist[][4] = {
  {0xF7, 0x6D, 0x16, 0x3F}, 
  {0xE5, 0x6B, 0x1A, 0x06}, 
  {0x1D, 0x8E, 0x39, 0x68}, 
  {0x01, 0x62, 0xAD, 0x1C}  
};
const int whitelistCount = sizeof(Whitelist) / sizeof(Whitelist[0]);

// ================= 全局对象 =================
WiFiClient espClient;
PubSubClient client(espClient);
MFRC522 mfrc522(NFC_SDA_PIN, NFC_RST_PIN);
HardwareSerial mySerial(1);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Servo doorServo;

// 状态变量
unsigned long doorOpenTime = 0;
bool isDoorOpen = false;
bool acIsOn = false; 
float outdoorTemp = 0.0;
unsigned long lastWeatherUpdate = 0;
unsigned long lastTouchTime = 0;
unsigned long mqttDisconnectTime = 0;

// ================= 函数声明 =================
void safeDelay(unsigned long ms);
void triggerLeaveHome();
void processArriveHome(String method);
void openDoor();
void closeDoor();
void updateWeather();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
void setupWiFi();
void checkNFC();
int getFingerprintID();
void enterEnrollMode();
void rigorousEnroll(int id);

// ================= 初始化 =================
void setup() {
  Serial.begin(115200);
  Serial.println("\n>>> System Init (Ultimate Edition) <<<");

  // 1. 硬件
  pinMode(TOUCH_PIN, INPUT); 
  
  ESP32PWM::allocateTimer(0);
  doorServo.setPeriodHertz(50);
  doorServo.attach(SERVO_PIN, PULSE_MIN, PULSE_MAX); // 500, 3000
  doorServo.write(ANGLE_CLOSE); // 0

  // 2. NFC 初始化 (Booster 补丁)
  // 软复位引脚
  pinMode(NFC_RST_PIN, OUTPUT);
  digitalWrite(NFC_RST_PIN, LOW); delay(50);
  digitalWrite(NFC_RST_PIN, HIGH); delay(50);
  
  SPI.begin(NFC_SCK_PIN, NFC_MISO_PIN, NFC_MOSI_PIN, NFC_SDA_PIN);
  mfrc522.PCD_Init();
  delay(100);
  
  // [关键] 设置最大增益 48dB
  mfrc522.PCD_SetAntennaGain(mfrc522.RxGain_max);
  Serial.println("[NFC] Antenna Gain set to Max (48dB)");
  
  // [关键] 检查固件版本 (排查接线)
  byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  Serial.print("[NFC] Version: 0x"); Serial.println(v, HEX);
  if(v == 0x00 || v == 0xFF) Serial.println(">>> WARNING: NFC Communication Fail! Check Wiring! <<<");

  // 3. 指纹初始化
  mySerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(57600);
  if (finger.verifyPassword()) {
    Serial.println("[FP] Sensor Found");
  } else {
    Serial.println("[FP] Sensor NOT Found");
  }

  // 4. 网络
  setupWiFi();
  if(WiFi.status() == WL_CONNECTED) updateWeather();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  
  // 启动时尝试非阻塞连接
  if (WiFi.status() == WL_CONNECTED) {
      Serial.print("Connecting MQTT...");
      if (client.connect(mqtt_uid)) {
          Serial.println("OK");
          client.subscribe(topic_cmd);
          client.subscribe(topic_door);
          client.publish(topic_door, "online");
          mqttDisconnectTime = 0;
      } else {
          Serial.println("Fail (will retry)");
          mqttDisconnectTime = millis();
      }
  }
  
  Serial.println(">>> Ready. Type 'E' to enroll. <<<");
}

// ================= 主循环 =================
void loop() {
  // 0. 串口指令
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'E' || c == 'e') enterEnrollMode(); 
  }

  // 1. 网络自愈
  if (!client.connected()) {
      reconnectMQTT();
  } else {
      client.loop();
      mqttDisconnectTime = 0;
  }

  // 2. 天气
  if (millis() - lastWeatherUpdate > 1200000) updateWeather();

  // 3. 离家模式
  if (digitalRead(TOUCH_PIN) == HIGH) {
    if (millis() - lastTouchTime > 2000) { 
      triggerLeaveHome();
      lastTouchTime = millis();
    }
  }

  // 4. 自动关门
  if (isDoorOpen && (millis() - doorOpenTime > 4000)) {
    closeDoor();
  }

  // 5. 生物识别
  if (!isDoorOpen) {
    // 指纹
    int fpID = getFingerprintID();
    if (fpID != -1) {
      Serial.printf("FP Match ID #%d\n", fpID);
      processArriveHome("Fingerprint");
    }
    
    // NFC (使用更灵敏的检测逻辑)
    if (mfrc522.PICC_IsNewCardPresent()) {
        if (mfrc522.PICC_ReadCardSerial()) {
            checkNFC();
        }
    }
  }
}

// ================= 动作逻辑 =================

void openDoor() {
  // 防止掉线，重新attach
  if (!doorServo.attached()) doorServo.attach(SERVO_PIN, PULSE_MIN, PULSE_MAX);
  
  Serial.println("Servo -> OPEN (300)");
  // 暴力发 3 次指令，确保大舵机收到信号
  for(int i=0; i<3; i++) {
    doorServo.write(ANGLE_OPEN); 
    delay(20);
  }
  
  isDoorOpen = true;
  doorOpenTime = millis();
}

void closeDoor() {
  if (!doorServo.attached()) doorServo.attach(SERVO_PIN, PULSE_MIN, PULSE_MAX);
  
  Serial.println("Servo -> CLOSE (0)");
  doorServo.write(ANGLE_CLOSE); 
  
  isDoorOpen = false;
  if(client.connected()) client.publish(topic_door, "off");
  
  delay(500);
  doorServo.detach(); // 断电保护
}

void triggerLeaveHome() {
  Serial.println(">>> LEAVE HOME <<<");
  if(client.connected()) client.publish(topic_cmd, "turn_off_ac"); 
  acIsOn = false;
  if(isDoorOpen) closeDoor();
}

void processArriveHome(String method) {
  Serial.println("Arrive: " + method);
  if(client.connected()) client.publish(topic_door, "on");
  openDoor();

  if (outdoorTemp > 30.0 && !acIsOn && outdoorTemp != 0.0) {
    if(client.connected()) client.publish(topic_cmd, "turn_on_ac"); 
    acIsOn = true; 
  }
}

// ================= 指纹录入 (严苛版/死磕模式) =================
void enterEnrollMode() {
  Serial.println("\n=== ENTERING ENROLL MODE ===");
  Serial.println("Type ID # (1-127) and press Enter...");
  
  int id = 0;
  while (true) {
    if (Serial.available()) {
      id = Serial.parseInt();
      if (id > 0 && id <= 127) break;
    }
    delay(100);
  }
  
  Serial.printf("Enrolling ID #%d...\n", id);
  rigorousEnroll(id); 
  Serial.println("=== FINISHED ===");
}

void rigorousEnroll(int id) {
  int p = -1;
  Serial.println(">>> STEP 1: Place finger <<<");
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      Serial.println("Taken.");
      p = finger.image2Tz(1);
      if (p != FINGERPRINT_OK) { Serial.println("Messy. Again."); p = -1; }
    }
    delay(100);
  }
  
  Serial.println("Remove finger");
  delay(1000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) { p = finger.getImage(); delay(50); }
  
  Serial.println(">>> STEP 2: Place SAME finger <<<");
  p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      Serial.println("Taken.");
      p = finger.image2Tz(2);
      if (p != FINGERPRINT_OK) { 
        Serial.println("Messy. Again."); p = -1; 
      } else {
        p = finger.createModel();
        if (p == FINGERPRINT_OK) {
          Serial.println("Matched! Saving...");
          p = finger.storeModel(id);
          if (p == FINGERPRINT_OK) {
            Serial.println(">>> SUCCESS! <<<");
            return;
          } else {
            Serial.println("Save Error. Again."); p = -1;
          }
        } else {
          Serial.println("NOT MATCH! Again."); p = -1;
        }
      }
    }
    delay(100);
  }
}

// ================= 指纹识别 (优化版) =================
int getFingerprintID() {
  uint8_t p = finger.getImage();
  // 仅当检测到手指时才继续，且不打印NOFINGER刷屏
  if (p == FINGERPRINT_NOFINGER) return -1; 
  if (p != FINGERPRINT_OK) return -1;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) return finger.fingerID;
  
  // 如果没匹配到，可以打印一下提示
  // Serial.println("Finger Not Match");
  return -1;
}

// ================= 辅助函数 =================

void safeDelay(unsigned long ms) {
  unsigned long start = millis();
  while(millis() - start < ms) {
    if (WiFi.status() == WL_CONNECTED && client.connected()) client.loop();
    delay(10);
  }
}

void updateWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(OWM_URL + CITY + "&appid=" + OWM_API_KEY);
  int code = http.GET();
  if (code == 200) {
    StaticJsonDocument<2048> doc;
    deserializeJson(doc, http.getString());
    outdoorTemp = doc["main"]["temp"];
    Serial.printf("[Weather] Temp: %.1f C\n", outdoorTemp);
    lastWeatherUpdate = millis();
  }
  http.end();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  if (String(topic) == topic_cmd && msg == "leave_home") triggerLeaveHome();
  if (String(topic) == topic_door && msg == "on" && !isDoorOpen) processArriveHome("Remote-MQTT");
}

void reconnectMQTT() {
  // WiFi 看门狗
  if (WiFi.status() != WL_CONNECTED) {
      if (mqttDisconnectTime == 0) mqttDisconnectTime = millis();
      if (millis() - mqttDisconnectTime > 30000) {
         Serial.println("[Watchdog] WiFi Lost. Resetting...");
         WiFi.disconnect(); WiFi.reconnect();
         mqttDisconnectTime = millis();
      }
      return; 
  }

  // MQTT 重连
  static unsigned long lastRec = 0;
  if (millis() - lastRec > 5000) {
    lastRec = millis();
    
    // MQTT 看门狗
    if (mqttDisconnectTime == 0) mqttDisconnectTime = millis();
    if (millis() - mqttDisconnectTime > 60000) {
        Serial.println("[Watchdog] MQTT Dead. Resetting WiFi...");
        WiFi.disconnect(); delay(100); WiFi.reconnect();
        mqttDisconnectTime = millis();
        return;
    }

    if (client.connect(mqtt_uid)) { 
      Serial.println("[MQTT] Reconnected");
      client.subscribe(topic_cmd);
      client.subscribe(topic_door);
      client.publish(topic_door, "online");
      client.publish(topic_door, isDoorOpen ? "on" : "off");
      mqttDisconnectTime = 0; 
    }
  }
}

void setupWiFi() {
  WiFi.begin(ssid, password);
  int t = 0;
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED && t < 20) {
    delay(500); Serial.print("."); t++;
  }
  if(WiFi.status() == WL_CONNECTED) Serial.println("\nWiFi OK");
  else Serial.println("\nWiFi Failed (Offline Mode)");
}

void checkNFC() {
  // 打印 UID 方便录入
  Serial.print("UID:");
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(mfrc522.uid.uidByte[i], HEX);
  }
  Serial.println();

  bool match = false;
  for (int i = 0; i < whitelistCount; i++) { 
    if (memcmp(mfrc522.uid.uidByte, Whitelist[i], 4) == 0) {
      match = true; break;
    }
  }
  if (match) processArriveHome("NFC");
  else {
    Serial.println("Unknown Card");
    safeDelay(1000); // 稍微防抖
  }
  
  // 必须停止加密，否则读下一张会失败
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

int getFingerprintID() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) return finger.fingerID;
  return -1;
}