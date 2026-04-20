# Minimal Temporary PIN Time Stability Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用最小改动消除临时 PIN 到期时间在前端输入/展示链路中的时区耦合风险，确保中国默认使用场景下行为稳定且更不容易被浏览器环境影响。

**Architecture:** 不修改设备端 NTP 同步、`configTzTime()`、时区常量或临时 PIN 的存储/校验语义。修复只收敛在 Web 门户：前端不再自行把 `datetime-local` 当作“北京时间文本”硬编码换算 epoch，而是改为显式输入“距现在多少分钟后过期”；后端继续以设备当前 epoch 计算与展示最终过期时间。这样把时区敏感逻辑集中在设备端，减少浏览器本地时区对结果的影响。

**Tech Stack:** Arduino-ESP32, `WebServer`, 静态 HTML/JS, `time.h`, `Preferences`, 浏览器原生表单控件。

---

## Scope Freeze

本计划**只修复**一个问题：

1. 临时 PIN 创建链路过度依赖浏览器本地 `datetime-local` 与前端“手工减 8 小时”的假设，导致时间稳定性风险。

本计划**明确不做**以下改动：

- 不修改 [menjin_esp32s3___official_version3.0.ino:198-235](esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino#L198-L235) 的 NTP / 本地时间初始化逻辑。
- 不修改 [access_control.cpp:522-557](esp32-s3-menjin-version3.0/access_control.cpp#L522-L557) 的临时 PIN 生成、唯一性与过期校验语义。
- 不处理用户已忽略的分区/开发板构建问题。
- 不引入新的页面、API、时区配置项或“选择时区”功能。

---

## Files and Responsibilities

- Modify: `esp32-s3-menjin-version3.0/web_portal.cpp`
  - 将临时 PIN 前端输入从 `datetime-local` 改为“有效期分钟数”这种与浏览器时区无关的值。
  - 让前端调用 `/time_status` 获取设备当前 epoch，并基于“设备当前时间 + 分钟数”生成 `expires_at_epoch`。
  - 保留后端 `/temp_pin_generate` 契约不变，降低改动面。
- Verify only: `esp32-s3-menjin-version3.0/access_control.cpp`
  - 确认 `generateTemporaryPin(...)` 继续只消费绝对 epoch，无需修改。
- Verify only: `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino`
  - 确认 `currentEpochSeconds()` / `currentLocalTimeString()` 继续作为设备时钟单一真源。

---

## Phase Plan

### Phase 1: 替换前端时间输入模型

目标：把“指定某个本地日期时间”改成“从设备当前时刻起 N 分钟后过期”。

### Phase 2: 基于设备 epoch 生成绝对过期时间

目标：前端不再使用浏览器时区推导 epoch，而是显式依赖设备返回的 epoch。

### Phase 3: 页面文案与回归验证

目标：确保用户理解新的输入方式，并确认生成结果、展示时间、过期逻辑一致。

---

### Task 1: Replace timezone-sensitive datetime input with duration-based expiry input

**Files:**
- Modify: `esp32-s3-menjin-version3.0/web_portal.cpp`
- Verify: `esp32-s3-menjin-version3.0/access_control.cpp`
- Verify: `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino`

- [ ] **Step 1: Replace the `datetime-local` field with a numeric duration field**

```html
<!-- web_portal.cpp CONTROL_PAGE_HTML 临时密码管理区域 -->
<div class="card">
  <h3>临时密码管理</h3>
  <input type="number" id="tempPinDurationMinutes" min="1" max="10080" step="1" placeholder="有效期（分钟），例如 60">
  <button onclick="generateTempPin()">生成临时 PIN</button>
  <div class="hint">按设备当前时间计算到期时间，默认适用于中国使用场景，避免浏览器时区影响。</div>
  <div id="timeStatus" class="status">正在获取时间状态...</div>
  <div id="tempPinResult" class="status" style="display:none"></div>
  <div id="tempPinsList" class="list"></div>
</div>
```

Why this change:
- `input type="number"` 只表达“持续时长”，不表达“某个浏览器本地时区下的日历时间”。
- 这比继续保留 `datetime-local` 然后在前端做北京时间换算更稳定，也更符合“最小修复”的范围。
- 设备端仍然返回最终的 `expiresAtLocal`，用户依然能看到实际到期时间。

- [ ] **Step 2: Remove the browser-side Beijing conversion helper**

Delete this function from `web_portal.cpp`:

```js
function beijingEpochFromLocalInput(value) {
  const match = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})$/.exec(value || '');
  if (!match) return 0;
  const year = Number(match[1]);
  const month = Number(match[2]);
  const day = Number(match[3]);
  const hour = Number(match[4]);
  const minute = Number(match[5]);
  return Math.floor(Date.UTC(year, month - 1, day, hour - 8, minute, 0) / 1000);
}
```

Reason:
- 这个 helper 的核心问题不是实现细节，而是它把浏览器输入值和“北京时间解释规则”硬绑定在前端。
- 只要浏览器环境、设备时钟、用户理解三者有偏差，就容易出现“看起来对，但实际不是那个时间”的问题。

- [ ] **Step 3: Generate `expires_at_epoch` from the device epoch plus a duration**

Replace `generateTempPin()` in `web_portal.cpp` with:

```js
async function generateTempPin() {
  const durationValue = document.getElementById('tempPinDurationMinutes').value.trim();
  const durationMinutes = Number(durationValue);
  if (!Number.isInteger(durationMinutes) || durationMinutes <= 0) {
    return alert('请输入有效的正整数分钟数');
  }

  const timeData = await requestJson('/time_status');
  const status = timeData.data || {};
  if (!status.timeSynced || !status.epoch) {
    return alert('设备尚未完成校时，暂时无法生成临时密码');
  }

  const expiresAtEpoch = Number(status.epoch) + durationMinutes * 60;
  const data = await requestJson('/temp_pin_generate', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: encodeForm({expires_at_epoch: expiresAtEpoch})
  });

  const result = document.getElementById('tempPinResult');
  result.style.display = 'block';
  result.innerHTML = `新临时 PIN：<span class="mono">${data.data.generatedPin}</span><br>到期：${data.data.expiresAtLocal}`;
  await loadTimeStatus();
  await loadTempPins();
}
```

Constraints:
- 不改 `/temp_pin_generate` 的请求字段名，仍然发送 `expires_at_epoch`。
- 不在前端做任何时区换算，只做简单的整数分钟加法。
- 允许页面继续显示后端返回的 `expiresAtLocal`，因为最终解释权仍在设备端。

- [ ] **Step 4: Keep the time status panel focused on the device clock**

Retain the existing `loadTimeStatus()` behavior in `web_portal.cpp` with this target shape:

```js
async function loadTimeStatus() {
  const data = await requestJson('/time_status');
  const status = data.data || {};
  const el = document.getElementById('timeStatus');
  if (status.timeSynced) {
    el.innerHTML = `<span class="ok">设备已校时</span><br>设备时间：${escapeHtml(status.localTime)}<br>Epoch：${status.epoch}`;
  } else {
    el.innerHTML = '<span class="warning">设备尚未完成校时，临时密码功能不可用</span>';
  }
}
```

This is intentionally small:
- 只把文案从“北京时间”弱化为“设备时间”。
- 避免页面文案比代码契约更强，减少用户把浏览器时间和设备时间混为一谈。

- [ ] **Step 5: Run a targeted grep/readback to confirm no browser-side timezone conversion remains**

Run:
```bash
grep -n "beijingEpochFromLocalInput\|datetime-local\|tempPinExpiresAt" esp32-s3-menjin-version3.0/web_portal.cpp
```

Expected:
- No matches for `beijingEpochFromLocalInput`
- No matches for `datetime-local`
- No matches for `tempPinExpiresAt`

If `grep` is unavailable in this environment, use Claude Code `Grep` tool with the same patterns before declaring the task complete.

- [ ] **Step 6: Commit the isolated front-end stability fix**

```bash
git add esp32-s3-menjin-version3.0/web_portal.cpp && \
 git commit -m "fix(web-portal): derive temp pin expiry from device time"
```

---

### Task 2: Verify the unchanged backend contract still produces correct expiry semantics

**Files:**
- Verify: `esp32-s3-menjin-version3.0/web_portal.cpp`
- Verify: `esp32-s3-menjin-version3.0/access_control.cpp`
- Verify: `esp32-s3-menjin-version3.0/menjin_esp32s3___official_version3.0.ino`

- [ ] **Step 1: Confirm the device remains the single source of truth for current time**

Read and confirm these existing functions remain unchanged:

```cpp
// menjin_esp32s3___official_version3.0.ino
uint32_t currentEpochSeconds() {
  time_t now = time(nullptr);
  if (now < 0) {
    return 0;
  }
  return static_cast<uint32_t>(now);
}

String currentLocalTimeString() {
  if (!isSystemTimeSynced()) {
    return "Unavailable";
  }

  time_t now = static_cast<time_t>(currentEpochSeconds());
  struct tm localTimeInfo = {};
  if (localtime_r(&now, &localTimeInfo) == nullptr) {
    return "Unavailable";
  }

  char buffer[32] = {0};
  if (strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTimeInfo) == 0) {
    return "Unavailable";
  }
  return String(buffer);
}
```

Expected conclusion:
- 前端只读取这里暴露的 `epoch` / `localTime`。
- 新方案没有新增第二套时间来源。

- [ ] **Step 2: Confirm the backend generate path still validates only absolute epochs**

Read and confirm this logic remains unchanged in `access_control.cpp`:

```cpp
int generateTemporaryPin(Preferences& prefs,
                         AccessControlState& state,
                         uint32_t expiresAtEpoch,
                         uint32_t nowEpoch,
                         bool timeSynced,
                         String& generatedPin,
                         String& message) {
  generatedPin = "";
  if (!timeSynced || nowEpoch < kMinValidEpoch) {
    message = "Device time is not synced";
    return 400;
  }
  if (expiresAtEpoch <= nowEpoch) {
    message = "Expiration time must be in the future";
    return 400;
  }

  pruneExpiredTemporaryPins(prefs, state, nowEpoch, timeSynced);
  if (state.temporaryPinCount >= kMaxTemporaryPins) {
    message = "Temporary PIN storage is full";
    return 507;
  }

  // existing unique-pin generation logic remains unchanged
}
```

Expected conclusion:
- 前端改成“duration -> device epoch + duration”后，后端无需改签名或改语义。
- 这样可以把风险控制在单文件前端逻辑内。

- [ ] **Step 3: Perform manual HTTP verification on real hardware**

Run against the device after flashing:

```bash
curl -u admin:esp32s3-menjin "http://DEVICE_IP/time_status"
curl -u admin:esp32s3-menjin -X POST "http://DEVICE_IP/temp_pin_generate" -d "expires_at_epoch=$((DEVICE_EPOCH + 3600))"
curl -u admin:esp32s3-menjin "http://DEVICE_IP/temp_pins"
```

Expected:
- `/time_status` returns `timeSynced=true`, `epoch`, and `localTime`.
- `/temp_pin_generate` returns `200` with `generatedPin` and `expiresAtLocal`.
- `/temp_pins` shows the same record with a future `expiresAtEpoch` and human-readable `expiresAtLocal`.

Manual browser check:
- Open the control page.
- Enter `60` into the duration field.
- Click `生成临时 PIN`.
- Confirm the returned expiry time is approximately one hour after the device time shown in `timeStatus`.

- [ ] **Step 4: Check the negative path when the device clock is not synced**

Manual verification steps:
1. Boot the device without Wi‑Fi / NTP availability.
2. Open the control page.
3. Try to generate a temporary PIN with any positive duration.

Expected:
- `loadTimeStatus()` shows `设备尚未完成校时，临时密码功能不可用`.
- `generateTempPin()` stops before POSTing and alerts `设备尚未完成校时，暂时无法生成临时密码`.
- No misleading browser-side conversion or guessed expiry time is shown.

- [ ] **Step 5: Commit the verification result if code changes were needed during validation**

If no further code changes were required, skip this step.
If a small wording/guard adjustment was needed during validation, commit with:

```bash
git add esp32-s3-menjin-version3.0/web_portal.cpp && \
 git commit -m "fix(web-portal): tighten temp pin time input guards"
```

---

## Self-Review

### Spec coverage
- 用户要求忽略 bug1：本计划未包含分区/FQBN 修复。
- 用户要求对 bug2 做最小化稳定修复：本计划只修改 `web_portal.cpp`，不改设备时钟或 PIN 核心语义，满足最小改动目标。
- 用户要求创建方案和计划：本文件已给出范围、文件职责、阶段、任务和验证步骤。

### Placeholder scan
- 无 `TODO` / `TBD` / “implement later”。
- 每个改动步骤都给了具体代码或明确删除目标。
- 验证步骤给了具体命令或手工操作路径。

### Type consistency
- 页面字段统一使用 `tempPinDurationMinutes`。
- 后端接口字段统一继续使用 `expires_at_epoch`。
- 设备时间来源统一为 `time_status -> epoch`。

### Notes for execution
- 不要把这个修复扩展成“任意时区支持”或“用户可选时区”。这超出本次范围。
- 不要顺手修改 `.ino` 里的时区字符串；当前目标是减少浏览器侧不稳定性，而不是重构时间系统。
- 如果必须做构建验证，请使用用户实际的 `n16r8` 板卡与其现有分区配置；不要再把默认 `esp32s3` 的容量问题当成这次修复的 blocker。
