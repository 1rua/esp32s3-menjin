#include "web_portal.h"

#include <mbedtls/base64.h>
#include <time.h>

namespace {
WebPortalContext* gContext = nullptr;
const char* kCollectedHeaderKeys[] = {"Authorization"};

const char* CONTROL_PAGE_HTML = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 Mech Master 控制台</title>
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #121212; color: #ffffff; margin: 0; padding: 20px; }
    h1 { color: #00bcd4; text-align: center; }
    .card { background: #1e1e1e; border-radius: 10px; padding: 20px; margin: 15px auto; max-width: 720px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }
    h3 { margin-top: 0; }
    button { background: #00bcd4; color: #000; border: none; padding: 10px 20px; font-size: 15px; border-radius: 5px; cursor: pointer; margin-top: 10px; font-weight: bold; }
    button:hover { background: #0097a7; }
    .btn-danger { background: #ff4081; color: #fff; }
    .btn-danger:hover { background: #c2185b; }
    .btn-secondary { background: #37474f; color: #fff; }
    .btn-secondary:hover { background: #263238; }
    input { width: calc(100% - 22px); padding: 10px; margin: 8px 0; border-radius: 5px; border: 1px solid #333; background: #2c2c2c; color: white; }
    .hint { color: #9e9e9e; font-size: 12px; margin-top: 6px; }
    .status { margin-top: 10px; padding: 10px; border-radius: 6px; background: #263238; }
    .list { margin-top: 12px; text-align: left; }
    .row { display: flex; gap: 10px; align-items: center; justify-content: space-between; border-bottom: 1px solid #333; padding: 10px 0; }
    .row:last-child { border-bottom: none; }
    .row-actions { display: flex; gap: 8px; }
    .warning { color: #ffd54f; }
    .ok { color: #81c784; }
    .mono { font-family: Consolas, monospace; }
    .footer { margin: 20px auto 0; max-width: 720px; text-align: center; color: #9e9e9e; font-size: 13px; }
    .footer a { color: #80deea; text-decoration: none; }
    .footer a:hover { text-decoration: underline; }
    .progress { width: 100%; height: 10px; background: #2c2c2c; border-radius: 999px; overflow: hidden; margin-top: 10px; }
    .progress-bar { height: 100%; width: 0%; background: #4caf50; transition: width 0.2s ease; }
    .inline-input { display: flex; gap: 10px; }
    .inline-input input { flex: 1; }
  </style>
</head>
<body>
  <h1>豆浆白倒の宿舍门禁</h1>

  <div class="card">
    <h3>基础门禁</h3>
    <button onclick="openDoorNow()">立即开门</button>
  </div>

  <div class="card">
    <h3>NFC 录入管理</h3>
    <input type="text" id="nfcUid" placeholder="输入 Hex UID (8~20位，如: F76D163F 或 046A12AB9C7D80)">
    <button onclick="addNfc()">写入 NVS 白名单</button>
  </div>

  <div class="card">
    <h3>指纹管理</h3>
    <div class="inline-input">
      <input type="text" id="fingerprintName" maxlength="24" placeholder="新指纹名称，例如：右手拇指">
      <button id="fingerprintEnrollStart" onclick="startFingerprintEnroll()">开始录入</button>
    </div>
    <button id="fingerprintEnrollCancel" class="btn-secondary" onclick="cancelFingerprintEnroll()" disabled>取消当前录入</button>
    <div id="fingerprintEnrollStatus" class="status">当前无录入任务</div>
    <div class="progress"><div id="fingerprintEnrollBar" class="progress-bar"></div></div>
    <div id="fingerprintEnrollPercent" class="hint">0%</div>
    <div id="fingerprintList" class="list"></div>
  </div>

  <div class="card">
    <h3>网络终端配置</h3>
    <input type="text" id="ssid" placeholder="WiFi 名称 (SSID)">
    <input type="password" id="pwd" placeholder="WiFi 密码 (Password)">
    <input type="text" id="mqttUid" placeholder="Bemfa MQTT UID (留空则保持当前值)">
    <button class="btn-danger" onclick="configureNetwork()">保存网络配置</button>
    <div class="hint">MQTT UID 留空时会保留已有配置。</div>
  </div>

  <div class="card">
    <h3>长期密码管理</h3>
    <input type="hidden" id="pinUserId">
    <input type="text" id="pinUserLabel" placeholder="用户标签，例如：家人 1">
    <input type="password" id="pinUserPin" placeholder="长期 PIN（4~10 位数字）">
    <button onclick="savePinUser()">新增 / 保存长期 PIN</button>
    <div id="pinUsersList" class="list"></div>
  </div>

  <div class="card">
    <h3>临时密码管理</h3>
    <input type="number" id="tempPinDurationMinutes" min="1" max="10080" step="1" placeholder="有效期（分钟），例如 60">
    <button onclick="generateTempPin()">生成临时 PIN</button>
    <div class="hint">按设备当前时间计算到期时间，默认适用于中国使用场景，避免浏览器时区影响。</div>
    <div id="timeStatus" class="status">正在获取时间状态...</div>
    <div id="tempPinResult" class="status" style="display:none"></div>
    <div id="tempPinsList" class="list"></div>
  </div>

  <div class="card">
    <h3>管理员账户设置</h3>
    <input type="text" id="adminUsername" value="admin" placeholder="管理员用户名">
    <input type="password" id="adminPassword" placeholder="新密码">
    <input type="password" id="adminPasswordConfirm" placeholder="确认新密码">
    <button class="btn-secondary" onclick="updateAdminPassword()">保存管理员密码</button>
    <div id="adminStatus" class="status">正在读取管理员状态...</div>
  </div>

  <script>
    let fingerprintPollTimer = 0;
    let fingerprintEnrollActive = false;
    let fingerprintItems = [];

    function escapeHtml(value) {
      return String(value)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
    }

    function escapeJsString(value) {
      return String(value)
        .replace(/\\/g, '\\\\')
        .replace(/\r/g, '\\r')
        .replace(/\n/g, '\\n')
        .replace(/'/g, "\\'");
    }

    function showMessage(data, fallback) {
      alert((data && data.message) ? data.message : fallback);
    }

    async function requestJson(url, options) {
      const response = await fetch(url, options || {});
      const data = await response.json();
      if (!response.ok) {
        throw new Error((data && data.message) ? data.message : '请求失败');
      }
      return data;
    }

    function encodeForm(data) {
      return Object.entries(data)
        .map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v == null ? '' : v))
        .join('&');
    }

    function stopFingerprintPolling() {
      if (fingerprintPollTimer) {
        clearInterval(fingerprintPollTimer);
        fingerprintPollTimer = 0;
      }
    }

    function updateFingerprintStatus(status) {
      const progress = Number(status.progress || 0);
      const wasActive = fingerprintEnrollActive;
      fingerprintEnrollActive = !!status.active;
      document.getElementById('fingerprintEnrollBar').style.width = progress + '%';
      document.getElementById('fingerprintEnrollPercent').textContent = progress + '%';
      document.getElementById('fingerprintEnrollStatus').innerHTML = `${escapeHtml(status.message || '当前无录入任务')}<br><span class="hint mono">ID: ${status.id || '-'} · ${escapeHtml(status.name || '-')}</span>${status.error ? `<br><span class="warning">${escapeHtml(status.error)}</span>` : ''}`;
      document.getElementById('fingerprintEnrollCancel').disabled = !status.active;
      document.getElementById('fingerprintEnrollStart').disabled = !!status.active;
      if (wasActive !== fingerprintEnrollActive) {
        renderFingerprints(fingerprintItems);
      }
    }

    function renderFingerprints(items) {
      const container = document.getElementById('fingerprintList');
      if (!items.length) {
        container.innerHTML = '<div class="hint">暂无已录入指纹</div>';
        return;
      }
      container.innerHTML = items.map(item => `
        <div class="row">
          <div>
            <div>${escapeHtml(item.name)}</div>
            <div class="hint mono">ID: ${item.id}</div>
          </div>
          <div class="row-actions">
            <button class="btn-secondary" onclick="renameFingerprint(${item.id}, '${escapeJsString(item.name)}')" ${fingerprintEnrollActive ? 'disabled' : ''}>重命名</button>
            <button class="btn-danger" onclick="deleteFingerprint(${item.id})" ${fingerprintEnrollActive ? 'disabled' : ''}>删除</button>
          </div>
        </div>`).join('');
    }

    async function loadFingerprints() {
      const data = await requestJson('/fingerprints');
      fingerprintItems = (data.data && data.data.items) || [];
      renderFingerprints(fingerprintItems);
    }

    async function loadFingerprintEnrollStatus() {
      const data = await requestJson('/fingerprint_enroll_status');
      const status = data.data || {};
      updateFingerprintStatus(status);
      if (!status.active) {
        stopFingerprintPolling();
        await loadFingerprints();
      }
    }

    function startFingerprintPolling() {
      stopFingerprintPolling();
      fingerprintPollTimer = setInterval(() => {
        loadFingerprintEnrollStatus().catch(error => {
          stopFingerprintPolling();
          alert(error.message || '指纹进度读取失败');
        });
      }, 800);
    }

    async function startFingerprintEnroll() {
      const name = document.getElementById('fingerprintName').value.trim();
      if (!name) return alert('必须输入指纹名称');
      const data = await requestJson('/fingerprint_enroll_start', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: encodeForm({name})
      });
      showMessage(data, '指纹录入已开始');
      updateFingerprintStatus(data.data || {});
      startFingerprintPolling();
    }

    async function cancelFingerprintEnroll() {
      const data = await requestJson('/fingerprint_enroll_cancel', {method: 'POST'});
      showMessage(data, '指纹录入已取消');
      stopFingerprintPolling();
      await loadFingerprintEnrollStatus();
    }

    async function renameFingerprint(id, currentName) {
      const name = prompt('输入新的指纹名称', currentName || '');
      if (name === null) return;
      const data = await requestJson('/fingerprint_rename', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: encodeForm({id, name})
      });
      showMessage(data, '指纹名称已更新');
      await loadFingerprints();
    }

    async function deleteFingerprint(id) {
      if (!confirm(`确认删除指纹 ID ${id} 吗？`)) return;
      const data = await requestJson('/fingerprint_delete', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: encodeForm({id})
      });
      showMessage(data, '指纹已删除');
      await loadFingerprints();
    }

    function renderPinUsers(items) {
      const container = document.getElementById('pinUsersList');
      if (!items.length) {
        container.innerHTML = '<div class="hint">暂无长期 PIN</div>';
        return;
      }
      container.innerHTML = items.map(item => `
        <div class="row">
          <div>
            <div>${escapeHtml(item.label)}</div>
            <div class="hint mono">ID: ${item.id} · ${item.enabled ? '启用' : '停用'}</div>
          </div>
          <div class="row-actions">
            <button class="btn-secondary" onclick="editPinUser(${item.id}, '${escapeJsString(item.label)}')">编辑</button>
            <button class="btn-danger" onclick="deletePinUser(${item.id})">删除</button>
          </div>
        </div>`).join('');
    }

    function renderTempPins(items) {
      const container = document.getElementById('tempPinsList');
      if (!items.length) {
        container.innerHTML = '<div class="hint">暂无临时 PIN</div>';
        return;
      }
      container.innerHTML = items.map(item => `
        <div class="row">
          <div>
            <div>到期：${item.expiresAtLocal}</div>
            <div class="hint mono">ID: ${item.id} · ${item.enabled ? '有效' : '不可用 / 已过期'}</div>
          </div>
          <div class="row-actions">
            <button class="btn-danger" onclick="revokeTempPin(${item.id})">撤销</button>
          </div>
        </div>`).join('');
    }

    function editPinUser(id, label) {
      document.getElementById('pinUserId').value = id;
      document.getElementById('pinUserLabel').value = label;
      document.getElementById('pinUserPin').value = '';
    }

    async function openDoorNow() {
      const data = await requestJson('/open', {method: 'POST'});
      showMessage(data, '指令已发送');
    }

    async function addNfc() {
      const uid = document.getElementById('nfcUid').value.trim();
      if (!uid || uid.length % 2 !== 0) return alert('请输入有效的偶数位十六进制 UID');
      if (!/^[0-9a-fA-F]+$/.test(uid)) return alert('UID 只能包含十六进制字符 0-9/A-F');
      const data = await requestJson('/add_nfc', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: encodeForm({uid})
      });
      showMessage(data, 'UID 已写入白名单');
    }

    async function configureNetwork() {
      const ssid = document.getElementById('ssid').value.trim();
      const password = document.getElementById('pwd').value;
      const mqttUid = document.getElementById('mqttUid').value.trim();
      if (!ssid) return alert('必须输入SSID');
      const data = await requestJson('/configure_network', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: encodeForm({ssid, pass: password, mqtt_uid: mqttUid})
      });
      showMessage(data, '网络配置已保存');
    }

    async function loadPinUsers() {
      const data = await requestJson('/pin_users');
      renderPinUsers((data.data && data.data.items) || []);
    }

    async function savePinUser() {
      const id = document.getElementById('pinUserId').value.trim();
      const label = document.getElementById('pinUserLabel').value.trim();
      const pin = document.getElementById('pinUserPin').value.trim();
      const data = await requestJson('/pin_user', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: encodeForm({id, label, pin})
      });
      showMessage(data, '长期 PIN 已保存');
      document.getElementById('pinUserId').value = '';
      document.getElementById('pinUserLabel').value = '';
      document.getElementById('pinUserPin').value = '';
      await loadPinUsers();
    }

    async function deletePinUser(id) {
      const data = await requestJson('/pin_user_delete', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: encodeForm({id})
      });
      showMessage(data, '长期 PIN 已删除');
      await loadPinUsers();
    }

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

    async function loadTempPins() {
      const data = await requestJson('/temp_pins');
      renderTempPins((data.data && data.data.items) || []);
    }

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

    async function revokeTempPin(id) {
      const data = await requestJson('/temp_pin_revoke', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: encodeForm({id})
      });
      showMessage(data, '临时 PIN 已撤销');
      await loadTempPins();
    }

    async function loadAdminStatus() {
      const data = await requestJson('/time_status');
      const adminStatus = document.getElementById('adminStatus');
      if (data.data && data.data.adminDefault) {
        adminStatus.innerHTML = '<span class="warning">正在使用默认管理员密码，请尽快修改</span>';
      } else {
        adminStatus.innerHTML = '<span class="ok">管理员密码已自定义</span>';
      }
    }

    async function updateAdminPassword() {
      const username = document.getElementById('adminUsername').value.trim();
      const newPassword = document.getElementById('adminPassword').value;
      const confirmPassword = document.getElementById('adminPasswordConfirm').value;
      if (!username) return alert('必须输入管理员用户名');
      if (!newPassword) return alert('必须输入新密码');
      if (newPassword !== confirmPassword) return alert('两次密码输入不一致');
      const data = await requestJson('/admin_password', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: encodeForm({username, new_password: newPassword, confirm_password: confirmPassword})
      });
      showMessage(data, '管理员密码已更新');
      document.getElementById('adminPassword').value = '';
      document.getElementById('adminPasswordConfirm').value = '';
      await loadAdminStatus();
    }

    async function initializePage() {
      try {
        await loadPinUsers();
        await loadTimeStatus();
        await loadTempPins();
        await loadAdminStatus();
        await loadFingerprints();
        await loadFingerprintEnrollStatus();
      } catch (error) {
        alert(error.message || '页面初始化失败');
      }
    }

    initializePage();
  </script>
  <div class="footer">
    <div>项目仓库：<a href="https://github.com/1rua/esp32s3-menjin" target="_blank" rel="noopener noreferrer">github.com/1rua/esp32s3-menjin</a></div>
    <div>作者主页：<a href="https://github.com/1rua" target="_blank" rel="noopener noreferrer">github.com/1rua</a></div>
  </div>
</body>
</html>
)rawliteral";

const char* PROVISIONING_PAGE_HTML = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 Mech Master 配网门户</title>
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(180deg, #0d1117, #161b22); color: #ffffff; text-align: center; margin: 0; padding: 24px; }
    h1 { color: #58a6ff; }
    .card { background: rgba(30, 41, 59, 0.92); border-radius: 12px; padding: 22px; margin: 18px auto; max-width: 420px; box-shadow: 0 12px 28px rgba(0,0,0,0.28); }
    button { background: #58a6ff; color: #0d1117; border: none; padding: 12px 20px; font-size: 16px; border-radius: 6px; cursor: pointer; margin-top: 10px; font-weight: bold; width: 100%; }
    button:hover { background: #388bfd; }
    .btn-secondary { background: #30363d; color: #ffffff; }
    .btn-secondary:hover { background: #484f58; }
    input { width: calc(100% - 22px); padding: 10px; margin: 10px 0; border-radius: 6px; border: 1px solid #30363d; background: #0d1117; color: white; }
    .hint { color: #8b949e; font-size: 12px; margin-top: 6px; }
    .footer { margin: 20px auto 0; max-width: 420px; text-align: center; color: #8b949e; font-size: 13px; }
    .footer a { color: #58a6ff; text-decoration: none; }
    .footer a:hover { text-decoration: underline; }
  </style>
</head>
<body>
  <h1>ESP32-S3 门禁配网门户</h1>
  <div class="card">
    <h3>配置 WiFi 与 MQTT</h3>
    <input type="text" id="ssid" placeholder="WiFi 名称 (SSID)">
    <input type="password" id="pwd" placeholder="WiFi 密码 (Password)">
    <input type="text" id="mqttUid" placeholder="Bemfa MQTT UID (留空则保持当前值)">
    <button onclick="configureNetwork()">保存并关闭配网门户</button>
    <div class="hint">保存后会停止 AP 门户，并进入后续联网流程。</div>
  </div>

  <div class="card">
    <h3>跳过自动配网</h3>
    <button class="btn-secondary" onclick="skipProvision()">跳过并进入本地运行</button>
    <div class="hint">适用于暂时不配置 WiFi，只保留本地门禁功能。</div>
  </div>

  <script>
    function encodeForm(data) {
      return Object.entries(data)
        .map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v == null ? '' : v))
        .join('&');
    }

    async function requestJson(url, options) {
      const response = await fetch(url, options || {});
      const data = await response.json();
      if (!response.ok) {
        throw new Error((data && data.message) ? data.message : '请求失败');
      }
      return data;
    }

    function showMessage(data, fallback) {
      alert((data && data.message) ? data.message : fallback);
    }

    async function configureNetwork() {
      const ssid = document.getElementById('ssid').value.trim();
      const password = document.getElementById('pwd').value;
      const mqttUid = document.getElementById('mqttUid').value.trim();
      if (!ssid) return alert('必须输入SSID');
      const data = await requestJson('/configure_network', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: encodeForm({ssid, pass: password, mqtt_uid: mqttUid})
      });
      showMessage(data, '网络配置已保存');
    }

    async function skipProvision() {
      const data = await requestJson('/skip_provision', {method: 'POST'});
      showMessage(data, '已跳过自动配网');
    }
  </script>
  <div class="footer">
    <div>项目仓库：<a href="https://github.com/1rua/esp32s3-menjin" target="_blank" rel="noopener noreferrer">github.com/1rua/esp32s3-menjin</a></div>
    <div>作者主页：<a href="https://github.com/1rua" target="_blank" rel="noopener noreferrer">github.com/1rua</a></div>
  </div>
</body>
</html>
)rawliteral";

String jsonEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value.charAt(i);
    if (ch == '\\' || ch == '"') {
      escaped += '\\';
      escaped += ch;
    } else if (ch == '\n') {
      escaped += "\\n";
    } else if (ch == '\r') {
      escaped += "\\r";
    } else {
      escaped += ch;
    }
  }
  return escaped;
}

void sendJson(int statusCode, const char* status, const String& message) {
  if (gContext == nullptr) {
    return;
  }
  const String body = String("{\"status\":\"") + status + "\",\"message\":\"" + jsonEscape(message) + "\"}";
  gContext->server.send(statusCode, "application/json", body);
}

void sendJsonData(int statusCode, const char* status, const String& message, const String& dataJson) {
  if (gContext == nullptr) {
    return;
  }
  const String body = String("{\"status\":\"") + status +
                      "\",\"message\":\"" + jsonEscape(message) +
                      "\",\"data\":" + dataJson + "}";
  gContext->server.send(statusCode, "application/json", body);
}

String readRequestArg(const String& key) {
  if (gContext->server.hasArg(key)) {
    return gContext->server.arg(key);
  }
  return "";
}

bool isAuthorized() {
  const String header = gContext->server.header("Authorization");
  if (!header.startsWith("Basic ")) {
    return false;
  }

  size_t outputLength = 0;
  unsigned char decoded[129] = {0};
  const int decodeStatus = mbedtls_base64_decode(
    decoded,
    sizeof(decoded) - 1,
    &outputLength,
    reinterpret_cast<const unsigned char*>(header.c_str() + 6),
    header.length() - 6);
  if (decodeStatus != 0 || outputLength == 0 || outputLength >= sizeof(decoded)) {
    return false;
  }

  decoded[outputLength] = '\0';
  String credentials(reinterpret_cast<const char*>(decoded));
  const int separator = credentials.indexOf(':');
  if (separator <= 0) {
    return false;
  }

  const String username = credentials.substring(0, separator);
  const String password = credentials.substring(separator + 1);
  return verifyAdminCredentials(gContext->accessControl, username, password);
}

bool requireAuth() {
  if (gContext == nullptr) {
    return false;
  }
  if (isAuthorized()) {
    return true;
  }
  gContext->server.requestAuthentication();
  return false;
}

void notifyProvisioningStateChanged() {
  if (gContext != nullptr && gContext->onProvisioningStateChanged != nullptr) {
    gContext->onProvisioningStateChanged();
  }
}

bool timeSynced() {
  return gContext->isTimeSynced != nullptr && gContext->isTimeSynced();
}

uint32_t currentEpoch() {
  return gContext->currentEpochSeconds != nullptr ? gContext->currentEpochSeconds() : 0;
}

String currentLocalTime() {
  return gContext->currentLocalTimeString != nullptr ? gContext->currentLocalTimeString() : String("Unavailable");
}

void handleRootRoute() {
  if (gContext == nullptr) {
    return;
  }
  if (!requireAuth()) {
    return;
  }

  const char* page = isProvisioningPortalActive(gContext->provisioningState)
    ? PROVISIONING_PAGE_HTML
    : CONTROL_PAGE_HTML;
  gContext->server.send(200, "text/html", page);
}

void handleOpenRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  if (gContext->onDoorOpen != nullptr) {
    gContext->onDoorOpen();
  }
  sendJson(200, "ok", "Door Opened");
}

void handleAddNfcRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  const String uid = readRequestArg("uid");
  if (uid.length() == 0) {
    sendJson(400, "error", "Missing UID");
    return;
  }

  String message;
  const int statusCode = gContext->onAddNfc != nullptr ? gContext->onAddNfc(uid, message) : 500;
  if (message.length() == 0) {
    message = statusCode == 200 ? "NFC Added to NVS" : "NFC add callback unavailable";
  }
  sendJson(statusCode, statusCode == 200 ? "ok" : "error", message);
}

void handleFingerprintsRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  String itemsJson;
  if (gContext->onListFingerprints != nullptr) {
    gContext->onListFingerprints(itemsJson);
  }
  sendJsonData(200, "ok", "Fingerprints loaded", String("{\"items\":[") + itemsJson + "]}");
}

void handleFingerprintEnrollStartRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  const String name = readRequestArg("name");
  String message;
  const int statusCode = gContext->onStartFingerprintEnroll != nullptr
    ? gContext->onStartFingerprintEnroll(name, message)
    : 500;
  String dataJson = "{}";
  if (gContext->onFingerprintEnrollStatus != nullptr) {
    gContext->onFingerprintEnrollStatus(dataJson);
  }
  sendJsonData(statusCode, statusCode == 200 ? "ok" : "error", message, dataJson);
}

void handleFingerprintEnrollStatusRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  String dataJson = "{}";
  if (gContext->onFingerprintEnrollStatus != nullptr) {
    gContext->onFingerprintEnrollStatus(dataJson);
  }
  sendJsonData(200, "ok", "Fingerprint enrollment status loaded", dataJson);
}

void handleFingerprintEnrollCancelRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  String message;
  const int statusCode = gContext->onCancelFingerprintEnroll != nullptr
    ? gContext->onCancelFingerprintEnroll(message)
    : 500;
  sendJson(statusCode, statusCode == 200 ? "ok" : "error", message.length() > 0 ? message : "Fingerprint enroll cancel callback unavailable");
}

void handleFingerprintRenameRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  const String idValue = readRequestArg("id");
  const String name = readRequestArg("name");
  if (idValue.length() == 0) {
    sendJson(400, "error", "Missing fingerprint id");
    return;
  }
  String message;
  const int statusCode = gContext->onRenameFingerprint != nullptr
    ? gContext->onRenameFingerprint(idValue.toInt(), name, message)
    : 500;
  sendJson(statusCode, statusCode == 200 ? "ok" : "error", message.length() > 0 ? message : "Fingerprint rename callback unavailable");
}

void handleFingerprintDeleteRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  const String idValue = readRequestArg("id");
  if (idValue.length() == 0) {
    sendJson(400, "error", "Missing fingerprint id");
    return;
  }
  String message;
  const int statusCode = gContext->onDeleteFingerprint != nullptr
    ? gContext->onDeleteFingerprint(idValue.toInt(), message)
    : 500;
  sendJson(statusCode, statusCode == 200 ? "ok" : "error", message.length() > 0 ? message : "Fingerprint delete callback unavailable");
}

void handleConfigureNetworkRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  String ssid = readRequestArg("ssid");
  ssid.trim();
  if (ssid.length() == 0) {
    sendJson(400, "error", "Missing ssid");
    return;
  }

  const String password = readRequestArg("pass");
  String mqttUid = readRequestArg("mqtt_uid");
  mqttUid.trim();

  saveWiFiConfig(gContext->prefs, gContext->deviceConfig, ssid, password);
  if (gContext->server.hasArg("mqtt_uid")) {
    saveMqttUid(gContext->prefs, gContext->deviceConfig, mqttUid, false);
  }
  handleProvisioningSaved(gContext->provisioningState);
  notifyProvisioningStateChanged();
  sendJson(200, "ok", "Network configuration saved. Provisioning portal stopped.");
}

void handleSkipProvisionRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  setSkipAutoProvision(gContext->prefs, gContext->deviceConfig, true);
  handleProvisioningSkipped(gContext->provisioningState);
  notifyProvisioningStateChanged();
  sendJson(200, "ok", "Auto provisioning skipped. Portal stopped.");
}

void handleTimeStatusRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  const bool synced = timeSynced();
  const String data = String("{\"timeSynced\":") + (synced ? "true" : "false") +
                      ",\"epoch\":" + String(currentEpoch()) +
                      ",\"localTime\":\"" + jsonEscape(currentLocalTime()) +
                      "\",\"timezone\":\"Asia/Shanghai\",\"adminDefault\":" +
                      (isAdminUsingDefaultCredentials(gContext->accessControl) ? "true" : "false") + "}";
  sendJsonData(200, "ok", "Time status loaded", data);
}

void handlePinUsersRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  String itemsJson;
  listPermanentPinsJson(gContext->accessControl, itemsJson);
  sendJsonData(200, "ok", "Permanent PIN users loaded", String("{\"items\":") + itemsJson + "}");
}

void handlePinUserRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  const String idValue = readRequestArg("id");
  const int32_t id = idValue.length() == 0 ? -1 : idValue.toInt();
  const String label = readRequestArg("label");
  const String pin = readRequestArg("pin");

  String message;
  const int statusCode = upsertPermanentPin(gContext->prefs, gContext->accessControl, id, label, pin, message);
  sendJson(statusCode, statusCode == 200 ? "ok" : "error", message);
}

void handlePinUserDeleteRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  const String idValue = readRequestArg("id");
  if (idValue.length() == 0) {
    sendJson(400, "error", "Missing PIN id");
    return;
  }
  String message;
  const int statusCode = deletePermanentPin(gContext->prefs, gContext->accessControl, idValue.toInt(), message);
  sendJson(statusCode, statusCode == 200 ? "ok" : "error", message);
}

void handleTempPinsRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  pruneExpiredTemporaryPins(gContext->prefs, gContext->accessControl, currentEpoch(), timeSynced());
  String itemsJson;
  listTemporaryPinsJson(gContext->accessControl, currentEpoch(), timeSynced(), itemsJson);
  sendJsonData(200, "ok", "Temporary PINs loaded", String("{\"items\":") + itemsJson + "}");
}

void handleTempPinGenerateRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  const String expiresAtEpochValue = readRequestArg("expires_at_epoch");
  if (expiresAtEpochValue.length() == 0) {
    sendJson(400, "error", "Missing expires_at_epoch");
    return;
  }

  pruneExpiredTemporaryPins(gContext->prefs, gContext->accessControl, currentEpoch(), timeSynced());
  String generatedPin;
  String message;
  const int statusCode = generateTemporaryPin(gContext->prefs, gContext->accessControl,
                                              static_cast<uint32_t>(strtoul(expiresAtEpochValue.c_str(), nullptr, 10)),
                                              currentEpoch(), timeSynced(), generatedPin, message);
  if (statusCode != 200) {
    sendJson(statusCode, "error", message);
    return;
  }

  const uint32_t expiresAtEpoch = static_cast<uint32_t>(strtoul(expiresAtEpochValue.c_str(), nullptr, 10));
  time_t rawTime = static_cast<time_t>(expiresAtEpoch);
  struct tm localTimeInfo = {};
  char buffer[24] = {0};
  localtime_r(&rawTime, &localTimeInfo);
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &localTimeInfo);

  const String data = String("{\"generatedPin\":\"") + jsonEscape(generatedPin) +
                      "\",\"expiresAtLocal\":\"" + jsonEscape(String(buffer)) + "\"}";
  sendJsonData(200, "ok", message, data);
}

void handleTempPinRevokeRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  const String idValue = readRequestArg("id");
  if (idValue.length() == 0) {
    sendJson(400, "error", "Missing PIN id");
    return;
  }

  String message;
  const int statusCode = revokeTemporaryPin(gContext->prefs, gContext->accessControl, idValue.toInt(), message);
  sendJson(statusCode, statusCode == 200 ? "ok" : "error", message);
}

void handleAdminPasswordRoute() {
  if (gContext == nullptr || !requireAuth()) {
    return;
  }
  const String username = readRequestArg("username");
  const String newPassword = readRequestArg("new_password");
  const String confirmPassword = readRequestArg("confirm_password");
  if (username.length() == 0 || newPassword.length() == 0) {
    sendJson(400, "error", "Missing admin credentials");
    return;
  }
  if (confirmPassword.length() > 0 && confirmPassword != newPassword) {
    sendJson(400, "error", "Password confirmation mismatch");
    return;
  }

  String message;
  const bool ok = updateAdminPassword(gContext->prefs, gContext->accessControl, username, newPassword, message);
  sendJson(ok ? 200 : 400, ok ? "ok" : "error", message);
}
}  // namespace

void setupWebRoutes(WebPortalContext& context) {
  gContext = &context;
  gContext->server.collectHeaders(kCollectedHeaderKeys, 1);

  gContext->server.on("/", HTTP_GET, handleRootRoute);
  gContext->server.on("/open", HTTP_POST, handleOpenRoute);
  gContext->server.on("/add_nfc", HTTP_POST, handleAddNfcRoute);
  gContext->server.on("/fingerprints", HTTP_GET, handleFingerprintsRoute);
  gContext->server.on("/fingerprint_enroll_start", HTTP_POST, handleFingerprintEnrollStartRoute);
  gContext->server.on("/fingerprint_enroll_status", HTTP_GET, handleFingerprintEnrollStatusRoute);
  gContext->server.on("/fingerprint_enroll_cancel", HTTP_POST, handleFingerprintEnrollCancelRoute);
  gContext->server.on("/fingerprint_rename", HTTP_POST, handleFingerprintRenameRoute);
  gContext->server.on("/fingerprint_delete", HTTP_POST, handleFingerprintDeleteRoute);
  gContext->server.on("/configure_network", HTTP_POST, handleConfigureNetworkRoute);
  gContext->server.on("/set_wifi", HTTP_POST, handleConfigureNetworkRoute);
  gContext->server.on("/skip_provision", HTTP_POST, handleSkipProvisionRoute);
  gContext->server.on("/pin_users", HTTP_GET, handlePinUsersRoute);
  gContext->server.on("/pin_user", HTTP_POST, handlePinUserRoute);
  gContext->server.on("/pin_user_delete", HTTP_POST, handlePinUserDeleteRoute);
  gContext->server.on("/temp_pins", HTTP_GET, handleTempPinsRoute);
  gContext->server.on("/temp_pin_generate", HTTP_POST, handleTempPinGenerateRoute);
  gContext->server.on("/temp_pin_revoke", HTTP_POST, handleTempPinRevokeRoute);
  gContext->server.on("/admin_password", HTTP_POST, handleAdminPasswordRoute);
  gContext->server.on("/time_status", HTTP_GET, handleTimeStatusRoute);
}
