/* main.cpp — IMP-1 shell: boot, home menu, tool launcher, idle, sleep.
 *
 * Spec v0.1: single vertical list + drill-in; Settings is the LAST item;
 * partial redraw only (full frame on wake/panic, in the disp layer);
 * idle drift after 8 s untouched; deep sleep between uses.
 * The approved mock: imp LEFT, tool list RIGHT, pose swap per tool.
 */
#include "imp_internal.h"

/* ---------------- settings (persisted to SD when a card is fitted) --- */
struct imp_settings {
    uint8_t backlight_pct = 30;
    uint32_t sleep_timeout_ms = 120000;
};
static imp_settings settings;

static void settings_load(void) {
    imp_file_t *f = imp_storage_open("/settings.cfg", "r");
    if (!f) return;
    char buf[64] = {0};
    imp_storage_read(f, buf, sizeof(buf) - 1);
    imp_storage_close(f);
    int bl, slp;
    if (sscanf(buf, "backlight=%d sleep=%d", &bl, &slp) == 2) {
        settings.backlight_pct = (uint8_t)bl;
        settings.sleep_timeout_ms = (uint32_t)slp;
    }
}

static void settings_save(void) {
    imp_file_t *f = imp_storage_open("/settings.cfg", "w");
    if (!f) return;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "backlight=%u sleep=%lu",
                     settings.backlight_pct, (unsigned long)settings.sleep_timeout_ms);
    imp_storage_write(f, buf, n);
    imp_storage_close(f);
}

/* ---------------- home menu ---------------- */
static imp_ui_t *shell_ui = nullptr;
static int menu_sel = 0;
static bool launch_pending = false;
static bool settings_pending = false;
static uint32_t drift_start = 0;
static int drift_idx = 0;

static int menu_count(void) { return imp_app_count() + 1; } /* + Settings */

static const char *menu_title(int i) {
    if (i < imp_app_count()) return imp_app_get(i)->title;
    return "Settings";
}

static const char *menu_pose(int i) {
    if (i < imp_app_count()) return imp_app_get(i)->pose;
    return "pose_settings";
}

static void shell_draw_menu(void) {
    const char *items[IMP_LIST_CACHE_MAX];
    int n = menu_count();
    if (n > IMP_LIST_CACHE_MAX) n = IMP_LIST_CACHE_MAX;
    for (int i = 0; i < n; i++) items[i] = menu_title(i);
    imp_ui_list(shell_ui, items, n, menu_sel);
    imp_ui_set_pose(shell_ui, menu_pose(menu_sel));
}

static void shell_on_input(void *ctx, imp_input_t ev) {
    (void)ctx;
    int n = menu_count();
    switch (ev) {
        case IMP_INPUT_ENC_CW:
            menu_sel = (menu_sel + 1) % n;
            shell_draw_menu();
            break;
        case IMP_INPUT_ENC_CCW:
            menu_sel = (menu_sel + n - 1) % n;
            shell_draw_menu();
            break;
        case IMP_INPUT_OK:
            if (menu_sel < imp_app_count()) launch_pending = true;
            else settings_pending = true;
            break;
        default:
            break;
    }
}

static bool requires_met(const imp_app_t *app) {
    if (!app->requires || strcmp(app->requires, "none") == 0) return true;
    char buf[48];
    snprintf(buf, sizeof(buf), "%s", app->requires);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(nullptr, ",")) {
        if (!imp_app_hw_available(tok)) return false;
    }
    return true;
}

static void run_tool(const imp_app_t *app) {
    imp_ui_close(shell_ui);
    shell_ui = nullptr;
    app->main_fn();
    shell_ui = imp_ui_open("IMP-1");
    imp_ui_set_backlight(shell_ui, settings.backlight_pct);
    imp_ui_set_input_cb(shell_ui, shell_on_input, nullptr);
    shell_draw_menu();
}

/* ---------------- settings screen ---------------- */
static bool settings_running = false;
static int settings_sel = 0;
static const char *settings_items[] = {
    "Backlight", "Sleep timer", "Battery", "About", "Reboot", "Back"
};

static void settings_on_input(void *ctx, imp_input_t ev) {
    imp_ui_t *ui = (imp_ui_t *)ctx;
    int n = 6;
    switch (ev) {
        case IMP_INPUT_ENC_CW: settings_sel = (settings_sel + 1) % n; break;
        case IMP_INPUT_ENC_CCW: settings_sel = (settings_sel + n - 1) % n; break;
        case IMP_INPUT_BACK: settings_running = false; return;
        case IMP_INPUT_OK:
            switch (settings_sel) {
                case 0: /* backlight cycles 10..100 */
                    settings.backlight_pct = settings.backlight_pct >= 100 ? 10 : settings.backlight_pct + 15;
                    imp_ui_set_backlight(ui, settings.backlight_pct);
                    settings_save();
                    break;
                case 1: /* sleep timer cycles 60..300 s */
                    settings.sleep_timeout_ms =
                        settings.sleep_timeout_ms >= 300000 ? 60000 : settings.sleep_timeout_ms + 60000;
                    settings_save();
                    break;
                case 3:
                    imp_ui_text(ui, 0, "IMP-1 firmware, build 1");
                    imp_ui_text(ui, 1, "ESP32-S3 / ST7789V3 172x320");
                    imp_ui_text(ui, 2, "Deadbug Labs");
                    break;
                case 4:
                    if (imp_ui_confirm(ui, "Reboot the IMP?")) imp_system_reboot();
                    break;
                case 5:
                    settings_running = false;
                    return;
                default: break;
            }
            break;
        default: break;
    }
    imp_ui_list(ui, settings_items, n, settings_sel);
    imp_ui_value(ui, "Backlight", settings.backlight_pct, "%");
    imp_ui_value(ui, "Sleep", (int32_t)(settings.sleep_timeout_ms / 1000), "s");
    imp_ui_value(ui, "Battery", imp_system_battery_mv(), "mV");
}

static void run_settings(void) {
    imp_ui_close(shell_ui);
    shell_ui = nullptr;
    imp_ui_t *ui = imp_ui_open("Settings");
    imp_ui_set_backlight(ui, settings.backlight_pct);
    imp_ui_set_pose(ui, "pose_settings");
    imp_ui_set_input_cb(ui, settings_on_input, ui);
    settings_sel = 0;
    settings_running = true;
    imp_ui_list(ui, settings_items, 6, 0);
    while (settings_running) {
        imp_ui_value(ui, "Battery", imp_system_battery_mv(), "mV");
        imp_delay_ms(50);
    }
    settings_save();
    imp_ui_close(ui);
    shell_ui = imp_ui_open("IMP-1");
    imp_ui_set_backlight(shell_ui, settings.backlight_pct);
    imp_ui_set_input_cb(shell_ui, shell_on_input, nullptr);
    shell_draw_menu();
}

/* ---------------- boot ---------------- */
static void boot_screen(void) {
    /* Silhouette-flame boot (approved mock): the imp fades up from a
     * dark flame silhouette to full red while the backlight ramps. */
    const imp_sprite_t *spr = imp_sprite_find("pose_default");
    disp_set_backlight(0);
    disp_draw_sprite(IMP_MASCOT_X, IMP_MASCOT_Y, spr, IMP_RGB565(0x60, 0x18, 0x00), IMP_COLOUR_BG);
    disp_draw_text(58, 200, "IMP-1", IMP_RGB565(0x60, 0x18, 0x00), IMP_COLOUR_BG, 1);
    for (int pct = 0; pct <= (int)settings.backlight_pct; pct += 5) {
        disp_set_backlight((uint8_t)pct);
        delay(30);
    }
    disp_draw_sprite(IMP_MASCOT_X, IMP_MASCOT_Y, spr, IMP_COLOUR_RED, IMP_COLOUR_BG);
    disp_draw_text(58, 200, "IMP-1", IMP_COLOUR_RED, IMP_COLOUR_BG, 1);
    delay(450);
}

void setup() {
    /* Switched outputs default OFF before anything else (stealth rule). */
    pinMode(IMP_PIN_A9_PWR, OUTPUT);
    digitalWrite(IMP_PIN_A9_PWR, LOW);
    pinMode(IMP_PIN_FLASHLED, OUTPUT);
    digitalWrite(IMP_PIN_FLASHLED, LOW);

    Serial.begin(115200);
    imp_bus_init();
    disp_init();
    imp_input_init();
    settings_load();

    boot_screen();

    shell_ui = imp_ui_open("IMP-1");
    imp_ui_set_backlight(shell_ui, settings.backlight_pct);
    imp_ui_set_input_cb(shell_ui, shell_on_input, nullptr);
    shell_draw_menu();
    if (!imp_storage_sd_ready())
        imp_ui_status(shell_ui, "No microSD fitted");
}

void loop() {
    if (launch_pending) {
        launch_pending = false;
        const imp_app_t *app = imp_app_get(menu_sel);
        if (app && requires_met(app)) {
            run_tool(app);
        } else if (app) {
            imp_ui_status(shell_ui, "Not fitted in build 1");
            imp_system_buzzer(180, 60);
        }
        return;
    }
    if (settings_pending) {
        settings_pending = false;
        run_settings();
        return;
    }

    imp_delay_ms(20);
    imp_ui_refresh_header();

    /* Idle drift: 8 s untouched on the home screen, the imp starts to
     * wander its poses; any input (pump updates activity) ends it. */
    uint32_t idle = millis() - imp_input_last_activity_ms();
    if (idle > 8000) {
        if (drift_start == 0) drift_start = millis();
        if (millis() - drift_start > 2500) {
            drift_start = millis();
            drift_idx = (drift_idx + 1) % 9;
            char pose[16];
            if (drift_idx == 0) snprintf(pose, sizeof(pose), "pose_default");
            else snprintf(pose, sizeof(pose), "pose_%02d", drift_idx);
            imp_ui_set_pose(shell_ui, pose);
        }
    } else if (drift_start != 0) {
        drift_start = 0;
        imp_ui_set_pose(shell_ui, menu_pose(menu_sel));
    }

    /* Deep sleep between uses. */
    if (settings.sleep_timeout_ms > 0 && idle > settings.sleep_timeout_ms) {
        imp_ui_close(shell_ui);
        imp_system_sleep();
    }
}
