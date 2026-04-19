#include "web_portal.h"

namespace {
WebPortalContext* gContext = nullptr;

const char* CONTROL_PAGE_HTML = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 Mech Master 控制台</title>
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #121212; color: #ffffff; text-align: center; margin: 0; padding: 20px; }
    h1 { color: #00bcd4; }
    .card { background: #1e1e1e; border-radius: 10px; padding: 20px; margin: 15px auto; max-width: 420px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }
    button { background: #00bcd4; color: #000; border: none; padding: 10px 20px; font-size: 16px; border-radius: 5px; cursor: pointer; margin-top: 10px; font-weight: bold; width: 100%; }
    button:hover { background: #0097a7; }
    .btn-danger { background: #ff4081; }
    .btn-danger:hover { background: #c2185b; }
    input { width: calc(100% - 22px); padding: 10px; margin: 10px 0; border-radius: 5px; border: 1px solid #333; background: #2c2c2c; color: white; }
    .hint { color: #9e9e9e; font-size: 12px; margin-top: 6px; }
  </style>
</head>
<body>
  <h1>盾级权限 | 灰风控制中枢</h1>

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
    <h3>网络终端配置</h3>
    <input type="text" id="ssid" placeholder="WiFi 名称 (SSID)">
    <input type="password" id="pwd" placeholder="WiFi 密码 (Password)">
    <input type="text" id="mqttUid" placeholder="Bemfa MQTT UID (留空则保持当前值)">
    <button class="btn-danger" onclick="configureNetwork()">保存网络配置</button>
    <div class="hint">MQTT UID 留空时会保留已有配置。</div>
  </div>

  <script>
    function showMessage(data, fallback) {
      alert((data && data.message) ? data.message : fallback);
    }

    function openDoorNow() {
      fetch('/open')
        .then(response => response.json())
        .then(data => showMessage(data, '指令已发送'));
    }

    function addNfc() {
      const uid = document.getElementById('nfcUid').value.trim();
      if (!uid || uid.length % 2 !== 0) return alert('请输入有效的偶数位十六进制 UID');
      if (!/^[0-9a-fA-F]+$/.test(uid)) return alert('UID 只能包含十六进制字符 0-9/A-F');
      fetch('/add_nfc?uid=' + encodeURIComponent(uid))
        .then(response => response.json())
        .then(data => showMessage(data, 'UID 已写入白名单'));
    }

    function configureNetwork() {
      const ssid = document.getElementById('ssid').value.trim();
      const password = document.getElementById('pwd').value;
      const mqttUid = document.getElementById('mqttUid').value.trim();
      if (!ssid) return alert('必须输入SSID');
      const url = '/configure_network?ssid=' + encodeURIComponent(ssid)
        + '&pass=' + encodeURIComponent(password)
        + '&mqtt_uid=' + encodeURIComponent(mqttUid);
      fetch(url)
        .then(response => response.json())
        .then(data => showMessage(data, '网络配置已保存'));
    }
  </script>
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
    function showMessage(data, fallback) {
      alert((data && data.message) ? data.message : fallback);
    }

    function configureNetwork() {
      const ssid = document.getElementById('ssid').value.trim();
      const password = document.getElementById('pwd').value;
      const mqttUid = document.getElementById('mqttUid').value.trim();
      if (!ssid) return alert('必须输入SSID');
      const url = '/configure_network?ssid=' + encodeURIComponent(ssid)
        + '&pass=' + encodeURIComponent(password)
        + '&mqtt_uid=' + encodeURIComponent(mqttUid);
      fetch(url)
        .then(response => response.json())
        .then(data => showMessage(data, '网络配置已保存'));
    }

    function skipProvision() {
      fetch('/skip_provision')
        .then(response => response.json())
        .then(data => showMessage(data, '已跳过自动配网'));
    }
  </script>
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

void notifyProvisioningStateChanged() {
  if (gContext != nullptr && gContext->onProvisioningStateChanged != nullptr) {
    gContext->onProvisioningStateChanged();
  }
}

void handleRootRoute() {
  if (gContext == nullptr) {
    return;
  }

  const char* page = isProvisioningPortalActive(gContext->provisioningState)
    ? PROVISIONING_PAGE_HTML
    : CONTROL_PAGE_HTML;
  gContext->server.send(200, "text/html", page);
}

void handleOpenRoute() {
  if (gContext == nullptr) {
    return;
  }

  if (gContext->onDoorOpen != nullptr) {
    gContext->onDoorOpen();
  }
  sendJson(200, "ok", "Door Opened");
}

void handleAddNfcRoute() {
  if (gContext == nullptr) {
    return;
  }

  if (!gContext->server.hasArg("uid")) {
    sendJson(400, "error", "Missing UID");
    return;
  }

  String message;
  const int statusCode = gContext->onAddNfc != nullptr
    ? gContext->onAddNfc(gContext->server.arg("uid"), message)
    : 500;

  if (message.length() == 0) {
    message = statusCode == 200 ? "NFC Added to NVS" : "NFC add callback unavailable";
  }

  sendJson(statusCode, statusCode == 200 ? "ok" : "error", message);
}

void handleConfigureNetworkRoute() {
  if (gContext == nullptr) {
    return;
  }

  if (!gContext->server.hasArg("ssid")) {
    sendJson(400, "error", "Missing ssid");
    return;
  }

  String ssid = gContext->server.arg("ssid");
  ssid.trim();
  if (ssid.length() == 0) {
    sendJson(400, "error", "Missing ssid");
    return;
  }

  const String password = gContext->server.hasArg("pass") ? gContext->server.arg("pass") : "";
  String mqttUid = gContext->server.hasArg("mqtt_uid") ? gContext->server.arg("mqtt_uid") : "";
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
  if (gContext == nullptr) {
    return;
  }

  setSkipAutoProvision(gContext->prefs, gContext->deviceConfig, true);
  handleProvisioningSkipped(gContext->provisioningState);
  notifyProvisioningStateChanged();

  sendJson(200, "ok", "Auto provisioning skipped. Portal stopped.");
}
}  // namespace

void setupWebRoutes(WebPortalContext& context) {
  gContext = &context;

  gContext->server.on("/", HTTP_GET, handleRootRoute);
  gContext->server.on("/open", HTTP_GET, handleOpenRoute);
  gContext->server.on("/add_nfc", HTTP_GET, handleAddNfcRoute);
  gContext->server.on("/configure_network", HTTP_GET, handleConfigureNetworkRoute);
  gContext->server.on("/set_wifi", HTTP_GET, handleConfigureNetworkRoute);
  gContext->server.on("/skip_provision", HTTP_GET, handleSkipProvisionRoute);
}
