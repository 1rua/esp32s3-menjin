# Remaining Web Auth and PIN Bugfixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复当前实现中剩余的 3 个缺陷：长期 PIN 与临时 PIN 的冲突判断错误、管理员改密缺少当前密码确认、前端对 Basic Auth 401 challenge 的非 JSON 响应处理不稳。

**Architecture:** 保持修复范围收敛在 `access_control` 与 `web_portal` 两个模块，不改动 `.ino` 主开门链路、不改时间同步实现、不改用户已明确搁置的两个问题。`access_control` 继续负责凭证/PIN 语义，`web_portal` 负责 HTTP 契约、页面字段和前端交互容错。

**Tech Stack:** Arduino-ESP32, `WebServer`, `Preferences`, 静态 HTML/JS, mbedTLS SHA-256/Base64, `arduino-cli` 编译验证。

---

## Scope Freeze

本计划**只修复**以下 3 个问题：

1. 长期 PIN 创建/更新时，把**所有**临时 PIN 都当成冲突对象，而不是只看**当前有效**临时 PIN。
2. `/admin_password` 当前只要请求已通过 Basic Auth，就能直接重置管理员密码，缺少“输入当前密码再次确认”这一步。
3. 控制台前端的 `requestJson()` 对 401 Basic Auth challenge 一律 `response.json()`，会把非 JSON 认证响应变成前端异常。

本计划**不修复**用户已明确搁置的两个问题：

- 非北京时间浏览器环境下的 `datetime-local` 解释误导。
- 删除最后一个长期 PIN 会导致没有长期键盘码可用。

---

## Files and Responsibilities

- Modify: `esp32-s3-menjin-version3.0/access_control.h`
  - 收敛访问控制公开接口；把“当前密码校验”和“只检查有效临时 PIN 冲突”的语义显式体现在函数签名里。
- Modify: `esp32-s3-menjin-version3.0/access_control.cpp`
  - 实现新的冲突判断 helper、管理员当前密码校验、管理员改密校验逻辑。
- Modify: `esp32-s3-menjin-version3.0/web_portal.cpp`
  - 更新 `/pin_user` 和 `/admin_password` 路由；调整管理员设置卡片字段；修复 `requestJson()` 对 401 / 非 JSON 响应的处理。
- Optional doc sync after implementation: `docs/superpowers/plans/2026-4-19-web-sharded-haven.md`
  - 只在代码修复完成后补记 API 契约变化（`current_password` 从可选增强变成必填字段），避免文档与实现脱节。

---

## Phase Plan

### Phase 1: 后端 PIN 冲突语义修复

目标：让长期 PIN 只与“当前有效的临时 PIN”冲突。

### Phase 2: 管理员改密重新确认当前密码

目标：即便请求已经通过 Basic Auth，也必须显式提供当前管理员密码才能改密。

### Phase 3: 前端 401 / 非 JSON 响应容错

目标：保留后端 Basic Auth challenge 行为，同时避免页面因为 `Unexpected token <` 之类的 JSON 解析错误而崩掉。

### Phase 4: 编译 + 手工回归

目标：确认 3 个修复不影响现有 Web 控制、临时 PIN、生效验证与管理员登录链路。

---

### Task 1: Restrict permanent PIN conflict checks to active temporary PINs

**Files:**
- Modify: `esp32-s3-menjin-version3.0/access_control.h`
- Modify: `esp32-s3-menjin-version3.0/access_control.cpp`
- Modify: `esp32-s3-menjin-version3.0/web_portal.cpp`
- Test: `.claude/compile-menjin/compile-menjin.ino` (generated during verification)

- [ ] **Step 1: Change the public API so permanent PIN writes know the current clock state**

```cpp
// access_control.h
int upsertPermanentPin(Preferences& prefs,
                       AccessControlState& state,
                       int32_t id,
                       const String& label,
                       const String& pin,
                       uint32_t nowEpoch,
                       bool timeSynced,
                       String& message);
```

Why this shape:
- `upsertPermanentPin(...)` is the place that enforces write-time uniqueness.
- The function cannot decide whether a temporary PIN is “currently active” unless it knows `nowEpoch` and `timeSynced`.
- This keeps the rule inside `access_control`, not duplicated in route code.

- [ ] **Step 2: Run compile to verify call sites fail before the implementation is updated**

Run:
```bash
mkdir -p .claude/compile-menjin && \
cp esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino .claude/compile-menjin/compile-menjin.ino && \
cp esp32-s3-menjin-version3.0/device_config.h esp32-s3-menjin-version3.0/device_config.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/provisioning.h esp32-s3-menjin-version3.0/provisioning.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/web_portal.h esp32-s3-menjin-version3.0/web_portal.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/access_control.h esp32-s3-menjin-version3.0/access_control.cpp .claude/compile-menjin/ && \
arduino-cli compile --fqbn esp32:esp32:esp32s3 .claude/compile-menjin
```

Expected: FAIL with a signature mismatch at the `/pin_user` call site in `web_portal.cpp`.

- [ ] **Step 3: Implement an “active temporary PIN only” helper and use it in permanent PIN writes**

```cpp
// access_control.cpp
bool temporaryPinMatchesActive(const AccessControlState& state,
                               const String& pin,
                               uint32_t nowEpoch,
                               bool timeSynced,
                               int32_t skipId = -1) {
  if (!timeSynced || nowEpoch < kMinValidEpoch) {
    return false;
  }

  for (uint8_t i = 0; i < state.temporaryPinCount; ++i) {
    const TemporaryPinRecord& record = state.temporaryPins[i];
    if (!record.enabled || static_cast<int32_t>(record.id) == skipId) {
      continue;
    }
    if (record.expiresAtEpoch <= nowEpoch) {
      continue;
    }
    if (verifyHash(record.salt, record.hash, pin)) {
      return true;
    }
  }
  return false;
}

int upsertPermanentPin(Preferences& prefs,
                       AccessControlState& state,
                       int32_t id,
                       const String& label,
                       const String& pin,
                       uint32_t nowEpoch,
                       bool timeSynced,
                       String& message) {
  const String normalizedLabel = trimCopy(label);
  if (normalizedLabel.length() == 0 || normalizedLabel.length() >= kPinLabelMaxLength) {
    message = "Invalid label";
    return 400;
  }
  if (!isValidPermanentPinFormat(pin)) {
    message = "Permanent PIN must be 4-10 digits";
    return 400;
  }
  if (permanentPinMatches(state, pin, id)) {
    message = "Permanent PIN already exists";
    return 409;
  }
  if (temporaryPinMatchesActive(state, pin, nowEpoch, timeSynced)) {
    message = "PIN conflicts with an active temporary PIN";
    return 409;
  }

  // keep the existing create/update branches unchanged
}
```

Important constraints:
- Do **not** change `verifyKeypadCode(...)`; it already checks expiry at read time.
- Do **not** change `generateTemporaryPin(...)`; it already prunes expired records before generating.
- The only semantic correction here is the write-time conflict set.

- [ ] **Step 4: Update the `/pin_user` route to prune first and pass the live clock state into `upsertPermanentPin(...)`**

```cpp
// web_portal.cpp inside handlePinUserRoute()
const String idValue = readRequestArg("id");
const int32_t id = idValue.length() == 0 ? -1 : idValue.toInt();
const String label = readRequestArg("label");
const String pin = readRequestArg("pin");

const bool synced = timeSynced();
const uint32_t now = currentEpoch();
pruneExpiredTemporaryPins(gContext->prefs, gContext->accessControl, now, synced);

String message;
const int statusCode = upsertPermanentPin(gContext->prefs,
                                          gContext->accessControl,
                                          id,
                                          label,
                                          pin,
                                          now,
                                          synced,
                                          message);
sendJson(statusCode, statusCode == 200 ? "ok" : "error", message);
```

Why prune here:
- It keeps the in-memory/NVS temp list tidy before conflict evaluation.
- It ensures an already-expired temp PIN does not keep occupying storage until some unrelated later action.

- [ ] **Step 5: Run compile to verify the new API and route build together**

Run:
```bash
mkdir -p .claude/compile-menjin && \
cp esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino .claude/compile-menjin/compile-menjin.ino && \
cp esp32-s3-menjin-version3.0/device_config.h esp32-s3-menjin-version3.0/device_config.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/provisioning.h esp32-s3-menjin-version3.0/provisioning.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/web_portal.h esp32-s3-menjin-version3.0/web_portal.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/access_control.h esp32-s3-menjin-version3.0/access_control.cpp .claude/compile-menjin/ && \
arduino-cli compile --fqbn esp32:esp32:esp32s3 .claude/compile-menjin
```

Expected: PASS.

- [ ] **Step 6: Manually verify the corrected conflict semantics over HTTP**

Run against a real device after flashing:
```bash
curl -u admin:esp32s3-menjin "http://DEVICE_IP/time_status"
curl -u admin:esp32s3-menjin -X POST "http://DEVICE_IP/temp_pin_generate" -d "expires_at_epoch=1893456000"
# note generatedPin from the response
curl -i -u admin:esp32s3-menjin -X POST "http://DEVICE_IP/pin_user" -d "label=TempConflict&pin=THE_GENERATED_PIN"
```

Expected while the temporary PIN is still active:
- `/temp_pin_generate` returns `200` with `generatedPin`.
- `/pin_user` returns `409` with message `PIN conflicts with an active temporary PIN`.

Then revoke or let the temp PIN expire:
```bash
curl -i -u admin:esp32s3-menjin -X POST "http://DEVICE_IP/temp_pin_revoke" -d "id=TEMP_PIN_ID"
curl -i -u admin:esp32s3-menjin -X POST "http://DEVICE_IP/pin_user" -d "label=TempConflict&pin=THE_GENERATED_PIN"
```

Expected after revoke/expiry:
- `/pin_user` returns `200` and the permanent PIN is created successfully.

- [ ] **Step 7: Commit the isolated semantic fix**

```bash
git add esp32-s3-menjin-version3.0/access_control.h \
        esp32-s3-menjin-version3.0/access_control.cpp \
        esp32-s3-menjin-version3.0/web_portal.cpp && \
git commit -m "fix(access-control): only block permanent pins on active temp-pin conflicts"
```

---

### Task 2: Require the current admin password when rotating admin credentials

**Files:**
- Modify: `esp32-s3-menjin-version3.0/access_control.h`
- Modify: `esp32-s3-menjin-version3.0/access_control.cpp`
- Modify: `esp32-s3-menjin-version3.0/web_portal.cpp`
- Test: `.claude/compile-menjin/compile-menjin.ino` (generated during verification)

- [ ] **Step 1: Extend the access-control API so credential rotation requires the current password**

```cpp
// access_control.h
bool verifyAdminPassword(const AccessControlState& state, const String& password);

bool updateAdminPassword(Preferences& prefs,
                         AccessControlState& state,
                         const String& currentPassword,
                         const String& username,
                         const String& newPassword,
                         String& message);
```

Design choice:
- Keep `requireAuth()` in `web_portal.cpp`; that is still the first gate.
- Add `currentPassword` as a second gate for the specific destructive action “rotate admin credentials”.
- Do **not** add session state, cookies, or CSRF machinery; that would exceed this bugfix scope.

- [ ] **Step 2: Run compile to verify the old caller fails before the implementation is updated**

Run:
```bash
mkdir -p .claude/compile-menjin && \
cp esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino .claude/compile-menjin/compile-menjin.ino && \
cp esp32-s3-menjin-version3.0/device_config.h esp32-s3-menjin-version3.0/device_config.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/provisioning.h esp32-s3-menjin-version3.0/provisioning.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/web_portal.h esp32-s3-menjin-version3.0/web_portal.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/access_control.h esp32-s3-menjin-version3.0/access_control.cpp .claude/compile-menjin/ && \
arduino-cli compile --fqbn esp32:esp32:esp32s3 .claude/compile-menjin
```

Expected: FAIL with a signature mismatch at `handleAdminPasswordRoute()`.

- [ ] **Step 3: Implement current-password verification inside `access_control.cpp`**

```cpp
// access_control.cpp
bool verifyAdminPassword(const AccessControlState& state, const String& password) {
  if (password.length() == 0) {
    return false;
  }
  return verifyHash(state.adminSalt, state.adminHash, password);
}

bool updateAdminPassword(Preferences& prefs,
                         AccessControlState& state,
                         const String& currentPassword,
                         const String& username,
                         const String& newPassword,
                         String& message) {
  const String normalizedUsername = trimCopy(username);
  if (!verifyAdminPassword(state, currentPassword)) {
    message = "Current admin password is incorrect";
    return false;
  }
  if (!isValidAdminUsername(normalizedUsername)) {
    message = "Invalid admin username";
    return false;
  }
  if (newPassword.length() == 0) {
    message = "Admin password cannot be empty";
    return false;
  }

  setCredential(state.adminUsername,
                sizeof(state.adminUsername),
                state.adminSalt,
                state.adminHash,
                normalizedUsername,
                newPassword);
  state.adminUsesDefaultCredentials = false;
  saveAccessControl(prefs, state);
  message = "Admin credentials updated";
  return true;
}
```

Important:
- Keep `adminUsesDefaultCredentials = false` only on success.
- Do not special-case the default password; the verification path must be identical for default and custom credentials.

- [ ] **Step 4: Add a current-password field to the control page and send it to `/admin_password`**

```html
<!-- web_portal.cpp CONTROL_PAGE_HTML admin card -->
<input type="text" id="adminUsername" value="admin" placeholder="管理员用户名">
<input type="password" id="adminCurrentPassword" placeholder="当前管理员密码">
<input type="password" id="adminPassword" placeholder="新密码">
<input type="password" id="adminPasswordConfirm" placeholder="确认新密码">
<button class="btn-secondary" onclick="updateAdminPassword()">保存管理员密码</button>
```

```js
// web_portal.cpp CONTROL_PAGE_HTML script
async function updateAdminPassword() {
  const username = document.getElementById('adminUsername').value.trim();
  const currentPassword = document.getElementById('adminCurrentPassword').value;
  const newPassword = document.getElementById('adminPassword').value;
  const confirmPassword = document.getElementById('adminPasswordConfirm').value;
  if (!username) return alert('必须输入管理员用户名');
  if (!currentPassword) return alert('必须输入当前管理员密码');
  if (!newPassword) return alert('必须输入新密码');
  if (newPassword !== confirmPassword) return alert('两次密码输入不一致');

  const data = await requestJson('/admin_password', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: encodeForm({
      username,
      current_password: currentPassword,
      new_password: newPassword,
      confirm_password: confirmPassword
    })
  });

  showMessage(data, '管理员密码已更新');
  document.getElementById('adminCurrentPassword').value = '';
  document.getElementById('adminPassword').value = '';
  document.getElementById('adminPasswordConfirm').value = '';
  await loadAdminStatus();
}
```

- [ ] **Step 5: Update the route handler so missing/incorrect current passwords fail cleanly**

```cpp
// web_portal.cpp inside handleAdminPasswordRoute()
const String username = readRequestArg("username");
const String currentPassword = readRequestArg("current_password");
const String newPassword = readRequestArg("new_password");
const String confirmPassword = readRequestArg("confirm_password");
if (username.length() == 0 || currentPassword.length() == 0 || newPassword.length() == 0) {
  sendJson(400, "error", "Missing admin credentials");
  return;
}
if (confirmPassword.length() > 0 && confirmPassword != newPassword) {
  sendJson(400, "error", "Password confirmation mismatch");
  return;
}

String message;
const bool ok = updateAdminPassword(gContext->prefs,
                                    gContext->accessControl,
                                    currentPassword,
                                    username,
                                    newPassword,
                                    message);
sendJson(ok ? 200 : 400, ok ? "ok" : "error", message);
```

- [ ] **Step 6: Run compile to verify the UI and route changes build together**

Run:
```bash
mkdir -p .claude/compile-menjin && \
cp esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino .claude/compile-menjin/compile-menjin.ino && \
cp esp32-s3-menjin-version3.0/device_config.h esp32-s3-menjin-version3.0/device_config.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/provisioning.h esp32-s3-menjin-version3.0/provisioning.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/web_portal.h esp32-s3-menjin-version3.0/web_portal.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/access_control.h esp32-s3-menjin-version3.0/access_control.cpp .claude/compile-menjin/ && \
arduino-cli compile --fqbn esp32:esp32:esp32s3 .claude/compile-menjin
```

Expected: PASS.

- [ ] **Step 7: Manually verify the password-rotation flow over HTTP**

Wrong current password must fail:
```bash
curl -i -u admin:esp32s3-menjin -X POST "http://DEVICE_IP/admin_password" \
  -d "username=admin&current_password=wrongpass&new_password=new-secret&confirm_password=new-secret"
```

Expected:
- `400 Bad Request`
- JSON message `Current admin password is incorrect`
- Old credentials still work on `/time_status`

Correct current password must succeed:
```bash
curl -i -u admin:esp32s3-menjin -X POST "http://DEVICE_IP/admin_password" \
  -d "username=admin&current_password=esp32s3-menjin&new_password=new-secret&confirm_password=new-secret"
```

Expected:
- `200 OK`
- JSON message `Admin credentials updated`

Then verify credential rollover:
```bash
curl -i -u admin:esp32s3-menjin "http://DEVICE_IP/time_status"
curl -i -u admin:new-secret "http://DEVICE_IP/time_status"
```

Expected:
- Old password now returns `401`.
- New password returns `200`.

- [ ] **Step 8: Commit the credential-rotation hardening**

```bash
git add esp32-s3-menjin-version3.0/access_control.h \
        esp32-s3-menjin-version3.0/access_control.cpp \
        esp32-s3-menjin-version3.0/web_portal.cpp && \
git commit -m "fix(web): require current password for admin credential changes"
```

---

### Task 3: Make frontend JSON requests tolerant of Basic Auth challenges

**Files:**
- Modify: `esp32-s3-menjin-version3.0/web_portal.cpp`
- Test: `.claude/compile-menjin/compile-menjin.ino` (generated during verification)

- [ ] **Step 1: Replace the blind `response.json()` path with content-type-aware parsing**

```js
// web_portal.cpp CONTROL_PAGE_HTML and PROVISIONING_PAGE_HTML scripts
async function requestJson(url, options) {
  const response = await fetch(url, options || {});
  const contentType = response.headers.get('content-type') || '';
  const isJson = contentType.includes('application/json');
  const data = isJson ? await response.json() : null;

  if (response.status === 401) {
    throw new Error('认证已失效，请刷新页面后重新登录');
  }
  if (!response.ok) {
    throw new Error((data && data.message) ? data.message : '请求失败');
  }
  if (!data) {
    throw new Error('服务器返回了非 JSON 响应');
  }
  return data;
}
```

Why this exact fix:
- It preserves the backend’s `requestAuthentication()` challenge behavior.
- It removes the brittle assumption that every failure body is JSON.
- It keeps the page logic simple: the caller still gets a normal `Error` with a user-facing message.

- [ ] **Step 2: Keep initialization and action handlers on the same promise/error model**

```js
async function initializePage() {
  try {
    await loadPinUsers();
    await loadTimeStatus();
    await loadTempPins();
    await loadAdminStatus();
  } catch (error) {
    alert(error.message || '页面初始化失败');
  }
}
```

Do not add a second fetch helper or fork the error model. The goal is to make the existing flow robust, not to redesign the page.

- [ ] **Step 3: Run compile to verify the static page changes do not break the sketch build**

Run:
```bash
mkdir -p .claude/compile-menjin && \
cp esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino .claude/compile-menjin/compile-menjin.ino && \
cp esp32-s3-menjin-version3.0/device_config.h esp32-s3-menjin-version3.0/device_config.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/provisioning.h esp32-s3-menjin-version3.0/provisioning.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/web_portal.h esp32-s3-menjin-version3.0/web_portal.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/access_control.h esp32-s3-menjin-version3.0/access_control.cpp .claude/compile-menjin/ && \
arduino-cli compile --fqbn esp32:esp32:esp32s3 .claude/compile-menjin
```

Expected: PASS.

- [ ] **Step 4: Manually verify the page behavior when auth is missing or stale**

Browser verification checklist:
1. Open `http://DEVICE_IP/` in a fresh private window.
2. Cancel the browser’s Basic Auth prompt or enter wrong credentials.
3. Confirm the page does **not** crash with a raw JSON parse exception.
4. Log in successfully.
5. Change the admin password in another tab or via `curl`.
6. Return to the original tab and trigger any action like “立即开门” or “读取时间状态”.
7. Confirm the user sees `认证已失效，请刷新页面后重新登录` rather than a generic `Unexpected token <` or silent failure.

CLI spot-check for the backend challenge remaining intact:
```bash
curl -i "http://DEVICE_IP/time_status"
```

Expected:
- `401 Unauthorized`
- `WWW-Authenticate: Basic ...`
- No requirement to change backend response format.

- [ ] **Step 5: Commit the frontend auth-challenge resilience fix**

```bash
git add esp32-s3-menjin-version3.0/web_portal.cpp && \
git commit -m "fix(web): handle non-json auth challenges in frontend requests"
```

---

### Task 4: Final regression, contract sync, and release-ready verification

**Files:**
- Modify: `docs/superpowers/plans/2026-4-19-web-sharded-haven.md` (only if implementation lands exactly as planned)
- Test: `.claude/compile-menjin/compile-menjin.ino` (generated during verification)

- [ ] **Step 1: Run one final compile on the integrated branch state**

Run:
```bash
mkdir -p .claude/compile-menjin && \
cp esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino .claude/compile-menjin/compile-menjin.ino && \
cp esp32-s3-menjin-version3.0/device_config.h esp32-s3-menjin-version3.0/device_config.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/provisioning.h esp32-s3-menjin-version3.0/provisioning.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/web_portal.h esp32-s3-menjin-version3.0/web_portal.cpp .claude/compile-menjin/ && \
cp esp32-s3-menjin-version3.0/access_control.h esp32-s3-menjin-version3.0/access_control.cpp .claude/compile-menjin/ && \
arduino-cli compile --fqbn esp32:esp32:esp32s3 .claude/compile-menjin
```

Expected: PASS.

- [ ] **Step 2: Run the focused regression checklist on a flashed device**

Checklist:
- `/` still requires Basic Auth.
- `/open` still works after successful authentication.
- `/pin_user` can create a normal long-term PIN that does not conflict with any active temp PIN.
- `/temp_pin_generate` still creates a 4-digit PIN when the device is time-synced.
- `/temp_pin_revoke` still removes the record.
- `/admin_password` now rejects missing `current_password` with `400`.
- `/admin_password` now rejects wrong `current_password` with `400`.
- `/admin_password` still invalidates the old password immediately after success.
- Browser page no longer shows raw JSON parse failures on auth problems.

- [ ] **Step 3: Sync the frozen design doc to the implemented contract**

```md
#### POST `/admin_password`

请求字段：

- `username`
- `current_password`
- `new_password`
- `confirm_password`

规则：

- 必须先通过 HTTP Basic Auth
- 还必须显式提供当前管理员密码进行二次确认
- 成功后 `admin_default = false`
- 成功后旧密码立刻失效
```

Only update the doc **after** the code lands with this exact behavior. If implementation diverges, document what actually shipped.

- [ ] **Step 4: Commit the doc sync if it was needed**

```bash
git add docs/superpowers/plans/2026-4-19-web-sharded-haven.md && \
git commit -m "docs: sync admin password api contract with implementation"
```

---

## Verification Notes

### Primary verification points

1. **Correct conflict semantics**
   - Active temp PIN blocks a permanent PIN with the same digits.
   - Revoked/expired temp PIN no longer blocks it.
   - Unsynced time should behave as “no active temporary PINs”.

2. **Correct admin-rotation semantics**
   - Being logged in is no longer enough by itself.
   - Wrong `current_password` fails.
   - Correct `current_password` succeeds.
   - Old password is invalid immediately after success.

3. **Correct frontend error behavior**
   - Auth failure no longer causes a JSON parsing exception.
   - The backend still emits a normal Basic Auth challenge.
   - The user gets a clear refresh/re-login instruction.

### Non-goals to re-check during review

Do **not** accidentally expand scope into:
- timezone UX redesign,
- last-permanent-PIN deletion rules,
- cookie/session auth,
- CSRF protection,
- `.ino` keypad refactors,
- API format redesign.

---

## Risks and Tradeoffs

1. **`/admin_password` contract becomes stricter**
   - Risk: any external script that currently calls `/admin_password` must add `current_password`.
   - Tradeoff: this is an intentional breaking change in favor of preventing silent admin takeover from an already-authenticated browser/session.

2. **`upsertPermanentPin(...)` signature changes**
   - Risk: future call sites will need the clock state.
   - Tradeoff: keeping the rule inside `access_control` is cleaner than re-implementing “active temp PIN” semantics in every route.

3. **Frontend still relies on page refresh for re-challenge UX**
   - Risk: after credentials expire, the user must refresh or reopen the page to re-enter Basic Auth credentials.
   - Tradeoff: this preserves the existing `requestAuthentication()` flow and avoids inventing a custom auth layer.

4. **No automated unit-test harness is introduced in this bugfix**
   - Risk: verification stays compile + real-device/manual HTTP driven.
   - Tradeoff: this keeps scope tight and matches the repo’s existing Arduino workflow instead of introducing a new fake-Preferences/fake-WebServer test framework mid-fix.

---

## Self-Review

- **Spec coverage:**
  - Active temp-PIN conflict semantics are covered by Task 1.
  - Admin credential hardening is covered by Task 2.
  - Non-JSON 401 frontend handling is covered by Task 3.
  - Compile/manual regression and doc sync are covered by Task 4.
- **Placeholder scan:** no `TODO`/`TBD` placeholders remain.
- **Type consistency:** the updated `upsertPermanentPin(...)` and `updateAdminPassword(...)` signatures are used consistently in later tasks.

Plan complete and saved to `docs/superpowers/plans/2026-04-20-remaining-web-auth-pin-bugfixes.md`. Two execution options:

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
