# 🛡️ ESP32-S3-menjin

### 智能宿舍门禁系统 (Smart Dormitory Access Control)

**全能型智能门禁解决方案 / All-in-One Smart Access Control Solution**

[功能特性](https://comfortable-guanaco-11a.notion.site/2ddd628e6e7a80428223e2b6f574119f?v=2eed628e6e7a80f7b923000ce6e442e5&source=copy_link) • [硬件清单](https://comfortable-guanaco-11a.notion.site/2ddd628e6e7a80428223e2b6f574119f?v=2eed628e6e7a80f7b923000ce6e442e5&source=copy_link) • [安装指南](https://comfortable-guanaco-11a.notion.site/2ddd628e6e7a80428223e2b6f574119f?v=2eed628e6e7a80f7b923000ce6e442e5&source=copy_link) • [使用说明](https://comfortable-guanaco-11a.notion.site/2ddd628e6e7a80428223e2b6f574119f?v=2eed628e6e7a80f7b923000ce6e442e5&source=copy_link)

## 📖 简介 (Introduction)

**ESP32-S3-menjin**是一个基于 ESP32-S3 开发板构建的高级智能门禁系统。它集成了多模态生物识别（指纹、NFC）、密码解锁、云端控制（MQTT）以及本地音频反馈。

不仅仅是开门，它还具备**网络自愈**、**OTA 无线升级**和**硬件看门狗**机制，确保系统长期稳定运行，即使在网络波动或硬件异常时也能自动恢复。

> ⚠️ 声明：核心代码逻辑由 Gemini 3 Pro (灰风) 生成与优化，经由人类指挥官 豆浆白倒 进行实地调试与部署。
> 

## ✨ 主要功能 (Features)

### 🔐 多重安全解锁

- **NFC 识别**: 支持 RC522 模块，内置**硬件看门狗**，解决长期运行后模块掉线问题。
- **指纹识别**: 集成 ZW101 电容指纹模块，优化了**双重录入模式**，大幅提高识别准确率。
- **密码解锁**: 支持 4x4 矩阵键盘，输入密码即可开门（默认密码 `11451`）。
- **远程控制**: 通过 MQTT (巴法云 Bemfa) 接入智能家居平台，支持手机远程一键开门，小爱同学等智能音箱语音控制。

### 🤖 机械与自动化

- **强力驱动**: 针对 20KG 大扭矩数字舵机优化，采用 `500-3000us` 宽脉宽驱动，杜绝卡死。
- **灯光控制**: 额外支持 2 路舵机控制物理开关灯（可选）。
- **自动关门**: 开门后自动延时关闭，防止尾随。

### 🔊 交互体验

- **语音反馈**: 集成 I2S 音频驱动 (MAX98357A)，播放本地 MP3 提示音（"欢迎回家"、"验证失败"等）（可选，仅预留未测试）。
- **离家模式**: 通过触摸按键一键触发“离家模式”，自动关灯、关空调。

### 🌐 网络与维护

- ~~**双重配网**: 支持代码硬编码 WiFi + **AP 热点配网** (WiFiManager)。~~【未（慢）来（慢）更（摸）新（鱼）】
- **网络自愈**: 智能 Watchdog 监测 MQTT 连接，断网自动重连，死锁自动重启 WiFi 协议栈。
- **OTA 升级**: 支持局域网无线固件更新，维护无需拆机插线。
- ~~**天气同步**: 获取 OpenWeatherMap 数据，支持高温自动开空调逻辑。~~（直接home assistant或米家自动化就行）

## 🛠️ 硬件清单 (Hardware Requirements)

| **组件** | **型号/备注** | **连接引脚 (GPIO)** |
| --- | --- | --- |
| **主控** | ESP32-S3 DevKitC (16MB Flash Recommended) | - |
| **NFC 模块** | RC522 (SPI 接口) | SDA:10, SCK:12, MOSI:11, MISO:13, RST:40 |
| **指纹模块** | ZW101 (UART 接口) | TX:17, RX:18 |
| **音频模块（可选）** | MAX98357A (I2S 接口) | DIN:6, BCLK:5, LRC:4 |
| **矩阵键盘（可选）** | 4x4 Keypad | Rows: 8,15,16,21 / Cols: 1,2,3,7 |
| **门锁舵机** | 20KG 数字舵机 (180°) | Pin 9 |
| **灯光舵机（可选）** | SG90 (x2) | Pin 48, 47 |
| **触摸传感器（可选）** | TTP223 (电容式) | Pin 14 |

## 📦 依赖库 (Dependencies)

请在 Arduino IDE 库管理器中搜索并安装以下库：

- `WiFi`, `HTTPClient`, `WebServer`, `ArduinoOTA` (ESP32 内置)
- `PubSubClient` (by Nick O'Leary) - 用于 MQTT
- `ArduinoJson` (by Benoit Blanchon) - 用于解析天气数据
- `MFRC522` (by GithubCommunity) - 用于 NFC
- `Adafruit Fingerprint Sensor Library` (by Adafruit) - 用于指纹
- `ESP32Servo` (by Kevin Harrington) - 用于舵机控制
- `ESP32-audioI2S` (by Schreibfaul1) - 用于音频播放
- `Keypad` (by Mark Stanley, Alexander Brevig) - 用于矩阵键盘
- `WiFiManager` (by tzpu) - 用于 AP 配网

## ⚙️ 配置指南 (Configuration)

在烧录前，请修改代码顶部的 `用户配置区`：

```
// ================= 🌐 用户配置区 =================

// 1. WiFi 设置 (优先连接，失败则开启 AP 配网)
const char* ssid        = "YOUR_WIFI_SSID";
const char* password    = "YOUR_WIFI_PASSWORD";

// 2. 解锁密码
const String DOOR_PASSWORD = "11451"; // 修改为你想要的密码

// 3. API & MQTT 设置
String OWM_API_KEY      = "YOUR_OWM_KEY"; // OpenWeatherMap API Key
const char* mqtt_uid    = "YOUR_BEMFA_UID"; // 巴法云私钥

```

## 📝 更新日志 (Changelog)

<details>

<summary>点击展开查看详细日志</summary>

### v2.02 (Stable) - *Current*

- **修复**: RC522 长期运行掉线问题，增加 NFC 硬件看门狗（超时自动复位）。
- **优化**: 舵机加入安全互锁，防止连续指令导致的抖动。

### v2.0

- **新增**: 4x4 矩阵键盘支持，允许密码解锁。

### v1.99

- **新增**: 局域网 OTA 无线升级功能。
-~~ **新增**: AP 配网模式。~~

### v1.2

- **优化**: 改进指纹录入逻辑（多次扫描直到成功），大幅提高识别率。
- **修复**: NFC 识别不稳定的问题。

### v1.1

- **修复**: WiFi 断连后巴法云 (MQTT) 无法自动重连的 Bug。

</details>

## 🚀 使用说明 (Usage)

### 1. 指纹录入

- 连接 ESP32 到电脑，打开串口监视器 (波特率 115200)。
- 在输入框输入 `E` 并发送，输入指纹编号。
- 按照提示：按压手指 -> 移开轻微移位 -> 再次按压，直到显示 **Stored!**。

### 2. NFC 录入

- 修改代码中的 `Whitelist` 数组，填入卡片的 4 字节 UID（十六进制）。
- 重新烧录或通过 OTA 更新。

### 3. OTA 无线升级

- 当电脑与 ESP32 处于同一 WiFi 下时。
- Arduino IDE 端口选择 `Network Ports` 下的设备。
- 点击上传即可，无需插线。

<div align="center">

Created by 豆浆白倒 (Fourth Crisis)

Powered by Gemini 3 Pro Code Gen

</div>
