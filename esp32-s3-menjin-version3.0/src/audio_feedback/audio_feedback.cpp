// 音频反馈模块实现：提供开机/开门/错误等统一提示音播放。
#include "audio_feedback.h"

namespace {
// SPIFFS 中约定的音频文件路径。
const char* kBootSound = "/boot.mp3";
const char* kOpenSound = "/open.mp3";
const char* kErrorSound = "/error.mp3";
}

void initAudioFeedback(Audio& audio, int bclkPin, int lrcPin, int doutPin, uint8_t volume) {
  audio.setPinout(bclkPin, lrcPin, doutPin);
  audio.setVolume(volume);
}

void tickAudioFeedback(Audio& audio) {
  audio.loop();
}

void playAudioFile(Audio& audio, const char* path) {
  if (SPIFFS.exists(path)) {
    audio.connecttoFS(SPIFFS, path);
  }
}

void playBootSound(Audio& audio) { playAudioFile(audio, kBootSound); }
void playOpenSound(Audio& audio) { playAudioFile(audio, kOpenSound); }
void playErrorSound(Audio& audio) { playAudioFile(audio, kErrorSound); }
