#ifndef BRIDGE_AUDIO_IO_H
#define BRIDGE_AUDIO_IO_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

typedef struct {
    uint32_t mic_underruns;
    uint32_t mic_overruns;
    uint32_t mic_adjustments;
    uint32_t speaker_underruns;
    uint32_t speaker_overruns;
    uint32_t speaker_adjustments;
} audio_stats_t;

bool audio_io_init(void);
void audio_io_terminate(void);
void audio_io_list_devices(void);

// Opens both Windows audio streams at the codec sample rate. Mic and speaker paths are
// independent: if one device cannot be opened the other still runs.
void audio_io_start(const bridge_config_t * config, int sample_rate);
void audio_io_stop(void);

// Called on the Bluetooth thread only.
void audio_io_push_mic(const int16_t * samples, int num_samples);
void audio_io_pull_speaker(int16_t * samples, int num_samples);

void audio_io_get_stats(audio_stats_t * stats);

#endif
