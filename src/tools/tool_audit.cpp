/* tool_audit.cpp — Security System Auto-Audit.
 *
 * PASSIVE ONLY: inventory and measurement. Wi-Fi/BLE scans the API
 * already provides, an RSSI watch on the user's own AP with dropout
 * timestamps, a camera-hardening checklist kept on the card, and a
 * text report. No deauth, no jamming, no connection attempts to
 * other people's gear, no active probing beyond those scans.
 */
#include "imp_api.h"
#include <string.h>
#include <stdio.h>

typedef enum {
    SCR_MENU = 0, SCR_WIFI_LIST, SCR_WIFI_DETAIL,
    SCR_BLE_LIST, SCR_BLE_DETAIL, SCR_WATCH_PICK, SCR_WATCHING, SCR_CHECKLIST
} audit_screen_t;
typedef enum {
    ACT_NONE = 0, ACT_WIFI_SCAN, ACT_BLE_SCAN, ACT_SAVE_REPORT
} audit_action_t;

#define CHECK_ITEMS 6
static const char *check_short[CHECK_ITEMS] = {
    "Password", "Firmware", "WPS off", "Remote?", "Defaults", "Segmented"
};
static const char *check_full[CHECK_ITEMS] = {
    "Unique password set", "Firmware is current", "WPS disabled",
    "Remote access needed?", "Default creds changed", "Network segmented"
};

typedef struct {
    imp_ui_t *ui;
    bool running;
    audit_screen_t screen;
    int sel;
    audit_action_t pending;
    imp_wifi_result_t wifi[IMP_WIFI_MAX_RESULTS];
    int wifi_count;
    imp_ble_result_t ble[IMP_BLE_MAX_RESULTS];
    int ble_count;
    bool check_state[CHECK_ITEMS];
    /* watch */
    bool watching;
    int16_t watch_rssi;
    bool watch_present;
    bool watch_dirty;
    uint32_t watch_events;
} audit_tool_t;

static const char *menu_items[] = {
    "Wi-Fi inventory", "BLE inventory", "Watch my AP",
    "Camera checks", "Save report", "Back"
};
#define MENU_COUNT 6

/* Watch callback: runs from imp_wifi_watch_poll() in the tool loop.
 * Appends one line per sweep to /audit/watch.log and stashes the
 * reading for the screen. */
static void audit_watch_cb(void *ctx, int16_t rssi_dbm, bool present, uint32_t now_ms) {
    audit_tool_t *s = (audit_tool_t *)ctx;
    s->watch_rssi = rssi_dbm;
    s->watch_present = present;
    s->watch_dirty = true;
    s->watch_events++;
    imp_file_t *f = imp_storage_open("/audit/watch.log", "a");
    if (f) {
        char line[64];
        int n = snprintf(line, sizeof(line), "uptime %lu ms  rssi %d  %s\n",
                         (unsigned long)now_ms, (int)rssi_dbm,
                         present ? "present" : "MISSING");
        imp_storage_write(f, line, (size_t)n);
        imp_storage_close(f);
    }
}

static void bssid_str(const uint8_t bssid[6], char *out, size_t outsz) {
    snprintf(out, outsz, "%02X:%02X:%02X:%02X:%02X:%02X",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

static void checklist_load(audit_tool_t *s) {
    for (int i = 0; i < CHECK_ITEMS; i++) s->check_state[i] = false;
    imp_file_t *f = imp_storage_open("/audit/checklist.txt", "r");
    if (!f) return;
    char buf[16] = {0};
    imp_storage_read(f, buf, sizeof(buf) - 1);
    imp_storage_close(f);
    for (int i = 0; i < CHECK_ITEMS && buf[i]; i++)
        s->check_state[i] = (buf[i] == '1');
}

static void checklist_save(audit_tool_t *s) {
    imp_file_t *f = imp_storage_open("/audit/checklist.txt", "w");
    if (!f) return;
    char buf[CHECK_ITEMS + 2];
    for (int i = 0; i < CHECK_ITEMS; i++) buf[i] = s->check_state[i] ? '1' : '0';
    buf[CHECK_ITEMS] = '\n';
    imp_storage_write(f, buf, CHECK_ITEMS + 1);
    imp_storage_close(f);
}

static void audit_draw(audit_tool_t *s) {
    switch (s->screen) {
        case SCR_MENU:
            imp_ui_list(s->ui, menu_items, MENU_COUNT, s->sel);
            break;
        case SCR_WIFI_LIST: {
            if (s->wifi_count == 0) {
                const char *empty[] = {"(none found)", "Back"};
                imp_ui_list(s->ui, empty, 2, s->sel);
            } else {
                const char *items[16];
                int n = s->wifi_count < 16 ? s->wifi_count : 16;
                for (int i = 0; i < n; i++) items[i] = s->wifi[i].ssid[0] ? s->wifi[i].ssid : "(hidden)";
                imp_ui_list(s->ui, items, n, s->sel);
            }
            break;
        }
        case SCR_WATCH_PICK: {
            const char *items[16];
            int n = s->wifi_count < 16 ? s->wifi_count : 16;
            for (int i = 0; i < n; i++) items[i] = s->wifi[i].ssid[0] ? s->wifi[i].ssid : "(hidden)";
            imp_ui_list(s->ui, items, n, s->sel);
            break;
        }
        case SCR_BLE_LIST: {
            if (s->ble_count == 0) {
                const char *empty[] = {"(none found)", "Back"};
                imp_ui_list(s->ui, empty, 2, s->sel);
            } else {
                const char *items[16];
                int n = s->ble_count < 16 ? s->ble_count : 16;
                for (int i = 0; i < n; i++) items[i] = s->ble[i].name[0] ? s->ble[i].name : "(no name)";
                imp_ui_list(s->ui, items, n, s->sel);
            }
            break;
        }
        case SCR_CHECKLIST: {
            char labels[CHECK_ITEMS][16];
            const char *items[CHECK_ITEMS];
            for (int i = 0; i < CHECK_ITEMS; i++) {
                snprintf(labels[i], 16, "[%c] %s", s->check_state[i] ? 'x' : ' ', check_short[i]);
                items[i] = labels[i];
            }
            imp_ui_list(s->ui, items, CHECK_ITEMS, s->sel);
            if (s->sel >= 0 && s->sel < CHECK_ITEMS)
                imp_ui_text(s->ui, 0, check_full[s->sel]);
            break;
        }
        default:
            break;
    }
}

static void audit_show_wifi_detail(audit_tool_t *s) {
    const imp_wifi_result_t *r = &s->wifi[s->sel];
    char line[40];
    bssid_str(r->bssid, line, sizeof(line));
    imp_ui_text(s->ui, 0, line);
    imp_ui_value(s->ui, "Channel", r->channel, "");
    imp_ui_value(s->ui, "RSSI", r->rssi_dbm, "dBm");
    imp_ui_text(s->ui, 1, r->encrypted ? "Encrypted" : "OPEN - no encryption");
    imp_ui_text(s->ui, 2, r->ssid[0] ? r->ssid : "(hidden SSID)");
}

static void audit_show_ble_detail(audit_tool_t *s) {
    const imp_ble_result_t *r = &s->ble[s->sel];
    char line[40];
    bssid_str(r->addr, line, sizeof(line));
    imp_ui_text(s->ui, 0, line);
    imp_ui_value(s->ui, "RSSI", r->rssi_dbm, "dBm");
    imp_ui_text(s->ui, 1, r->name[0] ? r->name : "(no name advertised)");
}

static void audit_stop_watch(audit_tool_t *s) {
    if (s->watching) {
        imp_wifi_watch_stop();
        s->watching = false;
    }
}

static void audit_on_input(void *ctx, imp_input_t ev) {
    audit_tool_t *s = (audit_tool_t *)ctx;
    switch (s->screen) {
        case SCR_MENU:
            if (ev == IMP_INPUT_ENC_CW) { s->sel = (s->sel + 1) % MENU_COUNT; audit_draw(s); }
            else if (ev == IMP_INPUT_ENC_CCW) { s->sel = (s->sel + MENU_COUNT - 1) % MENU_COUNT; audit_draw(s); }
            else if (ev == IMP_INPUT_BACK) s->running = false;
            else if (ev == IMP_INPUT_OK) {
                switch (s->sel) {
                    case 0: s->pending = ACT_WIFI_SCAN; s->screen = SCR_WIFI_LIST; break;
                    case 1: s->pending = ACT_BLE_SCAN; s->screen = SCR_BLE_LIST; break;
                    case 2:
                        if (s->wifi_count == 0) { s->pending = ACT_WIFI_SCAN; s->screen = SCR_WATCH_PICK; }
                        else { s->sel = 0; s->screen = SCR_WATCH_PICK; audit_draw(s); }
                        break;
                    case 3: s->sel = 0; s->screen = SCR_CHECKLIST; audit_draw(s); break;
                    case 4: s->pending = ACT_SAVE_REPORT; break;
                    case 5: s->running = false; break;
                    default: break;
                }
            }
            break;
        case SCR_WIFI_LIST: {
            int n = s->wifi_count > 0 ? (s->wifi_count < 16 ? s->wifi_count : 16) : 2;
            if (ev == IMP_INPUT_ENC_CW) { s->sel = (s->sel + 1) % n; audit_draw(s); }
            else if (ev == IMP_INPUT_ENC_CCW) { s->sel = (s->sel + n - 1) % n; audit_draw(s); }
            else if (ev == IMP_INPUT_BACK) { s->sel = 0; s->screen = SCR_MENU; audit_draw(s); }
            else if (ev == IMP_INPUT_OK && s->wifi_count > 0) {
                s->screen = SCR_WIFI_DETAIL;
                audit_show_wifi_detail(s);
            } else if (ev == IMP_INPUT_OK && s->wifi_count == 0 && s->sel == 1) {
                s->sel = 0; s->screen = SCR_MENU; audit_draw(s);
            }
            break;
        }
        case SCR_WIFI_DETAIL:
            if (ev == IMP_INPUT_BACK) {
                imp_ui_text(s->ui, 0, ""); imp_ui_text(s->ui, 1, ""); imp_ui_text(s->ui, 2, "");
                s->screen = SCR_WIFI_LIST; audit_draw(s);
            }
            break;
        case SCR_BLE_LIST: {
            int n = s->ble_count > 0 ? (s->ble_count < 16 ? s->ble_count : 16) : 2;
            if (ev == IMP_INPUT_ENC_CW) { s->sel = (s->sel + 1) % n; audit_draw(s); }
            else if (ev == IMP_INPUT_ENC_CCW) { s->sel = (s->sel + n - 1) % n; audit_draw(s); }
            else if (ev == IMP_INPUT_BACK) { s->sel = 1; s->screen = SCR_MENU; audit_draw(s); }
            else if (ev == IMP_INPUT_OK && s->ble_count > 0) {
                s->screen = SCR_BLE_DETAIL;
                audit_show_ble_detail(s);
            } else if (ev == IMP_INPUT_OK && s->ble_count == 0 && s->sel == 1) {
                s->sel = 1; s->screen = SCR_MENU; audit_draw(s);
            }
            break;
        }
        case SCR_BLE_DETAIL:
            if (ev == IMP_INPUT_BACK) {
                imp_ui_text(s->ui, 0, ""); imp_ui_text(s->ui, 1, "");
                s->screen = SCR_BLE_LIST; audit_draw(s);
            }
            break;
        case SCR_WATCH_PICK: {
            int n = s->wifi_count < 16 ? s->wifi_count : 16;
            if (n <= 0) { if (ev == IMP_INPUT_BACK) { s->sel = 2; s->screen = SCR_MENU; audit_draw(s); } break; }
            if (ev == IMP_INPUT_ENC_CW) { s->sel = (s->sel + 1) % n; audit_draw(s); }
            else if (ev == IMP_INPUT_ENC_CCW) { s->sel = (s->sel + n - 1) % n; audit_draw(s); }
            else if (ev == IMP_INPUT_BACK) { s->sel = 2; s->screen = SCR_MENU; audit_draw(s); }
            else if (ev == IMP_INPUT_OK) {
                if (imp_wifi_watch(s->wifi[s->sel].bssid, audit_watch_cb, s) == IMP_OK) {
                    s->watching = true;
                    s->watch_dirty = false;
                    s->watch_events = 0;
                    s->screen = SCR_WATCHING;
                    imp_ui_status(s->ui, "Watching - BACK stops");
                    imp_ui_text(s->ui, 0, s->wifi[s->sel].ssid[0] ? s->wifi[s->sel].ssid : "(hidden)");
                } else {
                    imp_ui_status(s->ui, "Watch could not start");
                }
            }
            break;
        }
        case SCR_WATCHING:
            if (ev == IMP_INPUT_BACK) {
                audit_stop_watch(s);
                imp_ui_text(s->ui, 0, "");
                imp_ui_status(s->ui, "Watch stopped");
                s->sel = 2; s->screen = SCR_MENU; audit_draw(s);
            }
            break;
        case SCR_CHECKLIST:
            if (ev == IMP_INPUT_ENC_CW) { s->sel = (s->sel + 1) % CHECK_ITEMS; audit_draw(s); }
            else if (ev == IMP_INPUT_ENC_CCW) { s->sel = (s->sel + CHECK_ITEMS - 1) % CHECK_ITEMS; audit_draw(s); }
            else if (ev == IMP_INPUT_BACK) {
                imp_ui_text(s->ui, 0, "");
                s->sel = 3; s->screen = SCR_MENU; audit_draw(s);
            }
            else if (ev == IMP_INPUT_OK) {
                s->check_state[s->sel] = !s->check_state[s->sel];
                checklist_save(s);
                audit_draw(s);
            }
            break;
    }
}

static void audit_save_report(audit_tool_t *s) {
    imp_file_t *f = imp_storage_open("/audit/report.txt", "w");
    if (!f) {
        imp_ui_status(s->ui, "No card - report not saved");
        return;
    }
    char line[96];
    int n;
    n = snprintf(line, sizeof(line), "IMP-1 Security Auto-Audit report\n");
    imp_storage_write(f, line, (size_t)n);
    /* No RTC in build 1 and the tool API exposes no clock: watch.log
     * lines carry driver-side uptime stamps; this report is a snapshot
     * of the counts below, in scan order. */
    n = snprintf(line, sizeof(line), "stamp: none (no RTC in build 1)\n");
    imp_storage_write(f, line, (size_t)n);
    n = snprintf(line, sizeof(line), "wifi networks seen: %d\n", s->wifi_count);
    imp_storage_write(f, line, (size_t)n);
    n = snprintf(line, sizeof(line), "ble devices seen: %d\n", s->ble_count);
    imp_storage_write(f, line, (size_t)n);
    int ticks = 0;
    for (int i = 0; i < CHECK_ITEMS; i++) if (s->check_state[i]) ticks++;
    n = snprintf(line, sizeof(line), "camera checklist: %d of %d ticked\n", ticks, CHECK_ITEMS);
    imp_storage_write(f, line, (size_t)n);
    for (int i = 0; i < CHECK_ITEMS; i++) {
        n = snprintf(line, sizeof(line), "  [%c] %s\n", s->check_state[i] ? 'x' : ' ', check_full[i]);
        imp_storage_write(f, line, (size_t)n);
    }
    for (int i = 0; i < s->wifi_count; i++) {
        char b[24];
        bssid_str(s->wifi[i].bssid, b, sizeof(b));
        n = snprintf(line, sizeof(line), "wifi: %s  %s  ch %u  rssi %d  %s\n",
                     s->wifi[i].ssid[0] ? s->wifi[i].ssid : "(hidden)", b,
                     s->wifi[i].channel, (int)s->wifi[i].rssi_dbm,
                     s->wifi[i].encrypted ? "encrypted" : "OPEN");
        imp_storage_write(f, line, (size_t)n);
    }
    imp_storage_close(f);
    imp_ui_status(s->ui, "Report saved to /audit");
}

int audit_main(void) {
    audit_tool_t s = {0};
    s.running = true;
    s.ui = imp_ui_open("Auto-Audit");
    if (!s.ui) return -1;
    imp_ui_set_input_cb(s.ui, audit_on_input, &s);
    imp_ui_set_pose(s.ui, "pose_audit");
    checklist_load(&s);
    imp_ui_status(s.ui, imp_storage_sd_ready() ? "Inventory and measure only" :
                                                 "No card - logs/reports off");
    audit_draw(&s);
    while (s.running) {
        if (s.pending == ACT_WIFI_SCAN) {
            s.pending = ACT_NONE;
            imp_ui_status(s.ui, "Scanning Wi-Fi...");
            int n = imp_wifi_scan(s.wifi, IMP_WIFI_MAX_RESULTS);
            s.wifi_count = n < 0 ? 0 : n;
            s.sel = 0;
            imp_ui_status(s.ui, n < 0 ? "Wi-Fi scan failed" : "Scan done");
            audit_draw(&s);
        }
        if (s.pending == ACT_BLE_SCAN) {
            s.pending = ACT_NONE;
            imp_ui_status(s.ui, "Scanning BLE...");
            int n = imp_ble_scan(s.ble, IMP_BLE_MAX_RESULTS);
            s.ble_count = n < 0 ? 0 : n;
            s.sel = 0;
            imp_ui_status(s.ui, n < 0 ? "BLE scan failed" : "Scan done");
            audit_draw(&s);
        }
        if (s.pending == ACT_SAVE_REPORT) {
            s.pending = ACT_NONE;
            audit_save_report(&s);
            audit_draw(&s);
        }
        if (s.watching) {
            imp_wifi_watch_poll();
            if (s.watch_dirty) {
                s.watch_dirty = false;
                if (s.watch_present) {
                    imp_ui_value(s.ui, "RSSI", s.watch_rssi, "dBm");
                    imp_ui_status(s.ui, "AP present");
                } else {
                    imp_ui_status(s.ui, "AP MISSING - dropout logged");
                }
                imp_ui_value(s.ui, "Sweeps", (int32_t)s.watch_events, "");
            }
        }
        imp_delay_ms(30);
    }
    audit_stop_watch(&s);
    imp_system_set_led(0);
    imp_ui_close(s.ui);
    return 0;
}

static const imp_app_t audit_app = {
    "audit", "Auto-Audit", "System", "audit_10px", "pose_audit", "wifi", 8192, audit_main
};
IMP_APP_REGISTER(audit_app);
