#define BTSTACK_FILE__ "scan.c"

#include "scan.h"

#include <stdio.h>
#include <string.h>

#include "btstack.h"

#define MAX_DEVICES                 24
#define MAX_NAME_LEN                64
#define INQUIRY_DURATION_1280MS     8

#define COD_MAJOR_COMPUTER          0x01
#define COD_MAJOR_PHONE             0x02
#define COD_MAJOR_AUDIO_VIDEO       0x04

typedef enum { NAME_NEEDED, NAME_REQUESTED, NAME_DONE } name_state_t;

typedef struct {
    bd_addr_t    addr;
    uint32_t     class_of_device;
    uint8_t      page_scan_repetition_mode;
    uint16_t     clock_offset;
    name_state_t name_state;
    char         name[MAX_NAME_LEN];
} scan_device_t;

static scan_device_t devices[MAX_DEVICES];
static int           num_devices;
static void       (* done_callback)(void);
static btstack_packet_callback_registration_t scan_callback_registration;

static const char * device_kind(uint32_t class_of_device){
    switch ((class_of_device >> 8) & 0x1f){
        case COD_MAJOR_AUDIO_VIDEO: return "audio";
        case COD_MAJOR_PHONE:       return "phone";
        case COD_MAJOR_COMPUTER:    return "computer";
        default:                    return "other";
    }
}

static scan_device_t * find_device(const bd_addr_t addr){
    for (int i = 0; i < num_devices; i++){
        if (bd_addr_cmp(devices[i].addr, addr) == 0) return &devices[i];
    }
    return NULL;
}

static void print_device(const scan_device_t * device){
    printf("Found device %s [%s] %s\n", bd_addr_to_str(device->addr), device_kind(device->class_of_device),
           device->name[0] ? device->name : "(no name)");
}

static void finish(void){
    printf("Scan finished: %d device(s) found.\n", num_devices);
    hci_remove_event_handler(&scan_callback_registration);
    if (done_callback) done_callback();
}

static void request_next_name(void){
    for (int i = 0; i < num_devices; i++){
        if (devices[i].name_state != NAME_NEEDED) continue;
        devices[i].name_state = NAME_REQUESTED;
        gap_remote_name_request(devices[i].addr, devices[i].page_scan_repetition_mode, devices[i].clock_offset | 0x8000);
        return;
    }
    finish();
}

static void scan_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    (void) channel; (void) size;
    if (packet_type != HCI_EVENT_PACKET) return;

    bd_addr_t addr;
    scan_device_t * device;

    switch (hci_event_packet_get_type(packet)){
        case GAP_EVENT_INQUIRY_RESULT:
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            if (find_device(addr) != NULL || num_devices >= MAX_DEVICES) break;
            device = &devices[num_devices++];
            memset(device, 0, sizeof(*device));
            bd_addr_copy(device->addr, addr);
            device->class_of_device           = gap_event_inquiry_result_get_class_of_device(packet);
            device->page_scan_repetition_mode = gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
            device->clock_offset              = gap_event_inquiry_result_get_clock_offset(packet);
            device->name_state                = NAME_NEEDED;
            if (gap_event_inquiry_result_get_name_available(packet)){
                int len = btstack_min(gap_event_inquiry_result_get_name_len(packet), MAX_NAME_LEN - 1);
                memcpy(device->name, gap_event_inquiry_result_get_name(packet), len);
                device->name[len] = '\0';
                device->name_state = NAME_DONE;
                print_device(device);
            }
            break;

        case GAP_EVENT_INQUIRY_COMPLETE:
            request_next_name();
            break;

        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
            hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
            device = find_device(addr);
            if (device == NULL || device->name_state != NAME_REQUESTED) break;
            if (hci_event_remote_name_request_complete_get_status(packet) == ERROR_CODE_SUCCESS){
                // the name field is 248 bytes and only NUL-terminated when shorter than that
                const char * name = hci_event_remote_name_request_complete_get_remote_name(packet);
                int len = btstack_min((int) strnlen(name, 248), MAX_NAME_LEN - 1);
                memcpy(device->name, name, len);
                device->name[len] = '\0';
            }
            device->name_state = NAME_DONE;
            print_device(device);
            request_next_name();
            break;

        default:
            break;
    }
}

void scan_start(void (*on_done)(void)){
    done_callback = on_done;
    num_devices   = 0;
    scan_callback_registration.callback = &scan_packet_handler;
    hci_add_event_handler(&scan_callback_registration);

    printf("Scanning for Bluetooth devices for about 10 seconds...\n");
    printf("Put the headset in pairing mode so it can be found.\n");
    gap_inquiry_start(INQUIRY_DURATION_1280MS);
}
