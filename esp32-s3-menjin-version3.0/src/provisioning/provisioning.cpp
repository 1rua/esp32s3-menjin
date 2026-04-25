// 配网状态机实现：处理按键长按触发、启动决策与 AP 门户开关。
#include "provisioning.h"

void resetForcedProvisioningButtonState(ProvisioningState& state) {
  state.bootButtonPressed = false;
  state.bootLongPressHandled = false;
  state.bootPressStartedAt = 0;
}

bool updateForcedProvisioningRequest(ProvisioningState& state, uint8_t bootButtonPin, uint32_t holdMs) {
  if (digitalRead(bootButtonPin) != LOW) {
    resetForcedProvisioningButtonState(state);
    return false;
  }

  if (!state.bootButtonPressed) {
    state.bootButtonPressed = true;
    state.bootLongPressHandled = false;
    state.bootPressStartedAt = millis();
    return false;
  }

  if (state.bootLongPressHandled) {
    return false;
  }

  if (millis() - state.bootPressStartedAt < holdMs) {
    return false;
  }

  state.bootLongPressHandled = true;
  return true;
}

StartupState decideStartupState(const DeviceConfig& deviceConfig, bool bootForcedProvision) {
  if (bootForcedProvision) {
    return StartupState::AP_PORTAL;
  }

  if (hasValidWiFiConfig(deviceConfig)) {
    return StartupState::CONNECTING_WIFI;
  }

  if (deviceConfig.skipAutoProvision) {
    return StartupState::NORMAL_RUNTIME;
  }

  return StartupState::AP_PORTAL;
}

void applyStartupDecision(ProvisioningState& state, StartupState startupState) {
  state.startupState = startupState;
  if (startupState != StartupState::AP_PORTAL) {
    state.portalActive = false;
  }
}

void startProvisioningPortal(ProvisioningState& state, const char* apSsid, const IPAddress& apIp, const IPAddress& apGateway, const IPAddress& apSubnet) {
  if (state.portalActive) {
    return;
  }

  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  const bool configured = WiFi.softAPConfig(apIp, apGateway, apSubnet);
  const bool started = configured && WiFi.softAP(apSsid);

  state.portalActive = started;
  state.startupState = started ? StartupState::AP_PORTAL : StartupState::NORMAL_RUNTIME;
}

void stopProvisioningPortal(ProvisioningState& state) {
  if (!state.portalActive) {
    return;
  }

  WiFi.softAPdisconnect(true);
  state.portalActive = false;
}

void handleProvisioningSaved(ProvisioningState& state) {
  stopProvisioningPortal(state);
  state.startupState = StartupState::CONNECTING_WIFI;
}

void handleProvisioningSkipped(ProvisioningState& state) {
  stopProvisioningPortal(state);
  state.startupState = StartupState::NORMAL_RUNTIME;
}

bool isProvisioningPortalActive(const ProvisioningState& state) {
  return state.portalActive;
}

bool shouldAttemptWiFi(const ProvisioningState& state) {
  return state.startupState == StartupState::CONNECTING_WIFI;
}
