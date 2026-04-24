#pragma once

#include <Audio.h>
#include <SPIFFS.h>

void initAudioFeedback(Audio& audio, int bclkPin, int lrcPin, int doutPin, uint8_t volume);
void tickAudioFeedback(Audio& audio);
void playAudioFile(Audio& audio, const char* path);
void playBootSound(Audio& audio);
void playOpenSound(Audio& audio);
void playErrorSound(Audio& audio);
