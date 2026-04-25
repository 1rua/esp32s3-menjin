# Web 可管理键盘密码与临时密码技术设计文档

## 1. 文档目的

本文档不是“想法清单”，而是给后续执行 agent 的落地规范。目标是让后续 agent 在不重新做需求澄清的前提下，按本文档直接实施代码修改、验证和回归。

---

## 2. 目标与已冻结需求

### 2.1 目标

把当前硬编码的单一键盘密码改造成：

1. Web 可管理的多用户长期键盘密码
2. Web 可生成的临时键盘密码
3. 临时密码按在线绝对时间失效
4. Web 管理界面增加管理员登录保护

### 2.2 已冻结需求

以下需求已经和用户确认，后续 agent 不应自行改动：

- 长期密码用户规模：**10 个以内**
- 长期密码字符集：**仅数字 0-9**
- 临时密码字符集：**仅数字 0-9**
- 临时密码长度：**固定 4 位**
- 临时密码使用规则：**到期前可重复使用**
- 临时密码到期规则：**在线绝对时间**
- 到期判断时区：**Asia/Shanghai**
- 设备未完成校时前：**临时密码禁用，长期密码照常可用**
- Web 保护方式：**HTTP Basic Auth**
- 默认管理员凭证：**admin / esp32s3-menjin**

### 2.3 非目标

本次修改**不包含**以下内容：

- 不新增云端用户系统
- 不新增 HTTPS / TLS / 证书体系
- 不引入 PlatformIO、ESP-IDF 工程化改造或新构建系统
- 不修改 NFC、指纹、MQTT、OTA 的既有业务逻辑
- 不把门禁权限模型扩展成角色系统
- 不做多页面前端框架化改造

---

## 3. 当前代码现状与复用点

### 3.1 当前硬编码键盘密码入口

- `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino:29`
  - `const String DOOR_PASSWORD = "11451";`
- `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino:624-665`
  - `checkKeypad()` 里在按下 `#` 时直接比较 `inputCode == DOOR_PASSWORD`

### 3.2 当前 Web 门户入口

- `esp32-s3-menjin-version3.0/web_portal.cpp:5-146`
  - 当前控制台 HTML 与配网页面 HTML
- `esp32-s3-menjin-version3.0/web_portal.cpp:167-279`
  - JSON 响应、路由处理、`server.on(...)` 注册
- 当前已有路由：
  - `/`
  - `/open`
  - `/add_nfc`
  - `/configure_network`
  - `/set_wifi`
  - `/skip_provision`

### 3.3 当前 Preferences/NVS 复用模式

- `esp32-s3-menjin-version3.0/device_config.cpp:10-54`
  - Wi‑Fi / MQTT 配置使用 `getString/putString/getBool/putBool`
- `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino:431-503`
  - NFC 白名单初始化、迁移与装载
- `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino:785-788`
  - NFC 白名单使用 `putBytes/getBytes + count`

### 3.4 当前 Web 与主固件的集成方式

- `esp32-s3-menjin-version3.0/web_portal.h:14-22`
  - `WebPortalContext` 当前持有 `server/prefs/deviceConfig/provisioningState` 和几个回调
- `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino:298-309`
  - `getWebPortalContext()` 返回静态上下文

### 3.5 当前时间能力

当前代码**没有** NTP / RTC / `time_t` / `configTime` / `getLocalTime` 实现。

这意味着临时密码功能必须新增时间同步能力，且“未校时前临时密码禁用”必须严格执行。

---

## 4. 总体设计

### 4.1 设计原则

1. **不再把键盘密码作为硬编码常量存在**
2. **密码不以明文持久化**
3. **长期密码与临时密码都复用同一套 PIN 校验入口**
4. **所有 Web 管理修改动作使用 POST，不把敏感信息放进 URL**
5. **继续复用当前 WebServer + 静态 HTML + fetch + Preferences 模式**
6. **临时密码只在系统时间可信时才生效**
7. **尽量少改既有业务链路，重点替换密码来源与管理方式**

### 4.2 推荐模块拆分

#### 新增文件

- `esp32-s3-menjin-version3.0/access_control.h`
- `esp32-s3-menjin-version3.0/access_control.cpp`

#### 保持在既有文件内实现的内容

- NTP / 时间同步辅助逻辑：先放在 `menjin_esp32s3___official_version3.0.ino`
- Web 页面与 Web 路由：继续放在 `web_portal.cpp`

#### 不推荐的做法

- 不要为了这次需求再拆出 3~4 个新模块
- 不要为了 Web 管理再引入前端框架
- 不要把主流程改成全新的架构

---

## 5. 数据模型与持久化规范

### 5.1 哈希与随机性

#### 密码存储要求

管理员密码、长期 PIN、临时 PIN **都不能明文存入 NVS**。

#### 推荐实现

- 哈希算法：`SHA-256`
- 每条记录单独随机盐值 `salt`
- 计算方式：`SHA256(salt || plaintext)`
- 盐值来源：`esp_random()` 生成随机字节

#### 建议依赖

优先使用 Arduino-ESP32 自带能力，不新增外部库：

- `mbedtls/sha256.h`
- 如需 Basic Auth Base64 解码，可优先使用 ESP32 core 已有 base64 能力；若现成接口不稳定，可在本地实现一个最小解码辅助函数

#### 比较要求

比较摘要时使用固定时间比较函数，不要直接在首字节不匹配时提前返回。

### 5.2 常量建议

```cpp
constexpr uint8_t kMaxPermanentPinUsers = 10;
constexpr uint8_t kMaxTemporaryPins = 10;
constexpr uint8_t kPinMinLength = 4;
constexpr uint8_t kPinMaxLength = 10;
constexpr uint8_t kTemporaryPinLength = 4;
constexpr uint8_t kAdminUsernameMaxLength = 16;
constexpr uint8_t kPinLabelMaxLength = 24;
constexpr uint8_t kSaltLength = 16;
constexpr uint8_t kHashLength = 32;
constexpr uint32_t kMinValidEpoch = 1704067200UL; // 2024-01-01 00:00:00 UTC
```

### 5.3 结构体建议

> 后续 agent 可在不破坏语义的前提下微调字段顺序，但必须保持固定尺寸、适合 `putBytes/getBytes`。

```cpp
struct PermanentPinRecord {
  uint32_t id;
  bool enabled;
  char label[kPinLabelMaxLength];
  uint8_t salt[kSaltLength];
  uint8_t hash[kHashLength];
};

struct TemporaryPinRecord {
  uint32_t id;
  bool enabled;
  uint32_t expiresAtEpoch;
  uint8_t salt[kSaltLength];
  uint8_t hash[kHashLength];
};

struct AccessControlState {
  char adminUsername[kAdminUsernameMaxLength];
  uint8_t adminSalt[kSaltLength];
  uint8_t adminHash[kHashLength];
  bool adminUsesDefaultCredentials;

  PermanentPinRecord permanentPins[kMaxPermanentPinUsers];
  uint8_t permanentPinCount;

  TemporaryPinRecord temporaryPins[kMaxTemporaryPins];
  uint8_t temporaryPinCount;

  uint32_t nextPermanentPinId;
  uint32_t nextTemporaryPinId;
};
```

#### 设计说明

- `id`：用于 Web 更新/删除定位，避免只靠数组下标
- `enabled`：便于未来停用而不是物理删除；本次 Web 先可做删除，也可保留禁用扩展位
- `label`：长期 PIN 用户显示名
- 临时 PIN 不要求 label
- `adminUsesDefaultCredentials`：用于页面提示“仍在使用默认管理员密码”
- `nextPermanentPinId` / `nextTemporaryPinId`：避免删除后复用旧 ID

### 5.4 NVS key 规范

统一继续放在当前命名空间：

- `prefs.begin("mech_master", false)`

建议新增 key：

```text
pin_store_ver
admin_user
admin_salt
admin_hash
admin_default
perm_pin_cnt
perm_pin_list
perm_pin_next_id
temp_pin_cnt
temp_pin_list
temp_pin_next_id
```

#### 版本策略

- 新模块引入时写入 `pin_store_ver = 1`
- 后续若结构体变更，递增版本并提供迁移逻辑
- 本次无需兼容多个历史版本，只需兼容“没有该模块数据”的情况

### 5.5 首次迁移规则

#### 触发条件

当 `perm_pin_cnt` 不存在或无有效长期 PIN 记录时：

1. 自动初始化管理员默认账号：
   - username = `admin`
   - password = `esp32s3-menjin`
   - `admin_default = true`
2. 自动创建一条默认长期 PIN：
   - label = `Default PIN`
   - plaintext PIN = `11451`
   - 写入哈希后保存

#### 目的

保证升级到新固件后：

- 设备还能继续用原键盘码开门
- Web 管理入口可立即登录
- 用户后续可自己改管理员密码和长期 PIN

---

## 6. access_control 模块职责与接口

### 6.1 模块职责

`access_control` 模块负责：

1. 装载与保存管理员凭证
2. 装载与保存长期 PIN 列表
3. 装载与保存临时 PIN 列表
4. 校验管理员用户名/密码
5. 校验键盘输入 PIN
6. 生成新的临时 PIN
7. 删除或撤销 PIN
8. 清理过期临时 PIN
9. 做 PIN 格式校验
10. 做旧硬编码 PIN 的一次性迁移

### 6.2 建议对外接口

```cpp
void loadAccessControl(Preferences& prefs, AccessControlState& state);
void saveAccessControl(Preferences& prefs, const AccessControlState& state);
void ensureAccessControlInitialized(Preferences& prefs, AccessControlState& state);

bool isAdminUsingDefaultCredentials(const AccessControlState& state);
bool verifyAdminCredentials(const AccessControlState& state, const String& username, const String& password);
bool updateAdminPassword(Preferences& prefs, AccessControlState& state, const String& username, const String& newPassword, String& message);

bool isValidPermanentPinFormat(const String& pin);
bool isValidTemporaryPinFormat(const String& pin);

bool verifyKeypadCode(AccessControlState& state, const String& pin, uint32_t nowEpoch, bool timeSynced, String& matchedSource);

int listPermanentPinsJson(const AccessControlState& state, String& json);
int upsertPermanentPin(Preferences& prefs, AccessControlState& state, int32_t id, const String& label, const String& pin, String& message);
int deletePermanentPin(Preferences& prefs, AccessControlState& state, int32_t id, String& message);

int listTemporaryPinsJson(AccessControlState& state, uint32_t nowEpoch, bool timeSynced, String& json);
int generateTemporaryPin(Preferences& prefs, AccessControlState& state, uint32_t expiresAtEpoch, uint32_t nowEpoch, bool timeSynced, String& generatedPin, String& message);
int revokeTemporaryPin(Preferences& prefs, AccessControlState& state, int32_t id, String& message);

void pruneExpiredTemporaryPins(Preferences& prefs, AccessControlState& state, uint32_t nowEpoch, bool timeSynced);
```

#### 说明

- `verifyKeypadCode(...)` 成功时返回 `true`，并把 `matchedSource` 设置为 `Permanent PIN` 或 `Temporary PIN`
- 列表接口返回 JSON 字符串是为了减少 `web_portal.cpp` 里重复拼接逻辑
- 如实现时觉得返回 JSON 过重，也可改成“返回结构体列表 + web_portal 自己序列化”，但不要把业务状态散落到多个文件

---

## 7. Web 鉴权设计

### 7.1 基本策略

所有管理页面与管理接口都走 HTTP Basic Auth。

#### 必须保护的路由

现有路由：

- `/`
- `/open`
- `/add_nfc`
- `/configure_network`
- `/set_wifi`
- `/skip_provision`

新增路由：

- `/pin_users`
- `/pin_user`
- `/pin_user_delete`
- `/temp_pins`
- `/temp_pin_generate`
- `/temp_pin_revoke`
- `/admin_password`
- `/time_status`

#### 目的

- 现有 Web 开门和配置动作必须被管理员保护
- 新增 PIN 管理功能不能裸奔在局域网

### 7.2 重要实现约束

**不要为了使用 `server.authenticate(user, pass)` 而把管理员密码改成明文存储。**

如果 `WebServer::authenticate(...)` 只能匹配明文账户：

1. 读取请求头 `Authorization`
2. 解析 `Basic <base64(username:password)>`
3. 解码后得到用户名和密码
4. 调用 `verifyAdminCredentials(...)`
5. 失败时调用 `server.requestAuthentication()` 返回 401 challenge

#### 结论

- `requestAuthentication()` 可以继续用
- 凭证校验要改为自定义校验
- 管理员密码依然只保存摘要

### 7.3 默认管理员密码提示

Web 页面应明确显示以下状态之一：

- `正在使用默认管理员密码，请尽快修改`
- `管理员密码已自定义`

该状态由 `admin_default` 决定，不要通过“猜当前哈希是否等于默认密码”来判断。

---

## 8. 时间同步设计

### 8.1 时间来源

设备时间通过 NTP 同步获得。

#### 推荐接入

在主 `.ino` 中增加：

- `void ensureTimeSyncStarted();`
- `bool isSystemTimeSynced();`
- `uint32_t currentEpochSeconds();`

#### 推荐实现

- 调用 `configTzTime("CST-8", ...)`
- NTP 服务器可用本地常见源 + 公共兜底
- `isSystemTimeSynced()` 通过 `time(nullptr) >= kMinValidEpoch` 判断

> `Asia/Shanghai` 无夏令时，`CST-8` 足够满足本需求。

### 8.2 时间同步接入点

#### 在 `setup()` 中

- 若设备已联网或即将联网，可初始化时间同步子系统

#### 在 `maintainWiFiConnection()` 中

- 当 Wi‑Fi 从未连接变成已连接时，确保时间同步逻辑被触发

#### 在 Web 状态接口中

新增 `/time_status`，返回：

- 是否已校时
- 当前 epoch
- 当前北京时间显示字符串

### 8.3 未校时规则

这是强约束：

- 长期 PIN：可正常使用
- 临时 PIN：不可创建、不可验证、不可显示为有效

#### UI 要求

Web 页面必须明确提示：

- `设备尚未完成校时，临时密码功能不可用`

#### 重启规则

用户已确认：

- 重启后，哪怕临时 PIN 仍存于 NVS 中，只要尚未重新校时，就**仍然禁用**

因此：

- **不要**持久化“最后同步时间偏移”来在重启后继续认为时间可信

---

## 9. PIN 规则与验证规范

### 9.1 键盘输入规则

保留现有行为：

- `*`：清空输入
- `#`：提交验证
- 输入超时：10 秒清空
- 输错锁定：5 次失败后锁定 30 秒

新增规则：

- 只有 `0-9` 会被拼入 PIN
- `A-D` 不参与 PIN，直接忽略
- 最大 PIN 长度仍保留 10

### 9.2 长期 PIN 规则

- 字符：仅数字
- 长度：建议 4~10 位
- 每个用户一个 label
- 用户数上限：10
- 应拒绝与其他长期 PIN 重复
- 建议也拒绝与当前有效临时 PIN 冲突

### 9.3 临时 PIN 规则

- 字符：仅数字
- 长度：固定 4 位
- 必须设置到期时间
- 到期前可重复开门
- 到期后立即失效
- 创建时应避免与：
  - 已存在长期 PIN 冲突
  - 已存在且未过期的临时 PIN 冲突

#### 生成策略

- 用 `esp_random()` 生成 4 位数字字符串
- 若碰撞则重试
- 设置合理重试上限，超过后返回 500 或 409，提示重试

### 9.4 验证顺序

键盘提交时按以下顺序验证：

1. 长期 PIN
2. 若 `timeSynced == true`，再验证临时 PIN

#### 设计原因

- 长期 PIN 是稳定主凭证
- 未校时前禁止临时 PIN
- 即使未来出现碰撞，也优先命中长期 PIN

---

## 10. Web API 契约

### 10.1 总体规范

- 读操作：`GET`
- 写操作：`POST`
- 所有敏感变更都使用 POST
- 不把密码、PIN 放在 URL query string 中
- 请求体推荐 `application/x-www-form-urlencoded`
  - 原因：比 GET 安全，且比 JSON 更容易被当前 `WebServer` 直接读取 `server.arg(...)`

#### 响应规范

继续沿用当前风格：

```json
{
  "status": "ok",
  "message": "...",
  "data": { ... }
}
```

为此可在 `web_portal.cpp` 增加一个新的 JSON 响应辅助函数，例如：

- `sendJsonData(...)`

不要把列表接口退化成纯文本。

### 10.2 接口明细

#### GET `/time_status`

返回：

- `timeSynced`
- `epoch`
- `localTime`
- `timezone`

#### GET `/pin_users`

返回长期 PIN 用户列表：

- `id`
- `label`
- `enabled`

**禁止返回 PIN 明文或哈希。**

#### POST `/pin_user`

请求字段：

- `id`：为空表示新增；非空表示更新
- `label`
- `pin`

规则：

- 新增：创建新长期 PIN
- 更新：按 `id` 更新指定长期 PIN

#### POST `/pin_user_delete`

请求字段：

- `id`

#### GET `/temp_pins`

返回临时 PIN 列表：

- `id`
- `enabled`
- `expiresAtEpoch`
- `expiresAtLocal`
- `timeSynced`

**默认不返回临时 PIN 明文。**

#### POST `/temp_pin_generate`

请求字段：

- `expires_at_epoch`

规则：

- 当前时间未同步时，返回错误
- `expires_at_epoch <= now` 时返回 400
- 成功时响应里返回一次性明文：
  - `generatedPin`
  - `expiresAtLocal`

#### POST `/temp_pin_revoke`

请求字段：

- `id`

#### POST `/admin_password`

请求字段：

- `username`
- `new_password`

可选附加：

- `confirm_password`

规则：

- 成功后 `admin_default = false`
- 成功后旧密码立刻失效

---

## 11. Web 页面改造规范

### 11.1 页面结构

继续使用 `web_portal.cpp` 中当前静态 HTML 模式，在控制台页面中新增 3 张卡片：

#### 卡片 A：长期密码管理

内容：

- 列表区域
- label 输入框
- PIN 输入框
- 新增/保存按钮
- 删除按钮

#### 卡片 B：临时密码管理

内容：

- 到期时间输入
- 生成按钮
- 当前临时 PIN 列表
- 撤销按钮
- 时间同步状态提示

#### 卡片 C：管理员账户设置

内容：

- 用户名输入框（默认为 admin，可保留可编辑或固定）
- 新密码输入框
- 确认输入框
- 保存按钮
- 默认密码提示文案

### 11.2 到期时间输入的前端规则

不要直接把浏览器本地时区的 `Date` 行为当成 Asia/Shanghai。

#### 推荐做法

前端仍可使用 `<input type="datetime-local">`，但提交时：

1. 手动解析 `YYYY-MM-DDTHH:mm`
2. 按 **Asia/Shanghai = UTC+8** 规则转换为 epoch
3. 把 `expires_at_epoch` 提交给后端

#### 原因

- 浏览器本地时区可能不是北京时间
- 用户要求的判定时区已经固定为 `Asia/Shanghai`

#### 展示规则

临时 PIN 列表里的到期时间统一按 Asia/Shanghai 格式展示。

---

## 12. 主固件修改规范

### 12.1 `menjin_esp32s3___official_version3.0.ino`

#### 必做项

1. 引入 `access_control.h`
2. 增加全局 `AccessControlState accessControl;`
3. 在 `setup()` 中：
   - `prefs.begin(...)` 后加载 access control
   - 执行首次迁移
4. 在 `getWebPortalContext()` 中把 access control 接给 Web 层
5. 在 Wi‑Fi 恢复后确保时间同步启动
6. 修改 `checkKeypad()`：
   - 删除 `DOOR_PASSWORD` 直接比较
   - 改为调用 `verifyKeypadCode(...)`
   - 成功后继续走 `authorizeDoorOpen(...)`

#### 不要改动的行为

- 键盘锁定机制
- `authorizeDoorOpen()` 的开门链路
- 指纹/NFC/MQTT 的开门链路

### 12.2 `web_portal.h`

#### 必做项

- 引入 `access_control.h`
- 在 `WebPortalContext` 中加入：
  - `AccessControlState& accessControl;`

#### 不建议做法

不要为了 PIN 管理再加一长串回调 typedef；这次直接在 `web_portal.cpp` 调用 `access_control` 模块即可。

### 12.3 `web_portal.cpp`

#### 必做项

1. 增加鉴权辅助函数
2. 所有受保护路由先执行鉴权
3. 扩展控制台 HTML
4. 新增 PIN 管理与管理员密码修改接口
5. 新增时间状态接口
6. 新增支持 `data` 的 JSON 响应辅助函数

#### 保持不变

- `sendJson()` 当前简单用法可保留
- 现有 `/open`、`/add_nfc`、`/configure_network` 等业务语义保持不变，只加保护

---

## 13. 过期清理策略

临时 PIN 过期清理需要在多个点触发：

1. `loadAccessControl()` 后，如果当前时间已同步，则立即清理
2. 每次 `GET /temp_pins` 前清理
3. 每次 `generateTemporaryPin()` 前清理
4. 每次 `verifyKeypadCode()` 验证临时 PIN 前清理

### 未校时时的处理

未校时时**不要**根据 `epoch=0` 之类的值误删临时 PIN；只是不让它生效。

---

## 14. 错误处理与返回码规范

### 400

请求参数缺失、格式错误、到期时间无效、PIN 格式非法。

### 401

未认证或认证失败。

### 404

指定 `id` 不存在。

### 409

PIN 冲突、重复用户、重复临时 PIN。

### 507

长期 PIN 或临时 PIN 存储已满。

### 500

内部状态异常、随机生成多次碰撞仍失败、NVS 写入失败等。

---

## 15. 分阶段实施顺序（给后续 agent）

### 阶段 1：access_control 模块

先完成：

- 结构体
- 哈希/盐
- NVS 读写
- 初始化迁移
- 管理员凭证校验
- 长期 PIN CRUD
- 临时 PIN CRUD 与校验

#### 阶段完成标志

- 模块能独立编译
- 不依赖 web_portal 页面已完成

### 阶段 2：主固件集成

完成：

- 全局状态接入
- `setup()` 中初始化
- `checkKeypad()` 改造
- NTP / time helper

#### 阶段完成标志

- 键盘长期 PIN 可通过新模块验证
- 编译通过

### 阶段 3：Web 鉴权与管理接口

完成：

- Basic Auth 校验
- 新增路由
- 新增 JSON 响应数据结构
- 页面新增 3 张卡片

#### 阶段完成标志

- 默认管理员可登录
- 能在页面创建/修改/删除长期 PIN
- 能生成/撤销临时 PIN
- 能修改管理员密码

### 阶段 4：联调与回归

完成：

- 首次迁移验证
- 时间同步验证
- 临时 PIN 生命周期验证
- 原 NFC / 开门 / 配网回归验证

---

## 16. 建议的 agent 分工

如果后续要并行执行，建议按以下切片：

### Agent A：access_control 存储与哈希

负责：

- `access_control.h/.cpp`
- NVS 结构
- SHA-256 + salt
- 默认迁移

### Agent B：主固件接入

负责：

- `.ino` 中 `AccessControlState` 接入
- `checkKeypad()` 改造
- NTP / time helper

### Agent C：web_portal 鉴权与页面

负责：

- Basic Auth
- 新路由
- HTML/JS 页面
- `time_status`

### Agent D：联调与回归

负责：

- 编译
- 浏览器验证
- 键盘验证
- 回归问题修补

### 依赖关系

- B 依赖 A
- C 依赖 A，部分依赖 B 的时间状态接口
- D 依赖 A/B/C

---

## 17. 验收标准

以下全部满足才算完成：

### 管理员登录

- 访问 `/` 会要求认证
- 默认凭证 `admin / esp32s3-menjin` 可登录
- 修改管理员密码后旧密码失效
- 页面可提示默认密码状态

### 长期 PIN

- 升级后首次启动可自动迁移出 `11451`
- 可新增长期 PIN
- 可更新长期 PIN
- 可删除长期 PIN
- 重启后长期 PIN 仍存在
- 键盘输入长期 PIN 可开门

### 临时 PIN

- 未校时前无法创建
- 未校时前即使已存在记录也无法生效
- 校时成功后可生成 4 位 PIN
- 到期前可重复使用
- 到期后失效
- 可手动撤销

### 键盘行为

- `*` 清空输入
- `#` 提交输入
- A-D 不被拼入 PIN
- 超时清空仍有效
- 连续输错锁定仍有效

### 回归

- `/open` 正常
- `/add_nfc` 正常
- `/configure_network` 与 `/set_wifi` 正常
- `/skip_provision` 正常
- NFC / 指纹 / MQTT / OTA / 门锁舵机流程无回归

---

## 18. 风险与注意事项

1. **HTTP Basic Auth 仍然是明文 HTTP 传输**
   - 这是用户接受的最小改动方案
   - 适合作为局域网管理保护，不应宣传成强安全远程方案
2. **不要用 GET 提交密码或 PIN**
   - URL 会被缓存和日志记录
3. **不要为了图方便把管理员密码明文存 NVS**
   - 哪怕 `server.authenticate()` 更省事，也不允许
4. **不要把临时 PIN 在列表接口里反复回显**
   - 明文只在“刚生成成功”的响应里返回一次
5. **不要在未校时时误判临时 PIN 有效**
   - 这是需求红线
6. **不要顺手重构现有门禁主流程**
   - 本次需求不需要大规模整理 `.ino`

---

## 19. 最终执行说明

后续 agent 实施时，应优先遵守本文档中的：

1. 已冻结需求
2. 数据模型与安全要求
3. API 契约
4. 分阶段实施顺序
5. 验收标准

若实现过程中发现现有 `WebServer` 或 Arduino-ESP32 API 与本文档某个“建议接口名”不完全一致，允许调整函数命名或局部实现方式，但**不允许改变本文档已固定的产品行为和安全边界**。
