# ESP32-S3 门禁开门延迟与阻塞设计修复计划

## 1. 背景

当前项目：`https://github.com/1rua/esp32s3-menjin`

目标分支建议：从当前 `main` 新建修复分支，例如：

```bash
git checkout main
git pull
git checkout -b fix/unlock-latency-nonblocking-loop
```

当前现象：

- MQTT 远程开门成功后，舵机转动可能存在 2 到 5 秒延迟。
- 指纹识别成功后，舵机转动可能存在 2 到 5 秒延迟。
- 舵机开门完成到重新锁定的总时间偏长。
- 代码中存在多个同步阻塞点，容易让本地开门链路排队等待后台任务。

本计划的核心原则：

> 本地开门链路优先级最高。网络、Web、MQTT、音频、NTP、OTA、NVS 写入都不能阻塞舵机开门命令。

---

## 2. 当前主要阻塞风险

### 2.1 `loop()` 优先级错误

当前 `loop()` 先执行：

```cpp
serviceCoreLoopSlice();
```

之后才执行：

```cpp
pollKeypadAccess(...);
pollFingerprintMatch(...);
maintainNfcReaderHealth(...);
pollNfcAccess(...);
```

而 `serviceCoreLoopSlice()` 内部包含：

```cpp
processForcedProvisioningButton(...);
tickAudioFeedback(audio);
tickDoorController(...);
tickFingerprintAccess(...);
ensureWebServerReady(...);
server.handleClient();
maintainWiFiConnection(...);
maintainMqttConnection(...);
ensureTimeSyncStarted(...);
pruneExpiredTemporaryPins(...);
ensureOtaReady(...);
ArduinoOTA.handle();
```

风险：

- `server.handleClient()`、`maintainMqttConnection()`、`WiFi` 重连、音频解码、NVS 写入等任何一个卡住，都会推迟本地指纹、键盘、NFC 开门检测。
- 本地门禁实时性被后台服务抢占。

### 2.2 MQTT 重连为同步调用

`maintainMqttConnection()` 中存在：

```cpp
context.mqttClient.connect(context.deviceConfig.mqttUid.c_str())
```

并且当前 MQTT 重连间隔为：

```cpp
constexpr uint32_t kMqttReconnectIntervalMs = 10000UL;
```

风险：

- MQTT 断线、DNS 慢、服务器无响应、WiFi 抖动时，`connect()` 可能阻塞当前主循环。
- 本地指纹和 MQTT 消息处理都在同一线程，网络异常时开门会延迟。

### 2.3 舵机每次开关门都会重新 `attach()` / `detach()`

当前 `requestDoorOpen()` 和 `closeDoorNow()` 都会：

```cpp
servo.attach(config.servoPin, 500, 3000);
servo.writeMicroseconds(...);
scheduleDetach(...);
```

风险：

- 某些数字舵机在重新接收 PWM 后响应慢。
- 供电不足、负载较大、舵机启动电流高时，重新 attach 可能放大物理延迟。
- 当前 `DOOR_SERVO_PULSE_MS = 500`，对大扭矩舵机可能偏短。

### 2.4 `authorizeDoorOpen()` 中混入非关键动作

当前开门授权函数里同时做：

```cpp
requestDoorOpen(...);
playOpenSound(audio);
client.publish(topic_door, "on");
```

虽然舵机命令在音频和 MQTT 发布之前，但音频与 MQTT 发布仍属于不应放在核心开门链路里的动作。

### 2.5 指纹匹配是同步串口流程

当前指纹匹配流程为：

```cpp
finger.getImage();
finger.image2Tz();
finger.fingerFastSearch();
```

风险：

- 指纹模块响应慢、串口不稳、供电不稳时，会阻塞当前循环。
- 高频空轮询会让系统负载变高。

### 2.6 NFC 健康检查可能同步重置 RC522

`maintainNfcReaderHealth()` 会周期性读寄存器并在异常时执行：

```cpp
reader.PCD_Init();
```

风险：

- SPI/NFC 模块异常时，该逻辑会阻塞主循环。
- 不应放在开门检测之前执行。

---

## 3. 修复目标

### 3.1 必须达成

- 本地键盘、指纹、NFC 开门检测必须优先于 Web/MQTT/WiFi/NTP/OTA 后台维护。
- `authorizeDoorOpen()` 中必须保证舵机命令第一时间发出。
- MQTT 断线重连不能阻塞本地开门。
- 舵机 attach/detach 策略需要提供“低延迟模式”。
- 增加耗时日志，能定位哪个任务造成阻塞。
- 修复后不能破坏 Web 管理门户、MQTT 远程开门、OTA、AP 配网、指纹录入。

### 3.2 建议性能指标

在串口日志中统计：

- 从 `authorizeDoorOpen()` 进入到 `requestDoorOpen()` 返回：应小于 20 ms。
- 本地指纹识别完成后到舵机命令发出：应小于 30 ms。
- 常规 loop 单轮耗时：绝大多数应小于 20 ms。
- 后台任务单次耗时超过 50 ms 必须打印 `[BLOCK]` 日志。
- MQTT 断网状态下，本地指纹开门仍应稳定响应。

---

## 4. 修复任务清单

## P0-1：添加阻塞耗时日志

### 目标

先能看见谁在阻塞主循环。不要盲修。

### 修改文件

- `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino`
- 可选新增：
  - `src/runtime_profiler/runtime_profiler.h`
  - `src/runtime_profiler/runtime_profiler.cpp`

### 实现要求

新增可开关的 profiling 宏：

```cpp
#define ENABLE_LOOP_PROFILING 1

#if ENABLE_LOOP_PROFILING
#define PROFILE_TASK(name, expr) do {   unsigned long __profileStart = millis();   expr;   unsigned long __profileCost = millis() - __profileStart;   if (__profileCost > 50UL) {     Serial.printf("[BLOCK] %s took %lu ms\n", name, __profileCost);   } } while (0)
#else
#define PROFILE_TASK(name, expr) do { expr; } while (0)
#endif
```

需要包裹以下任务：

```cpp
PROFILE_TASK("tickAudioFeedback", tickAudioFeedback(audio));
PROFILE_TASK("tickDoorController", tickDoorController(doorState, doorServo, kDoorControllerConfig, millis()));
PROFILE_TASK("tickFingerprintAccess", tickFingerprintAccess(...));
PROFILE_TASK("server.handleClient", server.handleClient());
PROFILE_TASK("maintainWiFiConnection", maintainWiFiConnection(...));
PROFILE_TASK("maintainMqttConnection", maintainMqttConnection(...));
PROFILE_TASK("ArduinoOTA.handle", ArduinoOTA.handle());
PROFILE_TASK("pollFingerprintMatch", fpID = pollFingerprintMatch(...));
PROFILE_TASK("maintainNfcReaderHealth", maintainNfcReaderHealth(...));
```

### 验收标准

- 编译通过。
- 串口能输出 `[BLOCK] xxx took N ms`。
- 正常运行时不会疯狂刷屏。
- MQTT 断网、Web 页面刷新、指纹识别时能观察各任务耗时。

---

## P0-2：重排 `loop()`，本地开门优先

### 目标

把键盘、指纹、NFC 本地开门检测移动到后台服务之前。

### 修改文件

- `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino`

### 实现要求

新增函数：

```cpp
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
    authorizeDoorOpen(keypadResult.matchedSource == "Temporary PIN" ? "Temporary PIN" : "Keypad Password");
    return true;
  }

  if (keypadResult.rejected) {
    playErrorSound(audio);
    return false;
  }

  int fpID = -1;
  PROFILE_TASK("pollFingerprintMatch", fpID = pollFingerprintMatch(fingerprintState, finger, runtimeServices.isOtaUpdating));
  if (fpID != -1) {
    authorizeDoorOpen("Fingerprint");
    return true;
  }

  PROFILE_TASK("maintainNfcReaderHealth", maintainNfcReaderHealth(nfcState, mfrc522, millis(), handleRuntimeOtaStartSideEffect));

  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    NfcPollResult nfcResult = pollNfcAccess(nfcState, mfrc522, runtimeServices.isOtaUpdating, millis());
    if (nfcResult.authorized) {
      authorizeDoorOpen("NFC");
      return true;
    }

    if (nfcResult.rejected) {
      playErrorSound(audio);
    }
  }

  return false;
}
```

将 `loop()` 改为：

```cpp
void loop() {
  if (runtimeServices.isOtaUpdating) {
    ArduinoOTA.handle();
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
    audio,
    runtimeServices.isOtaUpdating,
    isProvisioningPortalActive(provisioningState)
  );

  if (pollLocalAccessFirst()) {
    serviceFastPostUnlockTasks();
    return;
  }

  serviceCoreLoopSlice();
}
```

注意：

- `tickDoorController()` 必须仍然每轮执行，避免影响自动关门。
- 本地开门成功后可以立即 `return`，避免本轮继续跑慢任务。
- `maintainNfcReaderHealth()` 如果后续发现耗时明显，应改成低频慢任务，不能影响指纹开门。

### 验收标准

- 指纹、键盘、NFC 均可正常开门。
- Web、MQTT、OTA、AP 配网不受破坏。
- MQTT 断网时，本地指纹开门不再被 1 秒级阻塞拖慢。

---

## P0-3：让 `authorizeDoorOpen()` 只负责开门，音频和 MQTT 改为延后处理

### 目标

开门授权函数内只做最关键的舵机动作，其他都改为 pending 事件。

### 修改文件

- `esp32-s3-menjin-version3.0/esp32-s3-menjin-version3.0.ino`

### 实现要求

新增状态：

```cpp
static bool pendingOpenSound = false;
static bool pendingMqttDoorOn = false;
static bool pendingMqttDoorOff = false;
```

新增函数：

```cpp
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
  if (pendingOpenSound) {
    pendingOpenSound = false;
    playOpenSound(audio);
  }

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
```

修改 `authorizeDoorOpen()`：

```cpp
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

  pendingOpenSound = true;
  queueDoorMqttState(true);
}
```

在 `serviceCoreLoopSlice()` 或 `loop()` 尾部调用：

```cpp
servicePostUnlockEvents();
```

### 验收标准

- 开门命令发出前不再执行音频播放和 MQTT publish。
- Web/MQTT 状态上报仍然正常。
- 自动关门后仍能上报 `"off"`。
- `authorizeDoorOpen()` 到 `requestDoorOpen()` 完成耗时通常小于 20 ms。

---

## P0-4：限制 MQTT 重连阻塞

### 目标

MQTT 断开时不能频繁同步重连，更不能在门打开或刚发生本地操作时抢占开门链路。

### 修改文件

- `src/runtime_services/runtime_services.h`
- `src/runtime_services/runtime_services.cpp`
- `esp32-s3-menjin-version3.0.ino`

### 实现要求

将 MQTT 重连间隔从 10 秒改为 30 秒：

```cpp
constexpr uint32_t kMqttReconnectIntervalMs = 30000UL;
```

在 `RuntimeServicesState` 中新增：

```cpp
unsigned long lastLocalAccessActivityMs = 0;
```

或在 `.ino` 中新增全局变量：

```cpp
static unsigned long lastLocalAccessActivityMs = 0;
```

本地有键盘输入、检测到指纹图像、检测到 NFC 卡片、开门成功时更新：

```cpp
lastLocalAccessActivityMs = millis();
```

在调用 `maintainMqttConnection()` 前增加保护：

```cpp
const bool recentlyLocalActive = millis() - lastLocalAccessActivityMs < 3000UL;

if (!doorState.isOpen && !recentlyLocalActive) {
  PROFILE_TASK("maintainMqttConnection", maintainMqttConnection(...));
} else if (client.connected()) {
  client.loop();
}
```

如果希望保持 MQTT 消息接收能力，可新增轻量函数：

```cpp
void serviceMqttLoopOnly(RuntimeServicesContext& context) {
  if (context.mqttClient.connected()) {
    context.mqttClient.loop();
  }
}
```

重连逻辑与 `loop()` 逻辑分离：

- 已连接：允许频繁 `loop()`。
- 未连接：低频重连。
- 门打开时：不主动重连。
- 最近 3 秒有本地输入时：不主动重连。
- Web 指纹录入进行中：不主动重连。

### 验收标准

- MQTT 正常在线时远程开门仍可用。
- MQTT 断网时，不会每 10 秒造成明显卡顿。
- 门打开期间不触发 MQTT 重连。
- 本地操作期间不触发 MQTT 重连。
- MQTT 恢复后能重新订阅主题并上报当前门状态。

---

## P1-1：拆分 `serviceCoreLoopSlice()` 为快任务和慢任务

### 目标

减少每一轮 loop 中必须执行的后台工作。

### 修改文件

- `esp32-s3-menjin-version3.0.ino`

### 实现要求

拆分为：

```cpp
static void serviceFastTasks() {
  PROFILE_TASK("tickAudioFeedback", tickAudioFeedback(audio));

  PROFILE_TASK("tickFingerprintAccess", tickFingerprintAccess(
    prefs,
    fingerprintState,
    finger,
    audio,
    runtimeServices.isOtaUpdating,
    isProvisioningPortalActive(provisioningState),
    millis()
  ));

  ensureWebServerReady(runtimeServices, getRuntimeServicesContext());

  if (shouldServiceWebServer(getRuntimeServicesContext())) {
    PROFILE_TASK("server.handleClient", server.handleClient());
  }

  servicePostUnlockEvents();

  if (WiFi.status() == WL_CONNECTED) {
    PROFILE_TASK("ArduinoOTA.handle", ArduinoOTA.handle());
  }
}
```

```cpp
static void serviceSlowTasks() {
  PROFILE_TASK("processForcedProvisioningButton", processForcedProvisioningButton(...));

  PROFILE_TASK("maintainWiFiConnection", maintainWiFiConnection(runtimeServices, getRuntimeServicesContext()));

  if (!doorState.isOpen && millis() - lastLocalAccessActivityMs >= 3000UL) {
    PROFILE_TASK("maintainMqttConnection", maintainMqttConnection(...));
  }

  if (WiFi.status() == WL_CONNECTED) {
    ensureTimeSyncStarted(runtimeServices);

    static unsigned long lastPinPruneAt = 0;
    if (runtimeIsTimeSynced() && millis() - lastPinPruneAt >= 60000UL) {
      lastPinPruneAt = millis();
      PROFILE_TASK("pruneExpiredTemporaryPins", pruneExpiredTemporaryPins(prefs, accessControl, runtimeCurrentEpochSeconds(), true));
    }

    ensureOtaReady(runtimeServices, getRuntimeServicesContext(), "Mech-Master-S3", OTA_DEFAULT_PASSWORD);
  }
}
```

主循环调用：

```cpp
serviceFastTasks();

static unsigned long lastSlowTaskAt = 0;
if (millis() - lastSlowTaskAt >= 100UL) {
  lastSlowTaskAt = millis();
  serviceSlowTasks();
}
```

### 验收标准

- loop 中每轮固定工作明显减少。
- 慢任务不再每轮执行。
- 临时 PIN 过期清理改为每 60 秒一次。
- WiFi/MQTT 维护仍能工作。

---

## P1-2：优化舵机 attach/detach 策略

### 目标

降低数字舵机因重新 attach 或 PWM 脉冲过短导致的动作延迟。

### 修改文件

- `esp32-s3-menjin-version3.0.ino`
- `src/door_controller/door_controller.h`
- `src/door_controller/door_controller.cpp`

### 方案 A：低风险止血

把：

```cpp
const uint32_t DOOR_SERVO_PULSE_MS = 500;
```

改为：

```cpp
const uint32_t DOOR_SERVO_PULSE_MS = 1500;
```

验收：

- 舵机动作更稳定。
- 舵机不过热。
- 自动关门正常。

### 方案 B：低延迟模式

新增配置：

```cpp
const bool DOOR_SERVO_KEEP_ATTACHED = true;
```

修改 `door_controller`，在 keep attached 模式下：

- 初始化时 attach。
- 开门时只 `writeMicroseconds(openUs)`。
- 关门时只 `writeMicroseconds(closeUs)`。
- 不 schedule detach。
- OTA 开始时仍允许 `detach()`。

伪代码：

```cpp
static void ensureServoAttached(Servo& servo, const DoorControllerConfig& config) {
  if (!servo.attached()) {
    servo.attach(config.servoPin, 500, 3000);
  }
}
```

```cpp
void requestDoorOpen(...) {
  ensureServoAttached(servo, config);
  servo.writeMicroseconds(config.openUs);

  if (!config.keepAttached) {
    scheduleDetach(state, config.pulseMs);
  }

  state.isOpen = true;
  state.openedAt = nowMs;
}
```

### 验收标准

- 低延迟模式下，授权后舵机应立即响应。
- 常驻 attach 时舵机温度、电流可接受。
- 如果舵机持续抖动，则切回方案 A。
- OTA 开始时仍会停止音频并释放舵机。

---

## P1-3：降低指纹空轮询频率

### 目标

减少指纹模块同步 UART 命令对主循环的影响。

### 修改文件

- `esp32-s3-menjin-version3.0.ino`

### 实现要求

在 `pollLocalAccessFirst()` 中加入节流：

```cpp
static unsigned long lastFingerprintPollAt = 0;

if (millis() - lastFingerprintPollAt >= 80UL) {
  lastFingerprintPollAt = millis();

  int fpID = -1;
  PROFILE_TASK("pollFingerprintMatch", fpID = pollFingerprintMatch(fingerprintState, finger, runtimeServices.isOtaUpdating));

  if (fpID != -1) {
    lastLocalAccessActivityMs = millis();
    authorizeDoorOpen("Fingerprint");
    return true;
  }
}
```

### 验收标准

- 指纹识别体验无明显变慢。
- 空闲 loop 更顺滑。
- 串口 profiling 中 `pollFingerprintMatch` 不再高频出现耗时日志。

---

## P1-4：NFC 健康检查移入慢任务

### 目标

避免 RC522 异常时健康检查阻塞本地指纹/键盘开门链路。

### 修改文件

- `esp32-s3-menjin-version3.0.ino`

### 实现要求

将：

```cpp
maintainNfcReaderHealth(...)
```

从本地开门检测主路径中移出，放到 `serviceSlowTasks()`，例如每 1000 ms 检查一次：

```cpp
static unsigned long lastNfcHealthCheckTaskAt = 0;

if (millis() - lastNfcHealthCheckTaskAt >= 1000UL) {
  lastNfcHealthCheckTaskAt = millis();
  PROFILE_TASK("maintainNfcReaderHealth", maintainNfcReaderHealth(nfcState, mfrc522, millis(), handleRuntimeOtaStartSideEffect));
}
```

### 验收标准

- NFC 读卡仍正常。
- RC522 异常重置不会阻塞指纹开门。
- `[Watchdog] NFC Dead. Resetting...` 出现时，本地指纹开门仍可用。

---

## P2-1：将 MQTT/Web 后台服务迁移到 FreeRTOS 独立任务

### 目标

彻底隔离本地门禁实时链路和网络服务。

### 修改文件

- `esp32-s3-menjin-version3.0.ino`
- 可能新增：
  - `src/runtime_tasks/runtime_tasks.h`
  - `src/runtime_tasks/runtime_tasks.cpp`

### 实现建议

优先不要一开始就做本任务。先完成 P0/P1。如果仍有明显卡顿，再进行 FreeRTOS 化。

建议任务划分：

- 主 `loop()`：只处理本地门禁、舵机、最小状态机。
- `networkTask`：处理 WiFi/MQTT/NTP/OTA。
- `webTask`：处理 `server.handleClient()`。
- `audioTask`：视音频库线程安全情况决定是否独立。

注意：

- `PubSubClient`、`WebServer`、`Audio`、`Preferences` 不是天然线程安全。
- 跨任务共享变量必须使用 mutex 或队列。
- 舵机控制只允许主门禁任务直接调用。

### 验收标准

- 无 WDT 重启。
- MQTT/Web 与本地开门互不阻塞。
- 共享状态无竞态问题。
- 长时间运行稳定。

---

## 5. 推荐修改顺序

### 第一阶段：低风险止血

1. 添加 profiling 日志。
2. 重排 `loop()`，本地开门优先。
3. `authorizeDoorOpen()` 只发舵机命令，音频/MQTT 改 pending。
4. MQTT 断线重连限速，并在门打开或本地操作时跳过。
5. `DOOR_SERVO_PULSE_MS` 从 500 改成 1500。

### 第二阶段：结构优化

1. 拆分 `serviceCoreLoopSlice()` 为快任务和慢任务。
2. 临时 PIN 清理改为 60 秒一次。
3. 指纹空轮询加 80 ms 节流。
4. NFC 健康检查移入慢任务。

### 第三阶段：进阶隔离

1. 评估是否需要 FreeRTOS 独立任务。
2. 如仍存在 1 秒级阻塞，再迁移 MQTT/Web。
3. 舵机控制保持在主任务，不跨线程直接控制。

---

## 6. 测试用例

### 6.1 基础功能测试

- 上电后舵机应回到关闭位置。
- 指纹开门正常。
- 键盘长期 PIN 开门正常。
- 键盘临时 PIN 开门正常。
- NFC 白名单卡开门正常。
- Web 点击开门正常。
- MQTT 发送 `on` 开门正常。
- 自动关门正常。
- MQTT 状态应能上报 `on` 和 `off`。

### 6.2 阻塞测试

#### 测试 A：MQTT 服务器不可达

步骤：

1. 配置错误 MQTT UID 或断开外网。
2. 保持 WiFi 已连接。
3. 连续使用指纹开门。

期望：

- 串口可能出现 MQTT 相关 `[BLOCK]`，但频率明显降低。
- 指纹开门不出现 2 到 5 秒延迟。
- 本地开门不依赖 MQTT。

#### 测试 B：Web 页面持续刷新

步骤：

1. 打开 Web 控制台。
2. 连续刷新页面或频繁请求状态接口。
3. 同时使用指纹开门。

期望：

- 指纹开门优先。
- `server.handleClient` 即使出现耗时日志，也不应导致明显开门延迟。

#### 测试 C：音频播放期间开门

步骤：

1. 触发 boot/open/error 音频播放。
2. 同时测试指纹或键盘开门。

期望：

- 舵机命令优先。
- 音频不得阻塞开门动作。

#### 测试 D：舵机响应测试

步骤：

1. 在 `authorizeDoorOpen()` 和 `requestDoorOpen()` 内打印时间。
2. 比较串口时间和舵机实际动作。

期望：

- 如果日志显示命令已立即发出但舵机仍慢，问题转向供电、舵机 attach、机械负载或舵机型号。
- 如果日志显示命令发出前耗时高，继续查看 `[BLOCK]` 日志定位阻塞函数。

---

## 7. 关键验收标准

修复完成后必须满足：

- 编译通过，无新增库依赖。
- OTA、Web、MQTT、AP 配网、指纹录入功能不被破坏。
- 本地开门链路优先于网络维护。
- MQTT 断网不影响本地开门。
- `authorizeDoorOpen()` 中舵机命令前不执行音频播放、MQTT publish、Web 响应等非关键动作。
- 串口日志能定位超过 50 ms 的阻塞任务。
- 舵机授权后响应明显改善。

---

## 8. 回滚方案

若修改后出现异常：

1. 先关闭 `ENABLE_LOOP_PROFILING`。
2. 将舵机策略从常驻 attach 回退到延长 pulse 方案。
3. 将 `loop()` 调度保留本地优先，但恢复 `serviceCoreLoopSlice()` 原始内容。
4. 若 MQTT 行为异常，先恢复 `maintainMqttConnection()` 原始逻辑，仅保留重连间隔 30000 ms。
5. 若 Web 指纹录入异常，检查 `tickFingerprintAccess()` 是否仍被高频调用。

---

## 9. 不要做的事

- 不要在开门授权路径里添加 `delay()`。
- 不要在 `authorizeDoorOpen()` 里等待 MQTT publish 成功。
- 不要让 `server.handleClient()` 排在本地开门检测之前。
- 不要在本地开门路径里执行 NVS 写入。
- 不要把舵机控制放到多个 FreeRTOS 任务里同时调用。
- 不要在 OTA 过程中允许本地开门动作。
- 不要删除原有 AP 配网、Web 管理、指纹录入功能。

---

## 10. 给 Codex 的最终任务描述

请按本文件执行修复，优先完成 P0 任务：

1. 添加 profiling 日志。
2. 重排 `loop()`，让本地键盘、指纹、NFC 开门优先于后台服务。
3. 改造 `authorizeDoorOpen()`，确保舵机命令立即发出，音频与 MQTT 状态上报延后。
4. 限制 MQTT 重连，门打开或最近本地操作时禁止主动重连。
5. 将舵机 pulse 时间从 500 ms 提高到 1500 ms，或实现可配置低延迟常驻 attach 模式。
6. 保证编译通过，并保持 Web、MQTT、OTA、AP 配网、指纹录入功能可用。
