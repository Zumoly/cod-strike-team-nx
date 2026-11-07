// Sound: src/audio.c runs FMOD's mixer, and audio_out_* plays what it makes.
#ifndef PORT_AUDIO_H
#define PORT_AUDIO_H

#include <stdint.h>

#define AUDIO_BLOCK_FRAMES 1024 // must match RT_AUDOUT_FRAMES

int audio_start(void); // once FMOD's natives are registered

int audio_out_open(void);
void audio_out_write(const int16_t *stereo_48khz_block); // blocks while the device is full

#endif
