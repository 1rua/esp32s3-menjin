# 2026-04-17 门禁配网状态机与 Web/AP 解耦设计

## 背景
当前固件集中在 `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino` 单文件中，AP 配网、Web 页面、Wi‑Fi 连接、MQTT 初始化、本地门禁主链路混在一起，导致以下问题：

1. `isAPMode` 同时承担“正在开放 AP 门户”“停止本地门禁扫描”“停止联网逻辑”等多重语义，状态边界不清。
2. Wi‑Fi 连接失败时会重新进入 AP 配网模式，阻塞本地指纹 / NFC / 密码解锁。
3. Web 控制与 AP 配网逻辑耦合在主文件，后续维护难度高。

本次目标是在保留当前核心门禁能力的前提下，重构启动与配网流程，将 AP 配网和 Web 控制从主文件中解耦，并确保联网异常不影响本地开门链路。

## 目标
本次设计需要满足以下目标：

1. 启动时仅在以下两种场景进入 AP 配网页：
   - NVS 中没有完成过有效 Wi‑Fi 配置，且用户没有选择“永久跳过自动配网”
   - 上电启动窗口内检测到 BOOT 键长按 5 秒
2. AP 配网页支持：
   - 输入 Wi‑Fi SSID 和密码
   - 可选输入 / 修改巴法云 UID
   - 选择“跳过配网”
3. “跳过配网”语义为：
   - 写入永久标记
   - 后续即使没有 Wi‑Fi 配置，也不再自动进入 AP
   - 仅在 BOOT 长按 5 秒时再次进入 AP
4. 配网成功或跳过配网后，退出 AP 配网模式，进入正常运行路径。
5. 若已经完成过配网，但后续 Wi‑Fi 或 MQTT 连接失败：
   - 不重新唤起 AP 配网模式
   - 本地指纹 / NFC / 密码解锁继续可用
   - 联网部分在后台重连
6. 将 AP 配网、Web 控制、配置存储从主 ino 文件解耦为独立模块，便于后续维护。

## 非目标
本次不做以下工作：

1. 不修复或重构指纹录入流程。
2. 不改动现有门锁舵机开关动作逻辑（`openDoor()` / `closeDoor()`）的执行方式。
3. 不拆分 NFC、指纹、键盘等本地主链路到更多新模块。
4. 不引入新的第三方 Web 框架或复杂配置系统。
5. 不改变现有门禁业务规则（密码、NFC 白名单、指纹匹配规则）。

## 设计原则
1. **联网降级不等于门禁停机**：Wi‑Fi / MQTT 失败只能影响联网能力，不得阻塞本地门禁认证链路。
2. **状态显式化**：用明确的启动 / 配网状态机替代单一 `isAPMode` 的多重语义。
3. **模块职责单一**：主文件只保留门禁核心运行链路；AP 配网、Web 控制、配置存储拆出独立文件。
4. **兼容当前工程结构**：在 Arduino 工程内采用 3 文件方案，不做过度抽象。
5. **用户触发优先**：BOOT 长按 5 秒始终可强制进入 AP 配网，不受“跳过自动配网”标记影响。

## 方案选择
采用 **方案 C：显式启动状态机 + AP/Web 解耦 + 3 文件方案**。

相较于仅修改进入条件的轻量方案，本方案明确分离“启动决策”“AP 配网页生命周期”“联网状态”“本地门禁主链路”，可以从结构上避免后续再次出现“联网失败导致本地门禁停摆”的问题。

## 新文件划分
本次新增 3 组模块文件：

### 1. `device_config.h/.cpp`
职责：
- 封装 NVS 配置读写
- 管理 Wi‑Fi / MQTT / 跳过配网标记
- 提供“是否有有效 Wi‑Fi 配置”“是否启用 MQTT”等判定接口

建议包含的核心内容：
- 配置结构体，例如 `DeviceConfig`
- 读取配置：`loadDeviceConfig()`
- 保存 Wi‑Fi：`saveWiFiConfig(...)`
- 保存 MQTT UID：`saveMqttUid(...)`
- 设置跳过自动配网：`setSkipAutoProvision(...)`
- 判断 MQTT 是否启用：`isMqttConfigured(...)`

### 2. `provisioning.h/.cpp`
职责：
- 管理启动决策状态机
- 管理 AP 门户开启 / 关闭
- 检测启动时 BOOT 长按 5 秒
- 提供当前是否处于 AP 门户状态、是否应启动 Wi‑Fi 连接等接口

建议包含的核心内容：
- 启动状态枚举，例如 `StartupState`
- 判断是否进入 AP：`shouldEnterProvisioningPortal(...)`
- 启动 AP：`startProvisioningPortal()`
- 关闭 AP：`stopProvisioningPortal()`
- BOOT 检测：`detectForcedProvisioningRequest()`
- 配网结果处理：`handleProvisioningSaved()` / `handleProvisioningSkipped()`

### 3. `web_portal.h/.cpp`
职责：
- 注册 Web 路由
- 根据当前模式返回不同页面（普通控制页 / 配网页）
- 处理 `/open`、`/add_nfc`、`/configure_network`、`/skip_provision`
- 把 Web 层请求转发给配置与配网模块

建议包含的核心内容：
- HTML 模板拆分：控制页与配网页分离
- 路由注册：`setupWebRoutes(...)`
- Handler：
  - `handleRoot()`
  - `handleOpen()`
  - `handleAddNFC()`
  - `handleConfigureNetwork()`
  - `handleSkipProvision()`

## 启动状态机设计
建议将启动与运行阶段拆成以下显式状态：

1. `BOOT_CHECK`
   - 初始化基础硬件和 NVS
   - 检测 BOOT 键是否被持续按下 5 秒
   - 读取 NVS 配置

2. `PROVISION_DECISION`
   - 根据当前配置决定是否进入 AP 配网页
   - 决策条件来自：
     - 是否有有效 Wi‑Fi 配置
     - 是否存在永久跳过自动配网标记
     - 是否存在 BOOT 强制配网请求

3. `AP_PORTAL`
   - 启动 SoftAP
   - 提供 Web 配网页
   - 允许用户保存 Wi‑Fi / MQTT UID 或跳过配网

4. `CONNECTING_WIFI`
   - 如果系统应尝试联网，则开始 STA 连接
   - 连接成功后启动 OTA、MQTT 等联网子系统
   - 连接失败则进入正常运行，但保留后台重连

5. `NORMAL_RUNTIME`
   - 本地门禁主链路持续运行
   - Wi‑Fi / MQTT 根据当前连接状态在后台维护
   - 不自动回退到 AP 配网页

### 状态转移规则
- `BOOT_CHECK -> PROVISION_DECISION`
- `PROVISION_DECISION -> AP_PORTAL`：满足 AP 条件
- `PROVISION_DECISION -> CONNECTING_WIFI`：不进入 AP，且具备联网尝试条件
- `PROVISION_DECISION -> NORMAL_RUNTIME`：用户永久跳过自动配网且当前没有可用 Wi‑Fi 配置
- `AP_PORTAL -> CONNECTING_WIFI`：保存 Wi‑Fi 配置后
- `AP_PORTAL -> NORMAL_RUNTIME`：点击“跳过配网”后
- `CONNECTING_WIFI -> NORMAL_RUNTIME`：无论连接成功或失败，均进入正常运行；差别仅在联网能力是否激活

## NVS 配置设计
建议在现有 `Preferences` 命名空间中统一管理以下键：

- `wifi_ssid`
- `wifi_pass`
- `wifi_configured`
- `skip_auto_prov`
- `mqtt_uid`

### 字段语义
- `wifi_configured`：是否成功保存过一组有效 Wi‑Fi 配置
- `skip_auto_prov`：是否永久跳过“无 Wi‑Fi 配置时自动进入 AP”
- `mqtt_uid`：巴法云 UID，可为空；为空表示 MQTT 不启用

### 写入规则
#### 保存配网成功时
- 保存 `wifi_ssid`
- 保存 `wifi_pass`
- 若用户填写了新的 MQTT UID，则更新 `mqtt_uid`
- `wifi_configured = true`
- `skip_auto_prov = false`

#### 点击“跳过配网”时
- 不改动已有 Wi‑Fi 配置
- 不改动已有 MQTT UID
- `skip_auto_prov = true`

### MQTT UID 规则
用户已明确：
- 若本次未填写 MQTT UID，则保留旧值
- 若旧值为空，则 MQTT 视为未启用

## AP 进入与退出规则
### 自动进入 AP 的条件
满足以下全部条件时自动进入 AP：
- `wifi_configured == false`
- `skip_auto_prov == false`
- 当前启动未跳过 AP 决策

### 强制进入 AP 的条件
满足以下任一条件时强制进入 AP：
- BOOT 键上电后持续按下 5 秒

### 退出 AP 的条件
- 用户保存 Wi‑Fi 配置成功
- 用户点击“跳过配网”

### 禁止进入 AP 的场景
以下场景均不得自动进入 AP：
- 曾经完成过配网，但当前 Wi‑Fi 连接失败
- 曾经点击过“跳过配网”，且本次没有 BOOT 强制请求
- MQTT 连接失败

## BOOT 长按 5 秒规则
BOOT 触发配网仅在启动窗口检测，作为一次性人工入口。

建议行为：
1. 上电后进入 `BOOT_CHECK`
2. 在固定窗口内检测 BOOT 引脚状态
3. 若 BOOT 持续按下满 5 秒，则本次启动标记 `bootForcedProvision = true`
4. 该标记只对本次启动有效，不写入 NVS

这样可以保证：
- 用户任何时候都能手动进入 AP 配网
- 不会污染永久配置状态

## Web 页面与路由设计
### 页面模式
`GET /` 根据当前运行模式返回两种不同页面：

1. **AP 配网页**
   - Wi‑Fi SSID 输入框
   - Wi‑Fi 密码输入框
   - MQTT UID 输入框（可空）
   - “保存并连接”按钮
   - “跳过配网”按钮

2. **普通控制页**
   - 立即开门
   - NFC 白名单写入
   - 若后续需要，也可显示当前联网状态，但本次不强制新增

### 路由
建议保留或新增以下路由：
- `GET /`
- `GET /open`
- `GET /add_nfc`
- `POST` 或 `GET /configure_network`
- `POST` 或 `GET /skip_provision`

优先建议：
- 新功能使用 `/configure_network` 和 `/skip_provision`
- 逐步替代当前 `handleSetWiFi()` 对 `/set_wifi` 的单一路由处理

## 主文件职责调整
重构后，主文件 `menjin_esp32s3___official_version3.0.ino` 的职责应收敛为：

1. 基础硬件初始化
2. 门锁舵机初始化与自动关门
3. NFC / 指纹 / 键盘认证扫描
4. MQTT 回调接入
5. OTA 调用
6. 在 `setup()` 中调用：
   - 配置加载模块
   - 启动状态机决策模块
   - Web 路由初始化模块
7. 在 `loop()` 中调用：
   - Web 服务轮询
   - 本地门禁主链路
   - Wi‑Fi / MQTT 后台维护
   - OTA 维护（仅联网时）

主文件不再直接承载：
- AP 启停细节
- 配网页 HTML
- 配网参数保存细节
- 跳过配网语义处理

## 运行时行为设计
### 本地门禁主链路
以下能力在 `NORMAL_RUNTIME` 中必须始终保持可用，不得被联网状态阻塞：
- 键盘密码解锁
- 指纹解锁
- NFC 解锁
- 门锁自动关门

### 联网子系统
联网子系统按条件启用：
- Wi‑Fi 已连接：启用 OTA
- MQTT UID 非空且 Wi‑Fi 已连接：启用 MQTT 连接与订阅
- Wi‑Fi 未连接：后台重连，但不阻塞本地门禁
- MQTT 断线：后台重连，但不触发 AP

### AP 门户与本地门禁并存原则
本次设计要求将“AP 门户开启”和“本地门禁运行”解耦。也就是说，AP 门户只是临时配置入口，不是门禁停机模式。

是否在 AP 门户开启期间继续开放本地门禁扫描，可在实现时根据硬件稳定性做有限验证；但本设计的默认要求是：**AP 门户不得成为阻塞本地门禁主链路的理由**。

## 错误处理设计
### Wi‑Fi 连接失败
- 不自动回到 AP
- 标记当前网络不可用
- 保留后台定时重连
- 本地门禁继续工作

### MQTT 连接失败
- 仅影响远程控制
- 记录失败时间
- 后台周期性重连
- 不影响本地门禁

### 配网页输入非法
- 返回明确错误信息
- 不写入错误配置
- 保持当前 AP 页面可继续操作

### NVS 配置缺失或损坏
- 若 Wi‑Fi 配置判定无效，则按“未配网”规则进入启动决策
- 若 MQTT UID 缺失，则将 MQTT 视为未启用

## 测试与验证策略
### 1. 启动决策验证
- 无 Wi‑Fi 配置、未跳过：自动进入 AP
- 无 Wi‑Fi 配置、已跳过：不进入 AP
- 有 Wi‑Fi 配置：不自动进入 AP
- 有 Wi‑Fi 配置 + BOOT 长按 5 秒：进入 AP

### 2. AP 配网页验证
- 填写 Wi‑Fi 并保存后退出 AP
- 不填写 MQTT UID 时保留旧值
- 点击“跳过配网”后退出 AP，并写入永久跳过标记

### 3. 联网失败降级验证
- 已配网但 Wi‑Fi 断开：
  - 不进入 AP
  - 指纹 / NFC / 密码仍能开门
- MQTT 断线：
  - 本地门禁不受影响
  - 后台尝试重连

### 4. Web 路由验证
- 配网页和控制页按模式正确切换
- `/open` 仍能触发统一开门逻辑
- `/add_nfc` 仍能写入 NVS 白名单
- `/configure_network` 与 `/skip_provision` 语义正确

### 5. 结构验证
- 主 ino 文件体积明显缩减
- AP 配网、Web 控制、配置存储已迁移至新文件
- 主文件不再直接包含大段 HTML 和配网写入细节

## 实施结果判定
若实现后满足以下条件，则视为本次重构成功：

1. AP 配网仅在“未配网”或“BOOT 长按 5 秒”时进入。
2. 配网成功或跳过配网后，系统退出 AP 门户。
3. 跳过配网会永久关闭“自动进入 AP”行为，但不影响 BOOT 手动进入。
4. 已配网后的 Wi‑Fi / MQTT 异常不会重新拉起 AP。
5. 本地指纹 / NFC / 密码开门不再被联网异常阻塞。
6. AP 配网、Web 控制、配置存储已解耦到新文件，主文件职责收敛。
