/**
 * ESP32-S3 Mech Master (Version 2.3 Web-Enhanced)
 * * Base: Version 2.03 Final
 * * Added: Web UI, AP Provisioning, Persistent Door Open, Dynamic NFC (NVS)
 * * Servo Logic: Reverted to V2.02 Safe Guard (Attach -> Write -> Delay -> Detach)
 * * Author: Grey Goo & Fourth Crisis
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
#include <WebServer.h>      // [新增] Web服务器支持 (Web server support)
#include <Preferences.h>    // [新增] NVS持久化存储 (NVS persistent storage)

// ================= 🌐 默认/回退配置区 (Fallback Config) =================

const char* default_ssid     = "深圳湾一号尊享-5G";      // 默认WiFi名称 (Default WiFi SSID)
const char* default_password = "qzsfb2-210";  // 默认WiFi密码 (Default WiFi Pass)

// 🔐 键盘密码配置 (Keypad Password)
const String DOOR_PASSWORD = "11451"; // 在此处修改您的解锁密码 (Modify your unlock password here)

// 🌤️ 天气 API (Weather API)
String OWM_API_KEY      = "YOUR_OWM_KEY";    // API Key
String CITY             = "city_name,CN";    // 城市 (City)
String OWM_URL          = "http://api.openweathermap.org/data/2.5/weather?units=metric&q=";

// ☁️ MQTT (巴法云 Bemfa)
const char* mqtt_server = "mqtt.bemfa.com";
const int   mqtt_port   = 9501;
const char* mqtt_uid    = "YOUR_BEMFA_UID";      // 私钥 (Private Key)
const char* topic_door  = "homedoor006";         // 门锁主题 (Door Topic)
const char* topic_cmd   = "homecmd006";          // 指令主题 (Command Topic)
const char* topic_keep  = "homekeep006";         // [新增] 持续开门5分钟主题 (Keep open for 5 mins topic)

// ⚙️ 舵机角度 (Servo Angles)
#define ANGLE_NEUTRAL   90  
#define ANGLE_PUSH_ON   45  
#define ANGLE_PUSH_OFF  135 

// ================= 🤖 硬件引脚 (Hardware Pins) =================
#define SERVO_DOOR_PIN  9 
#define SERVO_LIGHT1    48 
#define SERVO_LIGHT2    47 
#define TOUCH_PIN       14 

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
Servo lightServo1;
Servo lightServo2;

Preferences prefs;          // NVS 存储对象 (NVS storage object)
WebServer server(80);       // Web 服务器挂载在 80 端口 (Web server on port 80)

// 状态变量 (State Variables)
unsigned long doorOpenTime = 0;
bool isDoorOpen = false;
unsigned long customDoorDuration = 4000; // [新增] 开门持续时间，默认4秒 (Door open duration, default 4s)

bool acIsOn = false; 
unsigned long sunsetTime = 0; 
float outdoorTemp = 0.0;
unsigned long lastWeatherUpdate = 0;
unsigned long lastTouchTime = 0;
unsigned long mqttDisconnectTime = 0; 
unsigned long lastNFCHealthCheck = 0; 
bool isOTAUpdating = false; 
bool isAPMode = false;      // [新增] 标记是否处于配网模式 (Flag for AP mode)

// 动态 NFC 白名单系统 (Dynamic NFC Whitelist System)
#define MAX_NFC_CARDS 30
uint32_t nfcWhitelist[MAX_NFC_CARDS];
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
    button { background: #00bcd4; color: #000; border: none; padding: 10px 20px; font-size: 16px; border-radius: 5px; cursor: pointer; margin-top: 10px; font-weight: bold; width: 100%;}
    button:hover { background: #0097a7; }
    .btn-danger { background: #ff4081; }
    .btn-danger:hover { background: #c2185b; }
    input { width: calc(100% - 20px); padding: 10px; margin: 10px 0; border-radius: 5px; border: 1px solid #333; background: #2c2c2c; color: white; }
    hr { border-color: #333; }
  </style>
</head>
<body>
  <h1>盾级权限 | 灰风控制中枢</h1>
  
  <div class="card">
    <h3>🚪 基础门禁</h3>
    <button onclick="fetch('/open').then(r=>alert('指令已发送'))">立即开门 (默认时长)</button>
  </div>

  <div class="card">
    <h3>⏳ 持久开门</h3>
    <input type="number" id="keepMin" placeholder="输入开启分钟数 (如: 5)" min="1">
    <button class="btn-danger" onclick="setKeepOpen()">启动持久开启</button>
  </div>

  <div class="card">
    <h3>💳 NFC 录入管理</h3>
    <input type="text" id="nfcUid" placeholder="输入 8位 Hex UID (如: F76D163F)">
    <button onclick="addNfc()">写入 NVS 白名单</button>
  </div>

  <div class="card">
    <h3>🌐 网络终端配置</h3>
    <input type="text" id="ssid" placeholder="WiFi 名称 (SSID)">
    <input type="password" id="pwd" placeholder="WiFi 密码 (Password)">
    <button class="btn-danger" onclick="setWifi()">重写网络并重启系统</button>
  </div>

  <script>
    function setKeepOpen() {
      let min = document.getElementById('keepMin').value;
      if(!min) return alert('请输入时间');
      fetch('/keep?min=' + min).then(r=>alert('持久开启协议已激活: ' + min + ' 分钟'));
    }
    function addNfc() {
      let uid = document.getElementById('nfcUid').value;
      if(!uid || uid.length !== 8) return alert('请输入有效的 8位 十六进制 UID');
      fetch('/add_nfc?uid=' + uid).then(r=>alert('UID: ' + uid + ' 已并入核心白名单'));
    }
    function setWifi() {
      let s = document.getElementById('ssid').value;
      let p = document.getElementById('pwd').value;
      if(!s) return alert('必须输入SSID');
      fetch('/set_wifi?ssid=' + encodeURIComponent(s) + '&pass=' + encodeURIComponent(p))
      .then(r=>alert('配置已覆写，系统正在重启...'));
    }
  </script>
</body>
</html>
)rawliteral";

// ================= 函数声明 (Function Declarations) =================
void playLocalFile(const char *filename);
void safeDelay(unsigned long ms);
void triggerLeaveHome();
void processArriveHome(String method, unsigned long customDurationMs = 4000);
void physicallySwitchLight(int id, bool state);
void openDoor();
void closeDoor();
void updateWeather();
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

// ================= Web 服务器路由处理 (Web Server Route Handlers) =================

void handleRoot() {
  // 返回主控页面 (Return main control page)
  server.send(200, "text/html", WEB_HTML);
}

void handleOpen() {
  // Web触发普通开门 (Web triggered normal open)
  processArriveHome("Web-App", 4000);
  server.send(200, "text/plain", "Door Opened");
}

void handleKeepOpen() {
  // Web触发持久开门 (Web triggered persistent open)
  if (server.hasArg("min")) {
    int m = server.arg("min").toInt();
    if (m > 0) {
      processArriveHome("Web-Persistent", m * 60 * 1000);
      server.send(200, "text/plain", "Persistent Mode Activated");
      return;
    }
  }
  server.send(400, "text/plain", "Invalid Parameter"); // 无效参数
}

void handleAddNFC() {
  // Web添加NFC白名单 (Web add NFC whitelist)
  if (server.hasArg("uid")) {
    String uidStr = server.arg("uid");
    uint32_t newUid = strtoul(uidStr.c_str(), NULL, 16);
    
    if (whitelistCount < MAX_NFC_CARDS) {
      nfcWhitelist[whitelistCount++] = newUid;
      prefs.putBytes("nfc_list", nfcWhitelist, whitelistCount * sizeof(uint32_t));
      prefs.putInt("nfc_cnt", whitelistCount);
      Serial.printf("[NVS] Added New NFC: %X\n", newUid);
      server.send(200, "text/plain", "NFC Added to NVS");
      return;
    } else {
      server.send(500, "text/plain", "Whitelist Full!"); // 白名单已满
      return;
    }
  }
  server.send(400, "text/plain", "Missing UID"); // 缺少UID参数
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
  }
}

// ================= 初始化 (Setup) =================
void setup() {
  Serial.begin(115200);
  
  // 1. 初始化 NVS 存储与 NFC 列表 (Init NVS and NFC list)
  initNVSAndNFC();

  // 2. 文件系统 & 音频 (FS & Audio)
  if(!SPIFFS.begin(true)) Serial.println("SPIFFS Fail"); // SPIFFS挂载失败
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(15); 

  // 3. 硬件与引脚 (Hardware & Pins)
  pinMode(TOUCH_PIN, INPUT); 

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  doorServo.setPeriodHertz(50);
  lightServo1.setPeriodHertz(50);
  lightServo2.setPeriodHertz(50);

  // 初始化舵机并立刻卸力断电，防抖保护 (Init servos and detach immediately to prevent jitter)
  doorServo.attach(SERVO_DOOR_PIN, 500, 3000);
  doorServo.write(0); 
  lightServo1.attach(SERVO_LIGHT1, 500, 2500);
  lightServo2.attach(SERVO_LIGHT2, 500, 2500);
  lightServo1.write(ANGLE_NEUTRAL);
  lightServo2.write(ANGLE_NEUTRAL);
  
  delay(600); // 强制等待舵机归位 (Wait for physical homing)
  doorServo.detach();
  lightServo1.detach();
  lightServo2.detach();

  // 传感器 (Sensors)
  SPI.begin(NFC_SCK_PIN, NFC_MISO_PIN, NFC_MOSI_PIN, NFC_SDA_PIN);
  mfrc522.PCD_Init();
  delay(10);
  mfrc522.PCD_DumpVersionToSerial(); 
  
  mySerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(57600);
  
  if (finger.verifyPassword()) {
    Serial.println("Fingerprint Sensor Found!"); // 找到指纹模块
  } else {
    Serial.println("Fingerprint Sensor NOT FOUND :("); // 未找到指纹模块
  }

  // 4. 网络与 Web 服务器 (Network & Web Server)
  setupWiFi();
  setupWebServer();
  
  if (!isAPMode) {
    configTime(8 * 3600, 0, "pool.ntp.org", "time.aliyun.com"); 
    updateWeather();
    
    // MQTT 设置 (MQTT Setup)
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(mqttCallback);
    
    Serial.print("Connecting to Bemfa MQTT..."); // 尝试连接巴法云
    if (client.connect(mqtt_uid)) {
      Serial.println("\n[SUCCESS] MQTT Connected!");
      client.subscribe(topic_cmd);
      client.subscribe(topic_door);
      client.subscribe(topic_keep); // 订阅持久开门主题
      client.publish(topic_door, "online");
      mqttDisconnectTime = 0; 
    } else {
      Serial.print(" Failed! rc=");
      Serial.println(client.state());
      mqttDisconnectTime = millis(); 
    }
  }
  
  Serial.println(">>> System Ready. Type 'E' to enroll fingerprint. <<<"); // 系统就绪，串口输入E录入指纹
  playLocalFile("/boot.mp3");
}

// ================= 主循环 (Main Loop) =================
void loop() {
  // [OTA 优先级保护] (OTA Priority Protection)
  if (isOTAUpdating) {
    ArduinoOTA.handle();
    return; 
  }

  audio.loop(); 
  if(!isAPMode) ArduinoOTA.handle(); 
  server.handleClient(); // [新增] 处理 Web 请求 (Handle Web Requests)

  // 0. 检查串口指令 (Check Serial Commands)
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'E' || c == 'e') {
      enterEnrollMode(); 
    }
  }

  // 如果在 AP 配网模式下，跳过 MQTT 和后续传感逻辑 (Skip logic if in AP mode)
  if (isAPMode) return;

  // MQTT 守护 (MQTT Watchdog)
  if (!client.connected()) {
      reconnectMQTT();
  } else {
      client.loop();
      mqttDisconnectTime = 0; 
  }

  // 1. 天气 (Weather Update)
  if (millis() - lastWeatherUpdate > 1200000) updateWeather();

  // 2. 离家模式 (Leave Home Mode)
  if (digitalRead(TOUCH_PIN) == HIGH) {
    if (millis() - lastTouchTime > 2000) { 
      triggerLeaveHome();
      lastTouchTime = millis();
    }
  }

  // 3. 自动关门检测 - 结合了自定义时长 (Auto Door Close Logic - with custom duration)
  if (isDoorOpen && (millis() - doorOpenTime > customDoorDuration)) {
    closeDoor();
  }

  // 4. 生物识别 & 密码键盘 (Biometrics & Keypad)
  if (!isDoorOpen) {
    checkKeypad(); 
    
    int fpID = getFingerprintID();
    if (fpID != -1) processArriveHome("Fingerprint", 4000);
    
    // [加强版 NFC 看门狗] (Enhanced NFC Watchdog)
    if (millis() - lastNFCHealthCheck > 3000) {
        byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
        if (v == 0x00 || v == 0xFF) {
            Serial.println("[Watchdog] NFC Dead. Resetting..."); // NFC死机，执行复位
            if(audio.isRunning()) audio.stopSong();
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
    byte defaultWhitelist[][4] = {
      {0xF7, 0x6D, 0x16, 0x3F}, 
      {0xE5, 0x6B, 0x1A, 0x06},
      {0x1D, 0x8E, 0x39, 0x68},
      {0xE5, 0x6B, 0x1A, 0x06},
      {0xAD, 0xE9, 0x31, 0x55},
      {0x01, 0x62, 0xAD, 0x1C} 
    };
    int defCount = sizeof(defaultWhitelist) / 4;
    whitelistCount = defCount < MAX_NFC_CARDS ? defCount : MAX_NFC_CARDS;
    
    for(int i=0; i<whitelistCount; i++) {
       // 将 4 byte 合成 uint32_t (Pack 4 bytes into uint32_t)
       nfcWhitelist[i] = (defaultWhitelist[i][0] << 24) | 
                         (defaultWhitelist[i][1] << 16) | 
                         (defaultWhitelist[i][2] << 8)  | 
                         (defaultWhitelist[i][3]);
    }
    prefs.putBytes("nfc_list", nfcWhitelist, whitelistCount * sizeof(uint32_t));
    prefs.putInt("nfc_cnt", whitelistCount);
  } else {
    // 读取已有的白名单 (Read existing whitelist)
    prefs.getBytes("nfc_list", nfcWhitelist, whitelistCount * sizeof(uint32_t));
    Serial.printf("[NVS] Loaded %d NFC cards from storage.\n", whitelistCount); // 从存储成功加载卡片数量
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
    ArduinoOTA.onStart([]() { isOTAUpdating = true; audio.stopSong(); doorServo.detach(); });
    ArduinoOTA.onEnd([]() { isOTAUpdating = false; ESP.restart(); });
    ArduinoOTA.begin();
    Serial.print("IP address: "); Serial.println(WiFi.localIP());
    
  } else {
    Serial.println("\n[WIFI] Failed! Starting AP Provisioning Mode."); // 连接失败，启动配网模式
    isAPMode = true;
    
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    // 配置固定 IP: 192.168.10.10 (Config static IP)
    WiFi.softAPConfig(IPAddress(192,168,10,10), IPAddress(192,168,10,1), IPAddress(255,255,255,0));
    WiFi.softAP("esp32s3-menjin"); // 配网热点名称 (AP SSID)
    
    Serial.println("[AP] Access Point started: esp32s3-menjin"); // 热点已启动
    Serial.println("[AP] IP Address: 192.168.10.10");
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/open", HTTP_GET, handleOpen);
  server.on("/keep", HTTP_GET, handleKeepOpen);
  server.on("/add_nfc", HTTP_GET, handleAddNFC);
  server.on("/set_wifi", HTTP_GET, handleSetWiFi);
  server.begin();
  Serial.println("[WEB] Server Engine Started on port 80."); // Web服务器启动
}

void checkNFC() {
  if (isOTAUpdating || isAPMode) return;
  
  uint32_t currentUid = 0;
  Serial.print("UID:");
  for (byte i = 0; i < mfrc522.uid.size && i < 4; i++) {
    Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(mfrc522.uid.uidByte[i], HEX);
    currentUid = (currentUid << 8) | mfrc522.uid.uidByte[i];
  }
  Serial.println();
  
  bool match = false;
  for (int i = 0; i < whitelistCount; i++) { 
    if (nfcWhitelist[i] == currentUid) {
      match = true;
      break;
    }
  }
  
  if (match) processArriveHome("NFC", 4000);
  else {
    Serial.println("Unknown Card"); // 未知卡片，拒绝访问
    playLocalFile("/error.mp3");
    safeDelay(1000);
  }
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (isOTAUpdating) return; 
  String msg;
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  
  if (String(topic) == topic_cmd && msg == "leave_home") {
      triggerLeaveHome();
  }
  else if (String(topic) == topic_door && msg == "on" && !isDoorOpen) {
      processArriveHome("Remote-MQTT", 4000);
  }
  else if (String(topic) == topic_keep && msg == "on") {
      // 收到长时开门指令，持续 5 分钟 (Keep open 5 mins via MQTT)
      Serial.println("[MQTT] Received Persistent Open Command (5 Mins)"); // 接收到持久开门指令
      processArriveHome("MQTT-KeepOpen", 5 * 60 * 1000);
  }
}

// 统一开门接口 (Unified door open logic)
void processArriveHome(String method, unsigned long customDurationMs) {
  if (isOTAUpdating) return; 
  Serial.println("Arrive via: " + method); // 记录进入方式
  
  customDoorDuration = customDurationMs;   // 设置本次开门维持时间 (Set duration for this open cycle)
  
  playLocalFile("/open.mp3"); 
  if(client.connected()) client.publish(topic_door, "on");
  openDoor();

  time_t now;
  time(&now); 
  if (sunsetTime > 0 && now > sunsetTime) {
    Serial.println("Night Detected. Lights ON."); // 侦测到夜晚，自动开灯
    safeDelay(800); 
    physicallySwitchLight(1, true);
    safeDelay(300);
    physicallySwitchLight(2, true);
  }
  if (outdoorTemp > 30.0 && !acIsOn) {
    Serial.println("Heat Alert. AC ON."); // 侦测到高温，启动空调
    if(client.connected()) client.publish(topic_cmd, "turn_on_ac"); 
    acIsOn = true; 
  }
}

// ================= 其他遗留函数保持不变 (Legacy Functions Remain Unchanged) =================

void safeDelay(unsigned long ms) {
  unsigned long start = millis();
  while(millis() - start < ms) {
    if(isOTAUpdating) { ArduinoOTA.handle(); return; }
    audio.loop(); 
    server.handleClient(); // [新增] 延时期间也要处理网络请求 (Handle web requests during delay)
    if(!isAPMode) ArduinoOTA.handle(); 
    if (!isAPMode && WiFi.status() == WL_CONNECTED && client.connected()) client.loop();
  }
}

void checkKeypad() {
  char key = keypad.getKey();
  if (key) {
    lastKeyTime = millis(); 
    Serial.print("Key Pressed: "); Serial.println(key); // 键盘按下
    if (key == '*') {
      inputCode = ""; Serial.println("Input Cleared"); // 清除输入
    }
    else if (key == '#') {
      if (inputCode == DOOR_PASSWORD) {
        Serial.println("Password Correct!"); // 密码正确
        inputCode = ""; 
        processArriveHome("Keypad Password", 4000);
      } else {
        Serial.println("Password Wrong!"); // 密码错误
        inputCode = ""; 
        playLocalFile("/error.mp3"); 
      }
    }
    else {
      inputCode += key;
      if (inputCode.length() > 10) { inputCode = ""; Serial.println("Input Overflow"); } // 输入溢出
    }
  }
  if (inputCode.length() > 0 && (millis() - lastKeyTime > 10000)) {
    inputCode = ""; Serial.println("Keypad Timeout"); // 输入超时自动清除
  }
}

void reconnectMQTT() {
  if (isOTAUpdating || isAPMode) return;

  if (WiFi.status() != WL_CONNECTED) {
      if (mqttDisconnectTime == 0) mqttDisconnectTime = millis();
      if (millis() - mqttDisconnectTime > 30000) {
         Serial.println("[Watchdog] WiFi Lost for 30s. Restarting WiFi..."); // WiFi断开看门狗重启
         WiFi.disconnect(); WiFi.reconnect(); mqttDisconnectTime = millis(); 
      }
      return; 
  }

  static unsigned long lastRec = 0;
  if (millis() - lastRec > 5000) {
    lastRec = millis();
    if (mqttDisconnectTime == 0) mqttDisconnectTime = millis();
    if (millis() - mqttDisconnectTime > 60000) {
        Serial.println("[Watchdog] MQTT Dead for 60s. Forcing WiFi Reset..."); // MQTT无响应重启WiFi
        WiFi.disconnect(); delay(100); WiFi.reconnect(); mqttDisconnectTime = millis();
        return;
    }
    Serial.print("Attempting MQTT connection...");
    if (client.connect(mqtt_uid)) { 
      Serial.println("[Reconnected]");
      client.subscribe(topic_cmd); client.subscribe(topic_door); client.subscribe(topic_keep);
      client.publish(topic_door, "online"); client.publish(topic_door, isDoorOpen ? "on" : "off");
      mqttDisconnectTime = 0; 
    }
  }
}

void triggerLeaveHome() {
  if (isOTAUpdating) return; 
  Serial.println(">>> MODE: LEAVE HOME <<<"); // 触发离家模式
  playLocalFile("/close.mp3"); 
  physicallySwitchLight(1, false); safeDelay(300); physicallySwitchLight(2, false); 
  if(client.connected()) client.publish(topic_cmd, "turn_off_ac"); 
  acIsOn = false;
}

// [修复] 舵机加入安全互锁，强制等待完成并断电 (Restored safety interlock & detach)
void physicallySwitchLight(int id, bool state) {
  Servo *s = (id == 1) ? &lightServo1 : &lightServo2;
  int pin = (id == 1) ? SERVO_LIGHT1 : SERVO_LIGHT2;
  s->attach(pin, 500, 2500);
  int targetAngle = state ? ANGLE_PUSH_ON : ANGLE_PUSH_OFF;
  s->write(targetAngle); 
  safeDelay(400); 
  s->write(ANGLE_NEUTRAL);
  safeDelay(400);
  s->detach(); 
}

// [修复] 门锁舵机防抖动断电保护逻辑 (Restored door servo anti-jitter logic)
void openDoor() {
  doorServo.attach(SERVO_DOOR_PIN, 500, 3000);
  doorServo.write(300); 
  safeDelay(500); // 强制等待物理动作完成 (Wait for physical movement)
  doorServo.detach(); // 卸载舵机防抖发热 (Detach to prevent jitter)
  isDoorOpen = true; 
  doorOpenTime = millis();
}

void closeDoor() {
  doorServo.attach(SERVO_DOOR_PIN, 500, 3000);
  doorServo.write(0); 
  safeDelay(500); // 强制等待物理动作完成 (Wait for physical movement)
  doorServo.detach(); // 卸载舵机防抖发热 (Detach to prevent jitter)
  isDoorOpen = false;
  if(client.connected()) client.publish(topic_door, "off");
  // 恢复默认的开门维持时长 (Reset duration to default 4s)
  customDoorDuration = 4000; 
}

void updateWeather() {
  if (isOTAUpdating || isAPMode || WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(OWM_URL + CITY + "&appid=" + OWM_API_KEY);
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc; // [修复] 适配 ArduinoJson V7 的新语法 (Fix for ArduinoJson V7 syntax)
    deserializeJson(doc, http.getString());
    outdoorTemp = doc["main"]["temp"]; sunsetTime = doc["sys"]["sunset"];
    lastWeatherUpdate = millis();
  }
  http.end();
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