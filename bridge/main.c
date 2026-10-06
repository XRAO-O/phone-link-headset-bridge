// Headset bridge: runs a Bluetooth Hands-Free Audio Gateway on a dedicated USB dongle (WinUSB)
// so a headset keeps its microphone while Windows' own Bluetooth radio is busy with Phone Link.
// Headset mic audio is played into a virtual cable, and a Voicemeeter bus is sent to the headset.

#define BTSTACK_FILE__ "main.c"

#include <windows.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack_config.h"
#include "btstack.h"
#include "ble/le_device_db_tlv.h"
#include "btstack_run_loop_windows.h"
#include "btstack_stdin.h"
#include "btstack_stdin_windows.h"
#include "btstack_tlv_windows.h"
#include "classic/btstack_cvsd_plc.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "classic/btstack_sbc.h"
#include "classic/btstack_sbc_bluedroid.h"
#include "classic/hfp_codec.h"
#include "hci_dump.h"
#include "hci_dump_windows_fs.h"
#include "hci_transport_usb.h"

#include "audio_io.h"
#include "config.h"
#include "scan.h"
#include "version.h"

#define RFCOMM_CHANNEL_NR        1
#define AUDIO_CONNECT_DELAY_MS   500
#define AUDIO_RETRY_DELAY_MS     2000
#define STATS_INTERVAL_MS        30000
#define MSBC_SAMPLES_PER_FRAME   120
#define CVSD_MAX_SAMPLES         128

static bridge_config_t config;
static bd_addr_t       headset_addr;
static char            exe_dir[MAX_PATH];
static bool            scan_mode;

// Lets a parent process (the GUI) request a clean shutdown, since it has no console to send Ctrl+C to.
static btstack_data_source_t stop_event_source;

// --- BTstack port setup (persistent pairing keys) ---

static btstack_packet_callback_registration_t hci_event_callback_registration;
static const btstack_tlv_t * tlv_impl;
static btstack_tlv_windows_t tlv_context;
static char tlv_db_path[MAX_PATH + 64];

// --- connection state ---

static hci_con_handle_t acl_handle = HCI_CON_HANDLE_INVALID;
static hci_con_handle_t sco_handle = HCI_CON_HANDLE_INVALID;
static bool shutdown_triggered;
static bool slc_connecting;
static int  connect_attempts;

typedef enum { ACTION_NONE, ACTION_CONNECT_SLC, ACTION_CONNECT_AUDIO } pending_action_t;
static btstack_timer_source_t action_timer;
static pending_action_t       pending_action;
static btstack_timer_source_t stats_timer;
static audio_stats_t          last_stats;

// --- codec state ---

static uint8_t negotiated_codec;
static btstack_cvsd_plc_state_t        cvsd_plc;
static const btstack_sbc_decoder_t *   sbc_decoder;
static btstack_sbc_decoder_bluedroid_t sbc_decoder_context;
static const btstack_sbc_encoder_t *   sbc_encoder;
static btstack_sbc_encoder_bluedroid_t sbc_encoder_context;
static hfp_codec_t                     msbc_codec;

// --- HFP AG configuration ---

static uint8_t hfp_service_buffer[150];
static const char hfp_ag_service_name[] = "Headset Bridge";

static uint8_t codecs[] = { HFP_CODEC_CVSD, HFP_CODEC_MSBC };

static hfp_ag_indicator_t ag_indicators[] = {
    // index, name, min range, max range, status, mandatory, enabled, status changed
    {1, "service",   0, 1, 1, 0, 0, 0},
    {2, "call",      0, 1, 0, 1, 1, 0},
    {3, "callsetup", 0, 3, 0, 1, 1, 0},
    {4, "battchg",   0, 5, 5, 0, 0, 0},
    {5, "signal",    0, 5, 5, 0, 1, 0},
    {6, "roam",      0, 1, 0, 0, 1, 0},
    {7, "callheld",  0, 2, 0, 1, 1, 0}
};

static const char * call_hold_services[] = {"1", "1x", "2", "2x", "3"};

static hfp_generic_status_indicator_t hf_indicators[] = {
    {1, 1},
    {2, 1},
};

// ---------------------------------------------------------------------------------------------
// Codec handling

static void msbc_pcm_handler(int16_t * data, int num_samples, int num_channels, int sample_rate, void * context){
    (void) num_channels; (void) sample_rate; (void) context;
    audio_io_push_mic(data, num_samples);
}

static int codec_start(uint8_t codec){
    negotiated_codec = codec;
    if (codec == HFP_CODEC_MSBC){
        sbc_decoder = btstack_sbc_decoder_bluedroid_init_instance(&sbc_decoder_context);
        sbc_decoder->configure(&sbc_decoder_context, SBC_MODE_mSBC, &msbc_pcm_handler, NULL);
        sbc_encoder = btstack_sbc_encoder_bluedroid_init_instance(&sbc_encoder_context);
        hfp_codec_init_msbc_with_codec(&msbc_codec, sbc_encoder, &sbc_encoder_context);
        return 16000;
    }
    btstack_cvsd_plc_init(&cvsd_plc);
    return 8000;
}

static void codec_receive(const uint8_t * packet, uint16_t size){
    if (negotiated_codec == HFP_CODEC_MSBC){
        sbc_decoder->decode_signed_16(&sbc_decoder_context, (packet[1] >> 4) & 3, packet + 3, size - 3);
        return;
    }

    int num_samples = (size - 3) / 2;
    if (num_samples > CVSD_MAX_SAMPLES) return;
    int16_t samples_in[CVSD_MAX_SAMPLES];
    int16_t samples_out[CVSD_MAX_SAMPLES];
    for (int i = 0; i < num_samples; i++){
        samples_in[i] = (int16_t) little_endian_read_16(packet, 3 + i * 2);
    }
    bool bad_frame = (packet[1] & 0x30) != 0;
    btstack_cvsd_plc_process_data(&cvsd_plc, bad_frame, samples_in, num_samples, samples_out);
    audio_io_push_mic(samples_out, num_samples);
}

static void codec_fill_payload(uint8_t * payload, uint16_t payload_len){
    if (negotiated_codec == HFP_CODEC_MSBC){
        while (payload_len > 0){
            if (hfp_codec_can_encode_audio_frame_now(&msbc_codec)){
                int16_t samples[MSBC_SAMPLES_PER_FRAME];
                audio_io_pull_speaker(samples, MSBC_SAMPLES_PER_FRAME);
                hfp_codec_encode_audio_frame(&msbc_codec, samples);
            }
            uint16_t bytes_to_read = btstack_min(hfp_codec_num_bytes_available(&msbc_codec), payload_len);
            hfp_codec_read_from_stream(&msbc_codec, payload, bytes_to_read);
            payload_len -= bytes_to_read;
            payload     += bytes_to_read;
        }
        return;
    }

    int num_samples = btstack_min(payload_len / 2, CVSD_MAX_SAMPLES);
    int16_t samples[CVSD_MAX_SAMPLES];
    audio_io_pull_speaker(samples, num_samples);
    for (int i = 0; i < num_samples; i++){
        little_endian_store_16(payload, i * 2, (uint16_t) samples[i]);
    }
}

static void send_sco_packet(hci_con_handle_t handle){
    if (handle == HCI_CON_HANDLE_INVALID || handle != sco_handle) return;

    int sco_packet_length  = hci_get_sco_packet_length_for_connection(handle);
    int sco_payload_length = sco_packet_length - 3;

    hci_reserve_packet_buffer();
    uint8_t * sco_packet = hci_get_outgoing_packet_buffer();
    codec_fill_payload(&sco_packet[3], (uint16_t) sco_payload_length);
    little_endian_store_16(sco_packet, 0, handle);
    sco_packet[2] = (uint8_t) sco_payload_length;
    hci_send_sco_packet_buffer(sco_packet_length);

    hci_request_sco_can_send_now_event_for_con_handle(handle);
}

// ---------------------------------------------------------------------------------------------
// Connection management

static void connect_headset(void);
static void connect_audio(void);
static void trigger_shutdown(void);

static void action_timer_handler(btstack_timer_source_t * timer){
    (void) timer;
    pending_action_t action = pending_action;
    pending_action = ACTION_NONE;
    switch (action){
        case ACTION_CONNECT_SLC:   connect_headset(); break;
        case ACTION_CONNECT_AUDIO: connect_audio();   break;
        default: break;
    }
}

static void schedule_action(pending_action_t action, uint32_t delay_ms){
    if (shutdown_triggered) return;
    btstack_run_loop_remove_timer(&action_timer);
    pending_action = action;
    btstack_run_loop_set_timer_handler(&action_timer, &action_timer_handler);
    btstack_run_loop_set_timer(&action_timer, delay_ms);
    btstack_run_loop_add_timer(&action_timer);
}

static void connect_headset(void){
    if (shutdown_triggered || acl_handle != HCI_CON_HANDLE_INVALID || slc_connecting) return;

    connect_attempts++;
    if (connect_attempts == 1){
        printf("Connecting to headset %s... (turn it on if it is off; retrying every %d s)\n",
               bd_addr_to_str(headset_addr), config.reconnect_interval_s);
    }
    uint8_t status = hfp_ag_establish_service_level_connection(headset_addr);
    if (status == ERROR_CODE_SUCCESS){
        slc_connecting = true;
    } else {
        schedule_action(ACTION_CONNECT_SLC, config.reconnect_interval_s * 1000);
    }
}

static void connect_audio(void){
    if (shutdown_triggered || acl_handle == HCI_CON_HANDLE_INVALID || sco_handle != HCI_CON_HANDLE_INVALID) return;
    uint8_t status = hfp_ag_establish_audio_connection(acl_handle);
    if (status != ERROR_CODE_SUCCESS){
        schedule_action(ACTION_CONNECT_AUDIO, AUDIO_RETRY_DELAY_MS);
    }
}

static void stop_audio(void){
    audio_io_stop();
    if (sco_handle != HCI_CON_HANDLE_INVALID){
        sco_handle = HCI_CON_HANDLE_INVALID;
    }
}

static void stats_timer_handler(btstack_timer_source_t * timer){
    audio_stats_t stats;
    audio_io_get_stats(&stats);
    if (sco_handle != HCI_CON_HANDLE_INVALID && memcmp(&stats, &last_stats, sizeof(stats)) != 0){
        printf("Audio buffers - mic: %u underruns, %u overruns, %u drift fixes | headset ear: %u underruns, %u overruns, %u drift fixes\n",
               (unsigned) stats.mic_underruns, (unsigned) stats.mic_overruns, (unsigned) stats.mic_adjustments,
               (unsigned) stats.speaker_underruns, (unsigned) stats.speaker_overruns, (unsigned) stats.speaker_adjustments);
    }
    last_stats = stats;
    btstack_run_loop_set_timer(timer, STATS_INTERVAL_MS);
    btstack_run_loop_add_timer(timer);
}

static void handle_hfp_event(uint8_t * event){
    bd_addr_t addr;
    uint8_t status;

    switch (hci_event_hfp_meta_get_subevent_code(event)){
        case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED:
            slc_connecting = false;
            status = hfp_subevent_service_level_connection_established_get_status(event);
            if (status != ERROR_CODE_SUCCESS){
                if (status == ERROR_CODE_AUTHENTICATION_FAILURE || status == ERROR_CODE_PIN_OR_KEY_MISSING){
                    printf("Headset rejected the stored pairing. Put the headset in pairing mode to pair again.\n");
                    gap_drop_link_key_for_bd_addr(headset_addr);
                }
                schedule_action(ACTION_CONNECT_SLC, config.reconnect_interval_s * 1000);
                break;
            }
            hfp_subevent_service_level_connection_established_get_bd_addr(event, addr);
            if (bd_addr_cmp(addr, headset_addr) != 0){
                printf("Refusing unexpected device %s\n", bd_addr_to_str(addr));
                hfp_ag_release_service_level_connection(hfp_subevent_service_level_connection_established_get_acl_handle(event));
                break;
            }
            acl_handle = hfp_subevent_service_level_connection_established_get_acl_handle(event);
            connect_attempts = 0;
            printf("Headset connected.\n");
            schedule_action(ACTION_CONNECT_AUDIO, AUDIO_CONNECT_DELAY_MS);
            break;

        case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED:
            if (acl_handle == HCI_CON_HANDLE_INVALID) break;
            printf("Headset disconnected.\n");
            acl_handle = HCI_CON_HANDLE_INVALID;
            stop_audio();
            schedule_action(ACTION_CONNECT_SLC, config.reconnect_interval_s * 1000);
            break;

        case HFP_SUBEVENT_AUDIO_CONNECTION_ESTABLISHED: {
            status = hfp_subevent_audio_connection_established_get_status(event);
            if (status != ERROR_CODE_SUCCESS){
                printf("Audio link failed (status 0x%02x), retrying...\n", status);
                schedule_action(ACTION_CONNECT_AUDIO, AUDIO_RETRY_DELAY_MS);
                break;
            }
            sco_handle = hfp_subevent_audio_connection_established_get_sco_handle(event);
            uint8_t codec = hfp_subevent_audio_connection_established_get_negotiated_codec(event);
            int sample_rate = codec_start(codec);
            printf("Audio link open (%s, %d kHz):\n", codec == HFP_CODEC_MSBC ? "mSBC wideband" : "CVSD narrowband", sample_rate / 1000);
            audio_io_start(&config, sample_rate);
            hci_request_sco_can_send_now_event_for_con_handle(sco_handle);
            break;
        }

        case HFP_SUBEVENT_AUDIO_CONNECTION_RELEASED:
            if (sco_handle == HCI_CON_HANDLE_INVALID) break;
            printf("Audio link closed.\n");
            stop_audio();
            if (acl_handle != HCI_CON_HANDLE_INVALID){
                schedule_action(ACTION_CONNECT_AUDIO, AUDIO_RETRY_DELAY_MS);
            }
            break;

        case HFP_SUBEVENT_SPEAKER_VOLUME:
            printf("Headset volume: %u/15\n", hfp_subevent_speaker_volume_get_gain(event));
            break;

        default:
            break;
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    (void) channel;
    bd_addr_t addr;

    if (packet_type == HCI_SCO_DATA_PACKET){
        if (READ_SCO_CONNECTION_HANDLE(packet) == sco_handle){
            codec_receive(packet, size);
        }
        return;
    }
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)){
        case BTSTACK_EVENT_STATE:
            switch (btstack_event_state_get_state(packet)){
                case HCI_STATE_WORKING: {
                    bd_addr_t local_addr;
                    gap_local_bd_addr(local_addr);
                    snprintf(tlv_db_path, sizeof(tlv_db_path), "%sbtstack_%s.tlv", exe_dir,
                             bd_addr_to_str_with_delimiter(local_addr, '-'));
                    tlv_impl = btstack_tlv_windows_init_instance(&tlv_context, tlv_db_path);
                    btstack_tlv_set_instance(tlv_impl, &tlv_context);
                    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, &tlv_context));
                    le_device_db_tlv_configure(tlv_impl, &tlv_context);
                    printf("Bluetooth dongle ready (%s).\n", bd_addr_to_str(local_addr));
                    if (scan_mode){
                        scan_start(&trigger_shutdown);
                    } else {
                        connect_headset();
                    }
                    break;
                }
                case HCI_STATE_OFF:
                    btstack_tlv_windows_deinit(&tlv_context);
                    if (!shutdown_triggered) break;
                    audio_io_terminate();
                    btstack_stdin_reset();
                    printf("Bridge stopped.\n");
                    exit(0);
                    break;
                default:
                    break;
            }
            break;

        case BTSTACK_EVENT_POWERON_FAILED:
            printf("Could not open the Bluetooth USB dongle. Check that it is plugged in, uses the WinUSB driver "
                   "(see README), and is not in use by another program.\n");
            audio_io_terminate();
            exit(2);
            break;

        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            if (bd_addr_cmp(addr, headset_addr) == 0){
                printf("Pairing with headset...\n");
                gap_ssp_confirmation_response(addr);
            } else {
                printf("Refusing pairing request from %s\n", bd_addr_to_str(addr));
                gap_ssp_confirmation_negative(addr);
            }
            break;

        case HCI_EVENT_SCO_CAN_SEND_NOW:
            send_sco_packet(hci_event_sco_can_send_now_get_handle(packet));
            break;

        case HCI_EVENT_HFP_META:
            handle_hfp_event(packet);
            break;

        default:
            break;
    }
}

static void trigger_shutdown(void){
    if (shutdown_triggered) return;
    printf("Shutting down...\n");
    shutdown_triggered = true;
    btstack_run_loop_remove_timer(&action_timer);
    stop_audio();
    hci_power_control(HCI_POWER_OFF);
}

static void setup_hfp_ag(void){
    hci_set_sco_voice_setting(0x60);    // linear, 16-bit, CVSD; mSBC uses transparent mode automatically
    hci_set_master_slave_policy(0);
    gap_set_local_name("Headset Bridge 00:00:00:00:00:00");
    gap_discoverable_control(0);

    l2cap_init();
    sm_init();

    uint16_t supported_features =
        (1 << HFP_AGSF_ESCO_S4)                     |
        (1 << HFP_AGSF_HF_INDICATORS)               |
        (1 << HFP_AGSF_CODEC_NEGOTIATION)           |
        (1 << HFP_AGSF_EXTENDED_ERROR_RESULT_CODES) |
        (1 << HFP_AGSF_ENHANCED_CALL_STATUS)        |
        (1 << HFP_AGSF_EC_NR_FUNCTION);

    rfcomm_init();
    hfp_ag_init(RFCOMM_CHANNEL_NR);
    hfp_ag_init_supported_features(supported_features);
    hfp_ag_init_codecs(sizeof(codecs), codecs);
    hfp_ag_init_ag_indicators(sizeof(ag_indicators) / sizeof(ag_indicators[0]), ag_indicators);
    hfp_ag_init_hf_indicators(sizeof(hf_indicators) / sizeof(hf_indicators[0]), hf_indicators);
    hfp_ag_init_call_hold_services(sizeof(call_hold_services) / sizeof(call_hold_services[0]), call_hold_services);

    sdp_init();
    memset(hfp_service_buffer, 0, sizeof(hfp_service_buffer));
    hfp_ag_create_sdp_record_with_codecs(hfp_service_buffer, sdp_create_service_record_handle(), RFCOMM_CHANNEL_NR,
                                         hfp_ag_service_name, 0, supported_features, sizeof(codecs), codecs);
    sdp_register_service(hfp_service_buffer);

    hci_register_sco_packet_handler(&packet_handler);
    hfp_ag_register_packet_handler(&packet_handler);
}

static void stop_event_handler(btstack_data_source_t * ds, btstack_data_source_callback_type_t callback_type){
    (void) callback_type;
    btstack_run_loop_remove_data_source(ds);
    trigger_shutdown();
}

static bool setup_stop_event(const char * name){
    HANDLE event = OpenEventA(SYNCHRONIZE, FALSE, name);
    if (event == NULL){
        printf("Could not open stop event '%s' (error %lu).\n", name, GetLastError());
        return false;
    }
    stop_event_source.source.handle = event;
    btstack_run_loop_set_data_source_handler(&stop_event_source, &stop_event_handler);
    btstack_run_loop_enable_data_source_callbacks(&stop_event_source, DATA_SOURCE_CALLBACK_READ);
    btstack_run_loop_add_data_source(&stop_event_source);
    return true;
}

static void resolve_exe_dir(void){
    DWORD len = GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
    while (len > 0 && exe_dir[len - 1] != '\\' && exe_dir[len - 1] != '/') len--;
    exe_dir[len] = '\0';
}

static void print_usage(void){
    printf("Usage: headset_bridge.exe [--list] [--scan] [--log] [--config <path>]\n");
    printf("  --list        list audio devices and exit\n");
    printf("  --scan        search for nearby Bluetooth devices (to find the headset address) and exit\n");
    printf("  --log         write Bluetooth packet log to hci_dump.pklg\n");
    printf("  --config      use a different config file (default: bridge.ini next to the exe)\n");
    printf("  --stop-event  name of a Windows event that stops the bridge when signaled (used by the GUI)\n");
    printf("  --version     print the version and exit\n");
}

int main(int argc, const char * argv[]){
    setvbuf(stdout, NULL, _IONBF, 0);
    resolve_exe_dir();

    bool list_devices = false;
    bool packet_log   = false;
    const char * stop_event_name = NULL;
    char config_path[MAX_PATH + 32];
    snprintf(config_path, sizeof(config_path), "%sbridge.ini", exe_dir);

    for (int i = 1; i < argc; i++){
        if      (strcmp(argv[i], "--list") == 0) list_devices = true;
        else if (strcmp(argv[i], "--scan") == 0) scan_mode = true;
        else if (strcmp(argv[i], "--log") == 0)  packet_log = true;
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) snprintf(config_path, sizeof(config_path), "%s", argv[++i]);
        else if (strcmp(argv[i], "--stop-event") == 0 && i + 1 < argc) stop_event_name = argv[++i];
        else if (strcmp(argv[i], "--version") == 0){ printf("headset_bridge %s\n", BRIDGE_VERSION); return 0; }
        else { print_usage(); return 1; }
    }

    printf("Phone Link headset bridge %s\n", BRIDGE_VERSION);
    config_set_defaults(&config);
    if (!scan_mode && !config_load(&config, config_path)){
        printf("No config file at %s - using defaults.\n", config_path);
    }

    if (!scan_mode){
        if (!audio_io_init()) return 1;
        if (list_devices){
            audio_io_list_devices();
            audio_io_terminate();
            return 0;
        }

        if (config.headset_address[0] == '\0'){
            printf("No headset_address set in %s.\n", config_path);
            printf("Run \"headset_bridge.exe --scan\" with the headset in pairing mode to find its address.\n");
            audio_io_terminate();
            return 1;
        }
        if (sscanf_bd_addr(config.headset_address, headset_addr) == 0){
            printf("Invalid headset_address '%s' in config.\n", config.headset_address);
            audio_io_terminate();
            return 1;
        }
        config_print(&config);
    }

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_windows_get_instance());

    if (packet_log){
        char log_path[MAX_PATH + 32];
        snprintf(log_path, sizeof(log_path), "%shci_dump.pklg", exe_dir);
        hci_dump_windows_fs_open(log_path, HCI_DUMP_PACKETLOGGER);
        hci_dump_init(hci_dump_windows_fs_get_instance());
        printf("Packet log: %s\n", log_path);
    }

    hci_init(hci_transport_usb_instance(), NULL);

    if (stop_event_name != NULL){
        if (!setup_stop_event(stop_event_name)) return 1;
    } else {
        btstack_stdin_windows_init();
        btstack_stdin_window_register_ctrl_c_callback(&trigger_shutdown);
    }

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    if (scan_mode){
        hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    } else {
        setup_hfp_ag();
        btstack_run_loop_set_timer_handler(&stats_timer, &stats_timer_handler);
        btstack_run_loop_set_timer(&stats_timer, STATS_INTERVAL_MS);
        btstack_run_loop_add_timer(&stats_timer);
    }

    if (stop_event_name == NULL) printf("Press Ctrl+C to stop.\n");
    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
    return 0;
}
