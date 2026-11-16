/* tool_subghz.cpp — Sub-GHz tool (CC1101 via imp_subghz_*).
 * Band picker -> live RSSI + RSSI-gated capture loop -> save to
 * /subghz/, plus a saved-capture picker whose TX path passes through
 * imp_ui_confirm(). The fitted antenna is the 915 MHz Johanson, so any
 * other band says so on screen instead of pretending otherwise.
 * Non-blocking throughout: the input callback only records intent,
 * the main loop does the API work in imp_delay_ms(20) slices.
 * Exit (including error exits) stops RX and drops the radio state. */
#include "imp_api.h"
#include <string.h>
#include <stdio.h>

static const uint32_t band_hz[4] = { 315000000UL, 433920000UL, 868300000UL, 915000000UL };
static const char *band_label[4] = { "315 MHz", "433 MHz", "868 MHz", "915 MHz" };
/* 915 is the strong band: it is the one the fitted antenna is cut for. */

enum { SG_BANDS = 0, SG_RX, SG_SAVED, SG_LOADED };
enum { ACT_NONE = 0, ACT_ENTER_RX, ACT_ENTER_SAVED, ACT_SAVE, ACT_LOAD, ACT_TX, ACT_STOP_RX };

typedef struct {
    imp_ui_t *ui;
    bool running;
    int mode;
    int sel;             /* selection in the current list */
    int band;            /* band index while in SG_RX */
    bool have_cap;       /* a capture is waiting in cap_rx */
    int act;             /* pending action for the main loop */
    int drawn_mode, drawn_sel;
    char saved[16][32];
    int saved_count;
    int file_count;      /* scratch for list callbacks */
} sg_state_t;

static imp_subghz_capture_t cap_rx;   /* static: 2 KB, not stack */
static imp_subghz_capture_t cap_ld;
static sg_state_t s;

static void list_count_cb(void *ctx, const char *name) {
    sg_state_t *st = (sg_state_t *)ctx;
    (void)name;
    st->file_count++;
}

static void list_collect_cb(void *ctx, const char *name) {
    sg_state_t *st = (sg_state_t *)ctx;
    if (st->saved_count < 16) {
        snprintf(st->saved[st->saved_count], 32, "%s", name);
        st->saved_count++;
    }
}

static void sg_on_input(void *ctx, imp_input_t ev) {
    sg_state_t *st = (sg_state_t *)ctx;
    int list_n = 0;
    if (st->mode == SG_BANDS) list_n = 5;
    else if (st->mode == SG_SAVED) list_n = st->saved_count;
    switch (ev) {
        case IMP_INPUT_ENC_CW:
            if (list_n > 0) st->sel = (st->sel + 1) % list_n;
            break;
        case IMP_INPUT_ENC_CCW:
            if (list_n > 0) st->sel = (st->sel + list_n - 1) % list_n;
            break;
        case IMP_INPUT_OK:
            if (st->mode == SG_BANDS) st->act = (st->sel < 4) ? ACT_ENTER_RX : ACT_ENTER_SAVED;
            else if (st->mode == SG_RX) { if (st->have_cap) st->act = ACT_SAVE; }
            else if (st->mode == SG_SAVED) st->act = ACT_LOAD;
            else if (st->mode == SG_LOADED) st->act = ACT_TX;
            break;
        case IMP_INPUT_BACK:
            if (st->mode == SG_RX) {
                if (st->have_cap) {          /* first BACK discards the capture */
                    st->have_cap = false;
                    imp_ui_status(st->ui, "Discarded - listening");
                    imp_ui_text(st->ui, 1, "");
                } else st->act = ACT_STOP_RX;
            } else if (st->mode == SG_SAVED) { st->mode = SG_BANDS; st->sel = 4; }
            else if (st->mode == SG_LOADED) { st->mode = SG_SAVED; }
            else st->running = false;
            break;
        default: break;
    }
}

static void sg_show_bands(sg_state_t *st) {
    const char *items[5] = { band_label[0], band_label[1], band_label[2], band_label[3], "Saved..." };
    imp_ui_list(st->ui, items, 5, st->sel);
}

static void sg_show_saved(sg_state_t *st) {
    const char *items[16];
    for (int i = 0; i < st->saved_count; i++) items[i] = st->saved[i];
    imp_ui_list(st->ui, items, st->saved_count, st->sel);
}

static void sg_enter_rx(sg_state_t *st) {
    imp_err_t e = imp_subghz_set_freq(band_hz[st->sel]);
    if (e == IMP_ERR_NO_HW) { imp_ui_status(st->ui, "No Sub-GHz radio fitted"); return; }
    if (e != IMP_OK) { imp_ui_status(st->ui, "Radio refused that band"); return; }
    st->band = st->sel;
    e = imp_subghz_rx_start();
    if (e != IMP_OK) { imp_ui_status(st->ui, "Receiver would not start"); return; }
    st->mode = SG_RX;
    st->have_cap = false;
    imp_ui_set_pose(st->ui, "pose_subghz");
    if (st->band != 3) {
        /* antenna honesty: the fitted Johanson is a 915 MHz part */
        imp_ui_text(st->ui, 0, "Fitted antenna is 915 MHz");
        imp_ui_status(st->ui, "Fitted antenna is 915 MHz");
    } else {
        imp_ui_text(st->ui, 0, "Strong band - fitted antenna");
        imp_ui_status(st->ui, "Listening - strong band");
    }
    imp_ui_text(st->ui, 1, "");
}

static void sg_save_capture(sg_state_t *st) {
    if (!imp_storage_sd_ready()) { imp_ui_status(st->ui, "No microSD - cannot save"); return; }
    st->file_count = 0;
    imp_storage_list("/subghz", list_count_cb, st);
    char path[48];
    int idx = st->file_count + 1;
    do {
        snprintf(path, sizeof(path), "/subghz/cap%03d.sub", idx++);
    } while (imp_storage_exists(path) && idx < 1000);
    imp_err_t e = imp_subghz_save(&cap_rx, path);
    if (e == IMP_OK) {
        char msg[40];
        snprintf(msg, sizeof(msg), "Saved %s", path + 8);
        imp_ui_status(st->ui, msg);
        st->have_cap = false;
        imp_ui_text(st->ui, 1, "");
    } else {
        imp_ui_status(st->ui, "Save failed - card problem?");
    }
}

static void sg_do_actions(sg_state_t *st) {
    int act = st->act;
    st->act = ACT_NONE;
    switch (act) {
        case ACT_ENTER_RX: sg_enter_rx(st); break;
        case ACT_ENTER_SAVED: {
            st->saved_count = 0;
            imp_storage_list("/subghz", list_collect_cb, st);
            if (st->saved_count == 0) imp_ui_status(st->ui, "No saved captures yet");
            else { st->mode = SG_SAVED; st->sel = 0; }
            break;
        }
        case ACT_SAVE: sg_save_capture(st); break;
        case ACT_LOAD: {
            char path[48];
            snprintf(path, sizeof(path), "/subghz/%s", st->saved[st->sel]);
            imp_err_t e = imp_subghz_load(path, &cap_ld);
            if (e != IMP_OK) { imp_ui_status(st->ui, "That capture will not load"); break; }
            st->mode = SG_LOADED;
            imp_ui_text(st->ui, 0, st->saved[st->sel]);
            imp_ui_value(st->ui, "Freq", (int32_t)cap_ld.freq_hz, "Hz");
            imp_ui_value(st->ui, "Timings", (int32_t)cap_ld.timing_count, "");
            imp_ui_value(st->ui, "RSSI", cap_ld.rssi_dbm, "dBm");
            imp_ui_status(st->ui, "OK transmits - BACK = list");
            break;
        }
        case ACT_TX: {
            if (!imp_ui_confirm(st->ui, "Transmit capture?")) {
                imp_ui_status(st->ui, "Transmit cancelled");
                break;
            }
            imp_err_t e = imp_subghz_tx(&cap_ld);
            if (e == IMP_OK) imp_ui_status(st->ui, "Transmitted");
            else if (e == IMP_ERR_NO_HW) imp_ui_status(st->ui, "No Sub-GHz radio fitted");
            else imp_ui_status(st->ui, "Transmit failed");
            break;
        }
        case ACT_STOP_RX: {
            imp_subghz_rx_stop();
            st->mode = SG_BANDS;
            st->sel = st->band;
            imp_ui_text(st->ui, 0, "");
            imp_ui_text(st->ui, 1, "");
            imp_ui_status(st->ui, "Pick a band");
            break;
        }
        default: break;
    }
}

int subghz_main(void) {
    memset(&s, 0, sizeof(s));
    s.running = true;
    s.mode = SG_BANDS;
    s.drawn_mode = -1;
    s.ui = imp_ui_open("Sub-GHz");
    if (!s.ui) return -1;
    imp_ui_set_input_cb(s.ui, sg_on_input, &s);
    imp_ui_set_pose(s.ui, "pose_subghz");
    imp_ui_status(s.ui, imp_storage_sd_ready() ? "Pick a band" : "No microSD - captures unsaved");
    sg_show_bands(&s);
    s.drawn_mode = s.mode; s.drawn_sel = s.sel;

    while (s.running) {
        sg_do_actions(&s);
        if (s.mode != s.drawn_mode || s.sel != s.drawn_sel) {
            if (s.mode == SG_BANDS) sg_show_bands(&s);
            else if (s.mode == SG_SAVED) sg_show_saved(&s);
            s.drawn_mode = s.mode; s.drawn_sel = s.sel;
        }
        if (s.mode == SG_RX) {
            imp_ui_value(s.ui, "Freq", (int32_t)band_hz[s.band], "Hz");
            imp_ui_value(s.ui, "RSSI", imp_subghz_rssi(), "dBm");
            if (!s.have_cap) {
                imp_subghz_capture_t *dst = &cap_rx;
                if (imp_subghz_rx_poll(dst) == IMP_OK) {
                    s.have_cap = true;
                    imp_ui_value(s.ui, "Timings", (int32_t)dst->timing_count, "");
                    imp_ui_text(s.ui, 1, "Capture ready");
                    imp_ui_status(s.ui, "OK saves - BACK discards");
                    imp_ui_set_pose(s.ui, "pose_success");
                }
            }
        }
        imp_delay_ms(20);
    }

    /* restore on every exit path, error exits included */
    imp_subghz_rx_stop();
    imp_ui_close(s.ui);
    return 0;
}

static const imp_app_t subghz_app = {
    "subghz", "Sub-GHz", "RF", "subghz_10px", "pose_subghz", "subghz", 8192, subghz_main
};
IMP_APP_REGISTER(subghz_app);
