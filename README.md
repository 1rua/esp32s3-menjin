# 🛡️ ESP32-S3-menjin

## 智能宿舍门禁系统 (Smart Dormitory Access Control)

## 全能型智能门禁解决方案 / All-in-One Smart Access Control Solution

[功能特性](#-主要功能-features) • [硬件清单](#️-硬件清单-hardware-requirements) • [快速开始](#️-快速开始-quick-start) • [使用说明](#-使用说明-usage)

## 📖 简介 (Introduction)

**ESP32-S3-menjin** 是一个基于 ESP32-S3 开发板构建的智能门禁系统。当前版本主实现位于 `esp32-s3-menjin-version3.0/`，集成了 **NFC、指纹、矩阵键盘、Web 管理门户、MQTT 远程控制、AP 配网与 OTA 升级** 等能力。

fork 以来，项目从“依赖源码硬编码配置”的形态，演进为**以 NVS 持久化配置 + 配网状态机 + Web 管理后台**为核心的 3.0 架构：既保留本地门禁优先的可用性，又增强了后期维护和日常管理体验。

## ✨ 主要功能 (Features)

### 🔄 3.0的重要更新

- **移除硬编码 Wi‑Fi**：Wi‑Fi 和 MQTT UID 改为持久化保存在 NVS 中，不再依赖源码里写死的默认网络配置。
- **新增 AP 配网状态机**：首次无 Wi‑Fi 配置时自动进入 AP 配网；运行中也可通过 **长按 BOOT 5 秒** 强制进入配网门户。
- **新增 Web 管理门户**：可通过浏览器执行开门、写入 NFC、管理指纹、配置网络、管理长期 PIN、生成/撤销临时 PIN、修改管理员账号密码。
- **新增网页指纹管理**：支持在控制台发起录入、查看列表、重命名、删除指纹模板，无需再依赖串口完成日常维护。
- **改进指纹录入算法**：录入流程改为 **2 次建模 + 2 次交叉验算**，共 `4` 次按压校验，降低错误模板写入概率。
- **临时 PIN 改为按时长生成**：基于设备当前时间计算过期时间，避免浏览器本地时区干扰。
- **新增设备时间状态接口**：联网后自动进行 NTP 校时，临时 PIN 仅在设备时间有效时可用。
- **新增自定义分区表**：为 16MB Flash 设备提供 **双 OTA 分区 + 大 SPIFFS 分区**，便于 OTA 和本地音频文件共存。
- **保留本地门禁优先**：即使没有联网或主动跳过自动配网，NFC / 指纹 / 键盘等本地开门链路仍可继续工作。

### 🔐 多重安全解锁

- **NFC 识别**：支持 RC522 模块，NFC 白名单保存在 NVS 中，并可通过 Web 门户动态添加。
- **指纹识别**：集成 ZW101 / Adafruit 协议兼容指纹模块，支持 Web 控制台发起录入、查看、重命名、删除；录入流程采用 `4` 次按压校验。
- **密码解锁**：支持 4x4 矩阵键盘。
  - 长期 PIN：`4~10` 位数字
  - 临时 PIN：固定 `4` 位数字，带过期时间
- **远程控制**：通过 MQTT (巴法云 Bemfa) 接入远程控制链路，收到指定主题消息后自动开门。

### 🤖 机械与自动化

- **舵机驱动优化**：门锁舵机采用 `500-3000us` 范围驱动，适配大扭矩数字舵机。
- **自动关门**：开门后默认约 `4` 秒自动回位关门。
- **本地优先运行**：即使未联网、断网或主动跳过自动配网，本地门禁仍可正常工作。

### 🔊 交互体验

- **语音反馈**：支持 I2S 音频输出，可从 SPIFFS 播放 `boot/open/error` 等 MP3 提示音。
- **Web 控制台**：浏览器访问设备地址即可执行开门、NFC 写入、PIN 管理和网络配置。
- **设备时间可视化**：控制台可显示当前校时状态、本地时间与管理员默认密码状态。

### 🌐 网络与维护

- **AP 热点配网**：AP 名称默认 `esp32s3-menjin`，默认门户地址为 `192.168.10.10`。
- **运行时强制配网**：长按开发板 **BOOT 键 5 秒** 可重新进入配网流程。
- **管理员认证保护**：配网页与控制页共用管理员认证。
- **OTA 升级**：联网后可通过 Arduino OTA 无线升级。
- **时间同步**：联网后自动进行 NTP 校时，临时 PIN 功能依赖有效系统时间。

## 🛠️ 硬件清单 (Hardware Requirements)

| **组件** | **型号/备注** | **连接引脚 (GPIO)** |
| --- | --- | --- |
| **主控** | ESP32-S3 DevKitC / 同类 16MB Flash 板卡 | - |
| **门锁舵机** | 大扭矩舵机 | Pin 9 |
| **NFC 模块** | RC522 (SPI 接口) | SDA:10, SCK:12, MOSI:11, MISO:13, RST:40 |
| **指纹模块** | ZW101 / 兼容模块 (UART 接口) | TX:17, RX:18 |
| **音频模块（可选）** | MAX98357A (I2S 接口) | DIN:6, BCLK:5, LRC:4 |
| **矩阵键盘（可选）** | 4x4 Keypad | Rows: 8,15,16,21 / Cols: 1,2,3,7 |

## 📦 依赖库 (Dependencies)

请在 Arduino IDE 库管理器中搜索并安装以下库：

- `WiFi`, `WebServer`, `Preferences`, `SPIFFS`, `FS`, `WiFiUdp`, `ArduinoOTA` (ESP32 内置)
- `PubSubClient` (by Nick O'Leary) - 用于 MQTT
- `MFRC522` (by GithubCommunity) - 用于 NFC
- `Adafruit Fingerprint Sensor Library` (by Adafruit) - 用于指纹
- `ESP32Servo` (by Kevin Harrington) - 用于舵机控制
- `ESP32-audioI2S` (by Schreibfaul1) - 用于音频播放
- `Keypad` (by Mark Stanley, Alexander Brevig) - 用于矩阵键盘

## ⚙️ 快速开始 (Quick Start)

### 1. 打开工程

请使用 Arduino IDE 打开：

```text
esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino
```

### 2. 编译产物

仓库当前已导出 Arduino 构建产物，位于：

```text
esp32-s3-menjin-version3.0/build/esp32.esp32.esp32s3/
```

常用文件包括：

- `esp32-s3-menjin-version3.0.ino.bin`
- `esp32-s3-menjin-version3.0.ino.bootloader.bin`
- `esp32-s3-menjin-version3.0.ino.merged.bin`
- `esp32-s3-menjin-version3.0.ino.partitions.bin`

如需直接烧录，可结合该目录下的 `flash_args` 与分区文件使用。

### 3. 使用当前 3.0 工程结构

```text
esp32-s3-menjin-version3.0/
├─ esp32-s3-menjin-version3.0.ino
├─ src/
│  ├─ access_control/
│  │  ├─ access_control.h
│  │  └─ access_control.cpp
│  ├─ audio_feedback/
│  │  ├─ audio_feedback.h
│  │  └─ audio_feedback.cpp
│  ├─ device_config/
│  │  ├─ device_config.h
│  │  └─ device_config.cpp
│  ├─ door_controller/
│  │  ├─ door_controller.h
│  │  └─ door_controller.cpp
│  ├─ fingerprint_access/
│  │  ├─ fingerprint_access.h
│  │  └─ fingerprint_access.cpp
│  ├─ keypad_access/
│  │  ├─ keypad_access.h
│  │  └─ keypad_access.cpp
│  ├─ nfc_access/
│  │  ├─ nfc_access.h
│  │  └─ nfc_access.cpp
│  ├─ provisioning/
│  │  ├─ provisioning.h
│  │  └─ provisioning.cpp
│  ├─ runtime_services/
│  │  ├─ runtime_services.h
│  │  └─ runtime_services.cpp
│  └─ web_portal/
│     ├─ web_portal.h
│     └─ web_portal.cpp
├─ build/
│  └─ esp32.esp32.esp32s3/
│     ├─ esp32-s3-menjin-version3.0.ino.bin
│     ├─ esp32-s3-menjin-version3.0.ino.bootloader.bin
│     ├─ esp32-s3-menjin-version3.0.ino.merged.bin
│     ├─ esp32-s3-menjin-version3.0.ino.partitions.bin
│     └─ ...
├─ libraries/
└─ partitions.csv
```

### 3. 烧录建议

- 建议使用 **16MB Flash** 的 ESP32-S3 设备。
- 编译时使用项目自带的 `partitions.csv`。
- 当前分区表为 **双 OTA 分区 + 大 SPIFFS 分区**。

## 🔧 配置指南 (Configuration)

与旧版本不同，当前版本**不再要求你在源码里硬编码 Wi‑Fi 与 MQTT**。首次烧录后，大部分配置通过 Web 门户完成。

### 首次启动逻辑

- **没有已保存 Wi‑Fi**：设备会进入 AP 配网模式。
- **已保存 Wi‑Fi**：设备会尝试联网并进入正常运行。
- **需要强制重新配网**：长按 **BOOT** 键 `5` 秒。

### AP 配网模式

- AP SSID：`esp32s3-menjin`
- AP 地址：`192.168.10.10`
- 浏览器访问设备根路径即可进入配网页面
- 配网页面同样需要管理员认证

### 默认管理员认证

- 用户名：`admin`
- 密码：`esp32s3-menjin`

> 强烈建议首次登录后立即在 Web 门户中修改管理员密码。

### 门户可配置内容

- Wi‑Fi SSID
- Wi‑Fi 密码
- Bemfa MQTT UID
- 是否跳过自动配网并进入本地运行
- 管理员用户名与密码
- 长期 / 临时 PIN
- NFC 白名单
- 指纹录入、重命名、删除

## 🧩 默认值与重要参数 (Defaults)

### 门禁与认证

- 默认长期 PIN：`11451`
- 默认管理员账号：`admin`
- 默认管理员密码：`esp32s3-menjin`
- 长期 PIN 数量上限：`10`
- 临时 PIN 数量上限：`10`

### OTA

- OTA Hostname：`Mech-Master-S3`
- OTA 默认密码：`esp32s3-menjin`

> Web 管理员密码与 OTA 密码是两套独立凭据；修改 Web 密码不会自动修改 OTA 密码。

### MQTT

- 服务器：`mqtt.bemfa.com:9501`
- 默认主题：`homedoor006`
- 需要在门户中填写 MQTT UID 后才会连接

## 📝 更新日志 (Changelog)

### v3.0 - Current

- **新增**：NVS 持久化 Wi‑Fi / MQTT UID / 管理员凭据 / PIN / NFC 白名单。
- **新增**：AP 配网状态机与长按 BOOT 5 秒强制配网。
- **新增**：Web 管理门户，支持开门、配网、NFC 写入、指纹管理、PIN 管理和管理员账号密码修改。
- **新增**：指纹 Web 管理接口，支持录入进度查询、重命名、删除。
- **新增**：长期 PIN / 临时 PIN 分离管理，临时 PIN 按时长生成并自动过期。
- **新增**：NTP 校时与时间状态接口。
- **新增**：16MB Flash 自定义分区表，支持双 OTA 与大 SPIFFS。
- **调整**：指纹录入流程升级为 `4` 次按压校验（2 次建模 + 2 次交叉验算）。
- **调整**：模块代码整理到 `src/` 目录，并导出 `build/esp32.esp32.esp32s3/` 编译产物。
- **调整**：移除旧版 README 中依赖源码硬编码 Wi‑Fi / WiFiManager / 天气接口的描述。

## 🚀 使用说明 (Usage)

### 1. 指纹录入与管理

推荐通过 Web 门户完成：

- 登录控制台。
- 在 **指纹管理** 中输入名称并点击 **开始录入**。
- 按页面提示完成 `4` 次按压：前 `2` 次用于建模，后 `2` 次用于交叉验算。
- 录入完成后，可直接在页面中查看列表、重命名或删除指纹。

兼容保留串口触发录入：

- 打开串口监视器（波特率 `115200`）。
- 等待提示：`Type 'E' to enroll fingerprint`。
- 输入 `E` 或 `e` 后开始录入当前空闲 ID。

### 2. NFC 录入

推荐通过 Web 门户完成：

- 登录控制台。
- 在 **NFC 录入管理** 中输入 UID 的十六进制字符串。
- UID 必须为偶数位，且长度对应 `4~10` 字节。
- 提交后写入 NVS 白名单，无需重新烧录。

### 3. 键盘 PIN 开门

- 输入数字
- `#` 提交
- `*` 清空输入
- 超过 `10` 秒未完成输入会自动清空
- 连续错误 `5` 次锁定 `30` 秒

### 4. 长期 PIN 管理

- 通过 Web 门户新增、修改、删除。
- PIN 必须为 `4~10` 位纯数字。
- 默认会生成一个长期 PIN：`11451`。

### 5. 临时 PIN 管理

- 通过 Web 门户输入有效时长（分钟）生成。
- 临时 PIN 固定为 `4` 位数字。
- 设备必须已经完成 NTP 校时，否则无法生成临时 PIN。
- 过期后会自动失效并在后续访问时清理。

### 6. MQTT 远程开门

- 在门户中填写 MQTT UID。
- 设备连接成功后会订阅 `homedoor006`。
- 收到消息 `on` 时执行开门。
- 设备会回报 `online` / `on` / `off` 状态。

### 7. OTA 无线升级

- 当电脑与 ESP32 处于同一 Wi‑Fi 下时。
- Arduino IDE 端口选择 `Network Ports` 下的设备。
- 使用 OTA 密码 `esp32s3-menjin`。
- 点击上传即可，无需插线。

## 💾 存储说明 (Storage)

以下数据会保存在 NVS：

- Wi‑Fi 配置
- MQTT UID
- 跳过自动配网标记
- 管理员账号与密码哈希
- 长期 PIN / 临时 PIN
- NFC 白名单

音频文件使用 SPIFFS 存储；当前分区表为 16MB Flash 预留了双 OTA 分区和较大的 SPIFFS 空间。

## 📌 注意事项 (Notes)

- 如果你没有连接音频模块或没有上传 SPIFFS 音频文件，设备仍可运行，只是不会播放提示音。
- 临时 PIN 依赖系统时间；若设备尚未联网校时，该功能会不可用。
- 如需修改 OTA 密码、MQTT 主题、AP 名称等固定参数，请编辑主程序中的常量定义。
- Web 门户和配网门户共用同一套管理员认证，部署前请务必修改默认密码。

---

Created by 豆浆白倒
