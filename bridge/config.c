#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_set_defaults(bridge_config_t * config){
    memset(config, 0, sizeof(*config));
    strcpy(config->mic_output_device,    "CABLE Input");
    strcpy(config->speaker_input_device, "Voicemeeter Out B1");
    config->mic_gain_db          = 0.0f;
    config->speaker_gain_db      = 0.0f;
    config->latency_ms           = 40;
    config->reconnect_interval_s = 5;
}

static char * trim(char * s){
    while (isspace((unsigned char) *s)) s++;
    char * end = s + strlen(s);
    while (end > s && isspace((unsigned char) end[-1])) end--;
    *end = '\0';
    return s;
}

static void copy_string(char * dest, const char * src){
    strncpy(dest, src, CONFIG_STRING_LEN - 1);
    dest[CONFIG_STRING_LEN - 1] = '\0';
}

bool config_load(bridge_config_t * config, const char * path){
    FILE * file = fopen(path, "r");
    if (file == NULL) return false;

    char line[512];
    int line_nr = 0;
    while (fgets(line, sizeof(line), file) != NULL){
        line_nr++;
        char * text = trim(line);
        if (*text == '\0' || *text == '#' || *text == ';' || *text == '[') continue;

        char * eq = strchr(text, '=');
        if (eq == NULL){
            printf("Config line %d ignored (no '='): %s\n", line_nr, text);
            continue;
        }
        *eq = '\0';
        char * key   = trim(text);
        char * value = trim(eq + 1);

        if      (strcmp(key, "headset_address") == 0)      copy_string(config->headset_address, value);
        else if (strcmp(key, "mic_output_device") == 0)    copy_string(config->mic_output_device, value);
        else if (strcmp(key, "speaker_input_device") == 0) copy_string(config->speaker_input_device, value);
        else if (strcmp(key, "mic_gain_db") == 0)          config->mic_gain_db = (float) atof(value);
        else if (strcmp(key, "speaker_gain_db") == 0)      config->speaker_gain_db = (float) atof(value);
        else if (strcmp(key, "latency_ms") == 0)           config->latency_ms = atoi(value);
        else if (strcmp(key, "reconnect_interval_s") == 0) config->reconnect_interval_s = atoi(value);
        else printf("Config line %d ignored (unknown key '%s')\n", line_nr, key);
    }
    fclose(file);

    if (config->latency_ms < 20)  config->latency_ms = 20;
    if (config->latency_ms > 500) config->latency_ms = 500;
    if (config->reconnect_interval_s < 2) config->reconnect_interval_s = 2;
    return true;
}

bool config_save(const bridge_config_t * config, const char * path){
    FILE * file = fopen(path, "w");
    if (file == NULL) return false;

    fprintf(file,
        "# Phone Link headset bridge settings\n"
        "# Device names are matched by case-insensitive substring. Run \"headset_bridge.exe --list\" to see them.\n"
        "\n"
        "# Bluetooth address of the headset. Run \"headset_bridge.exe --scan\" to find it.\n"
        "headset_address = %s\n"
        "\n"
        "# Where the headset microphone is played. \"CABLE Input\" makes the mic appear in Windows as\n"
        "# \"CABLE Output (VB-Audio Virtual Cable)\" - select that as the microphone for Phone Link.\n"
        "mic_output_device = %s\n"
        "\n"
        "# What the headset plays. Route Phone Link / PC audio to this device (e.g. a Voicemeeter bus).\n"
        "speaker_input_device = %s\n"
        "\n"
        "# Volume adjustments in dB (e.g. 6 = louder, -6 = quieter)\n"
        "mic_gain_db = %g\n"
        "speaker_gain_db = %g\n"
        "\n"
        "# Audio buffer in each direction. Raise it if you hear crackles, lower it for less delay.\n"
        "latency_ms = %d\n"
        "\n"
        "# Seconds between reconnect attempts while the headset is off or out of range\n"
        "reconnect_interval_s = %d\n",
        config->headset_address, config->mic_output_device, config->speaker_input_device,
        config->mic_gain_db, config->speaker_gain_db, config->latency_ms, config->reconnect_interval_s);

    bool ok = ferror(file) == 0;
    return fclose(file) == 0 && ok;
}

void config_print(const bridge_config_t * config){
    printf("  Headset:              %s\n", config->headset_address);
    printf("  Headset mic  -> plays into  '%s' (gain %+.1f dB)\n", config->mic_output_device, config->mic_gain_db);
    printf("  Headset ear  <- records from '%s' (gain %+.1f dB)\n", config->speaker_input_device, config->speaker_gain_db);
    printf("  Buffer latency:       %d ms\n", config->latency_ms);
}
