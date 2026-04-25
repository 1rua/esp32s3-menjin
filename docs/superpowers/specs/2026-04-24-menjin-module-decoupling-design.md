# 门禁固件 6 模块解耦设计

- 日期：2026-04-24
- 目标工程：`esp32-s3-menjin-version3.0`
- 主文件：[`esp32-s3-menjin-version3.0.ino`](../../../../esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino)

## 1. 背景

当前主入口 [`esp32-s3-menjin-version3.0.ino`](../../../../esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino) 同时承担了系统装配、硬件初始化、门锁执行、音频反馈、NFC 白名单管理、指纹识别与录入、键盘输入状态机、WiFi/MQTT/OTA/校时维护等多种职责。

其中最明显的结构问题有两个：

1. 主循环和延时保活存在重复运行时维护逻辑：[`esp32-s3-menjin-version3.0.ino:452-467`](../../../../esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino#L452-L467) 与 [`esp32-s3-menjin-version3.0.ino:823-844`](../../../../esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino#L823-L844)
2. 多个硬件相关状态机直接堆积在 `.ino` 中，导致职责边界模糊，后续维护和扩展风险上升

本次重构目标是在不改变现有功能、不修改现有 Web API 路径、不改变现有 NVS key 和行为语义的前提下，将以下 6 个模块从主 `.ino` 中解耦：

- `runtime_services`
- `door_controller`
- `audio_feedback`
- `fingerprint_access`
- `keypad_access`
- `nfc_access`

## 2. 目标与非目标

### 2.1 目标

- 让主 `.ino` 退化为系统装配与顶层调度入口
- 将重复的后台维护逻辑统一收口到运行时模块
- 为门锁、音频、指纹、键盘、NFC 建立独立、可理解的边界
- 保证每个迁移阶段都可单独编译并保持现有行为不变
- 降低后续新增功能时对主 `.ino` 的修改幅度

### 2.2 非目标

- 不重写现有访问控制逻辑 [`access_control`](../../../../esp32-s3-menjin-version3.0/access_control/)
- 不调整现有 Web 门户功能与路由 [`web_portal`](../../../../esp32-s3-menjin-version3.0/web_portal/)
- 不引入 RTOS、多线程、消息总线或事件框架
- 不修改现有存储结构、默认数据或启动行为
- 不在本次重构中顺带清理无关代码

## 3. 总体架构

重构后的 [`esp32-s3-menjin-version3.0.ino`](../../../../esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino) 仅保留三类职责：

1. **系统装配**：在 `setup()` 中创建对象、初始化模块、建立模块依赖
2. **顶层调度**：在 `loop()` 中按固定顺序调用各模块的 `tick()`
3. **少量 glue code**：例如 Web 门户上下文组装 [`esp32-s3-menjin-version3.0.ino:356-371`](../../../../esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino#L356-L371)

依赖方向固定为：

- `runtime_services` 负责网络、OTA、校时、WebServer 驱动和配网按钮逻辑
- `door_controller` 只负责门锁机械动作和门状态
- `audio_feedback` 只负责提示音播放与音频 loop 驱动
- `fingerprint_access`、`keypad_access`、`nfc_access` 只负责对应输入源与状态机
- `access_control` 继续作为密码/权限规则模块存在
- Web 门户继续通过上下文回调访问门锁控制和 NFC 管理，而不是直接持有底层硬件细节

## 4. 模块边界

### 4.1 runtime_services

**职责**

- 收口运行时后台服务逻辑
- 统一主循环和阻塞等待期间的系统保活路径
- 管理 WiFi、MQTT、OTA、校时、WebServer servicing、强制配网按钮

**迁移内容**

- `processForcedProvisioningButton`
- `ensureWebServerReady`
- `maintainWiFiConnection`
- `maintainMqttConnection`
- `ensureTimeSyncStarted`
- `ensureOtaReady`
- `ArduinoOTA.handle()` 的调度逻辑
- 已联网状态下的临时 PIN 过期清理
- `safeDelay()` 中与后台维护相关的重复逻辑

**关键动机**

首先解决 [`esp32-s3-menjin-version3.0.ino:452-467`](../../../../esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino#L452-L467) 与 [`esp32-s3-menjin-version3.0.ino:823-844`](../../../../esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino#L823-L844) 的重复问题，为后续 5 个模块提供稳定调度骨架。

**建议接口**

- `begin(...)`
- `tick()`
- `serviceFor(ms)` 或 `pumpUntil(timeoutMs)`
- `isOtaUpdating()`
- `isWebServerReady()`
- `isTimeSynced()`
- `currentEpochSeconds()`
- `currentLocalTimeString()`

### 4.2 door_controller

**职责**

- 管理舵机开门、关门、自动关门和门状态
- 管理门锁执行阶段的脉冲保持与自动 detach

**迁移内容**

- `authorizeDoorOpen`
- `openDoor`
- `closeDoor`
- `scheduleDoorServoDetach`
- `maintainDoorServoPulse`
- `isDoorOpen`
- `doorOpenTime`
- `customDoorDuration`
- `doorServoPulseActive`
- `doorServoDetachAt`

**建议接口**

- `begin(Servo&)`
- `requestOpen(const char* source)`
- `tick()`
- `closeNow()`
- `isOpen()`
- `setDefaultDuration(ms)`

**边界约束**

门锁模块只负责机械动作和门状态。播放音效、打印日志、MQTT 发布状态等副作用通过回调或外部薄封装处理，不塞回门锁核心逻辑。

### 4.3 audio_feedback

**职责**

- 管理本地音频反馈
- 隐藏具体音频文件路径
- 提供统一播放入口和 `audio.loop()` 调度

**迁移内容**

- `playLocalFile`
- `audio.loop()` 的统一驱动入口
- 与开机音、开门音、错误音相关的调用包装

**建议接口**

- `begin(Audio&)`
- `tick()`
- `playBoot()`
- `playOpen()`
- `playError()`
- `stop()`
- `isRunning()`

### 4.4 fingerprint_access

**职责**

- 封装指纹识别
- 封装录入状态机和串口触发录入流程

**迁移内容**

- `FingerprintEnrollPhase`
- `startFingerprintEnrollMode`
- `maintainFingerprintEnroll`
- `resetFingerprintEnrollState`
- `getFingerprintID`
- 串口录入触发和录入输入缓冲

**建议接口**

- `begin(HardwareSerial&, Adafruit_Fingerprint&)`
- `tick()`
- `pollMatch()`
- `startEnrollFromConsole()`
- `isBusy()`

**边界约束**

指纹模块负责识别和录入，不直接控制门锁。认证成功后由主调度或授权协调层决定是否调用门锁模块开门。

### 4.5 keypad_access

**职责**

- 管理矩阵键盘输入缓冲
- 管理输入超时和失败次数锁定
- 调用 [`verifyKeypadCode`](../../../../esp32-s3-menjin-version3.0/access_control/access_control.h#L61) 完成 PIN 验证

**迁移内容**

- `checkKeypad`
- `isKeypadLocked`
- `resetKeypadLockIfExpired`
- `inputCode`
- `lastKeyTime`
- `keypadFailedAttempts`
- `keypadLockoutUntil`

**建议接口**

- `begin(Keypad&, Preferences&, AccessControlState&)`
- `tick(uint32_t nowEpoch, bool timeSynced)`
- `isLocked()`
- `clearInput()`

**边界约束**

键盘模块不持有门锁控制权；它只输出“认证成功/失败”和来源信息，例如 `Temporary PIN` 或 `Keypad Password`。

### 4.6 nfc_access

**职责**

- 管理 NFC 白名单数据结构和 NVS 持久化
- 管理 NFC UID 解析、白名单匹配、刷卡识别和读卡器自恢复

**迁移内容**

- `NfcCard`
- `initNVSAndNFC`
- `checkNFC`
- `parseUidHex`
- `isDuplicateNfcCard`
- `clearNfcWhitelist`
- `persistNfcWhitelist`
- `addNfcCardFromWeb`
- `nfcWhitelist`
- `whitelistCount`

**建议接口**

- `begin(Preferences&, MFRC522&)`
- `tick()`
- `addCardFromHex(const String&, String& message)`
- `containsPresentedCard()`
- `resetReaderIfNeeded()`

**边界约束**

本次重构中，NFC 白名单存储与运行时刷卡识别保留在同一模块，不继续细拆，以控制迁移复杂度。

## 5. 分阶段执行计划

### Phase 1：抽离 runtime_services

**目标**

- 建立统一后台服务骨架
- 消除主循环和 `safeDelay()` 中的重复运行时维护逻辑

**步骤**

1. 新建 `runtime_services/` 模块
2. 迁移联网、MQTT、OTA、校时、WebServer servicing、强制配网按钮逻辑
3. 引入统一后台服务函数，供 `loop()` 和阻塞等待场景复用
4. 将 `safeDelay()` 改为薄封装，或直接转为调用 `runtime_services` 提供的保活接口

**验收标准**

- 编译通过
- 原有联网、MQTT、OTA、Web 门户行为保持不变
- [`esp32-s3-menjin-version3.0.ino:823-844`](../../../../esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino#L823-L844) 中的大部分后台维护代码不再直接存在于 `.ino`

### Phase 2：抽离 audio_feedback 与 door_controller

**目标**

- 先迁移最独立的执行层与反馈层

**步骤**

1. 新建 `audio_feedback/`，迁移音频播放与 loop 驱动
2. 新建 `door_controller/`，迁移门开关、自动关门、舵机 detach 管理
3. 保持外部副作用（日志、MQTT、音效触发）接口最小化，不做额外抽象扩张

**验收标准**

- 编译通过
- 开机音、开门音、错误音行为不变
- 开门、自动关门、舵机 detach 行为不变

### Phase 3：抽离 fingerprint_access

**目标**

- 迁移最长的本地输入状态机

**步骤**

1. 新建 `fingerprint_access/`
2. 迁移录入状态机、录入输入处理、识别逻辑
3. 暴露 `isBusy()`，供主循环判断是否暂停其他本地认证源

**验收标准**

- 编译通过
- 指纹识别仍可触发开门
- 串口录入流程行为不变
- 录入期间仍会阻止普通本地认证流程冲突

### Phase 4：抽离 keypad_access

**目标**

- 迁移本地 PIN 输入状态机

**步骤**

1. 新建 `keypad_access/`
2. 迁移输入缓存、超时、错误次数锁定逻辑
3. 通过现有 [`access_control`](../../../../esp32-s3-menjin-version3.0/access_control/) 完成 PIN 验证

**验收标准**

- 编译通过
- 键盘输入、输入清空、超时、锁定行为不变
- 长期 PIN 与临时 PIN 均保持现有验证行为

### Phase 5：抽离 nfc_access

**目标**

- 迁移 NFC 白名单与刷卡逻辑

**步骤**

1. 新建 `nfc_access/`
2. 迁移白名单数据结构和 NVS 持久化
3. 保持现有存储迁移逻辑与 UID 格式兼容逻辑
4. Web 门户继续通过回调调用 NFC 新模块的添加接口

**验收标准**

- 编译通过
- 既有白名单数据能继续使用
- 新增 NFC 和刷卡识别行为不变
- 读卡器异常恢复逻辑保持可用

### Phase 6：收尾并将主 ino 缩减为装配层

**目标**

- 清理主 `.ino` 中剩余实现细节
- 固化顶层调度顺序与模块组合方式

**步骤**

1. 删除已迁移函数声明与状态变量
2. 保留必要的全局对象与上下文装配代码
3. 整理 `loop()` 为明确的模块调度顺序
4. 复核 Web 门户上下文边界，确保其只依赖模块公开接口

**验收标准**

- 主 `.ino` 的职责集中为装配与调度
- 模块间依赖方向符合本设计
- 不引入新行为变化

## 6. 顶层调度原则

最终的主循环不再内嵌大段实现，而是按顺序调用模块：

1. `runtime_services.tick()`
2. `audio_feedback.tick()`
3. `door_controller.tick()`
4. `fingerprint_access.tick()`
5. `keypad_access.tick(...)`
6. `nfc_access.tick()`

具体顺序可在实现阶段微调，但必须满足以下原则：

- OTA 更新优先级最高
- 后台运行时维护必须可在主循环和等待场景复用
- 指纹录入忙碌态能够阻止其他本地输入源竞争
- 门锁自动关门与音频 loop 不应被阻塞

## 7. 执行约束

本次重构执行时必须遵守以下约束：

- 每个 Phase 都必须能独立编译通过
- 每个 Phase 只迁移一个清晰职责，不顺手做无关清理
- 不修改现有 NVS key
- 不修改现有 Web API 路由
- 不改变现有认证和开门行为
- 不引入新的抽象层级，除非它直接服务于这 6 个模块的边界稳定性
- `safeDelay()` 重复维护问题必须在 Phase 1 解决，不后移

## 8. 风险与控制

### 风险 1：迁移过程中破坏现有行为

**控制方式**

- 每个 Phase 完成后立即编译验证
- 每次迁移保持外部接口兼容，先搬实现，再考虑是否进一步整理调用方式

### 风险 2：运行时模块变成新的“大杂烩”

**控制方式**

- `runtime_services` 只负责后台维护，不吸收门禁业务逻辑
- 门禁授权、门锁执行、输入状态机保持在各自模块中

### 风险 3：过早抽象导致实现复杂度上升

**控制方式**

- 先做薄接口
- 只在现有重复逻辑已经明显时才提取公共回调或公共状态

## 9. 预期结果

完成后，项目将获得以下收益：

- 主 `.ino` 明显缩短，结构更接近装配器
- 各输入源与执行层职责更清晰
- 运行时后台维护路径统一，`safeDelay()` 不再复制主循环逻辑
- 未来新增认证源、替换硬件实现或修改联网策略时，变更范围更可控
