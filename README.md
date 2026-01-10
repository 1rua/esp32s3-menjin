🛡️ ESP32-S3 Mech Master (智能机械管家)

Version: 2.01 (Official Stable)
Author: Fourth Crisis & Grey Goo

这是一个基于 ESP32-S3 构建的高级智能门禁与家居控制系统。它不仅仅是一个锁，更是一个集成了多模态生物识别、云端控制和自动化逻辑的中心枢纽。

✨ 主要功能 (Features)

🔐 多重解锁方式:

NFC 识别: 支持 RC522 模块，具备硬件看门狗（自动复位防死机）。

指纹识别: 集成 AS608 光学指纹模块，支持高精度比对。

密码解锁: 4x4 矩阵键盘，支持密码输入与错误音效。

远程控制: 通过 MQTT (巴法云) 接入智能家居平台，支持远程开门。

🤖 机械控制:

控制大扭矩舵机进行物理开锁。

控制额外的舵机用于物理开关灯光（可选）。

🔊 交互反馈:

I2S 音频驱动 (MAX98357A)，播放本地 MP3 提示音（开门、报错、欢迎语）。

🌐 网络与维护:

WiFi 自动重连: 具备 WiFi 和 MQTT 双重连接守护。

OTA 升级: 支持局域网无线固件更新，无需插线即可维护。

天气同步: 获取 OpenWeatherMap 数据，辅助自动化决策（如高温自动开空调）。

🛠️ 硬件清单 (Hardware Requirements)

组件

型号/备注

连接引脚 (GPIO)

主控

ESP32-S3 DevKitC

-

NFC 模块

RC522 (SPI)

SDA:10, SCK:12, MOSI:11, MISO:13, RST:40

指纹模块

AS608 (UART)

TX:17, RX:18

音频模块

MAX98357A (I2S)

DIN:6, BCLK:5, LRC:4

矩阵键盘

4x4 Keypad

Rows: 8,15,16,21

门锁舵机

20KG 数字舵机

Pin 9

灯光舵机

SG90 (x2)

Pin 48, 47

触摸传感器

TTP223

Pin 14

📦 依赖库 (Dependencies)

请在 Arduino IDE 或 PlatformIO 中安装以下库：

WiFi (Built-in)

HTTPClient (Built-in)

PubSubClient (by Nick O'Leary) - MQTT

ArduinoJson (by Benoit Blanchon)

MFRC522 (by GithubCommunity)

Adafruit Fingerprint Sensor Library (by Adafruit)

ESP32Servo (by Kevin Harrington)

ESP32-audioI2S (by Schreibfaul1)

Keypad (by Mark Stanley, Alexander Brevig)

ArduinoOTA (Built-in)

⚙️ 配置指南 (Configuration)

在烧录前，请修改代码顶部的 用户配置区：

// 1. WiFi 设置
const char* ssid        = "YOUR_WIFI_SSID";
const char* password    = "YOUR_WIFI_PASSWORD";

// 2. 解锁密码
const String DOOR_PASSWORD = "YOUR_PASSWORD"; // 默认: 11451

// 3. API & MQTT 设置
String OWM_API_KEY      = "YOUR_OWM_KEY";
const char* mqtt_uid    = "YOUR_BEMFA_UID";


📝 更新日志 (Changelog)

v2.01: 修复 RC522 长期运行掉线问题，增加 NFC 硬件看门狗（超时自动复位）。

v2.0: 增加 4x4 矩阵键盘支持，允许密码解锁。

v1.99: 增加局域网 OTA 功能。

v1.2: 优化指纹和 NFC 识别率。

v1.1: 修复 WiFi 断连后巴法云无法重连的 Bug。

🚀 使用说明

指纹录入: 在串口监视器输入 E 进入录入模式，按提示操作。

NFC 录入: 修改代码中的 Whitelist 数组，填入卡片 UID。

OTA 升级: 电脑与 ESP32 处于同一局域网时，在 Arduino IDE 端口选择网络端口进行烧录。

Created by Fourth Crisis & Grey Goo
