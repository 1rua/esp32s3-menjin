// 键盘鉴权模块接口：维护输入状态、失败锁定与校验结果。
#pragma once

#include <Arduino.h>
#include <Keypad.h>
#include <Preferences.h>

#include "../access_control/access_control.h"

struct KeypadAccessState {
  String inputCode;
  unsigned long lastKeyTime = 0;
  uint8_t failedAttempts = 0;
  unsigned long lockoutUntil = 0;
};

struct KeypadAccessResult {
  bool authorized = false;
  bool rejected = false;
  String matchedSource;
};

KeypadAccessResult pollKeypadAccess(KeypadAccessState& state, Keypad& keypad, Preferences& prefs, AccessControlState& accessControl, unsigned long nowMs, uint32_t nowEpoch, bool timeSynced);
bool keypadAccessLocked(const KeypadAccessState& state, unsigned long nowMs);
