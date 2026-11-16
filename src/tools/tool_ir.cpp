/* tool_ir.cpp — IR tool: learn a remote key, save it to /ir on the
 * card, replay saved signals from the library.
 *
 * The input callback only records intent (selection moves and pending
 * actions); all driver calls, confirms and file work run in the main
 * loop, so nothing long ever runs inside the input pump. Learn is the
 * driver's start+poll pair, polled once per loop slice.
 */
#include "imp_api.h"
#include <string.h>
#include <stdio.h>

#define IR_LIB_MAX 16 /* matches the UI list cache */

enum ir_mode_t { IR_MODE_MENU = 0, IR_MODE_LEARN, IR_MODE_LIBRARY, IR_MODE_DETAIL };
enum ir_action_t {
    IR_ACT_NONE = 0, IR_ACT_OK, IR_ACT_BACK
};

typedef struct {
    imp_ui_t *ui;
    bool running;
    int mode;
    int sel;              /* menu or library selection */
    int pending;          /* ir_action_t recorded by the callback */
    bool learning;        /* driver learn session is live */
    imp_ir_signal_t sig;  /* last learned signal */
    imp_ir_signal_t loaded; /* signal open in the detail screen */
    char loaded_name[24];
    char lib_names[IR_LIB_MAX][24];
    int lib_count;
} ir_tool_t;

/* One static instance: two imp_ir_signal_t are ~2 KB each, too big to
 * be casual about on an 8 KB tool stack alongside the list buffers. */
static ir_tool_t s;

static const char *ir_menu_items[] = {"Learn", "Library", "Back"};
#define IR_MENU_COUNT 3

static const char *ir_proto_label(uint8_t proto) {
    switch (proto) {
        case IMP_IR_PROTO_NEC:  return "NEC";
        case IMP_IR_PROTO_RC5:  return "RC5";
        case IMP_IR_PROTO_SIRC: return "SIRC";
        default:                return "RAW";
    }
}

static void ir_list_cb(void *ctx, const char *name) {
    ir_tool_t *t = (ir_tool_t *)ctx;
    if (t->lib_count < IR_LIB_MAX)
        snprintf(t->lib_names[t->lib_count++], 24, "%s", name);
}

static void ir_collect_library(ir_tool_t *t) {
    t->lib_count = 0;
    imp_storage_list("/ir", ir_list_cb, t);
    if (t->sel >= t->lib_count) t->sel = t->lib_count > 0 ? t->lib_count - 1 : 0;
}

/* First free /ir/remoteNNN.ir — the card is the state, not RAM. */
static bool ir_next_remote_path(char *out, size_t out_len) {
    for (int i = 1; i <= 999; i++) {
        snprintf(out, out_len, "/ir/remote%03d.ir", i);
        if (!imp_storage_exists(out)) return true;
    }
    return false;
}

static void ir_show_menu(ir_tool_t *t) {
    t->mode = IR_MODE_MENU;
    t->sel = 0;
    imp_ui_list(t->ui, ir_menu_items, IR_MENU_COUNT, t->sel);
    imp_ui_status(t->ui, "Learn a key or open Library");
}

static void ir_start_learn(ir_tool_t *t) {
    imp_err_t e = imp_ir_learn_start();
    if (e != IMP_OK) {
        imp_ui_status(t->ui, "IR receiver not ready");
        imp_ui_text(t->ui, 0, "Could not start learning.");
        imp_ui_text(t->ui, 1, "BACK returns to the menu.");
        return;
    }
    t->learning = true;
    t->mode = IR_MODE_LEARN;
    imp_ui_text(t->ui, 0, "");
    imp_ui_text(t->ui, 1, "");
    imp_ui_status(t->ui, "Point a remote, press a key");
}

static void ir_stop_learn(ir_tool_t *t) {
    if (t->learning) imp_ir_learn_stop();
    t->learning = false;
}

/* A signal landed: show what it is, offer the save. Runs in the main
 * loop, so the confirm dialog is safe here. */
static void ir_on_signal(ir_tool_t *t) {
    ir_stop_learn(t);
    char row[32];
    snprintf(row, sizeof(row), "Protocol: %s", ir_proto_label(t->sig.protocol));
    imp_ui_text(t->ui, 0, row);
    snprintf(row, sizeof(row), "Timings: %lu", (unsigned long)t->sig.timing_count);
    imp_ui_text(t->ui, 1, row);
    snprintf(row, sizeof(row), "Carrier: %lu Hz", (unsigned long)t->sig.carrier_hz);
    imp_ui_text(t->ui, 2, row);
    imp_ui_status(t->ui, "Signal caught");
    if (!imp_storage_sd_ready()) {
        imp_ui_status(t->ui, "No microSD - cannot save");
        ir_show_menu(t);
        return;
    }
    if (imp_ui_confirm(t->ui, "Save this signal?")) {
        char path[24];
        if (ir_next_remote_path(path, sizeof(path))) {
            if (imp_ir_save(&t->sig, path) == IMP_OK) {
                const char *base = strrchr(path, '/');
                char msg[40];
                snprintf(msg, sizeof(msg), "Saved %s", base ? base + 1 : path);
                imp_ui_status(t->ui, msg);
            } else {
                imp_ui_status(t->ui, "Save failed - card problem");
            }
        } else {
            imp_ui_status(t->ui, "Library full - remote999 hit");
        }
    } else {
        imp_ui_status(t->ui, "Not saved");
    }
    ir_show_menu(t);
}

static void ir_open_library(ir_tool_t *t) {
    ir_collect_library(t);
    t->mode = IR_MODE_LIBRARY;
    t->sel = 0;
    if (t->lib_count == 0) {
        imp_ui_text(t->ui, 0, "No saved remotes yet.");
        imp_ui_text(t->ui, 1, "Learn one first.");
        imp_ui_status(t->ui, "Folder /ir is empty - BACK");
    } else {
        imp_ui_text(t->ui, 0, "");
        imp_ui_text(t->ui, 1, "");
        imp_ui_status(t->ui, "Pick a remote - OK opens it");
    }
}

static void ir_open_detail(ir_tool_t *t) {
    char path[40];
    snprintf(path, sizeof(path), "/ir/%s", t->lib_names[t->sel]);
    if (imp_ir_load(path, &t->loaded) != IMP_OK) {
        imp_ui_status(t->ui, "Could not read that file");
        return;
    }
    snprintf(t->loaded_name, sizeof(t->loaded_name), "%s", t->lib_names[t->sel]);
    t->mode = IR_MODE_DETAIL;
    char row[32];
    imp_ui_text(t->ui, 0, t->loaded_name);
    snprintf(row, sizeof(row), "Protocol: %s", ir_proto_label(t->loaded.protocol));
    imp_ui_text(t->ui, 1, row);
    snprintf(row, sizeof(row), "Timings: %lu", (unsigned long)t->loaded.timing_count);
    imp_ui_text(t->ui, 2, row);
    snprintf(row, sizeof(row), "Carrier: %lu Hz", (unsigned long)t->loaded.carrier_hz);
    imp_ui_text(t->ui, 3, row);
    imp_ui_status(t->ui, "OK = send  BACK = library");
}

static void ir_send_loaded(ir_tool_t *t) {
    /* Sending is a transmit: confirm first, always. */
    if (!imp_ui_confirm(t->ui, "Send this IR signal?")) {
        imp_ui_status(t->ui, "Send cancelled");
        return;
    }
    imp_ui_status(t->ui, "Sending...");
    imp_err_t e = imp_ir_send(&t->loaded);
    imp_ui_status(t->ui, e == IMP_OK ? "Sent" : "Send failed");
}

/* Process one recorded OK/BACK in the main loop. */
static void ir_handle_action(ir_tool_t *t, int act) {
    if (act == IR_ACT_BACK) {
        switch (t->mode) {
            case IR_MODE_LEARN:
                ir_stop_learn(t);
                ir_show_menu(t);
                break;
            case IR_MODE_LIBRARY:
                ir_show_menu(t);
                break;
            case IR_MODE_DETAIL:
                ir_open_library(t);
                break;
            default: /* menu: Back leaves the tool */
                t->running = false;
                break;
        }
        return;
    }
    if (act != IR_ACT_OK) return;
    switch (t->mode) {
        case IR_MODE_MENU:
            if (t->sel == 0) ir_start_learn(t);
            else if (t->sel == 1) ir_open_library(t);
            else t->running = false;
            break;
        case IR_MODE_LIBRARY:
            if (t->lib_count > 0) ir_open_detail(t);
            break;
        case IR_MODE_DETAIL:
            ir_send_loaded(t);
            break;
        default: /* LEARN: OK does nothing, a signal or BACK ends it */
            break;
    }
}

static void ir_refresh(ir_tool_t *t) {
    switch (t->mode) {
        case IR_MODE_MENU:
            imp_ui_list(t->ui, ir_menu_items, IR_MENU_COUNT, t->sel);
            break;
        case IR_MODE_LIBRARY: {
            const char *items[IR_LIB_MAX];
            for (int i = 0; i < t->lib_count; i++) items[i] = t->lib_names[i];
            imp_ui_list(t->ui, items, t->lib_count, t->sel); /* n=0 clears the slot */
            break;
        }
        default:
            break; /* LEARN and DETAIL live in the text rows */
    }
}

static void ir_on_input(void *ctx, imp_input_t ev) {
    ir_tool_t *t = (ir_tool_t *)ctx;
    int n = 0;
    if (t->mode == IR_MODE_MENU) n = IR_MENU_COUNT;
    else if (t->mode == IR_MODE_LIBRARY) n = t->lib_count;
    switch (ev) {
        case IMP_INPUT_ENC_CW:
            if (n > 0) t->sel = (t->sel + 1) % n;
            break;
        case IMP_INPUT_ENC_CCW:
            if (n > 0) t->sel = (t->sel + n - 1) % n;
            break;
        case IMP_INPUT_OK:
            t->pending = IR_ACT_OK;
            break;
        case IMP_INPUT_BACK:
            t->pending = IR_ACT_BACK;
            break;
        default: break;
    }
}

int ir_main(void) {
    memset(&s, 0, sizeof(s));
    s.running = true;
    s.mode = IR_MODE_MENU;
    s.ui = imp_ui_open("IR");
    if (!s.ui) return -1;
    imp_ui_set_input_cb(s.ui, ir_on_input, &s);
    imp_ui_set_pose(s.ui, "pose_ir");
    if (!imp_storage_sd_ready())
        imp_ui_status(s.ui, "No microSD - saving is off");
    ir_show_menu(&s);
    while (s.running) {
        if (s.pending != IR_ACT_NONE) {
            int act = s.pending;
            s.pending = IR_ACT_NONE;
            ir_handle_action(&s, act);
        }
        if (s.mode == IR_MODE_LEARN && s.learning) {
            if (imp_ir_learn_poll(&s.sig) == IMP_OK) ir_on_signal(&s);
        }
        ir_refresh(&s);
        imp_delay_ms(30);
    }
    /* Restore on every exit path: receiver released, radio icon off. */
    ir_stop_learn(&s);
    imp_ui_radio_state(IMP_RADIO_IR, false);
    imp_system_set_led(0);
    imp_ui_close(s.ui);
    return 0;
}

static const imp_app_t ir_app = {
    "ir", "IR", "IR", "ir_10px", "pose_ir", "ir", 8192, ir_main
};
IMP_APP_REGISTER(ir_app);
