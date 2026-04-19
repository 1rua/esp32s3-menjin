#ifndef PROVISIONING_H
#define PROVISIONING_H

#include <Arduino.h>
#include <WiFi.h>

#include "device_config.h"

enum class StartupState {
  BOOT_CHECK,
  PROVISION_DECISION,
  AP_PORTAL,
  CONNECTING_WIFI,
  NORMAL_RUNTIME
};

struct ProvisioningState {
  StartupState startupState = StartupState::BOOT_CHECK;
  bool portalActive = false;
  bool bootForcedProvision = false;
  bool bootButtonPressed = false;
  bool bootLongPressHandled = false;
  uint32_t bootPressStartedAt = 0;
};

void resetForcedProvisioningButtonState(ProvisioningState& state);
bool updateForcedProvisioningRequest(ProvisioningState& state, uint8_t bootButtonPin, uint32_t holdMs);
StartupState decideStartupState(const DeviceConfig& deviceConfig, bool bootForcedProvision);
void applyStartupDecision(ProvisioningState& state, StartupState startupState);
void startProvisioningPortal(ProvisioningState& state, const char* apSsid, const IPAddress& apIp, const IPAddress& apGateway, const IPAddress& apSubnet);
void stopProvisioningPortal(ProvisioningState& state);
void handleProvisioningSaved(ProvisioningState& state);
void handleProvisioningSkipped(ProvisioningState& state);
bool isProvisioningPortalActive(const ProvisioningState& state);
bool shouldAttemptWiFi(const ProvisioningState& state);

#endif
