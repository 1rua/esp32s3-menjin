#include "provisioning.h"

namespace {
const uint32_t kPollIntervalMs = 50;
}

bool detectForcedProvisioningRequest(uint8_t bootButtonPin, uint32_t holdMs) {
  if (digitalRead(bootButtonPin) != LOW) {
    return false;
  }

  const uint32_t startMs = millis();
  while ((millis() - startMs) < holdMs) {
    if (digitalRead(bootButtonPin) != LOW) {
      return false;
    }
    delay(kPollIntervalMs);
  }

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
