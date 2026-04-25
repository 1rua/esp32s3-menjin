// 设备配置实现：负责从 Preferences 读取、保存与规范化配置值。
#include "device_config.h"

namespace {
const char* kWifiSsidKey = "wifi_ssid";
const char* kWifiPassKey = "wifi_pass";
const char* kWifiConfiguredKey = "wifi_configured";
const char* kSkipAutoProvisionKey = "skip_auto_prov";
const char* kMqttUidKey = "mqtt_uid";
}

void loadDeviceConfig(Preferences& prefs, DeviceConfig& config) {
  config.wifiSsid = prefs.getString(kWifiSsidKey, "");
  config.wifiPassword = prefs.getString(kWifiPassKey, "");
  config.wifiConfigured = prefs.getBool(kWifiConfiguredKey, false);
  config.skipAutoProvision = prefs.getBool(kSkipAutoProvisionKey, false);
  config.mqttUid = prefs.getString(kMqttUidKey, "");
}

bool hasValidWiFiConfig(const DeviceConfig& config) {
  return config.wifiConfigured && config.wifiSsid.length() > 0;
}

bool isMqttConfigured(const DeviceConfig& config) {
  return config.mqttUid.length() > 0;
}

void saveWiFiConfig(Preferences& prefs, DeviceConfig& config, const String& ssid, const String& password) {
  String normalizedSsid = ssid;
  normalizedSsid.trim();
  const bool wifiConfigured = normalizedSsid.length() > 0;

  prefs.putString(kWifiSsidKey, normalizedSsid);
  prefs.putString(kWifiPassKey, password);
  prefs.putBool(kWifiConfiguredKey, wifiConfigured);
  prefs.putBool(kSkipAutoProvisionKey, false);

  config.wifiSsid = normalizedSsid;
  config.wifiPassword = password;
  config.wifiConfigured = wifiConfigured;
  config.skipAutoProvision = false;
}

void saveMqttUid(Preferences& prefs, DeviceConfig& config, const String& mqttUid, bool overwriteIfEmpty) {
  if (mqttUid.length() == 0 && !overwriteIfEmpty) {
    return;
  }

  prefs.putString(kMqttUidKey, mqttUid);
  config.mqttUid = mqttUid;
}

void setSkipAutoProvision(Preferences& prefs, DeviceConfig& config, bool skipAutoProvision) {
  prefs.putBool(kSkipAutoProvisionKey, skipAutoProvision);
  config.skipAutoProvision = skipAutoProvision;
}
