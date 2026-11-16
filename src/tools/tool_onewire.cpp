/* tool_onewire.cpp — 1-Wire tool (guarded stub).
 * Not fitted in build 1 (D-02 — no free pad for the bus). The shell
 * refuses entry via `requires`; if entered anyway, plain words and
 * BACK. Ready on IMP-2.
 */
#include "imp_api.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    imp_ui_t *ui;
    bool running;
} onewire_tool_t;

static void onewire_on_input(void *ctx, imp_input_t ev) {
    onewire_tool_t *s = (onewire_tool_t *)ctx;
    if (ev == IMP_INPUT_BACK) s->running = false;
}

int onewire_main(void) {
    onewire_tool_t s = {0};
    s.running = true;
    s.ui = imp_ui_open("1-Wire");
    if (!s.ui) return -1;
    imp_ui_set_input_cb(s.ui, onewire_on_input, &s);
    imp_ui_set_pose(s.ui, "pose_onewire");
    imp_ui_text(s.ui, 0, "1-Wire bus not fitted");
    imp_ui_text(s.ui, 1, "in build 1 (D-02).");
    imp_ui_text(s.ui, 2, "Ready on IMP-2.");
    imp_ui_status(s.ui, "Not fitted - BACK exits");
    while (s.running) {
        imp_delay_ms(30);
    }
    imp_system_set_led(0);
    imp_ui_close(s.ui);
    return 0;
}

static const imp_app_t onewire_app = {
    "onewire", "1-Wire", "Tools", "onewire_10px", "pose_onewire", "onewire", 4096, onewire_main
};
IMP_APP_REGISTER(onewire_app);
