#include "runtime_services.h"

#include <ArduinoOTA.h>
#include <WiFi.h>
#include <time.h>

namespace {
constexpr const char* kTimeZone = "CST-8";
constexpr uint32_t kMqttReconnectIntervalMs = 1000UL;
}

bool shouldServiceWebServer(const RuntimeServicesContext& context) {
  return isProvisioningPortalActive(context.provisioningState) || hasValidWiFiConfig(context.deviceConfig);
}

bool shouldAttemptWiFiConnection(const RuntimeServicesContext& context) {
  return hasValidWiFiConfig(context.deviceConfig) && !isProvisioningPortalActive(context.provisioningState);
}

void syncLegacyApStateFromProvisioningPortal(RuntimeServicesState& state, const ProvisioningState& provisioningState) {
  if (!isProvisioningPortalActive(provisioningState)) {
    return;
  }

  state.wifiConnectionAttemptActive = false;
}

void ensureTimeSyncStarted(RuntimeServicesState& state) {
  if (state.timeSyncStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }

  configTzTime(kTimeZone, "ntp.aliyun.com", "ntp.tencent.com", "pool.ntp.org");
  state.timeSyncStarted = true;
}

bool runtimeIsTimeSynced() {
  return runtimeCurrentEpochSeconds() >= kMinValidEpoch;
}

uint32_t runtimeCurrentEpochSeconds() {
  time_t now = time(nullptr);
  if (now < 0) {
    return 0;
  }
  return static_cast<uint32_t>(now);
}

String runtimeCurrentLocalTimeString() {
  if (!runtimeIsTimeSynced()) {
    return "Unavailable";
  }

  time_t now = static_cast<time_t>(runtimeCurrentEpochSeconds());
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

void beginWiFiConnectionAttempt(RuntimeServicesState& state, const RuntimeServicesContext& context) {
  if (!hasValidWiFiConfig(context.deviceConfig)) {
    return;
  }

  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
  }
  WiFi.begin(context.deviceConfig.wifiSsid.c_str(), context.deviceConfig.wifiPassword.c_str());
  state.wifiConnectionAttemptActive = true;
  state.wifiConnectStartedAt = millis();
  context.provisioningState.startupState = StartupState::CONNECTING_WIFI;
}

void processForcedProvisioningButton(RuntimeServicesState& state, RuntimeServicesContext& context, uint8_t bootButtonPin, uint32_t holdMs, const char* apSsid, const IPAddress& apIp, const IPAddress& apGateway, const IPAddress& apSubnet) {
  if (isProvisioningPortalActive(context.provisioningState)) {
    resetForcedProvisioningButtonState(context.provisioningState);
    return;
  }

  if (!updateForcedProvisioningRequest(context.provisioningState, bootButtonPin, holdMs)) {
    return;
  }

  Serial.println("[BOOT] Long press detected. Starting provisioning portal.");
  state.wifiConnectionAttemptActive = false;
  state.wifiConnectStartedAt = 0;
  state.lastWiFiReconnectAttempt = 0;
  state.lastMqttReconnectAttempt = 0;
  startProvisioningPortal(context.provisioningState, apSsid, apIp, apGateway, apSubnet);
  syncLegacyApStateFromProvisioningPortal(state, context.provisioningState);
  ensureWebServerReady(state, context);
}

void ensureOtaReady(RuntimeServicesState& state, RuntimeServicesContext& context, const char* otaHostname, const char* otaPassword) {
  if (state.otaReady || WiFi.status() != WL_CONNECTED) {
    return;
  }

  ArduinoOTA.setHostname(otaHostname);
  ArduinoOTA.setPassword(otaPassword);
  ArduinoOTA.onStart([&state, &context]() {
    state.isOtaUpdating = true;
    if (context.onOtaStartSideEffect != nullptr) {
      context.onOtaStartSideEffect();
    }
  });

  ArduinoOTA.onEnd([&state]() {
    state.isOtaUpdating = false;
    ESP.restart();
  });
  ArduinoOTA.begin();
  state.otaReady = true;
}

void maintainWiFiConnection(RuntimeServicesState& state, RuntimeServicesContext& context) {
  if (!shouldAttemptWiFiConnection(context)) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    state.wifiConnectionAttemptActive = false;
    if (context.provisioningState.startupState == StartupState::CONNECTING_WIFI) {
      context.provisioningState.startupState = StartupState::NORMAL_RUNTIME;
    }
    ensureTimeSyncStarted(state);
    state.mqttDisconnectTime = 0;
    return;
  }

  if (state.wifiConnectionAttemptActive) {
    if (millis() - state.wifiConnectStartedAt < 10000UL) {
      return;
    }
    state.wifiConnectionAttemptActive = false;
    if (context.provisioningState.startupState == StartupState::CONNECTING_WIFI) {
      context.provisioningState.startupState = StartupState::NORMAL_RUNTIME;
    }
  }

  if (millis() - state.lastWiFiReconnectAttempt < 30000UL) {
    return;
  }

  state.lastWiFiReconnectAttempt = millis();
  WiFi.disconnect();
  beginWiFiConnectionAttempt(state, context);
}

void maintainMqttConnection(RuntimeServicesState& state, RuntimeServicesContext& context, const char* mqttServer, int mqttPort, const char* topicDoor, RuntimeMqttMessageCallback mqttCallback) {
  if (!isMqttConfigured(context.deviceConfig) || WiFi.status() != WL_CONNECTED) {
    return;
  }

  context.mqttClient.setServer(mqttServer, mqttPort);
  context.mqttClient.setCallback(mqttCallback);
  context.mqttClient.setSocketTimeout(1);

  if (context.mqttClient.connected()) {
    context.mqttClient.loop();
    state.mqttDisconnectTime = 0;
    return;
  }

  if (millis() - state.lastMqttReconnectAttempt < kMqttReconnectIntervalMs) {
    return;
  }

  state.lastMqttReconnectAttempt = millis();
  if (context.mqttClient.connect(context.deviceConfig.mqttUid.c_str())) {
    context.mqttClient.subscribe(topicDoor);
    context.mqttClient.publish(topicDoor, "online");
    const bool doorOpen = context.isDoorOpen != nullptr ? context.isDoorOpen() : false;
    context.mqttClient.publish(topicDoor, doorOpen ? "on" : "off");
    state.mqttDisconnectTime = 0;
  }
}

void ensureWebServerReady(RuntimeServicesState& state, RuntimeServicesContext& context) {
  if (state.webServerReady || !shouldServiceWebServer(context)) {
    return;
  }

  setupWebRoutes(context.webPortalContext);
  context.server.begin();
  state.webServerReady = true;
  Serial.println("[WEB] Server Engine Started on port 80.");
}
