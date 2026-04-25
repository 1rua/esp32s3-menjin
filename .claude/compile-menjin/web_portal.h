#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>

#include "access_control.h"
#include "device_config.h"
#include "provisioning.h"

typedef void (*WebPortalDoorOpenCallback)();
typedef int (*WebPortalAddNfcCallback)(const String& uid, String& message);
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
  WebPortalStateChangedCallback onProvisioningStateChanged;
  WebPortalTimeSyncedCallback isTimeSynced;
  WebPortalCurrentEpochCallback currentEpochSeconds;
  WebPortalLocalTimeCallback currentLocalTimeString;
};

void setupWebRoutes(WebPortalContext& context);

#endif
