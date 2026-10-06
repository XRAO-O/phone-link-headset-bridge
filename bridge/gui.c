// Windows GUI for the headset bridge. Runs headset_bridge.exe as a hidden child process, shows its
// status and log, edits bridge.ini and lives in the notification area while the bridge is running.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "config.h"
#include "gui_resource.h"
#include "version.h"

#define APP_TITLE          L"Phone Link Headset Bridge"
#define WINDOW_CLASS       L"PhoneLinkHeadsetBridgeGui"
#define INSTANCE_MUTEX     L"Local\\PhoneLinkHeadsetBridgeGui"
#define RUN_KEY            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE          L"PhoneLinkHeadsetBridge"
#define BRIDGE_EXE         L"headset_bridge.exe"
#define AUTOSTART_ARG      L"--autostart"

#define WM_APP_CHILD_LINE  (WM_APP + 1)
#define WM_APP_CHILD_EXIT  (WM_APP + 2)
#define WM_APP_TRAY        (WM_APP + 3)
#define WM_APP_SHOW        (WM_APP + 4)

#define TIMER_FORCE_STOP   1    // + child kind
#define TIMER_RETRY        10
#define FORCE_STOP_MS      5000
#define RETRY_DELAY_MS     10000
#define QUIT_WAIT_MS       4000

#define EXIT_CONFIG_ERROR  1
#define MAX_LOG_CHARS      200000
#define MAX_SCAN_RESULTS   24
#define MAX_AUDIO_DEVICES  64
#define TRAY_ID            1

enum {
    ID_START = 100, ID_ADDRESS, ID_FIND, ID_MIC_DEVICE, ID_SPEAKER_DEVICE, ID_REFRESH,
    ID_MIC_GAIN, ID_SPEAKER_GAIN, ID_LATENCY, ID_RECONNECT, ID_AUTOSTART, ID_SAVE, ID_LOG,
    ID_TRAY_OPEN, ID_TRAY_TOGGLE, ID_TRAY_EXIT,
    ID_SCAN_RESULT_FIRST = 1000
};

// --- child processes ---

typedef enum { CHILD_BRIDGE, CHILD_LIST, CHILD_SCAN, CHILD_COUNT } child_kind_t;

typedef struct {
    child_kind_t kind;
    HANDLE       process;
    HANDLE       stdout_read;
    HANDLE       stop_event;
    bool         running;
    bool         stopping;
} child_t;

static child_t children[CHILD_COUNT];
static HANDLE  job;

// --- status ---

typedef enum { LIGHT_OFF, LIGHT_BUSY, LIGHT_OK, LIGHT_ERROR } light_t;
static const COLORREF light_colors[] = { RGB(154, 160, 166), RGB(242, 163, 58), RGB(52, 168, 83), RGB(234, 67, 53) };

enum { ROW_DONGLE, ROW_HEADSET, ROW_AUDIO, ROW_COUNT };
typedef struct {
    const wchar_t * label;
    light_t         light;
    wchar_t         text[160];
} status_row_t;
static status_row_t status_rows[ROW_COUNT] = {
    { L"Dongle",  LIGHT_OFF, L"-" },
    { L"Headset", LIGHT_OFF, L"-" },
    { L"Audio",   LIGHT_OFF, L"-" },
};

// --- app state ---

static HINSTANCE instance;
static HWND      main_window;
static UINT      dpi = 96;
static UINT      taskbar_created_msg;
static HFONT     ui_font, bold_font, mono_font;
static HICON     app_icon_large, app_icon_small;
static HICON     tray_icons[4];
static bool      tray_added;
static bool      tray_hint_shown;
static RECT      status_rect;

static wchar_t   exe_dir[MAX_PATH];
static wchar_t   gui_exe_path[MAX_PATH];
static wchar_t   bridge_exe_path[MAX_PATH];
static char      config_path[MAX_PATH * 2];

static bridge_config_t settings;
static bool      settings_dirty;
static bool      loading_controls;
static bool      bridge_wanted;
static bool      restart_pending;
static bool      scan_after_stop;
static bool      headset_connected;
static bool      quitting;

typedef struct { wchar_t addr[18]; wchar_t kind[16]; wchar_t name[64]; } scan_result_t;
static scan_result_t scan_results[MAX_SCAN_RESULTS];
static int           num_scan_results;

typedef struct { wchar_t name[CONFIG_STRING_LEN]; bool wasapi; bool is_input; } audio_device_t;
static audio_device_t audio_devices[MAX_AUDIO_DEVICES];
static int            num_audio_devices;
static bool           list_section_input;

static HWND start_button, address_edit, find_button, mic_combo, speaker_combo, refresh_button;
static HWND mic_gain_edit, speaker_gain_edit, latency_edit, reconnect_edit, autostart_check, save_button;
static HWND settings_group, log_label, log_edit;
static HWND labels[8];

static int scale(int value){
    return MulDiv(value, (int) dpi, 96);
}

static bool starts_with(const wchar_t * s, const wchar_t * prefix){
    return wcsncmp(s, prefix, wcslen(prefix)) == 0;
}

// ---------------------------------------------------------------------------------------------
// Text helpers

static wchar_t * bytes_to_wide_alloc(const char * text){
    UINT code_page = CP_UTF8;
    DWORD flags    = MB_ERR_INVALID_CHARS;
    int len = MultiByteToWideChar(code_page, flags, text, -1, NULL, 0);
    if (len == 0){
        code_page = CP_ACP;
        flags     = 0;
        len = MultiByteToWideChar(code_page, flags, text, -1, NULL, 0);
    }
    wchar_t * wide = malloc((size_t) (len > 0 ? len : 1) * sizeof(wchar_t));
    if (wide == NULL) return NULL;
    wide[0] = L'\0';
    if (len > 0) MultiByteToWideChar(code_page, flags, text, -1, wide, len);
    return wide;
}

static void utf8_to_wide(const char * text, wchar_t * out, int out_len){
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, out, out_len) == 0) out[0] = L'\0';
}

static bool wide_to_utf8(const wchar_t * text, char * out, int out_len){
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, out, out_len, NULL, NULL) == 0){
        out[0] = '\0';
        return false;
    }
    return true;
}

static void trim_wide(wchar_t * text){
    wchar_t * start = text;
    while (*start == L' ' || *start == L'\t') start++;
    size_t len = wcslen(start);
    while (len > 0 && (start[len - 1] == L' ' || start[len - 1] == L'\t')) len--;
    memmove(text, start, len * sizeof(wchar_t));
    text[len] = L'\0';
}

static void get_control_text(HWND control, wchar_t * out, int out_len){
    GetWindowTextW(control, out, out_len);
    trim_wide(out);
}

// Copies the text between the first '(' and the following ')'.
static void text_in_parens(const wchar_t * text, wchar_t * out, size_t out_len){
    out[0] = L'\0';
    const wchar_t * open = wcschr(text, L'(');
    if (open == NULL) return;
    const wchar_t * close = wcschr(open, L')');
    size_t len = close ? (size_t) (close - open - 1) : wcslen(open + 1);
    if (len >= out_len) len = out_len - 1;
    wmemcpy(out, open + 1, len);
    out[len] = L'\0';
}

// ---------------------------------------------------------------------------------------------
// Log

static void log_line(const wchar_t * format, ...){
    wchar_t text[1024];
    va_list args;
    va_start(args, format);
    vswprintf(text, sizeof(text) / sizeof(text[0]), format, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t line[1100];
    swprintf(line, sizeof(line) / sizeof(line[0]), L"[%02u:%02u:%02u] %ls\r\n", now.wHour, now.wMinute, now.wSecond, text);

    int len = GetWindowTextLengthW(log_edit);
    if (len > MAX_LOG_CHARS){
        SendMessageW(log_edit, EM_SETSEL, 0, len / 2);
        SendMessageW(log_edit, EM_REPLACESEL, FALSE, (LPARAM) L"[...]\r\n");
        len = GetWindowTextLengthW(log_edit);
    }
    SendMessageW(log_edit, EM_SETSEL, len, len);
    SendMessageW(log_edit, EM_REPLACESEL, FALSE, (LPARAM) line);
}

// ---------------------------------------------------------------------------------------------
// Icons

// Draws a headphone glyph in the given colour. Rendered at 4x and box-filtered for anti-aliasing,
// since plain GDI shapes are not anti-aliased.
static HICON create_headset_icon(int size, COLORREF color){
    const int ss  = 4;
    const int big = size * ss;

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = big;
    bi.bmiHeader.biHeight      = -big;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(NULL);
    uint32_t * big_bits = NULL;
    HBITMAP big_bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **) &big_bits, NULL, 0);
    if (big_bitmap == NULL){
        DeleteDC(dc);
        return NULL;
    }
    HGDIOBJ old_bitmap = SelectObject(dc, big_bitmap);
    memset(big_bits, 0, (size_t) big * big * 4);

    LOGBRUSH pen_brush = { BS_SOLID, RGB(255, 255, 255), 0 };
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_FLAT, big * 12 / 100, &pen_brush, 0, NULL);
    HGDIOBJ old_pen   = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int band_left = big * 17 / 100, band_top = big * 10 / 100, band_right = big * 83 / 100, band_bottom = big * 80 / 100;
    int band_mid  = (band_top + band_bottom) / 2;
    Arc(dc, band_left, band_top, band_right, band_bottom, band_right, band_mid, band_left, band_mid);

    SelectObject(dc, GetStockObject(NULL_PEN));
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    int cup_top = big * 44 / 100, cup_bottom = big * 94 / 100, radius = big * 22 / 100;
    RoundRect(dc, big * 6 / 100, cup_top, big * 34 / 100, cup_bottom, radius, radius);
    RoundRect(dc, big * 66 / 100, cup_top, big * 94 / 100, cup_bottom, radius, radius);
    GdiFlush();

    bi.bmiHeader.biWidth  = size;
    bi.bmiHeader.biHeight = -size;
    uint32_t * bits = NULL;
    HBITMAP color_bitmap = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **) &bits, NULL, 0);
    if (color_bitmap != NULL){
        uint32_t rgb = ((uint32_t) GetRValue(color) << 16) | ((uint32_t) GetGValue(color) << 8) | GetBValue(color);
        for (int y = 0; y < size; y++){
            for (int x = 0; x < size; x++){
                uint32_t sum = 0;
                for (int sy = 0; sy < ss; sy++){
                    for (int sx = 0; sx < ss; sx++){
                        sum += big_bits[(y * ss + sy) * big + x * ss + sx] & 0xff;
                    }
                }
                uint32_t alpha = sum / (ss * ss);
                bits[y * size + x] = (alpha << 24) | (alpha ? rgb : 0);
            }
        }
    }

    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_bitmap);
    DeleteObject(pen);
    DeleteObject(big_bitmap);
    DeleteDC(dc);
    if (color_bitmap == NULL) return NULL;

    size_t mask_bytes = (size_t) ((size + 15) / 16 * 2) * size;
    void * mask_bits = calloc(1, mask_bytes);
    HBITMAP mask_bitmap = CreateBitmap(size, size, 1, 1, mask_bits);
    free(mask_bits);

    ICONINFO icon_info = { TRUE, 0, 0, mask_bitmap, color_bitmap };
    HICON icon = CreateIconIndirect(&icon_info);
    DeleteObject(mask_bitmap);
    DeleteObject(color_bitmap);
    return icon;
}

// ---------------------------------------------------------------------------------------------
// Tray icon

static light_t overall_light(void){
    if (!bridge_wanted) return LIGHT_OFF;
    for (int i = 0; i < ROW_COUNT; i++){
        if (status_rows[i].light == LIGHT_ERROR) return LIGHT_ERROR;
    }
    return status_rows[ROW_AUDIO].light == LIGHT_OK ? LIGHT_OK : LIGHT_BUSY;
}

static void fill_tray_data(NOTIFYICONDATAW * data){
    memset(data, 0, sizeof(*data));
    data->cbSize = sizeof(*data);
    data->hWnd   = main_window;
    data->uID    = TRAY_ID;
}

static void update_tray(void){
    NOTIFYICONDATAW data;
    fill_tray_data(&data);
    data.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = WM_APP_TRAY;
    data.hIcon            = tray_icons[overall_light()];

    const wchar_t * state;
    if (!bridge_wanted)                                  state = L"Stopped";
    else if (status_rows[ROW_AUDIO].light == LIGHT_OK)   state = status_rows[ROW_AUDIO].text;
    else if (overall_light() == LIGHT_ERROR)             state = L"Problem - open for details";
    else                                                 state = status_rows[ROW_HEADSET].text[0] ? status_rows[ROW_HEADSET].text : L"Starting...";
    swprintf(data.szTip, sizeof(data.szTip) / sizeof(data.szTip[0]), L"Headset bridge: %ls", state);

    if (!tray_added){
        tray_added = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
}

static void remove_tray(void){
    if (!tray_added) return;
    NOTIFYICONDATAW data;
    fill_tray_data(&data);
    Shell_NotifyIconW(NIM_DELETE, &data);
    tray_added = false;
}

static void show_tray_balloon(const wchar_t * title, const wchar_t * text){
    if (!tray_added) return;
    NOTIFYICONDATAW data;
    fill_tray_data(&data);
    data.uFlags      = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    wcsncpy(data.szInfoTitle, title, sizeof(data.szInfoTitle) / sizeof(data.szInfoTitle[0]) - 1);
    wcsncpy(data.szInfo, text, sizeof(data.szInfo) / sizeof(data.szInfo[0]) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

// ---------------------------------------------------------------------------------------------
// Status and controls

static void set_status(int row, light_t light, const wchar_t * format, ...){
    status_row_t * status = &status_rows[row];
    status->light = light;
    va_list args;
    va_start(args, format);
    vswprintf(status->text, sizeof(status->text) / sizeof(status->text[0]), format, args);
    va_end(args);
    InvalidateRect(main_window, &status_rect, TRUE);
    update_tray();
}

static void update_controls(void){
    bool scanning = children[CHILD_SCAN].running;
    SetWindowTextW(start_button, bridge_wanted ? L"Stop bridge" : L"Start bridge");
    EnableWindow(start_button, !scanning);
    SetWindowTextW(find_button, scanning ? L"Cancel scan" : L"Find headset...");
    EnableWindow(refresh_button, !children[CHILD_LIST].running);
    EnableWindow(save_button, settings_dirty);
    update_tray();
}

static void mark_dirty(void){
    if (loading_controls || settings_dirty) return;
    settings_dirty = true;
    update_controls();
}

// WM_SETTEXT on a drop-down combo box replaces the text with the first list item it prefixes,
// so write to the inner edit control to show exactly what is in bridge.ini.
static void set_combo_text(HWND combo, const wchar_t * text){
    COMBOBOXINFO info;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    SendMessageW(combo, CB_SETCURSEL, (WPARAM) -1, 0);
    if (GetComboBoxInfo(combo, &info) && info.hwndItem != NULL){
        SetWindowTextW(info.hwndItem, text);
    } else {
        SetWindowTextW(combo, text);
    }
}

static void set_number_text(HWND control, const wchar_t * format, double value){
    wchar_t text[32];
    swprintf(text, sizeof(text) / sizeof(text[0]), format, value);
    SetWindowTextW(control, text);
}

static void settings_to_controls(const bridge_config_t * config){
    wchar_t text[CONFIG_STRING_LEN];
    loading_controls = true;
    utf8_to_wide(config->headset_address, text, CONFIG_STRING_LEN);
    SetWindowTextW(address_edit, text);
    utf8_to_wide(config->mic_output_device, text, CONFIG_STRING_LEN);
    set_combo_text(mic_combo, text);
    utf8_to_wide(config->speaker_input_device, text, CONFIG_STRING_LEN);
    set_combo_text(speaker_combo, text);
    set_number_text(mic_gain_edit, L"%g", config->mic_gain_db);
    set_number_text(speaker_gain_edit, L"%g", config->speaker_gain_db);
    set_number_text(latency_edit, L"%.0f", config->latency_ms);
    set_number_text(reconnect_edit, L"%.0f", config->reconnect_interval_s);
    loading_controls = false;
    settings_dirty = false;
}

static bool validation_error(HWND control, const wchar_t * message){
    MessageBoxW(main_window, message, APP_TITLE, MB_OK | MB_ICONWARNING);
    SetFocus(control);
    SendMessageW(control, EM_SETSEL, 0, -1);
    return false;
}

// Accepts 12 hex digits with optional ':' or '-' separators and normalizes to AA:BB:CC:DD:EE:FF.
static bool normalize_address(const wchar_t * text, char * out){
    int digits = 0;
    char hex[12];
    for (const wchar_t * p = text; *p; p++){
        if (*p == L':' || *p == L'-' || *p == L' ') continue;
        if (!iswxdigit(*p) || digits == 12) return false;
        hex[digits++] = (char) towupper(*p);
    }
    if (digits != 12) return false;
    snprintf(out, 18, "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c", hex[0], hex[1], hex[2], hex[3], hex[4], hex[5],
             hex[6], hex[7], hex[8], hex[9], hex[10], hex[11]);
    return true;
}

static bool parse_number(HWND control, double min, double max, double * out){
    wchar_t text[32];
    get_control_text(control, text, 32);
    wchar_t * end = NULL;
    double value = wcstod(text, &end);
    if (text[0] == L'\0' || end == NULL || *end != L'\0' || value < min || value > max) return false;
    *out = value;
    return true;
}

static bool controls_to_settings(bridge_config_t * config){
    wchar_t text[CONFIG_STRING_LEN];
    double value;

    get_control_text(address_edit, text, CONFIG_STRING_LEN);
    if (text[0] == L'\0'){
        config->headset_address[0] = '\0';
    } else if (!normalize_address(text, config->headset_address)){
        return validation_error(address_edit, L"The headset address should look like 00:1A:2B:3C:4D:5E.\n\n"
                                              L"Use \"Find headset...\" to look it up.");
    }

    get_control_text(mic_combo, text, CONFIG_STRING_LEN);
    if (text[0] == L'\0' || !wide_to_utf8(text, config->mic_output_device, CONFIG_STRING_LEN)){
        return validation_error(mic_combo, L"Choose the playback device the headset microphone should play into.");
    }
    get_control_text(speaker_combo, text, CONFIG_STRING_LEN);
    if (text[0] == L'\0' || !wide_to_utf8(text, config->speaker_input_device, CONFIG_STRING_LEN)){
        return validation_error(speaker_combo, L"Choose the recording device the headset should listen to.");
    }

    if (!parse_number(mic_gain_edit, -30, 30, &value)) return validation_error(mic_gain_edit, L"Mic gain must be a number between -30 and 30 dB.");
    config->mic_gain_db = (float) value;
    if (!parse_number(speaker_gain_edit, -30, 30, &value)) return validation_error(speaker_gain_edit, L"Headset gain must be a number between -30 and 30 dB.");
    config->speaker_gain_db = (float) value;
    if (!parse_number(latency_edit, 20, 500, &value)) return validation_error(latency_edit, L"Buffer must be between 20 and 500 ms.");
    config->latency_ms = (int) value;
    if (!parse_number(reconnect_edit, 2, 300, &value)) return validation_error(reconnect_edit, L"Reconnect interval must be between 2 and 300 seconds.");
    config->reconnect_interval_s = (int) value;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Child process management

static void post_child_line(child_kind_t kind, const char * line){
    wchar_t * wide = bytes_to_wide_alloc(line);
    if (wide == NULL) return;
    if (!PostMessageW(main_window, WM_APP_CHILD_LINE, (WPARAM) kind, (LPARAM) wide)) free(wide);
}

static DWORD WINAPI child_reader_thread(void * param){
    child_t * child = param;
    char  buffer[512];
    char  line[1024];
    size_t len = 0;
    DWORD bytes;

    while (ReadFile(child->stdout_read, buffer, sizeof(buffer), &bytes, NULL) && bytes > 0){
        for (DWORD i = 0; i < bytes; i++){
            char c = buffer[i];
            if (c == '\r') continue;
            if (c == '\n' || len == sizeof(line) - 1){
                line[len] = '\0';
                post_child_line(child->kind, line);
                len = 0;
                if (c == '\n') continue;
            }
            line[len++] = c;
        }
    }
    if (len > 0){
        line[len] = '\0';
        post_child_line(child->kind, line);
    }

    WaitForSingleObject(child->process, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(child->process, &exit_code);
    PostMessageW(main_window, WM_APP_CHILD_EXIT, (WPARAM) child->kind, (LPARAM) exit_code);
    return 0;
}

static bool start_child(child_kind_t kind, const wchar_t * args){
    static unsigned counter;
    child_t * child = &children[kind];
    if (child->running) return false;

    if (GetFileAttributesW(bridge_exe_path) == INVALID_FILE_ATTRIBUTES){
        log_line(L"Cannot find %ls next to this program.", BRIDGE_EXE);
        return false;
    }

    wchar_t event_name[96];
    swprintf(event_name, 96, L"Local\\HeadsetBridgeStop-%lu-%u", GetCurrentProcessId(), ++counter);
    HANDLE stop_event = CreateEventW(NULL, TRUE, FALSE, event_name);

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE read_end, write_end;
    if (stop_event == NULL || !CreatePipe(&read_end, &write_end, &sa, 0)){
        log_line(L"Could not start %ls (error %lu).", BRIDGE_EXE, GetLastError());
        if (stop_event) CloseHandle(stop_event);
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    wchar_t command_line[MAX_PATH + 256];
    swprintf(command_line, sizeof(command_line) / sizeof(command_line[0]), L"\"%ls\" --stop-event %ls %ls",
             bridge_exe_path, event_name, args);

    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError  = write_end;

    PROCESS_INFORMATION process;
    BOOL ok = CreateProcessW(bridge_exe_path, command_line, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             NULL, exe_dir, &startup, &process);
    CloseHandle(write_end);
    if (!ok){
        log_line(L"Could not start %ls (error %lu).", BRIDGE_EXE, GetLastError());
        CloseHandle(read_end);
        CloseHandle(stop_event);
        return false;
    }
    // the job kills the child if this program dies, so the dongle is never left claimed
    AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);

    child->kind        = kind;
    child->process     = process.hProcess;
    child->stdout_read = read_end;
    child->stop_event  = stop_event;
    child->running     = true;
    child->stopping    = false;

    HANDLE thread = CreateThread(NULL, 0, &child_reader_thread, child, 0, NULL);
    if (thread == NULL){
        TerminateProcess(child->process, 1);
        WaitForSingleObject(child->process, INFINITE);
        CloseHandle(child->process);
        CloseHandle(child->stdout_read);
        CloseHandle(child->stop_event);
        child->running = false;
        return false;
    }
    CloseHandle(thread);
    return true;
}

static void stop_child(child_kind_t kind){
    child_t * child = &children[kind];
    if (!child->running || child->stopping) return;
    child->stopping = true;
    SetEvent(child->stop_event);
    SetTimer(main_window, TIMER_FORCE_STOP + kind, FORCE_STOP_MS, NULL);
}

static void force_stop_child(child_kind_t kind){
    KillTimer(main_window, TIMER_FORCE_STOP + kind);
    child_t * child = &children[kind];
    if (!child->running) return;
    log_line(L"%ls did not stop in time and was terminated.", BRIDGE_EXE);
    TerminateProcess(child->process, 1);
}

// ---------------------------------------------------------------------------------------------
// Bridge

static void reset_bridge_status(void){
    headset_connected = false;
    set_status(ROW_DONGLE,  LIGHT_OFF, L"Bridge not running");
    set_status(ROW_HEADSET, LIGHT_OFF, settings.headset_address[0] ? L"-" : L"Not set up - use \"Find headset...\" below");
    set_status(ROW_AUDIO,   LIGHT_OFF, L"-");
}

static bool save_settings(void);

static void launch_bridge(void){
    KillTimer(main_window, TIMER_RETRY);
    bridge_wanted = true;
    if (!children[CHILD_BRIDGE].running){
        headset_connected = false;
        set_status(ROW_DONGLE,  LIGHT_BUSY, L"Opening...");
        set_status(ROW_HEADSET, LIGHT_OFF,  L"-");
        set_status(ROW_AUDIO,   LIGHT_OFF,  L"-");
        log_line(L"Starting bridge...");
        if (!start_child(CHILD_BRIDGE, L"")){
            bridge_wanted = false;
            set_status(ROW_DONGLE, LIGHT_ERROR, L"Could not start %ls", BRIDGE_EXE);
        }
    }
    update_controls();
}

static void start_bridge(void){
    if (settings_dirty && !save_settings()) return;
    if (settings.headset_address[0] == '\0'){
        MessageBoxW(main_window, L"Set the headset address first.\n\nPut the headset in pairing mode and click \"Find headset...\".",
                    APP_TITLE, MB_OK | MB_ICONINFORMATION);
        SetFocus(address_edit);
        return;
    }
    if (children[CHILD_SCAN].running) return;
    launch_bridge();
}

static void stop_bridge(void){
    bridge_wanted   = false;
    restart_pending = false;
    KillTimer(main_window, TIMER_RETRY);
    if (children[CHILD_BRIDGE].running){
        log_line(L"Stopping bridge...");
        stop_child(CHILD_BRIDGE);
    }
    update_controls();
}

static void handle_bridge_line(const wchar_t * line){
    wchar_t inner[128];
    const wchar_t * text = line;
    while (*text == L' ') text++;

    if (starts_with(text, L"Bluetooth dongle ready (")){
        text_in_parens(text, inner, 128);
        set_status(ROW_DONGLE, LIGHT_OK, L"Ready (%ls)", inner);
    } else if (starts_with(text, L"Could not open the Bluetooth USB dongle")){
        set_status(ROW_DONGLE, LIGHT_ERROR, L"Not found - is it plugged in and using the WinUSB driver?");
    } else if (starts_with(text, L"Connecting to headset")){
        set_status(ROW_HEADSET, LIGHT_BUSY, L"Connecting... (turn the headset on)");
    } else if (starts_with(text, L"Pairing with headset")){
        set_status(ROW_HEADSET, LIGHT_BUSY, L"Pairing...");
    } else if (starts_with(text, L"Headset rejected the stored pairing")){
        set_status(ROW_HEADSET, LIGHT_ERROR, L"Pairing rejected - put the headset in pairing mode");
    } else if (starts_with(text, L"Headset connected.")){
        headset_connected = true;
        set_status(ROW_HEADSET, LIGHT_OK, L"Connected");
        set_status(ROW_AUDIO, LIGHT_BUSY, L"Opening...");
    } else if (starts_with(text, L"Headset disconnected.")){
        headset_connected = false;
        set_status(ROW_HEADSET, LIGHT_BUSY, L"Disconnected - waiting for the headset");
        set_status(ROW_AUDIO, LIGHT_OFF, L"-");
    } else if (starts_with(text, L"Headset volume: ")){
        if (headset_connected) set_status(ROW_HEADSET, LIGHT_OK, L"Connected (volume %ls)", text + wcslen(L"Headset volume: "));
    } else if (starts_with(text, L"Audio link open (")){
        text_in_parens(text, inner, 128);
        set_status(ROW_AUDIO, LIGHT_OK, L"%ls", inner);
    } else if (starts_with(text, L"Audio link failed")){
        set_status(ROW_AUDIO, LIGHT_BUSY, L"Failed - retrying");
    } else if (starts_with(text, L"Audio link closed.")){
        set_status(ROW_AUDIO, headset_connected ? LIGHT_BUSY : LIGHT_OFF, headset_connected ? L"Closed - reopening" : L"-");
    } else if (starts_with(text, L"No playback device matching") || starts_with(text, L"No recording device matching") ||
               starts_with(text, L"Could not open '")){
        set_status(ROW_AUDIO, LIGHT_ERROR, L"Audio device problem - see log");
    } else if (starts_with(text, L"No headset_address set") || starts_with(text, L"Invalid headset_address")){
        set_status(ROW_HEADSET, LIGHT_ERROR, L"No valid headset address");
    }
}

static void start_scan(void);

static void on_bridge_exit(DWORD exit_code, bool requested){
    headset_connected = false;
    if (status_rows[ROW_DONGLE].light != LIGHT_ERROR)  set_status(ROW_DONGLE,  LIGHT_OFF, L"Bridge not running");
    if (status_rows[ROW_HEADSET].light != LIGHT_ERROR) set_status(ROW_HEADSET, LIGHT_OFF, L"-");
    if (status_rows[ROW_AUDIO].light != LIGHT_ERROR)   set_status(ROW_AUDIO,   LIGHT_OFF, L"-");

    if (!requested){
        log_line(L"Bridge exited unexpectedly (code %lu).", exit_code);
    } else if (exit_code != 0){
        log_line(L"Bridge stopped (code %lu).", exit_code);
    }

    if (restart_pending){
        restart_pending = false;
        launch_bridge();
    } else if (scan_after_stop){
        scan_after_stop = false;
        start_scan();
    } else if (bridge_wanted && !requested){
        if (exit_code == EXIT_CONFIG_ERROR){
            bridge_wanted = false;
        } else {
            // e.g. the dongle is still enumerating after boot, or was briefly unplugged
            log_line(L"Retrying in %d seconds.", RETRY_DELAY_MS / 1000);
            SetTimer(main_window, TIMER_RETRY, RETRY_DELAY_MS, NULL);
        }
    }
    update_controls();
}

// ---------------------------------------------------------------------------------------------
// Settings file

static bool save_settings(void){
    bridge_config_t updated = settings;
    if (!controls_to_settings(&updated)) return false;
    if (!config_save(&updated, config_path)){
        MessageBoxW(main_window, L"Could not write bridge.ini.\n\nMake sure the program folder is writable "
                                 L"(e.g. not inside Program Files).", APP_TITLE, MB_OK | MB_ICONERROR);
        return false;
    }
    settings = updated;
    settings_to_controls(&settings);
    log_line(L"Settings saved.");
    if (!bridge_wanted && status_rows[ROW_HEADSET].light == LIGHT_OFF) reset_bridge_status();

    if (children[CHILD_BRIDGE].running && !children[CHILD_BRIDGE].stopping){
        log_line(L"Restarting bridge to apply the new settings...");
        restart_pending = true;
        stop_child(CHILD_BRIDGE);
    }
    update_controls();
    return true;
}

static void load_settings(void){
    config_set_defaults(&settings);
    bool exists = config_load(&settings, config_path);
    settings_to_controls(&settings);
    if (!exists){
        log_line(L"No bridge.ini yet. Put the headset in pairing mode, click \"Find headset...\", check the audio devices and click Save.");
    } else {
        log_line(L"Settings loaded from bridge.ini.");
    }
}

// ---------------------------------------------------------------------------------------------
// Audio device list

static void handle_list_line(const wchar_t * line){
    if (starts_with(line, L"Playback devices"))  { list_section_input = false; return; }
    if (starts_with(line, L"Recording devices")) { list_section_input = true;  return; }
    if (!starts_with(line, L"  [") || num_audio_devices >= MAX_AUDIO_DEVICES) return;

    const wchar_t * close = wcsstr(line, L"] ");
    if (close == NULL) return;
    audio_device_t * device = &audio_devices[num_audio_devices];
    wcsncpy(device->name, close + 2, CONFIG_STRING_LEN - 1);
    device->name[CONFIG_STRING_LEN - 1] = L'\0';
    trim_wide(device->name);
    device->wasapi   = wcsstr(line, L"WASAPI") != NULL && wcsstr(line, L"WASAPI") < close;
    device->is_input = list_section_input;
    if (device->name[0]) num_audio_devices++;
}

static void fill_device_combo(HWND combo, bool is_input){
    wchar_t current[CONFIG_STRING_LEN];
    GetWindowTextW(combo, current, CONFIG_STRING_LEN);

    // WASAPI names are complete; MME truncates them, so only fall back to other APIs if WASAPI is missing
    bool have_wasapi = false;
    for (int i = 0; i < num_audio_devices; i++){
        if (audio_devices[i].is_input == is_input && audio_devices[i].wasapi) have_wasapi = true;
    }

    loading_controls = true;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < num_audio_devices; i++){
        const audio_device_t * device = &audio_devices[i];
        if (device->is_input != is_input || (have_wasapi && !device->wasapi)) continue;
        if (SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM) -1, (LPARAM) device->name) != CB_ERR) continue;
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM) device->name);
    }
    set_combo_text(combo, current);
    loading_controls = false;
}

static void refresh_devices(void){
    if (children[CHILD_LIST].running) return;
    num_audio_devices  = 0;
    list_section_input = false;
    if (start_child(CHILD_LIST, L"--list")) update_controls();
}

static void on_list_exit(DWORD exit_code){
    if (exit_code != 0 || num_audio_devices == 0){
        log_line(L"Could not list audio devices (code %lu).", exit_code);
        return;
    }
    fill_device_combo(mic_combo, false);
    fill_device_combo(speaker_combo, true);
}

// ---------------------------------------------------------------------------------------------
// Headset scan

static void start_scan(void){
    if (children[CHILD_SCAN].running){
        stop_child(CHILD_SCAN);
        return;
    }
    if (bridge_wanted || children[CHILD_BRIDGE].running){
        if (MessageBoxW(main_window, L"Searching uses the Bluetooth dongle, so the bridge has to stop first.\n\nStop the bridge and search now?",
                        APP_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        scan_after_stop = children[CHILD_BRIDGE].running;
        stop_bridge();
        if (scan_after_stop) return;
    }

    num_scan_results = 0;
    log_line(L"Searching for Bluetooth devices...");
    if (start_child(CHILD_SCAN, L"--scan")) update_controls();
}

static void handle_scan_line(const wchar_t * line){
    log_line(L"%ls", line);
    // format from scan.c: "Found device AA:BB:CC:DD:EE:FF [kind] name"
    const wchar_t * prefix = L"Found device ";
    if (!starts_with(line, prefix) || num_scan_results >= MAX_SCAN_RESULTS) return;
    const wchar_t * addr = line + wcslen(prefix);
    if (wcslen(addr) < 17) return;

    scan_result_t * result = &scan_results[num_scan_results];
    wmemcpy(result->addr, addr, 17);
    result->addr[17] = L'\0';
    result->kind[0]  = L'\0';
    result->name[0]  = L'\0';

    const wchar_t * open  = wcschr(addr + 17, L'[');
    const wchar_t * close = open ? wcschr(open, L']') : NULL;
    if (open && close){
        size_t len = (size_t) (close - open - 1);
        if (len >= 16) len = 15;
        wmemcpy(result->kind, open + 1, len);
        result->kind[len] = L'\0';
        wcsncpy(result->name, close + 1, 63);
        result->name[63] = L'\0';
        trim_wide(result->name);
    }
    num_scan_results++;
}

static void show_scan_results(void){
    if (num_scan_results == 0){
        MessageBoxW(main_window, L"No Bluetooth devices were found.\n\nPut the headset in pairing mode (usually by holding "
                                 L"the power or Bluetooth button until the light flashes) and try again.",
                    APP_TITLE, MB_OK | MB_ICONINFORMATION);
        return;
    }

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Choose your headset:");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    // audio devices first, since that is almost certainly what the user is looking for
    for (int pass = 0; pass < 2; pass++){
        for (int i = 0; i < num_scan_results; i++){
            bool is_audio = wcscmp(scan_results[i].kind, L"audio") == 0;
            if (is_audio != (pass == 0)) continue;
            wchar_t item[128];
            swprintf(item, 128, L"%ls  (%ls)%ls", scan_results[i].name, scan_results[i].addr, is_audio ? L"" : L"  - not a headset?");
            AppendMenuW(menu, MF_STRING, ID_SCAN_RESULT_FIRST + i, item);
        }
    }

    RECT rect;
    GetWindowRect(find_button, &rect);
    SetForegroundWindow(main_window);
    int command = (int) TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, rect.left, rect.bottom, 0, main_window, NULL);
    DestroyMenu(menu);

    if (command >= ID_SCAN_RESULT_FIRST && command < ID_SCAN_RESULT_FIRST + num_scan_results){
        const scan_result_t * result = &scan_results[command - ID_SCAN_RESULT_FIRST];
        SetWindowTextW(address_edit, result->addr);
        log_line(L"Selected %ls (%ls). Click Save to keep it.", result->name, result->addr);
        mark_dirty();
    }
}

static void on_scan_exit(DWORD exit_code, bool cancelled){
    update_controls();
    if (cancelled){
        log_line(L"Search cancelled.");
    } else if (exit_code == 2){
        MessageBoxW(main_window, L"Could not open the Bluetooth USB dongle.\n\nCheck that it is plugged in and uses the WinUSB "
                                 L"driver (see the README).", APP_TITLE, MB_OK | MB_ICONWARNING);
    } else {
        show_scan_results();
    }
}

// ---------------------------------------------------------------------------------------------
// Start with Windows

static bool autostart_registered(void){
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static bool set_autostart(bool enable){
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    LSTATUS status;
    if (enable){
        wchar_t command[MAX_PATH + 32];
        swprintf(command, sizeof(command) / sizeof(command[0]), L"\"%ls\" %ls", gui_exe_path, AUTOSTART_ARG);
        status = RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, (const BYTE *) command, (DWORD) ((wcslen(command) + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, RUN_VALUE);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// Window

static void create_fonts(void){
    if (ui_font)   DeleteObject(ui_font);
    if (bold_font) DeleteObject(bold_font);
    if (mono_font) DeleteObject(mono_font);

    NONCLIENTMETRICSW metrics;
    memset(&metrics, 0, sizeof(metrics));
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    ui_font = CreateFontIndirectW(&metrics.lfMessageFont);

    LOGFONTW bold = metrics.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    bold_font = CreateFontIndirectW(&bold);

    LOGFONTW mono;
    memset(&mono, 0, sizeof(mono));
    mono.lfHeight = -MulDiv(9, (int) dpi, 72);
    mono.lfCharSet = DEFAULT_CHARSET;
    wcscpy(mono.lfFaceName, L"Consolas");
    mono_font = CreateFontIndirectW(&mono);
}

static BOOL CALLBACK apply_font(HWND child, LPARAM param){
    (void) param;
    SendMessageW(child, WM_SETFONT, (WPARAM) (child == log_edit ? mono_font : ui_font), TRUE);
    return TRUE;
}

static HWND add_control(const wchar_t * class_name, const wchar_t * text, DWORD style, DWORD ex_style, int id){
    return CreateWindowExW(ex_style, class_name, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                           main_window, (HMENU) (INT_PTR) id, instance, NULL);
}

static void create_controls(void){
    start_button   = add_control(L"BUTTON", L"Start bridge", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_START);
    settings_group = add_control(L"BUTTON", L"Settings", BS_GROUPBOX, 0, 0);

    const wchar_t * label_texts[] = { L"Headset address", L"Headset mic plays into", L"Headset hears audio from",
                                      L"Mic gain (dB)", L"Headset gain (dB)", L"Buffer (ms)", L"Reconnect every (s)", NULL };
    for (int i = 0; label_texts[i]; i++){
        labels[i] = add_control(L"STATIC", label_texts[i], SS_LEFT | SS_CENTERIMAGE, 0, 0);
    }

    address_edit      = add_control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_UPPERCASE, WS_EX_CLIENTEDGE, ID_ADDRESS);
    find_button       = add_control(L"BUTTON", L"Find headset...", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_FIND);
    mic_combo         = add_control(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, 0, ID_MIC_DEVICE);
    speaker_combo     = add_control(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, 0, ID_SPEAKER_DEVICE);
    refresh_button    = add_control(L"BUTTON", L"Refresh", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_REFRESH);
    mic_gain_edit     = add_control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, ID_MIC_GAIN);
    speaker_gain_edit = add_control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, ID_SPEAKER_GAIN);
    latency_edit      = add_control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, WS_EX_CLIENTEDGE, ID_LATENCY);
    reconnect_edit    = add_control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, WS_EX_CLIENTEDGE, ID_RECONNECT);
    autostart_check   = add_control(L"BUTTON", L"Start with Windows", WS_TABSTOP | BS_AUTOCHECKBOX, 0, ID_AUTOSTART);
    save_button       = add_control(L"BUTTON", L"Save", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_SAVE);
    log_label         = add_control(L"STATIC", L"Log", SS_LEFT, 0, 0);
    log_edit          = add_control(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, WS_EX_CLIENTEDGE, ID_LOG);

    SendMessageW(address_edit, EM_SETLIMITTEXT, 17, 0);
    SendMessageW(log_edit, EM_SETLIMITTEXT, MAX_LOG_CHARS * 2, 0);
    SendMessageW(address_edit, EM_SETCUEBANNER, TRUE, (LPARAM) L"AA:BB:CC:DD:EE:FF");
    EnumChildWindows(main_window, &apply_font, 0);
}

static void move(HWND control, int x, int y, int width, int height){
    SetWindowPos(control, NULL, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void layout(void){
    RECT client;
    GetClientRect(main_window, &client);
    int width  = client.right;
    int height = client.bottom;

    int margin = scale(16), row = scale(32), field_h = scale(24), button_w = scale(110);
    int right  = width - margin;

    int status_top = scale(12), status_h = scale(26) * ROW_COUNT;
    int start_w = scale(130), start_h = scale(40);
    move(start_button, right - start_w, status_top + (status_h - start_h) / 2, start_w, start_h);
    SetRect(&status_rect, margin, status_top, right - start_w - scale(12), status_top + status_h);
    InvalidateRect(main_window, &status_rect, TRUE);

    int group_top = status_top + status_h + scale(14);
    int group_h   = scale(26) + row * 6;
    move(settings_group, margin, group_top, width - 2 * margin, group_h);

    int inner_left  = margin + scale(12);
    int inner_right = right - scale(12);
    int label_w     = scale(160);
    int field_x     = inner_left + label_w;
    int combo_w     = inner_right - field_x - button_w - scale(8);
    int number_w    = scale(70);
    int y           = group_top + scale(24);

    move(labels[0], inner_left, y, label_w, field_h);
    move(address_edit, field_x, y, scale(170), field_h);
    move(find_button, field_x + scale(178), y, scale(130), field_h);
    y += row;
    move(labels[1], inner_left, y, label_w, field_h);
    move(mic_combo, field_x, y, combo_w, scale(300));
    y += row;
    move(labels[2], inner_left, y, label_w, field_h);
    move(speaker_combo, field_x, y, combo_w, scale(300));
    move(refresh_button, inner_right - button_w, y, button_w, field_h);
    y += row;

    int second_label_x = field_x + number_w + scale(28);
    int second_label_w = scale(140);
    move(labels[3], inner_left, y, label_w, field_h);
    move(mic_gain_edit, field_x, y, number_w, field_h);
    move(labels[4], second_label_x, y, second_label_w, field_h);
    move(speaker_gain_edit, second_label_x + second_label_w, y, number_w, field_h);
    y += row;
    move(labels[5], inner_left, y, label_w, field_h);
    move(latency_edit, field_x, y, number_w, field_h);
    move(labels[6], second_label_x, y, second_label_w, field_h);
    move(reconnect_edit, second_label_x + second_label_w, y, number_w, field_h);
    y += row;
    move(autostart_check, inner_left, y, scale(220), field_h);
    move(save_button, inner_right - button_w, y, button_w, field_h);

    int log_top = group_top + group_h + scale(10);
    move(log_label, margin, log_top, scale(200), scale(20));
    int log_edit_top = log_top + scale(22);
    move(log_edit, margin, log_edit_top, width - 2 * margin, max(scale(60), height - margin - log_edit_top));
}

static void paint_status(HDC dc){
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    int row_h  = scale(26);
    int dot    = scale(10);
    int text_x = status_rect.left + dot + scale(10);
    int value_x = text_x + scale(80);

    for (int i = 0; i < ROW_COUNT; i++){
        const status_row_t * status = &status_rows[i];
        int top = status_rect.top + i * row_h;

        HBRUSH brush = CreateSolidBrush(light_colors[status->light]);
        HGDIOBJ old_brush = SelectObject(dc, brush);
        HGDIOBJ old_pen   = SelectObject(dc, GetStockObject(NULL_PEN));
        int dot_top = top + (row_h - dot) / 2;
        Ellipse(dc, status_rect.left, dot_top, status_rect.left + dot + 1, dot_top + dot + 1);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(brush);

        RECT label_rect = { text_x, top, value_x, top + row_h };
        HGDIOBJ old_font = SelectObject(dc, bold_font);
        DrawTextW(dc, status->label, -1, &label_rect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        SelectObject(dc, ui_font);
        RECT value_rect = { value_x, top, status_rect.right, top + row_h };
        DrawTextW(dc, status->text, -1, &value_rect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, old_font);
    }
}

static void show_main_window(void){
    ShowWindow(main_window, IsIconic(main_window) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(main_window);
}

static void show_tray_menu(void){
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Open");
    AppendMenuW(menu, MF_STRING | (children[CHILD_SCAN].running ? MF_GRAYED : 0), ID_TRAY_TOGGLE,
                bridge_wanted ? L"Stop bridge" : L"Start bridge");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetMenuDefaultItem(menu, ID_TRAY_OPEN, FALSE);

    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(main_window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, main_window, NULL);
    PostMessageW(main_window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void quit_app(void){
    if (quitting) return;
    quitting      = true;
    bridge_wanted = false;
    KillTimer(main_window, TIMER_RETRY);

    for (int i = 0; i < CHILD_COUNT; i++){
        if (children[i].running) SetEvent(children[i].stop_event);
    }
    for (int i = 0; i < CHILD_COUNT; i++){
        if (!children[i].running) continue;
        if (WaitForSingleObject(children[i].process, QUIT_WAIT_MS) == WAIT_TIMEOUT){
            TerminateProcess(children[i].process, 1);
        }
    }
    remove_tray();
    DestroyWindow(main_window);
}

static void on_child_exit(child_kind_t kind, DWORD exit_code){
    child_t * child = &children[kind];
    KillTimer(main_window, TIMER_FORCE_STOP + kind);
    bool requested = child->stopping;
    CloseHandle(child->process);
    CloseHandle(child->stdout_read);
    CloseHandle(child->stop_event);
    child->running  = false;
    child->stopping = false;
    if (quitting) return;

    switch (kind){
        case CHILD_BRIDGE: on_bridge_exit(exit_code, requested); break;
        case CHILD_LIST:   on_list_exit(exit_code); update_controls(); break;
        case CHILD_SCAN:   on_scan_exit(exit_code, requested); break;
        default: break;
    }
}

static void on_command(int id, int code){
    switch (id){
        case ID_START:
        case ID_TRAY_TOGGLE:
            if (bridge_wanted) stop_bridge(); else start_bridge();
            break;
        case ID_FIND:     start_scan(); break;
        case ID_REFRESH:  refresh_devices(); break;
        case ID_SAVE:     save_settings(); break;
        case ID_TRAY_OPEN: show_main_window(); break;
        case ID_TRAY_EXIT: quit_app(); break;
        case ID_AUTOSTART: {
            bool enable = SendMessageW(autostart_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!set_autostart(enable)){
                MessageBoxW(main_window, L"Could not change the Windows startup setting.", APP_TITLE, MB_OK | MB_ICONERROR);
                SendMessageW(autostart_check, BM_SETCHECK, enable ? BST_UNCHECKED : BST_CHECKED, 0);
            }
            break;
        }
        case ID_ADDRESS: case ID_MIC_GAIN: case ID_SPEAKER_GAIN: case ID_LATENCY: case ID_RECONNECT:
            if (code == EN_CHANGE) mark_dirty();
            break;
        case ID_MIC_DEVICE: case ID_SPEAKER_DEVICE:
            if (code == CBN_EDITCHANGE || code == CBN_SELCHANGE) mark_dirty();
            break;
        default:
            break;
    }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam){
    if (message == taskbar_created_msg && taskbar_created_msg != 0){
        tray_added = false;
        update_tray();
        return 0;
    }

    switch (message){
        case WM_CREATE:
            main_window = hwnd;
            dpi = GetDpiForWindow(hwnd);
            create_fonts();
            create_controls();
            layout();
            return 0;

        case WM_SIZE:
            layout();
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO * info = (MINMAXINFO *) lparam;
            info->ptMinTrackSize.x = scale(600);
            info->ptMinTrackSize.y = scale(560);
            return 0;
        }

        case WM_DPICHANGED: {
            dpi = HIWORD(wparam);
            create_fonts();
            EnumChildWindows(hwnd, &apply_font, 0);
            const RECT * suggested = (const RECT *) lparam;
            SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(hwnd, &paint);
            paint_status(dc);
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC) wparam;
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT) GetSysColorBrush(COLOR_WINDOW);
        }

        case WM_COMMAND:
            on_command(LOWORD(wparam), HIWORD(wparam));
            return 0;

        case WM_TIMER:
            if (wparam == TIMER_RETRY){
                KillTimer(hwnd, TIMER_RETRY);
                if (bridge_wanted && !children[CHILD_BRIDGE].running) launch_bridge();
            } else if (wparam >= TIMER_FORCE_STOP && wparam < TIMER_FORCE_STOP + CHILD_COUNT){
                force_stop_child((child_kind_t) (wparam - TIMER_FORCE_STOP));
            }
            return 0;

        case WM_APP_CHILD_LINE: {
            wchar_t * line = (wchar_t *) lparam;
            switch ((child_kind_t) wparam){
                case CHILD_BRIDGE: log_line(L"%ls", line); handle_bridge_line(line); break;
                case CHILD_LIST:   handle_list_line(line); break;
                case CHILD_SCAN:   handle_scan_line(line); break;
                default: break;
            }
            free(line);
            return 0;
        }

        case WM_APP_CHILD_EXIT:
            on_child_exit((child_kind_t) wparam, (DWORD) lparam);
            return 0;

        case WM_APP_TRAY:
            switch (LOWORD(lparam)){
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK: show_main_window(); break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:   show_tray_menu(); break;
                default: break;
            }
            return 0;

        case WM_APP_SHOW:
            show_main_window();
            return 0;

        case WM_CLOSE:
            if (bridge_wanted && !quitting){
                ShowWindow(hwnd, SW_HIDE);
                if (!tray_hint_shown){
                    tray_hint_shown = true;
                    show_tray_balloon(APP_TITLE, L"The bridge is still running here. Right-click the icon to stop it or exit.");
                }
            } else {
                quit_app();
            }
            return 0;

        case WM_ENDSESSION:
            if (wparam) quit_app();
            return 0;

        case WM_DESTROY:
            remove_tray();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static void resolve_paths(void){
    DWORD len = GetModuleFileNameW(NULL, gui_exe_path, MAX_PATH);
    wcsncpy(exe_dir, gui_exe_path, MAX_PATH);
    while (len > 0 && exe_dir[len - 1] != L'\\' && exe_dir[len - 1] != L'/') len--;
    exe_dir[len] = L'\0';
    swprintf(bridge_exe_path, MAX_PATH, L"%ls%ls", exe_dir, BRIDGE_EXE);

    // the console bridge resolves bridge.ini through the ANSI APIs, so do the same to read the same file
    wchar_t wide_config[MAX_PATH];
    swprintf(wide_config, MAX_PATH, L"%lsbridge.ini", exe_dir);
    WideCharToMultiByte(CP_ACP, 0, wide_config, -1, config_path, sizeof(config_path), NULL, NULL);
}

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE previous, PWSTR command_line, int show_command){
    (void) previous;
    instance = hinstance;
    bool autostart = command_line != NULL && wcsstr(command_line, AUTOSTART_ARG) != NULL;

    HANDLE mutex = CreateMutexW(NULL, TRUE, INSTANCE_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS){
        HWND other = FindWindowW(WINDOW_CLASS, NULL);
        if (other != NULL && !autostart) PostMessageW(other, WM_APP_SHOW, 0, 0);
        return 0;
    }

    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);
    resolve_paths();

    job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    UINT system_dpi = GetDpiForSystem();
    int small_icon = GetSystemMetricsForDpi(SM_CXSMICON, system_dpi);
    for (int i = 0; i < 4; i++) tray_icons[i] = create_headset_icon(small_icon, light_colors[i]);
    app_icon_large = LoadImageW(hinstance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXICON, system_dpi),
                                GetSystemMetricsForDpi(SM_CYICON, system_dpi), 0);
    app_icon_small = LoadImageW(hinstance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, small_icon, small_icon, 0);

    WNDCLASSEXW window_class;
    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize        = sizeof(window_class);
    window_class.lpfnWndProc   = &window_proc;
    window_class.hInstance     = hinstance;
    window_class.hIcon         = app_icon_large;
    window_class.hIconSm       = app_icon_small;
    window_class.hCursor       = LoadCursorW(NULL, (LPCWSTR) IDC_ARROW);
    window_class.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    window_class.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&window_class);
    taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");

    HWND hwnd = CreateWindowExW(0, WINDOW_CLASS, APP_TITLE L" " BRIDGE_VERSION, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                MulDiv(680, (int) system_dpi, 96), MulDiv(700, (int) system_dpi, 96), NULL, NULL, hinstance, NULL);
    if (hwnd == NULL) return 1;

    load_settings();
    reset_bridge_status();
    if (autostart_registered()){
        SendMessageW(autostart_check, BM_SETCHECK, BST_CHECKED, 0);
        set_autostart(true);    // keeps the registered path current if the folder was moved
    }
    update_controls();
    refresh_devices();

    if (autostart){
        if (settings.headset_address[0]) start_bridge();
        else show_main_window();
    } else {
        ShowWindow(hwnd, show_command);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0){
        if (IsDialogMessageW(main_window, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(job);
    if (mutex) CloseHandle(mutex);
    return (int) msg.wParam;
}
