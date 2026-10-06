#ifndef BRIDGE_CONFIG_H
#define BRIDGE_CONFIG_H

#include <stdbool.h>

#define CONFIG_STRING_LEN 128

typedef struct {
    char  headset_address[CONFIG_STRING_LEN];
    char  mic_output_device[CONFIG_STRING_LEN];
    char  speaker_input_device[CONFIG_STRING_LEN];
    float mic_gain_db;
    float speaker_gain_db;
    int   latency_ms;
    int   reconnect_interval_s;
} bridge_config_t;

void config_set_defaults(bridge_config_t * config);

// Returns false if the file could not be opened; unknown keys are reported and ignored.
bool config_load(bridge_config_t * config, const char * path);

// Writes all settings with explanatory comments, replacing the file.
bool config_save(const bridge_config_t * config, const char * path);

void config_print(const bridge_config_t * config);

#endif
