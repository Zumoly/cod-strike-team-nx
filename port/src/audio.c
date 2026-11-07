// Stands in for org.fmod.FMODAudioDevice.  On Android that Java class owns a thread which polls
// FMOD until its output exists, then loops on fmodProcess and AudioTrack.write, and the blocking
// write paces the mixer.  Here audio_out_write does the pacing, and audio_out_* is audout on the
// console (src/audio_nx.c).
#include "port/audio.h"
#include "port/bionic_thread.h"
#include "port/fake_jni.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INFO_RATE 0
#define INFO_LENGTH 1
#define INFO_BUFFERS 2
#define OUT_RATE 48000
#define CHANNELS 2
#define WAIT_NS (20 * 1000 * 1000)

void b_sleep_ns(int64_t ns);

static int (*g_get_info)(void *, void *, int);
static int (*g_process)(void *, void *, void *);
static void *g_device;

// Linear interpolation between FMOD's rate and the device's: `last` carries the frame before
// this block, so one block's end joins the next block's start.
static void resample_and_play(const int16_t *in, int frames, int rate)
{
    static int16_t block[AUDIO_BLOCK_FRAMES * CHANNELS];
    static int used;
    static int16_t last[CHANNELS];
    static uint32_t phase; // position between frames, 16.16
    uint32_t step = (uint32_t)(((uint64_t)rate << 16) / OUT_RATE);

    for (;;) {
        int at = (int)(phase >> 16);
        if (at >= frames)
            break;
        int fraction = (int)(phase & 0xFFFF);
        for (int c = 0; c < CHANNELS; c++) {
            int a = at == 0 ? last[c] : in[(at - 1) * CHANNELS + c];
            int b = in[at * CHANNELS + c];
            block[used * CHANNELS + c] = (int16_t)(a + (((b - a) * fraction) >> 16));
        }
        phase += step;
        if (++used == AUDIO_BLOCK_FRAMES) {
            audio_out_write(block);
            used = 0;
        }
    }
    phase -= (uint32_t)frames << 16;
    memcpy(last, &in[(frames - 1) * CHANNELS], sizeof last);
}

static void *pump(void *unused)
{
    (void)unused;
    void *env = jni_env();
    for (;;) {
        // -1 until FMOD has started its output, and again if it shuts it down.
        int rate = g_get_info(env, g_device, INFO_RATE);
        if (rate <= 0) {
            b_sleep_ns(WAIT_NS);
            continue;
        }
        int length = g_get_info(env, g_device, INFO_LENGTH);
        int buffers = g_get_info(env, g_device, INFO_BUFFERS);
        size_t bytes = (size_t)length * CHANNELS * sizeof(int16_t);
        int16_t *pcm = calloc(1, bytes);
        void *direct = jni_direct_buffer(pcm, bytes);
        fprintf(stderr, "audio: FMOD output %d Hz, %d frames x %d buffers\n", rate, length, buffers);
        if (!pcm || audio_out_open() != 0) {
            fprintf(stderr, "audio: no sound\n");
            return NULL;
        }
        while (g_process(env, g_device, direct) >= 0)
            resample_and_play(pcm, length, rate);
        free(pcm);
    }
}

int audio_start(void)
{
    g_get_info = (int (*)(void *, void *, int))jni_native("fmodGetInfo");
    g_process = (int (*)(void *, void *, void *))jni_native("fmodProcess");
    void (*init)(void *, void *) = (void (*)(void *, void *))jni_native("fmodInitJni");
    if (!g_get_info || !g_process || !init) {
        fprintf(stderr, "audio: FMOD natives are missing\n");
        return -1;
    }
    g_device = jni_object("org/fmod/FMODAudioDevice");
    init(jni_env(), g_device);
    b_pthread_t thread;
    return b_pthread_create(&thread, NULL, pump, NULL);
}
