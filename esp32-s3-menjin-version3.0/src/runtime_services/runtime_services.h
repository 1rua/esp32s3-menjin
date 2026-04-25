// 运行时服务接口：统一 Wi-Fi/MQTT/OTA/NTP/Web 服务协同逻辑。
#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WebServer.h>

#include "../access_control/access_control.h"
#include "../device_config/device_config.h"
#include "../provisioning/provisioning.h"
#include "../web_portal/web_portal.h"

typedef void (*RuntimeOtaStartSideEffect)();
typedef void (*RuntimeMqttMessageCallback)(char* topic, byte* payload, unsigned int length);
typedef bool (*RuntimeDoorOpenStateProvider)();

struct RuntimeServicesState {
  bool isOtaUpdating = false;
  bool otaReady = false;
  bool wifiConnectionAttemptActive = false;
  bool webServerReady = false;
  bool timeSyncStarted = false;
  unsigned long mqttDisconnectTime = 0;
  unsigned long wifiConnectStartedAt = 0;
  unsigned long lastWiFiReconnectAttempt = 0;
  unsigned long lastMqttReconnectAttempt = 0;
};

struct RuntimeServicesContext {
  Preferences& prefs;
  DeviceConfig& deviceConfig;
  ProvisioningState& provisioningState;
  AccessControlState& accessControl;
  PubSubClient& mqttClient;
  WebServer& server;
  WebPortalContext& webPortalContext;
  RuntimeOtaStartSideEffect onOtaStartSideEffect;
  RuntimeDoorOpenStateProvider isDoorOpen;
};

bool shouldServiceWebServer(const RuntimeServicesContext& context);
bool shouldAttemptWiFiConnection(const RuntimeServicesContext& context);
void syncLegacyApStateFromProvisioningPortal(RuntimeServicesState& state, const ProvisioningState& provisioningState);
void beginWiFiConnectionAttempt(RuntimeServicesState& state, const RuntimeServicesContext& context);
void processForcedProvisioningButton(RuntimeServicesState& state, RuntimeServicesContext& context, uint8_t bootButtonPin, uint32_t holdMs, const char* apSsid, const IPAddress& apIp, const IPAddress& apGateway, const IPAddress& apSubnet);
void ensureWebServerReady(RuntimeServicesState& state, RuntimeServicesContext& context);
void maintainWiFiConnection(RuntimeServicesState& state, RuntimeServicesContext& context);
void maintainMqttConnection(RuntimeServicesState& state, RuntimeServicesContext& context, const char* mqttServer, int mqttPort, const char* topicDoor, RuntimeMqttMessageCallback mqttCallback);
void serviceMqttLoopOnly(RuntimeServicesContext& context);
void ensureTimeSyncStarted(RuntimeServicesState& state);
bool runtimeIsTimeSynced();
uint32_t runtimeCurrentEpochSeconds();
String runtimeCurrentLocalTimeString();
void ensureOtaReady(RuntimeServicesState& state, RuntimeServicesContext& context, const char* otaHostname, const char* otaPassword);
