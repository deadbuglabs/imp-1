/* tool_ibutton.cpp — iButton tool (guarded stub).
 * Not fitted in build 1 (D-02 — IO45 is a clean-NC strap, so the
 * 1-Wire line has no free pad). The shell refuses entry via
 * `requires`; if entered anyway, plain words and BACK. IMP-2 fits
 * the DS2482-100 and this screen becomes the reader.
 */
#include "imp_api.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    imp_ui_t *ui;
    bool running;
} ibutton_tool_t;

static void ibutton_on_input(void *ctx, imp_input_t ev) {
    ibutton_tool_t *s = (ibutton_tool_t *)ctx;
    if (ev == IMP_INPUT_BACK) s->running = false;
}

int ibutton_main(void) {
    ibutton_tool_t s = {0};
    s.running = true;
    s.ui = imp_ui_open("iButton");
    if (!s.ui) return -1;
    imp_ui_set_input_cb(s.ui, ibutton_on_input, &s);
    imp_ui_set_pose(s.ui, "pose_ibutton");
    imp_ui_text(s.ui, 0, "iButton line not fitted");
    imp_ui_text(s.ui, 1, "in build 1 (D-02 -");
    imp_ui_text(s.ui, 2, "no free pad).");
    imp_ui_text(s.ui, 3, "Ready on IMP-2.");
    imp_ui_status(s.ui, "Not fitted - BACK exits");
    while (s.running) {
        imp_delay_ms(30);
    }
    imp_system_set_led(0);
    imp_ui_close(s.ui);
    return 0;
}

static const imp_app_t ibutton_app = {
    "ibutton", "iButton", "Tools", "ibutton_10px", "pose_ibutton", "ibutton", 4096, ibutton_main
};
IMP_APP_REGISTER(ibutton_app);
