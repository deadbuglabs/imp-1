/* imp_ui.cpp — the one screen owner + the common tool template (spec v0.1).
 *
 * Template (every screen, rendered once; only dynamic regions repaint):
 *   header strip   — tool title, radio-state icons, battery
 *   mascot box     — left, pose swaps on selection/state change only
 *   content slot   — right column: lists and labelled values
 *   lower strip    — full-width plain-text rows, progress, status line
 *   footer         — keypad legend, static
 * Full-frame redraw happens ONLY on panic blank and wake (disp layer).
 *
 * Partial-redraw discipline: every public call caches what it last drew
 * and repaints only the strip whose content actually changed.
 */
#include "imp_internal.h"
#include "imp_font.h"

#define VALUE_SLOTS 6
#define TEXT_ROWS   10
#define TEXT_ROW_W  28   /* chars at scale 1 across the lower strip */
#define LIST_CACHE  IMP_LIST_CACHE_MAX

struct imp_ui_s {
    char title[24];
    char pose[24];
    uint8_t backlight;
    /* list cache */
    int list_n, list_sel, list_offset;
    char list_items[LIST_CACHE][16];
    bool list_valid;
    /* value slots keyed by label */
    struct { char label[16]; char rendered[28]; bool used; } values[VALUE_SLOTS];
    /* lower text rows */
    char text_rows[TEXT_ROWS][TEXT_ROW_W + 1];
    /* status + progress */
    char status[40];
    int progress; /* -1 = hidden */
    int mode;     /* 0 none, 1 list, 2 values — mode switch clears the slot */
};

static imp_ui_t *active_ui = nullptr;
static bool radio_state[IMP_RADIO_COUNT] = {false};
static int32_t header_batt_mv = -1;
static bool header_dirty = true;

/* ---------------- header ---------------- */
static const char radio_letters[IMP_RADIO_COUNT] = {'S', 'N', 'I', 'W', 'B'};

static void draw_header(imp_ui_t *ui) {
    disp_fill_rect(0, 0, IMP_DISP_W, IMP_HDR_H, IMP_COLOUR_BG);
    disp_draw_text(2, 5, ui->title, IMP_COLOUR_FG, IMP_COLOUR_BG, 1);
    /* radio icons: 9px cells at the right, battery text after them */
    int x = IMP_DISP_W - 34;
    char batt[8];
    int32_t mv = imp_system_battery_mv();
    snprintf(batt, sizeof(batt), "%d.%dV", (int)(mv / 1000), (int)((mv % 1000) / 100));
    disp_draw_text(x, 5, batt, IMP_COLOUR_DIM, IMP_COLOUR_BG, 1);
    x = 2 + disp_text_width(ui->title, 1) + 8;
    for (int i = 0; i < IMP_RADIO_COUNT; i++) {
        if (x > IMP_DISP_W - 48) break;
        uint16_t c = radio_state[i] ? IMP_COLOUR_ACCENT : IMP_COLOUR_DIM;
        char s[2] = {radio_letters[i], 0};
        disp_draw_text(x, 5, s, c, IMP_COLOUR_BG, 1);
        x += 8;
    }
    header_batt_mv = mv;
    header_dirty = false;
}

void imp_ui_refresh_header(void) {
    if (!active_ui) return;
    int32_t mv = imp_system_battery_mv();
    if (header_dirty || (mv > header_batt_mv + 60) || (mv < header_batt_mv - 60))
        draw_header(active_ui);
}

void imp_ui_radio_state(imp_radio_t radio, bool active) {
    if (radio < 0 || radio >= IMP_RADIO_COUNT) return;
    if (radio_state[radio] != active) {
        radio_state[radio] = active;
        header_dirty = true;
        imp_ui_refresh_header();
    }
}

/* ---------------- chrome ---------------- */
static void draw_chrome(imp_ui_t *ui) {
    disp_fill_rect(0, 0, IMP_DISP_W, IMP_DISP_H, IMP_COLOUR_BG);
    draw_header(ui);
    /* footer legend */
    disp_draw_text(2, IMP_FOOT_Y + 6, "PUSH OK  BOOT BACK", IMP_COLOUR_DIM, IMP_COLOUR_BG, 1);
    const imp_sprite_t *spr = imp_sprite_find(ui->pose);
    disp_draw_sprite(IMP_MASCOT_X, IMP_MASCOT_Y, spr, IMP_COLOUR_RED, IMP_COLOUR_BG);
}

static void clear_content_slot(void) {
    disp_fill_rect(IMP_CONTENT_X, IMP_HDR_H + 2, IMP_CONTENT_W, IMP_FOOT_Y - IMP_HDR_H - 4, IMP_COLOUR_BG);
}

static void clear_lower_strip(void) {
    disp_fill_rect(0, 172, IMP_DISP_W, IMP_FOOT_Y - 172, IMP_COLOUR_BG);
}

/* ---------------- lifecycle ---------------- */
imp_ui_t *imp_ui_open(const char *title) {
    imp_ui_t *ui = (imp_ui_t *)calloc(1, sizeof(imp_ui_t));
    if (!ui) return nullptr;
    snprintf(ui->title, sizeof(ui->title), "%s", title ? title : "");
    snprintf(ui->pose, sizeof(ui->pose), "pose_default");
    ui->backlight = 30;
    ui->list_sel = -1;
    ui->progress = -1;
    for (int i = 0; i < TEXT_ROWS; i++) ui->text_rows[i][0] = '\0';
    ui->status[0] = '\0';
    active_ui = ui;
    header_dirty = true;
    draw_chrome(ui);
    return ui;
}

void imp_ui_close(imp_ui_t *ui) {
    if (!ui) return;
    if (active_ui == ui) {
        active_ui = nullptr;
        imp_input_set_active(nullptr, nullptr);
    }
    free(ui);
}

void imp_ui_set_input_cb(imp_ui_t *ui, imp_input_cb_t cb, void *ctx) {
    (void)ui;
    imp_input_set_active(cb, ctx);
}

void imp_ui_set_backlight(imp_ui_t *ui, uint8_t pct) {
    ui->backlight = pct;
    disp_set_backlight(pct);
}

void imp_ui_set_pose(imp_ui_t *ui, const char *pose_id) {
    if (!pose_id || strcmp(ui->pose, pose_id) == 0) return;
    snprintf(ui->pose, sizeof(ui->pose), "%s", pose_id);
    const imp_sprite_t *spr = imp_sprite_find(pose_id);
    disp_fill_rect(IMP_MASCOT_X, IMP_MASCOT_Y, 82, 104, IMP_COLOUR_BG);
    disp_draw_sprite(IMP_MASCOT_X, IMP_MASCOT_Y, spr, IMP_COLOUR_RED, IMP_COLOUR_BG);
}

/* ---------------- list ---------------- */
/* Draw the visible row for item idx (window-relative positioning). */
static void draw_list_row(imp_ui_t *ui, int idx) {
    int row = idx - ui->list_offset;
    if (row < 0 || row >= IMP_LIST_MAX) return;
    int y = IMP_LIST_Y + row * IMP_LIST_ROW_H;
    bool sel = (idx == ui->list_sel);
    uint16_t bg = sel ? IMP_COLOUR_RED : IMP_COLOUR_BG;
    disp_fill_rect(IMP_CONTENT_X, y, IMP_CONTENT_W, IMP_LIST_ROW_H - 2, bg);
    disp_draw_text(IMP_CONTENT_X + 3, y + 7, ui->list_items[idx], IMP_COLOUR_FG, bg, 1);
}

static void draw_list_window(imp_ui_t *ui) {
    clear_content_slot();
    int end = ui->list_offset + IMP_LIST_MAX;
    if (end > ui->list_n) end = ui->list_n;
    for (int i = ui->list_offset; i < end; i++) draw_list_row(ui, i);
}

void imp_ui_list(imp_ui_t *ui, const char *const *items, int n, int sel) {
    if (n > LIST_CACHE) n = LIST_CACHE;
    if (ui->mode != 1) {
        ui->mode = 1;
        clear_content_slot();
        ui->list_valid = false;
    }
    bool items_changed = !ui->list_valid || ui->list_n != n;
    if (!items_changed) {
        for (int i = 0; i < n; i++)
            if (strcmp(ui->list_items[i], items[i]) != 0) { items_changed = true; break; }
    }
    int old_sel = ui->list_sel;
    int old_offset = ui->list_offset;
    /* keep the selection inside the visible window */
    int offset = old_offset;
    if (sel < offset) offset = sel;
    if (sel >= offset + IMP_LIST_MAX) offset = sel - IMP_LIST_MAX + 1;
    if (offset < 0) offset = 0;
    if (items_changed) {
        ui->list_n = n;
        for (int i = 0; i < n; i++) snprintf(ui->list_items[i], 16, "%s", items[i]);
        ui->list_sel = sel;
        ui->list_offset = offset;
        draw_list_window(ui);
        ui->list_valid = true;
    } else if (sel != old_sel || offset != old_offset) {
        ui->list_sel = sel;
        ui->list_offset = offset;
        if (offset != old_offset) {
            draw_list_window(ui);
        } else {
            if (old_sel >= 0 && old_sel < n) draw_list_row(ui, old_sel);
            if (sel >= 0 && sel < n) draw_list_row(ui, sel);
        }
    }
}

/* ---------------- values ---------------- */
void imp_ui_value(imp_ui_t *ui, const char *label, int32_t value, const char *unit) {
    if (ui->mode != 2) {
        ui->mode = 2;
        clear_content_slot();
        for (int i = 0; i < VALUE_SLOTS; i++) ui->values[i].used = false;
    }
    int slot = -1, free_slot = -1;
    for (int i = 0; i < VALUE_SLOTS; i++) {
        if (ui->values[i].used && strcmp(ui->values[i].label, label) == 0) { slot = i; break; }
        if (!ui->values[i].used && free_slot < 0) free_slot = i;
    }
    if (slot < 0) {
        if (free_slot < 0) return;
        slot = free_slot;
        ui->values[slot].used = true;
        snprintf(ui->values[slot].label, 16, "%s", label);
        ui->values[slot].rendered[0] = '\0';
        int y = IMP_LIST_Y + slot * 26;
        disp_draw_text(IMP_CONTENT_X + 2, y, label, IMP_COLOUR_DIM, IMP_COLOUR_BG, 1);
    }
    char buf[28];
    snprintf(buf, sizeof(buf), "%ld%s", (long)value, unit ? unit : "");
    if (strcmp(buf, ui->values[slot].rendered) == 0) return;
    snprintf(ui->values[slot].rendered, sizeof(ui->values[slot].rendered), "%s", buf);
    int y = IMP_LIST_Y + slot * 26 + 10;
    disp_fill_rect(IMP_CONTENT_X, y, IMP_CONTENT_W, 12, IMP_COLOUR_BG);
    disp_draw_text(IMP_CONTENT_X + 2, y, buf, IMP_COLOUR_FG, IMP_COLOUR_BG, 1);
}

/* ---------------- lower strip: text, status, progress ---------------- */
void imp_ui_text(imp_ui_t *ui, int row, const char *text) {
    if (row < 0 || row >= TEXT_ROWS) return;
    if (strcmp(ui->text_rows[row], text) == 0) return;
    snprintf(ui->text_rows[row], TEXT_ROW_W + 1, "%s", text);
    int y = 176 + row * 12;
    disp_fill_rect(2, y, IMP_DISP_W - 4, 10, IMP_COLOUR_BG);
    disp_draw_text(2, y + 1, ui->text_rows[row], IMP_COLOUR_FG, IMP_COLOUR_BG, 1);
}

void imp_ui_status(imp_ui_t *ui, const char *text) {
    if (strcmp(ui->status, text) == 0) return;
    snprintf(ui->status, sizeof(ui->status), "%s", text);
    int y = IMP_FOOT_Y - 14;
    disp_fill_rect(2, y, IMP_DISP_W - 4, 10, IMP_COLOUR_BG);
    disp_draw_text(2, y + 1, ui->status, IMP_COLOUR_ACCENT, IMP_COLOUR_BG, 1);
}

void imp_ui_progress(imp_ui_t *ui, uint8_t pct) {
    if (pct > 100) pct = 100;
    if (ui->progress == pct) return;
    ui->progress = pct;
    int y = IMP_FOOT_Y - 26, w = IMP_DISP_W - 4;
    disp_fill_rect(2, y, w, 7, IMP_COLOUR_BG);
    disp_fill_rect(2, y, (w * pct) / 100, 7, IMP_COLOUR_ACCENT);
}

/* ---------------- confirm (modal) ---------------- */
struct confirm_ctx { bool answered; bool yes; };
static void confirm_cb(void *ctx, imp_input_t ev) {
    confirm_ctx *c = (confirm_ctx *)ctx;
    if (ev == IMP_INPUT_OK) { c->answered = true; c->yes = true; }
    else if (ev == IMP_INPUT_BACK) { c->answered = true; c->yes = false; }
}

bool imp_ui_confirm(imp_ui_t *ui, const char *question) {
    /* box over the lower strip + content slot */
    disp_fill_rect(0, 150, IMP_DISP_W, 120, IMP_COLOUR_BG);
    disp_fill_rect(0, 150, IMP_DISP_W, 2, IMP_COLOUR_WARN);
    char q[64];
    snprintf(q, sizeof(q), "%s", question ? question : "");
    /* naive wrap at 28 chars */
    char line[TEXT_ROW_W + 1];
    size_t len = strlen(q), off = 0;
    int row = 0;
    while (off < len && row < 3) {
        size_t n = len - off > TEXT_ROW_W ? TEXT_ROW_W : len - off;
        memcpy(line, q + off, n); line[n] = '\0';
        disp_draw_text(2, 158 + row * 12, line, IMP_COLOUR_FG, IMP_COLOUR_BG, 1);
        off += n; row++;
    }
    disp_draw_text(2, 210, "OK = YES    BACK = NO", IMP_COLOUR_WARN, IMP_COLOUR_BG, 1);
    confirm_ctx c = {false, false};
    imp_input_set_modal(confirm_cb, &c);
    while (!c.answered && !imp_system_panic_requested()) {
        imp_input_pump();
        delay(5);
    }
    imp_input_clear_modal();
    /* Repaint whatever the box covered, from the caches. */
    disp_fill_rect(0, 150, IMP_DISP_W, 120, IMP_COLOUR_BG);
    if (ui->mode == 1) {
        for (int i = 0; i < ui->list_n; i++) draw_list_row(ui, i);
    } else if (ui->mode == 2) {
        for (int i = 0; i < VALUE_SLOTS; i++) {
            if (!ui->values[i].used) continue;
            int y = IMP_LIST_Y + i * 26;
            disp_draw_text(IMP_CONTENT_X + 2, y, ui->values[i].label, IMP_COLOUR_DIM, IMP_COLOUR_BG, 1);
            disp_draw_text(IMP_CONTENT_X + 2, y + 10, ui->values[i].rendered, IMP_COLOUR_FG, IMP_COLOUR_BG, 1);
        }
    }
    for (int r = 0; r < TEXT_ROWS; r++) {
        if (!ui->text_rows[r][0]) continue;
        int y = 176 + r * 12;
        disp_draw_text(2, y + 1, ui->text_rows[r], IMP_COLOUR_FG, IMP_COLOUR_BG, 1);
    }
    if (ui->status[0]) {
        int y = IMP_FOOT_Y - 14;
        disp_draw_text(2, y + 1, ui->status, IMP_COLOUR_ACCENT, IMP_COLOUR_BG, 1);
    }
    if (ui->progress >= 0) {
        int y = IMP_FOOT_Y - 26, w = IMP_DISP_W - 4;
        disp_fill_rect(2, y, (w * ui->progress) / 100, 7, IMP_COLOUR_ACCENT);
    }
    return c.yes;
}
