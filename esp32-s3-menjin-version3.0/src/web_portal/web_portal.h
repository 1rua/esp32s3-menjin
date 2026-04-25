#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>

#include "../access_control/access_control.h"
#include "../device_config/device_config.h"
#include "../provisioning/provisioning.h"

typedef void (*WebPortalDoorOpenCallback)();
typedef int (*WebPortalAddNfcCallback)(const String& uid, String& message);
typedef int (*WebPortalFingerprintEnrollStartCallback)(const String& name, String& message);
typedef void (*WebPortalFingerprintEnrollStatusCallback)(String& dataJson);
typedef int (*WebPortalFingerprintEnrollCancelCallback)(String& message);
typedef void (*WebPortalFingerprintListCallback)(String& itemsJson);
typedef int (*WebPortalFingerprintRenameCallback)(int id, const String& name, String& message);
typedef int (*WebPortalFingerprintDeleteCallback)(int id, String& message);
typedef void (*WebPortalStateChangedCallback)();
typedef bool (*WebPortalTimeSyncedCallback)();
typedef uint32_t (*WebPortalCurrentEpochCallback)();
typedef String (*WebPortalLocalTimeCallback)();

struct WebPortalContext {
  WebServer& server;
  Preferences& prefs;
  DeviceConfig& deviceConfig;
  ProvisioningState& provisioningState;
  AccessControlState& accessControl;
  WebPortalDoorOpenCallback onDoorOpen;
  WebPortalAddNfcCallback onAddNfc;
  WebPortalFingerprintEnrollStartCallback onStartFingerprintEnroll;
  WebPortalFingerprintEnrollStatusCallback onFingerprintEnrollStatus;
  WebPortalFingerprintEnrollCancelCallback onCancelFingerprintEnroll;
  WebPortalFingerprintListCallback onListFingerprints;
  WebPortalFingerprintRenameCallback onRenameFingerprint;
  WebPortalFingerprintDeleteCallback onDeleteFingerprint;
  WebPortalStateChangedCallback onProvisioningStateChanged;
  WebPortalTimeSyncedCallback isTimeSynced;
  WebPortalCurrentEpochCallback currentEpochSeconds;
  WebPortalLocalTimeCallback currentLocalTimeString;
};

void setupWebRoutes(WebPortalContext& context);
