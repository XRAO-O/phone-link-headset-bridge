#include "audio_io.h"

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <portaudio.h>
#include <pa_win_wasapi.h>

// Single-producer / single-consumer sample FIFO shared between the Bluetooth thread and a
// PortAudio callback thread. The consumer keeps the fill level near `target` by dropping or
// duplicating at most one sample per read, which absorbs the clock drift between the
// Bluetooth controller and the Windows audio device.

#define FIFO_CAPACITY  16384u
#define FIFO_MASK      (FIFO_CAPACITY - 1u)
#define MAX_CHUNK      1024u

typedef struct {
    int16_t     data[FIFO_CAPACITY];
    atomic_uint write_pos;
    atomic_uint read_pos;
    bool        primed;
    uint32_t    target;
    uint32_t    tolerance;
    atomic_uint underruns;
    atomic_uint overruns;
    atomic_uint adjustments;
} fifo_t;

static fifo_t mic_fifo;
static fifo_t speaker_fifo;

static PaStream *  mic_stream;
static PaStream *  speaker_stream;
static atomic_bool running;
static float       mic_gain = 1.0f;
static float       speaker_gain = 1.0f;
static bool        pa_initialized;

static void fifo_reset(fifo_t * fifo, uint32_t target){
    atomic_store(&fifo->write_pos, 0);
    atomic_store(&fifo->read_pos, 0);
    fifo->primed    = false;
    fifo->target    = target;
    fifo->tolerance = target / 2;
}

static void fifo_write(fifo_t * fifo, const int16_t * samples, uint32_t num_samples){
    uint32_t w = atomic_load_explicit(&fifo->write_pos, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&fifo->read_pos, memory_order_acquire);
    uint32_t space = FIFO_CAPACITY - (w - r);
    if (num_samples > space){
        atomic_fetch_add(&fifo->overruns, 1);
        num_samples = space;
    }
    for (uint32_t i = 0; i < num_samples; i++){
        fifo->data[(w + i) & FIFO_MASK] = samples[i];
    }
    atomic_store_explicit(&fifo->write_pos, w + num_samples, memory_order_release);
}

static uint32_t fifo_fill(fifo_t * fifo){
    uint32_t w = atomic_load_explicit(&fifo->write_pos, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&fifo->read_pos, memory_order_relaxed);
    return w - r;
}

// out may be NULL to discard samples
static void fifo_take(fifo_t * fifo, int16_t * out, uint32_t num_samples){
    uint32_t r = atomic_load_explicit(&fifo->read_pos, memory_order_relaxed);
    if (out != NULL){
        for (uint32_t i = 0; i < num_samples; i++){
            out[i] = fifo->data[(r + i) & FIFO_MASK];
        }
    }
    atomic_store_explicit(&fifo->read_pos, r + num_samples, memory_order_release);
}

static void fifo_read_chunk(fifo_t * fifo, int16_t * out, uint32_t n){
    uint32_t fill = fifo_fill(fifo);

    if (!fifo->primed){
        if (fill < fifo->target){
            memset(out, 0, n * sizeof(int16_t));
            return;
        }
        fifo->primed = true;
    }

    // after a stall, jump back to the target instead of carrying the extra latency
    if (fill > fifo->target * 3 + n){
        fifo_take(fifo, NULL, fill - fifo->target);
        fill = fifo->target;
        atomic_fetch_add(&fifo->adjustments, 1);
    }

    if (fill < n){
        fifo_take(fifo, out, fill);
        memset(&out[fill], 0, (n - fill) * sizeof(int16_t));
        fifo->primed = false;
        atomic_fetch_add(&fifo->underruns, 1);
        return;
    }

    uint32_t mid = n / 2;
    if (fill > fifo->target + fifo->tolerance && fill > n){
        int16_t tmp[MAX_CHUNK + 1];
        fifo_take(fifo, tmp, n + 1);
        memcpy(out, tmp, mid * sizeof(int16_t));
        out[mid] = (int16_t) (((int32_t) tmp[mid] + tmp[mid + 1]) / 2);
        memcpy(&out[mid + 1], &tmp[mid + 2], (n - mid - 1) * sizeof(int16_t));
        atomic_fetch_add(&fifo->adjustments, 1);
    } else if (fill < fifo->target - fifo->tolerance && n >= 2){
        fifo_take(fifo, out, n - 1);
        memmove(&out[mid + 1], &out[mid], (n - 1 - mid) * sizeof(int16_t));
        if (mid > 0){
            out[mid] = (int16_t) (((int32_t) out[mid - 1] + out[mid + 1]) / 2);
        }
        atomic_fetch_add(&fifo->adjustments, 1);
    } else {
        fifo_take(fifo, out, n);
    }
}

static void fifo_read_adaptive(fifo_t * fifo, int16_t * out, uint32_t num_samples){
    while (num_samples > 0){
        uint32_t n = num_samples > MAX_CHUNK ? MAX_CHUNK : num_samples;
        fifo_read_chunk(fifo, out, n);
        out += n;
        num_samples -= n;
    }
}

static void apply_gain(int16_t * samples, int num_samples, float gain){
    if (gain == 1.0f) return;
    for (int i = 0; i < num_samples; i++){
        float v = samples[i] * gain;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        samples[i] = (int16_t) v;
    }
}

static int mic_output_callback(const void * input, void * output, unsigned long frames,
                               const PaStreamCallbackTimeInfo * time_info, PaStreamCallbackFlags flags, void * user_data){
    (void) input; (void) time_info; (void) flags; (void) user_data;
    fifo_read_adaptive(&mic_fifo, (int16_t *) output, (uint32_t) frames);
    return paContinue;
}

static int speaker_input_callback(const void * input, void * output, unsigned long frames,
                                  const PaStreamCallbackTimeInfo * time_info, PaStreamCallbackFlags flags, void * user_data){
    (void) output; (void) time_info; (void) flags; (void) user_data;
    if (input != NULL){
        fifo_write(&speaker_fifo, (const int16_t *) input, (uint32_t) frames);
    }
    return paContinue;
}

static bool contains_ignore_case(const char * haystack, const char * needle){
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return false;
    for (; *haystack; haystack++){
        size_t i = 0;
        while (i < needle_len && haystack[i] &&
               tolower((unsigned char) haystack[i]) == tolower((unsigned char) needle[i])) i++;
        if (i == needle_len) return true;
    }
    return false;
}

// WASAPI is tried first for lower latency; AutoConvert lets Windows resample to the device mix format.
static PaStream * open_stream(const char * device_name, bool is_input, int sample_rate, PaStreamCallback * callback){
    static const PaHostApiTypeId host_api_order[] = { paWASAPI, paMME, paDirectSound };
    bool found = false;

    for (size_t a = 0; a < sizeof(host_api_order) / sizeof(host_api_order[0]); a++){
        PaHostApiIndex host_index = Pa_HostApiTypeIdToHostApiIndex(host_api_order[a]);
        if (host_index < 0) continue;
        const PaHostApiInfo * host_info = Pa_GetHostApiInfo(host_index);

        for (int i = 0; i < host_info->deviceCount; i++){
            PaDeviceIndex device = Pa_HostApiDeviceIndexToDeviceIndex(host_index, i);
            const PaDeviceInfo * info = Pa_GetDeviceInfo(device);
            int channels = is_input ? info->maxInputChannels : info->maxOutputChannels;
            if (channels < 1 || !contains_ignore_case(info->name, device_name)) continue;
            found = true;

            PaStreamParameters params;
            memset(&params, 0, sizeof(params));
            params.device           = device;
            params.channelCount     = 1;
            params.sampleFormat     = paInt16;
            params.suggestedLatency = is_input ? info->defaultLowInputLatency : info->defaultLowOutputLatency;

            PaWasapiStreamInfo wasapi_info;
            if (host_api_order[a] == paWASAPI){
                memset(&wasapi_info, 0, sizeof(wasapi_info));
                wasapi_info.size        = sizeof(wasapi_info);
                wasapi_info.hostApiType = paWASAPI;
                wasapi_info.version     = 1;
                wasapi_info.flags       = paWinWasapiAutoConvert;
                params.hostApiSpecificStreamInfo = &wasapi_info;
            }

            PaStream * stream = NULL;
            PaError err = Pa_OpenStream(&stream, is_input ? &params : NULL, is_input ? NULL : &params,
                                        sample_rate, sample_rate / 100, paClipOff, callback, NULL);
            if (err == paNoError){
                err = Pa_StartStream(stream);
                if (err == paNoError){
                    printf("  %s '%s' via %s at %d Hz\n", is_input ? "Recording from" : "Playing into",
                           info->name, host_info->name, sample_rate);
                    return stream;
                }
                Pa_CloseStream(stream);
            }
            printf("  Could not open '%s' via %s: %s\n", info->name, host_info->name, Pa_GetErrorText(err));
        }
    }

    if (!found){
        printf("  No %s device matching '%s' found. Run with --list to see device names.\n",
               is_input ? "recording" : "playback", device_name);
    }
    return NULL;
}

bool audio_io_init(void){
    PaError err = Pa_Initialize();
    if (err != paNoError){
        printf("PortAudio init failed: %s\n", Pa_GetErrorText(err));
        return false;
    }
    pa_initialized = true;
    return true;
}

void audio_io_terminate(void){
    audio_io_stop();
    if (pa_initialized){
        Pa_Terminate();
        pa_initialized = false;
    }
}

void audio_io_list_devices(void){
    int count = Pa_GetDeviceCount();
    printf("\nPlayback devices (use for mic_output_device):\n");
    for (int i = 0; i < count; i++){
        const PaDeviceInfo * info = Pa_GetDeviceInfo(i);
        if (info->maxOutputChannels < 1) continue;
        printf("  [%-15s] %s\n", Pa_GetHostApiInfo(info->hostApi)->name, info->name);
    }
    printf("\nRecording devices (use for speaker_input_device):\n");
    for (int i = 0; i < count; i++){
        const PaDeviceInfo * info = Pa_GetDeviceInfo(i);
        if (info->maxInputChannels < 1) continue;
        printf("  [%-15s] %s\n", Pa_GetHostApiInfo(info->hostApi)->name, info->name);
    }
    printf("\nDevice names are matched by case-insensitive substring; WASAPI is preferred.\n");
}

void audio_io_start(const bridge_config_t * config, int sample_rate){
    audio_io_stop();

    uint32_t target = (uint32_t) (config->latency_ms * sample_rate / 1000);
    fifo_reset(&mic_fifo, target);
    fifo_reset(&speaker_fifo, target);
    mic_gain     = powf(10.0f, config->mic_gain_db / 20.0f);
    speaker_gain = powf(10.0f, config->speaker_gain_db / 20.0f);
    atomic_store(&running, true);

    mic_stream     = open_stream(config->mic_output_device, false, sample_rate, &mic_output_callback);
    speaker_stream = open_stream(config->speaker_input_device, true, sample_rate, &speaker_input_callback);
}

static void close_stream(PaStream ** stream){
    if (*stream == NULL) return;
    Pa_StopStream(*stream);
    Pa_CloseStream(*stream);
    *stream = NULL;
}

void audio_io_stop(void){
    atomic_store(&running, false);
    close_stream(&mic_stream);
    close_stream(&speaker_stream);
}

void audio_io_push_mic(const int16_t * samples, int num_samples){
    if (!atomic_load(&running) || mic_stream == NULL) return;
    int16_t tmp[MAX_CHUNK];
    while (num_samples > 0){
        int n = num_samples > (int) MAX_CHUNK ? (int) MAX_CHUNK : num_samples;
        memcpy(tmp, samples, n * sizeof(int16_t));
        apply_gain(tmp, n, mic_gain);
        fifo_write(&mic_fifo, tmp, (uint32_t) n);
        samples     += n;
        num_samples -= n;
    }
}

void audio_io_pull_speaker(int16_t * samples, int num_samples){
    if (!atomic_load(&running) || speaker_stream == NULL){
        memset(samples, 0, num_samples * sizeof(int16_t));
        return;
    }
    fifo_read_adaptive(&speaker_fifo, samples, (uint32_t) num_samples);
    apply_gain(samples, num_samples, speaker_gain);
}

void audio_io_get_stats(audio_stats_t * stats){
    stats->mic_underruns       = atomic_load(&mic_fifo.underruns);
    stats->mic_overruns        = atomic_load(&mic_fifo.overruns);
    stats->mic_adjustments     = atomic_load(&mic_fifo.adjustments);
    stats->speaker_underruns   = atomic_load(&speaker_fifo.underruns);
    stats->speaker_overruns    = atomic_load(&speaker_fifo.overruns);
    stats->speaker_adjustments = atomic_load(&speaker_fifo.adjustments);
}
