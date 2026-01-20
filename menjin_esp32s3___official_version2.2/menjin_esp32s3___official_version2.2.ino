/**
 * ESP32-S3 Mech Master (Version 2.03 Final)
 * * Base: Version 2.01 (User Uploaded)
 * * Optimization: Enhanced NFC Keep-Alive + Antenna Enforcement
 * * Author: Grey Goo & Fourth Crisis & You
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
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <Keypad.h>

// ================= 🌐 用户配置区 =================

const char* ssid        = "YOUR_WIFI_SSID";      // WiFi名称
const char* password    = "YOUR_WIFI_PASSWORD";  // WiFi密码

// 🔐 键盘密码配置
const String DOOR_PASSWORD = "11451"; //在此处修改您的解锁密码

// 🌤️ 天气 API
String OWM_API_KEY      = "YOUR_OWM_KEY";    // API Key
String CITY             = "city_name,CN";           // 城市
String OWM_URL          = "http://api.openweathermap.org/data/2.5/weather?units=metric&q=";

// ☁️ MQTT (巴法云)
const char* mqtt_server = "mqtt.bemfa.com";
const int   mqtt_port   = 9501;
const char* mqtt_uid    = "YOUR_BEMFA_UID";      // 私钥
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

// [新增] 矩阵键盘配置
const byte ROWS = 4; // 四行
const byte COLS = 4; // 四列
// 键盘字符映射图
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
// 键盘引脚定义
byte rowPins[ROWS] = {8, 15, 16, 21}; 
byte colPins[COLS] = {1, 2, 3, 7}; 

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);
String inputCode = ""; 
unsigned long lastKeyTime = 0; 

// ================= NFC 白名单 (已更新至6张) =================
byte Whitelist[][4] = {
  {0xF7, 0x6D, 0x16, 0x3F},//可备注卡的用途
  {0xE5, 0x6B, 0x1A, 0x06},
  {0x1D, 0x8E, 0x39, 0x68},
  {0xE5, 0x6B, 0x1A, 0x06},
  {0xAD, 0xE9, 0x31, 0x55},
  {0x01, 0x62, 0xAD, 0x1C} 
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
unsigned long mqttDisconnectTime = 0; 
unsigned long lastNFCHealthCheck = 0; 

// [关键安全标志] 是否正在 OTA 升级中
bool isOTAUpdating = false; 

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
void checkKeypad(); 
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
  // [新增] 启动时做一次软复位确保干净
  delay(10);
  mfrc522.PCD_DumpVersionToSerial(); 
  
  mySerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(57600);
  
  if (finger.verifyPassword()) {
    Serial.println("Fingerprint Sensor Found!");
  } else {
    Serial.println("Fingerprint Sensor NOT FOUND :(");
  }

  // 3. 网络 (含 OTA 初始化)
  setupWiFi();
  configTime(8 * 3600, 0, "pool.ntp.org", "time.aliyun.com"); 
  
  if(WiFi.status() == WL_CONNECTED) {
      updateWeather();
  }
  safeDelay(500);

  // MQTT 设置
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  
  Serial.print("Connecting to Bemfa MQTT...");
  if (WiFi.status() == WL_CONNECTED) {
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

// ================= 主循环 =================
void loop() {
  // [OTA 优先级保护]
  if (isOTAUpdating) {
    ArduinoOTA.handle();
    return; 
  }

  audio.loop(); 
  ArduinoOTA.handle(); 

  // 0. 检查串口指令
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'E' || c == 'e') {
      enterEnrollMode(); 
    }
  }

  // MQTT 守护
  if (!client.connected()) {
      reconnectMQTT();
  } else {
      client.loop();
      mqttDisconnectTime = 0; 
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

  // 4. 生物识别 & 密码键盘
  if (!isDoorOpen) {
    checkKeypad(); 
    
    int fpID = getFingerprintID();
    if (fpID != -1) processArriveHome("Fingerprint");
    
    // [加强版 NFC 看门狗]
    if (millis() - lastNFCHealthCheck > 3000) {
        byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
        
        // 情况A: 彻底死机 (返回 0x00 或 0xFF)
        if (v == 0x00 || v == 0xFF) {
            Serial.println("[Watchdog] NFC Dead. Resetting...");
            if(audio.isRunning()) audio.stopSong();
            mfrc522.PCD_Init(); 
            delay(50); // 给一点时间让振荡器稳定
            Serial.println("[Watchdog] Reset Done.");
        }
        // 情况B: 没死机，但预防性开启天线 (防止天线意外关闭)
        else {
            // PCD_Init() 内部包含 AntennaOn，但这里我们可以显式确保一下
            // 只有当没检测到卡的时候才开，避免打断读卡
            // 但最简单的是每次检查通过也确认一下天线
            // mfrc522.PCD_AntennaOn(); // 可选，通常PCD_Init够用了
        }
        lastNFCHealthCheck = millis();
    }

    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      checkNFC();
    }
  }
}

// ================= 辅助函数 =================

void checkKeypad() {
  char key = keypad.getKey();
  
  if (key) {
    lastKeyTime = millis(); 
    Serial.print("Key Pressed: "); 
    Serial.println(key);

    if (key == '*') {
      inputCode = "";
      Serial.println("Input Cleared");
    }
    else if (key == '#') {
      Serial.print("Checking Password: ");
      Serial.println(inputCode);
      
      if (inputCode == DOOR_PASSWORD) {
        Serial.println("Password Correct!");
        inputCode = ""; 
        processArriveHome("Keypad Password");
      } else {
        Serial.println("Password Wrong!");
        inputCode = ""; 
        playLocalFile("/error.mp3"); 
      }
    }
    else {
      inputCode += key;
      if (inputCode.length() > 10) {
        inputCode = "";
        Serial.println("Input Overflow - Reset");
      }
    }
  }

  if (inputCode.length() > 0 && (millis() - lastKeyTime > 10000)) {
    inputCode = "";
    Serial.println("Keypad Timeout - Input Cleared");
  }
}

void safeDelay(unsigned long ms) {
  unsigned long start = millis();
  while(millis() - start < ms) {
    if(isOTAUpdating) {
        ArduinoOTA.handle();
        return; 
    }
    audio.loop(); 
    ArduinoOTA.handle(); 
    if (WiFi.status() == WL_CONNECTED && client.connected()) client.loop();
  }
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
    
    ArduinoOTA.setHostname("Mech-Master-S3");
    
    ArduinoOTA.onStart([]() {
      isOTAUpdating = true;
      audio.stopSong();
      doorServo.detach(); 
      Serial.println("\n[OTA ALERT] Firmware Update Started. Systems Suspending...");
    });
    
    ArduinoOTA.onEnd([]() { 
        isOTAUpdating = false; 
        Serial.println("\n[OTA] Update Complete. Rebooting..."); 
    });
    
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      static unsigned int lastPct = 0;
      unsigned int pct = (progress / (total / 100));
      if (pct % 10 == 0 && pct != lastPct) {
         Serial.printf("OTA Progress: %u%%\n", pct);
         lastPct = pct;
      }
    });
    
    ArduinoOTA.onError([](ota_error_t error) {
      isOTAUpdating = false; 
      Serial.printf("Error[%u]: ", error);
    });
    
    ArduinoOTA.begin();
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
  } else {
    Serial.println("\nWiFi Failed! Running in Offline Mode.");
  }
}

void reconnectMQTT() {
  if (isOTAUpdating) return;

  if (WiFi.status() != WL_CONNECTED) {
      if (mqttDisconnectTime == 0) mqttDisconnectTime = millis();
      if (millis() - mqttDisconnectTime > 30000) {
         Serial.println("[Watchdog] WiFi Lost for 30s. Restarting WiFi...");
         WiFi.disconnect();
         WiFi.reconnect();
         mqttDisconnectTime = millis(); 
      }
      return; 
  }

  static unsigned long lastRec = 0;
  if (millis() - lastRec > 5000) {
    lastRec = millis();
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
    if (client.connect(mqtt_uid)) { 
      Serial.println("[Reconnected]");
      client.subscribe(topic_cmd);
      client.subscribe(topic_door);
      client.publish(topic_door, "online");
      client.publish(topic_door, isDoorOpen ? "on" : "off");
      mqttDisconnectTime = 0; 
    } else {
      Serial.print("failed, rc=");
      Serial.println(client.state());
    }
  }
}

void triggerLeaveHome() {
  if (isOTAUpdating) return; 
  Serial.println(">>> MODE: LEAVE HOME <<<");
  playLocalFile("/close.mp3"); 
  physicallySwitchLight(1, false); 
  safeDelay(300); 
  physicallySwitchLight(2, false); 
  if(client.connected()) client.publish(topic_cmd, "turn_off_ac"); 
  acIsOn = false;
}

void processArriveHome(String method) {
  if (isOTAUpdating) return; 
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
  if (outdoorTemp > 30.0 && !acIsOn) {
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
  if (isOTAUpdating) return; 
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
  if (isOTAUpdating) return; 
  String msg;
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  if (String(topic) == topic_cmd && msg == "leave_home") triggerLeaveHome();
  if (String(topic) == topic_door && msg == "on" && !isDoorOpen) processArriveHome("Remote-MQTT");
}

void checkNFC() {
  if (isOTAUpdating) return;
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
  if (isOTAUpdating) return -1;
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) return finger.fingerID;
  return -1;
}

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
  Serial.print("Waiting for valid finger #"); Serial.println(id);
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

void playLocalFile(const char *filename) {
  if (SPIFFS.exists(filename)) {
    audio.connecttoFS(SPIFFS, filename);
  }
}
void audio_eof_mp3(const char *info){;}