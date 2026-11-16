/* tool_badusb.cpp — BadUSB tool.
 * Pick a USB personality, run DuckyScript-class payloads from
 * /badusb/ on the microSD, or type one test line. Every consequence
 * (payload run, test type) passes imp_ui_confirm() first. The USB
 * mode persists after the tool exits — the stack enumerates once per
 * boot — and the screen says so.
 */
#include "imp_api.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>

#define MAX_PAYLOADS 16 /* UI list cache limit */

typedef enum { SCR_MODES = 0, SCR_PAYLOADS } badusb_screen_t;
typedef enum { ACT_NONE = 0, ACT_TYPE_TEST, ACT_RUN_PAYLOAD } badusb_action_t;

typedef struct {
    imp_ui_t *ui;
    bool running;
    badusb_screen_t screen;
    int sel;
    imp_badusb_mode_t mode;
    bool mode_set;
    char payloads[MAX_PAYLOADS][48];
    int payload_count;
    int payload_sel;
    badusb_action_t pending;
    bool pending_is_mode;
    imp_badusb_mode_t pending_mode;
} badusb_tool_t;

static const char *mode_items[] = {
    "Keyboard", "Storage", "Serial", "Type test line", "Payloads", "Back"
};
#define MODE_ITEM_COUNT 6

static void collect_payload_cb(void *ctx, const char *name) {
    badusb_tool_t *s = (badusb_tool_t *)ctx;
    size_t len = strlen(name);
    if (len < 4) return;
    const char *ext = name + len - 4;
    if (strcasecmp(ext, ".txt") != 0) return;
    if (s->payload_count >= MAX_PAYLOADS) return;
    snprintf(s->payloads[s->payload_count], 48, "%s", name);
    s->payload_count++;
}

static void badusb_draw(badusb_tool_t *s) {
    if (s->screen == SCR_MODES) {
        imp_ui_list(s->ui, mode_items, MODE_ITEM_COUNT, s->sel);
    } else {
        if (s->payload_count == 0) {
            const char *empty[] = {"(no payloads)", "Back"};
            imp_ui_list(s->ui, empty, 2, s->payload_sel);
        } else {
            const char *items[MAX_PAYLOADS];
            for (int i = 0; i < s->payload_count; i++) items[i] = s->payloads[i];
            imp_ui_list(s->ui, items, s->payload_count, s->payload_sel);
        }
    }
}

static void badusb_on_input(void *ctx, imp_input_t ev) {
    badusb_tool_t *s = (badusb_tool_t *)ctx;
    if (s->screen == SCR_MODES) {
        switch (ev) {
            case IMP_INPUT_ENC_CW:
                s->sel = (s->sel + 1) % MODE_ITEM_COUNT; badusb_draw(s); break;
            case IMP_INPUT_ENC_CCW:
                s->sel = (s->sel + MODE_ITEM_COUNT - 1) % MODE_ITEM_COUNT; badusb_draw(s); break;
            case IMP_INPUT_BACK:
                s->running = false; break;
            case IMP_INPUT_OK:
                switch (s->sel) {
                    case 0: s->pending_is_mode = true; s->pending_mode = IMP_BADUSB_KEYBOARD; break;
                    case 1: s->pending_is_mode = true; s->pending_mode = IMP_BADUSB_STORAGE; break;
                    case 2: s->pending_is_mode = true; s->pending_mode = IMP_BADUSB_SERIAL; break;
                    case 3: s->pending = ACT_TYPE_TEST; break;
                    case 4:
                        s->payload_count = 0;
                        s->payload_sel = 0;
                        imp_storage_list("/badusb", collect_payload_cb, s);
                        s->screen = SCR_PAYLOADS;
                        badusb_draw(s);
                        break;
                    case 5: s->running = false; break;
                    default: break;
                }
                break;
            default: break;
        }
    } else { /* SCR_PAYLOADS */
        int n = s->payload_count > 0 ? s->payload_count : 2;
        switch (ev) {
            case IMP_INPUT_ENC_CW:
                s->payload_sel = (s->payload_sel + 1) % n; badusb_draw(s); break;
            case IMP_INPUT_ENC_CCW:
                s->payload_sel = (s->payload_sel + n - 1) % n; badusb_draw(s); break;
            case IMP_INPUT_BACK:
                s->screen = SCR_MODES; badusb_draw(s); break;
            case IMP_INPUT_OK:
                if (s->payload_count == 0) {
                    if (s->payload_sel == 1) { s->screen = SCR_MODES; badusb_draw(s); }
                } else {
                    s->pending = ACT_RUN_PAYLOAD;
                }
                break;
            default: break;
        }
    }
}

int badusb_main(void) {
    badusb_tool_t s = {0};
    s.running = true;
    s.screen = SCR_MODES;
    s.mode_set = false;
    s.ui = imp_ui_open("BadUSB");
    if (!s.ui) return -1;
    imp_ui_set_input_cb(s.ui, badusb_on_input, &s);
    imp_ui_set_pose(s.ui, "pose_badusb");
    if (!imp_storage_sd_ready()) {
        imp_ui_text(s.ui, 0, "No microSD fitted.");
        imp_ui_text(s.ui, 1, "Payloads need the card;");
        imp_ui_text(s.ui, 2, "typing still works.");
    }
    imp_ui_text(s.ui, 4, "USB mode stays until");
    imp_ui_text(s.ui, 5, "reboot or mode change.");
    imp_ui_status(s.ui, "Pick a USB mode");
    badusb_draw(&s);
    while (s.running) {
        if (s.pending_is_mode) {
            s.pending_is_mode = false;
            imp_err_t e = imp_badusb_present(s.pending_mode);
            if (e == IMP_OK) {
                s.mode = s.pending_mode;
                s.mode_set = true;
                imp_ui_status(s.ui, s.mode == IMP_BADUSB_KEYBOARD ? "Keyboard mode live" :
                                    s.mode == IMP_BADUSB_STORAGE ? "Storage mode live" :
                                                                   "Serial mode live");
            } else {
                imp_ui_status(s.ui, "Could not set USB mode");
            }
            badusb_draw(&s);
        }
        if (s.pending == ACT_TYPE_TEST) {
            s.pending = ACT_NONE;
            if (!s.mode_set || s.mode != IMP_BADUSB_KEYBOARD) {
                imp_ui_status(s.ui, "Pick Keyboard mode first");
            } else if (imp_ui_confirm(s.ui, "Type test line on the host?")) {
                imp_err_t e = imp_badusb_type("IMP-1 BadUSB OK");
                imp_ui_status(s.ui, e == IMP_OK ? "Test line typed" : "Typing failed - host ready?");
            } else {
                imp_ui_status(s.ui, "Cancelled");
            }
            badusb_draw(&s);
        }
        if (s.pending == ACT_RUN_PAYLOAD) {
            s.pending = ACT_NONE;
            if (!s.mode_set || s.mode != IMP_BADUSB_KEYBOARD) {
                imp_ui_status(s.ui, "Pick Keyboard mode first");
            } else if (s.payload_count > 0) {
                char path[64];
                snprintf(path, sizeof(path), "/badusb/%s", s.payloads[s.payload_sel]);
                if (imp_ui_confirm(s.ui, "Run payload on the host?")) {
                    imp_ui_status(s.ui, "Running payload...");
                    imp_err_t e = imp_badusb_run(path);
                    imp_ui_status(s.ui, e == IMP_OK ? "Payload finished" : "Payload file not read");
                } else {
                    imp_ui_status(s.ui, "Cancelled");
                }
            }
            badusb_draw(&s);
        }
        imp_delay_ms(30);
    }
    imp_system_set_led(0);
    imp_ui_close(s.ui);
    return 0;
}

static const imp_app_t badusb_app = {
    "badusb", "BadUSB", "USB", "badusb_10px", "pose_badusb", "badusb", 8192, badusb_main
};
IMP_APP_REGISTER(badusb_app);
