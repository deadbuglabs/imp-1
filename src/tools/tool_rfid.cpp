/* tool_rfid.cpp — RFID 125k tool (guarded stub).
 * The shell refuses entry: imp_app_hw_available("rfid") is false in
 * build 1 (D-15 — no 125 kHz front-end fitted). This tool is the
 * second line of defence: if it is ever entered, it says so in plain
 * words and waits for BACK. The code path stays ready for IMP-2.
 */
#include "imp_api.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    imp_ui_t *ui;
    bool running;
} rfid_tool_t;

static void rfid_on_input(void *ctx, imp_input_t ev) {
    rfid_tool_t *s = (rfid_tool_t *)ctx;
    if (ev == IMP_INPUT_BACK) s->running = false;
}

int rfid_main(void) {
    rfid_tool_t s = {0};
    s.running = true;
    s.ui = imp_ui_open("RFID 125k");
    if (!s.ui) return -1;
    imp_ui_set_input_cb(s.ui, rfid_on_input, &s);
    imp_ui_set_pose(s.ui, "pose_rfid");
    imp_ui_text(s.ui, 0, "125 kHz front-end");
    imp_ui_text(s.ui, 1, "not fitted in build 1");
    imp_ui_text(s.ui, 2, "(D-15). IMP-2 adds it.");
    imp_ui_status(s.ui, "Not fitted - BACK exits");
    while (s.running) {
        imp_delay_ms(30);
    }
    imp_system_set_led(0);
    imp_ui_close(s.ui);
    return 0;
}

static const imp_app_t rfid_app = {
    "rfid", "RFID 125k", "RF", "rfid_10px", "pose_rfid", "rfid", 4096, rfid_main
};
IMP_APP_REGISTER(rfid_app);
