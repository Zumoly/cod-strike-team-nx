#include "port/audio.h"

#include "rt/rt_audout.h"

#include <switch.h>

#define AUDIO_PRIORITY 0x2A // above the game's threads, so the mixer never runs dry

_Static_assert(AUDIO_BLOCK_FRAMES == RT_AUDOUT_FRAMES, "src/rt/rt_audout.c takes whole blocks");

int audio_out_open(void)
{
    svcSetThreadPriority(CUR_THREAD_HANDLE, AUDIO_PRIORITY);
    return rt_audout_open();
}
void audio_out_write(const int16_t *block) { rt_audout_submit(block); }
