#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>

#include "device_config.h"
#include "provisioning.h"

typedef void (*WebPortalDoorOpenCallback)();
typedef int (*WebPortalAddNfcCallback)(const String& uid, String& message);
typedef void (*WebPortalStateChangedCallback)();

struct WebPortalContext {
  WebServer& server;
  Preferences& prefs;
  DeviceConfig& deviceConfig;
  ProvisioningState& provisioningState;
  WebPortalDoorOpenCallback onDoorOpen;
  WebPortalAddNfcCallback onAddNfc;
  WebPortalStateChangedCallback onProvisioningStateChanged;
};

void setupWebRoutes(WebPortalContext& context);

#endif
