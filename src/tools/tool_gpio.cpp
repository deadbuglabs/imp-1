/* tool_gpio.cpp — GPIO tool, the reference tool (pack section 11).
 * Pin picker (the list pattern every picker reuses), claim-then-mode,
 * live value screen, release on every exit path. Driver-owned pins
 * refuse on screen with the reason shown — error-as-UI. */
#include "imp_api.h"
#include <string.h>
#include <stdio.h>

/* Candidate pins offered to the user: the keypad matrix lines are the
 * build-1 free GPIO; strapping/debug/radio pins are listed too so the
 * refusal path is visible and honest on screen. */
static const uint8_t gpio_pins[] = {12, 38, 39, 40, 41, 42, 14, 13, 1, 8, 35, 45};
static const int gpio_pin_count = sizeof(gpio_pins) / sizeof(gpio_pins[0]);

typedef struct {
    imp_ui_t *ui;
    bool running;
    int sel;
    int claimed;       /* index into gpio_pins, or -1 */
    bool level;
} gpio_tool_t;

static void gpio_refresh(gpio_tool_t *s) {
    char labels[12][16];
    const char *items[12];
    for (int i = 0; i < gpio_pin_count; i++) {
        snprintf(labels[i], 16, "IO%u%s", gpio_pins[i], i == s->claimed ? " *" : "");
        items[i] = labels[i];
    }
    imp_ui_list(s->ui, items, gpio_pin_count, s->sel);
    if (s->claimed >= 0) {
        imp_ui_value(s->ui, "Pin", gpio_pins[s->claimed], "");
        imp_ui_value(s->ui, "Level", imp_gpio_read(gpio_pins[s->claimed]) ? 1 : 0, "");
    }
}

static void gpio_on_input(void *ctx, imp_input_t ev) {
    gpio_tool_t *s = (gpio_tool_t *)ctx;
    switch (ev) {
        case IMP_INPUT_ENC_CW:
            s->sel = (s->sel + 1) % gpio_pin_count;
            break;
        case IMP_INPUT_ENC_CCW:
            s->sel = (s->sel + gpio_pin_count - 1) % gpio_pin_count;
            break;
        case IMP_INPUT_OK: {
            if (s->claimed == s->sel) {
                /* toggle the claimed output */
                s->level = !s->level;
                imp_gpio_write(gpio_pins[s->claimed], s->level);
            } else {
                if (s->claimed >= 0) imp_gpio_release(gpio_pins[s->claimed]);
                s->claimed = -1;
                imp_err_t e = imp_gpio_claim(gpio_pins[s->sel], IMP_GPIO_OUTPUT);
                if (e == IMP_OK) {
                    s->claimed = s->sel;
                    s->level = false;
                    imp_gpio_write(gpio_pins[s->claimed], false);
                    imp_ui_status(s->ui, "Claimed - OK toggles");
                } else if (e == IMP_ERR_BUSY) {
                    imp_ui_status(s->ui, "Driver-owned pin - refused");
                } else {
                    imp_ui_status(s->ui, "Not fitted on this build");
                }
            }
            break;
        }
        case IMP_INPUT_BACK:
            s->running = false;
            break;
        default: break;
    }
}

int gpio_main(void) {
    gpio_tool_t s = {0};
    s.claimed = -1;
    s.running = true;
    s.ui = imp_ui_open("GPIO");
    if (!s.ui) return -1;
    imp_ui_set_input_cb(s.ui, gpio_on_input, &s);
    imp_ui_set_pose(s.ui, "pose_gpio");
    imp_ui_status(s.ui, imp_storage_sd_ready() ? "Pick a pin - OK claims" : "No microSD - still fine here");
    gpio_refresh(&s);
    while (s.running) {
        if (s.claimed >= 0)
            imp_ui_value(s.ui, "Level", imp_gpio_read(gpio_pins[s.claimed]) ? 1 : 0, "");
        imp_delay_ms(30);
    }
    if (s.claimed >= 0) imp_gpio_release(gpio_pins[s.claimed]);
    imp_system_set_led(0);
    imp_ui_close(s.ui);
    return 0;
}

static const imp_app_t gpio_app = {
    "gpio", "GPIO", "Tools", "gpio_10px", "pose_gpio", "none", 4096, gpio_main
};
IMP_APP_REGISTER(gpio_app);
