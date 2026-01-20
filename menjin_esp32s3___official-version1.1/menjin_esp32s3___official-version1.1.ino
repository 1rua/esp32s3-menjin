/**
 * ESP32-S3 Mech Master (Auto-Heal Edition)
 * Fixed: Bamfa Cloud offline issue after WiFi reconnection.
 * Added: MQTT "online" signal on reconnect & WiFi Watchdog.
 * Author: Grey Goo & Fourth Crisis
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_Fingerprint.h>
#include <FS.h>
#include <SPIFFS.h>
#include <Audio.h>
#include <time.h> 
#include <ESP32Servo.h> 

// ================= 🌐 用户配置区 =================

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

// ⚙️ 舵机角度
#define ANGLE_NEUTRAL   90  
#define ANGLE_PUSH_ON   45  
#define ANGLE_PUSH_OFF  135 

// ================= 🤖 硬件引脚 =================
#define SERVO_DOOR_PIN  9 
#define SERVO_LIGHT1    48 
#define SERVO_LIGHT2    47 
#define TOUCH_PIN       14 

// I2S 音频
#define I2S_DOUT        6
#define I2S_BCLK        5
#define I2S_LRC         4

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
  {0xF7, 0x6D, 0x16, 0x3F}, //校卡
  {0xE5, 0x6B, 0x1A, 0x06}, //空卡
  {0x1D, 0x8E, 0x39, 0x68},//雨亮手表
  {0xE5, 0x6B, 0x1A, 0x06},//公交卡
  {0x01, 0x62, 0xAD, 0x1C} //公交卡
};
const int whitelistCount = sizeof(Whitelist) / sizeof(Whitelist[0]);

// ================= 全局对象 =================
WiFiClient espClient;
PubSubClient client(espClient);
Audio audio;
MFRC522 mfrc522(NFC_SDA_PIN, NFC_RST_PIN);
HardwareSerial mySerial(1);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

Servo doorServo;
Servo lightServo1;
Servo lightServo2;

// 状态变量
unsigned long doorOpenTime = 0;
bool isDoorOpen = false;
bool acIsOn = false; 
unsigned long sunsetTime = 0; 
float outdoorTemp = 0.0;
unsigned long lastWeatherUpdate = 0;
unsigned long lastTouchTime = 0;

// [新增] 掉线计时器
unsigned long mqttDisconnectTime = 0; 

// ================= 函数声明 =================
void playLocalFile(const char *filename);
void safeDelay(unsigned long ms);
void triggerLeaveHome();
void processArriveHome(String method);
void physicallySwitchLight(int id, bool state);
void openDoor();
void closeDoor();
void updateWeather();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
void setupWiFi();
void checkNFC();
int getFingerprintID();
void enterEnrollMode();
uint8_t getFingerprintEnroll(int id);

// ================= 初始化 =================
void setup() {
  Serial.begin(115200);
  
  // 1. 文件系统 & 音频
  if(!SPIFFS.begin(true)) Serial.println("SPIFFS Fail");
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(15); 

  // 2. 硬件
  pinMode(TOUCH_PIN, INPUT); 

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  doorServo.setPeriodHertz(50);
  lightServo1.setPeriodHertz(50);
  lightServo2.setPeriodHertz(50);

  doorServo.attach(SERVO_DOOR_PIN, 500, 3000);
  doorServo.write(0); 
  lightServo1.attach(SERVO_LIGHT1, 500, 2500);
  lightServo2.attach(SERVO_LIGHT2, 500, 2500);
  lightServo1.write(ANGLE_NEUTRAL);
  lightServo2.write(ANGLE_NEUTRAL);

  // 传感器
  SPI.begin(NFC_SCK_PIN, NFC_MISO_PIN, NFC_MOSI_PIN, NFC_SDA_PIN);
  mfrc522.PCD_Init();
  mySerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(57600);
  
  if (finger.verifyPassword()) {
    Serial.println("Fingerprint Sensor Found!");
  } else {
    Serial.println("Fingerprint Sensor NOT FOUND :(");
  }

  // 3. 网络
  setupWiFi();
  configTime(8 * 3600, 0, "pool.ntp.org", "time.aliyun.com"); 
  
  if(WiFi.status() == WL_CONNECTED) {
      updateWeather();
  }
  safeDelay(500);

  // MQTT 设置
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  
  // 启动时尝试一次连接
  Serial.print("Connecting to Bemfa MQTT...");
  if (WiFi.status() == WL_CONNECTED) {
    if (client.connect(mqtt_uid)) {
      Serial.println("\n[SUCCESS] MQTT Connected!");
      client.subscribe(topic_cmd);
      client.subscribe(topic_door);
      client.publish(topic_door, "online");
      mqttDisconnectTime = 0; // 重置计时器
    } else {
      Serial.print(" Failed! rc=");
      Serial.println(client.state());
      mqttDisconnectTime = millis(); // 开始记录掉线时间
    }
  }
  
  Serial.println(">>> System Ready. Type 'E' to enroll fingerprint. <<<");
  playLocalFile("/boot.mp3");
}

// ================= 主循环 =================
void loop() {
  audio.loop(); 

  // 0. 检查串口指令
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'E' || c == 'e') {
      enterEnrollMode(); 
    }
  }

  // 🛡️ MQTT 守护与自愈逻辑
  if (!client.connected()) {
      reconnectMQTT();
  } else {
      client.loop();
      mqttDisconnectTime = 0; // 只要连接着，就清零计时器
  }

  // 1. 天气
  if (millis() - lastWeatherUpdate > 1200000) updateWeather();

  // 2. 离家模式
  if (digitalRead(TOUCH_PIN) == HIGH) {
    if (millis() - lastTouchTime > 2000) { 
      triggerLeaveHome();
      lastTouchTime = millis();
    }
  }

  // 3. 自动关门
  if (isDoorOpen && (millis() - doorOpenTime > 4000)) {
    closeDoor();
  }

  // 4. 生物识别
  if (!isDoorOpen) {
    int fpID = getFingerprintID();
    if (fpID != -1) processArriveHome("Fingerprint");
    
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      checkNFC();
    }
  }
}

// ================= 🧬 增强版：MQTT 重连与自愈 =================
void reconnectMQTT() {
  // 1. 检查 WiFi 是否断开
  if (WiFi.status() != WL_CONNECTED) {
      if (mqttDisconnectTime == 0) mqttDisconnectTime = millis();
      // 如果 WiFi 断开超过 30秒，尝试重启 WiFi 协议栈
      if (millis() - mqttDisconnectTime > 30000) {
         Serial.println("[Watchdog] WiFi Lost for 30s. Restarting WiFi...");
         WiFi.disconnect();
         WiFi.reconnect();
         mqttDisconnectTime = millis(); // 重置时间，再给它30秒
      }
      return; 
  }

  // 2. 如果 WiFi 连着，但 MQTT 断了，每 5 秒重连一次
  static unsigned long lastRec = 0;
  if (millis() - lastRec > 5000) {
    lastRec = millis();
    
    // 如果 MQTT 断开超过 60秒（虽然 WiFi 好像还连着），强制重启 WiFi
    // 这能解决“网络假死”问题
    if (mqttDisconnectTime == 0) mqttDisconnectTime = millis();
    if (millis() - mqttDisconnectTime > 60000) {
        Serial.println("[Watchdog] MQTT Dead for 60s. Forcing WiFi Reset...");
        WiFi.disconnect();
        delay(100);
        WiFi.reconnect();
        mqttDisconnectTime = millis();
        return;
    }

    Serial.print("Attempting MQTT connection...");
    // 尝试用 UID 连接
    if (client.connect(mqtt_uid)) { 
      Serial.println("[Reconnected]");
      // 重新订阅
      client.subscribe(topic_cmd);
      client.subscribe(topic_door);
      // [关键] 必须补发 online，否则巴法云不知道你回来了
      client.publish(topic_door, "online");
      client.publish(topic_door, isDoorOpen ? "on" : "off");
      
      mqttDisconnectTime = 0; // 成功连接，重置看门狗
    } else {
      Serial.print("failed, rc=");
      Serial.println(client.state());
    }
  }
}

// ================= 其他函数 (保持不变) =================

void enterEnrollMode() {
  Serial.println("\n=== ENTERING ENROLL MODE ===");
  Serial.println("Please type the ID # (1-127) and press Enter...");
  audio.stopSong(); 
  int id = 0;
  while (true) {
    if (Serial.available()) {
      id = Serial.parseInt();
      if (id > 0 && id <= 127) break;
    }
    delay(100);
  }
  Serial.print("Enrolling ID #"); Serial.println(id);
  while (!getFingerprintEnroll(id));
  Serial.println("=== ENROLLMENT FINISHED ===");
  playLocalFile("/boot.mp3"); 
}

uint8_t getFingerprintEnroll(int id) {
  int p = -1;
  Serial.print("Waiting for valid finger to enroll as #"); Serial.println(id);
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) Serial.print(".");
    else if (p == FINGERPRINT_OK) Serial.println("\nImage taken");
    delay(100);
  }
  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) { Serial.println("Image Error"); return false; }
  Serial.println("Remove finger");
  delay(2000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) { p = finger.getImage(); delay(50); }
  Serial.println("Place same finger again");
  p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) Serial.print(".");
    else if (p == FINGERPRINT_OK) Serial.println("\nImage taken");
    delay(100);
  }
  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) { Serial.println("Image Error"); return false; }
  p = finger.createModel();
  if (p == FINGERPRINT_OK) { Serial.println("Prints matched!"); } 
  else { Serial.println("Fingerprints did not match"); return false; }
  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) { Serial.println("Stored!"); return true; } 
  else { Serial.println("Store Error"); return false; }
}

void safeDelay(unsigned long ms) {
  unsigned long start = millis();
  while(millis() - start < ms) {
    audio.loop(); 
    if (WiFi.status() == WL_CONNECTED && client.connected()) client.loop();
  }
}

void triggerLeaveHome() {
  Serial.println(">>> MODE: LEAVE HOME <<<");
  playLocalFile("/close.mp3"); 
  physicallySwitchLight(1, false); 
  safeDelay(300); 
  physicallySwitchLight(2, false); 
  if(client.connected()) client.publish(topic_cmd, "turn_off_ac"); 
  acIsOn = false;
}

void processArriveHome(String method) {
  Serial.println("Arrive via: " + method);
  playLocalFile("/open.mp3"); 
  if(client.connected()) client.publish(topic_door, "on");
  openDoor();
  time_t now;
  time(&now); 
  if (sunsetTime > 0 && now > sunsetTime) {
    Serial.println("Night Detected. Lights ON.");
    safeDelay(800); 
    physicallySwitchLight(1, true);
    safeDelay(300);
    physicallySwitchLight(2, true);
  }
  if (outdoorTemp > 30.0 && !acIsOn && outdoorTemp != 0.0) {
    Serial.println("Heat Alert. AC ON.");
    if(client.connected()) client.publish(topic_cmd, "turn_on_ac"); 
    acIsOn = true; 
  }
}

void physicallySwitchLight(int id, bool state) {
  Servo *s;
  if (id == 1) { s = &lightServo1; }
  else         { s = &lightServo2; }
  int targetAngle = state ? ANGLE_PUSH_ON : ANGLE_PUSH_OFF;
  s->write(targetAngle); 
  safeDelay(400); 
  s->write(ANGLE_NEUTRAL);
}

void openDoor() {
  if (!doorServo.attached()) doorServo.attach(SERVO_DOOR_PIN, 500, 3000);
  doorServo.write(300); 
  isDoorOpen = true;
  doorOpenTime = millis();
}

void closeDoor() {
  if (!doorServo.attached()) doorServo.attach(SERVO_DOOR_PIN, 500, 3000);
  doorServo.write(0); 
  isDoorOpen = false;
  if(client.connected()) client.publish(topic_door, "off");
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
    sunsetTime = doc["sys"]["sunset"];
    Serial.printf("[Weather] Temp: %.1f, Sunset: %lu\n", outdoorTemp, sunsetTime);
    lastWeatherUpdate = millis();
  } else {
    Serial.printf("[Weather] Failed, code: %d\n", code);
  }
  http.end();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("MQTT [%s]: %s\n", topic, msg.c_str());
  if (String(topic) == topic_cmd && msg == "leave_home") triggerLeaveHome();
  if (String(topic) == topic_door && msg == "on" && !isDoorOpen) processArriveHome("Remote-MQTT");
}

void setupWiFi() {
  WiFi.begin(ssid, password);
  int t = 0;
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED && t < 20) {
    delay(500); Serial.print("."); t++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK");
  } else {
    Serial.println("\nWiFi Failed! Running in Offline Mode.");
  }
}

void checkNFC() {
  Serial.print("UID:");
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(mfrc522.uid.uidByte[i], HEX);
  }
  Serial.println();
  bool match = false;
  for (int i = 0; i < whitelistCount; i++) { 
    if (memcmp(mfrc522.uid.uidByte, Whitelist[i], 4) == 0) {
      match = true;
      break;
    }
  }
  if (match) processArriveHome("NFC");
  else {
    Serial.println("Unknown Card");
    playLocalFile("/error.mp3");
    safeDelay(1000);
  }
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

int getFingerprintID() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) return -1;
  return finger.fingerID;
}

void playLocalFile(const char *filename) {
  if (SPIFFS.exists(filename)) {
    audio.connecttoFS(SPIFFS, filename);
  }
}
void audio_eof_mp3(const char *info){;}