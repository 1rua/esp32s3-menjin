#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <Arduino.h>
#include <Preferences.h>

struct DeviceConfig {
  String wifiSsid;
  String wifiPassword;
  bool wifiConfigured = false;
  bool skipAutoProvision = false;
  String mqttUid;
};

void loadDeviceConfig(Preferences& prefs, DeviceConfig& config);
bool hasValidWiFiConfig(const DeviceConfig& config);
bool isMqttConfigured(const DeviceConfig& config);
void saveWiFiConfig(Preferences& prefs, DeviceConfig& config, const String& ssid, const String& password);
void saveMqttUid(Preferences& prefs, DeviceConfig& config, const String& mqttUid, bool overwriteIfEmpty = false);
void setSkipAutoProvision(Preferences& prefs, DeviceConfig& config, bool skipAutoProvision);

#endif
