/* tool_archive.cpp — Archive tool: browse what the other tools saved
 * on the microSD, see how big a file is, delete it (confirmed).
 * Names come from imp_storage_list; the API carries no size, so the
 * detail screen measures the file by reading it once.
 */
#include "imp_api.h"
#include <string.h>
#include <stdio.h>

#define MAX_FILES 16 /* UI list cache limit */
static const char *archive_folders[] = {
    "/subghz", "/nfc", "/ir", "/badusb", "/flash", "/audit"
};
#define FOLDER_COUNT 6

typedef enum { SCR_FOLDERS = 0, SCR_FILES, SCR_DETAIL, SCR_NO_SD } archive_screen_t;

typedef struct {
    imp_ui_t *ui;
    bool running;
    archive_screen_t screen;
    int folder_sel;
    char files[MAX_FILES][48];
    int file_count;
    int file_sel;
    int action_sel;
    bool pending_delete;
    bool pending_measure;
    uint32_t detail_size;
    bool detail_measured;
} archive_tool_t;

static void collect_file_cb(void *ctx, const char *name) {
    archive_tool_t *s = (archive_tool_t *)ctx;
    if (s->file_count >= MAX_FILES) return;
    snprintf(s->files[s->file_count], 48, "%s", name);
    s->file_count++;
}

static const char *detail_items[] = {"View info", "Delete", "Back"};
#define DETAIL_ITEM_COUNT 3

static void archive_draw(archive_tool_t *s) {
    switch (s->screen) {
        case SCR_FOLDERS: {
            const char *items[FOLDER_COUNT];
            for (int i = 0; i < FOLDER_COUNT; i++) items[i] = archive_folders[i] + 1;
            imp_ui_list(s->ui, items, FOLDER_COUNT, s->folder_sel);
            break;
        }
        case SCR_FILES: {
            if (s->file_count == 0) {
                const char *empty[] = {"(folder empty)", "Back"};
                imp_ui_list(s->ui, empty, 2, s->file_sel);
            } else {
                const char *items[MAX_FILES];
                for (int i = 0; i < s->file_count; i++) items[i] = s->files[i];
                imp_ui_list(s->ui, items, s->file_count, s->file_sel);
            }
            break;
        }
        case SCR_DETAIL:
            imp_ui_list(s->ui, detail_items, DETAIL_ITEM_COUNT, s->action_sel);
            break;
        case SCR_NO_SD:
            break;
    }
}

static void archive_show_detail_text(archive_tool_t *s) {
    char line[32];
    snprintf(line, sizeof(line), "%s", s->files[s->file_sel]);
    imp_ui_text(s->ui, 0, line);
    snprintf(line, sizeof(line), "in %s", archive_folders[s->folder_sel]);
    imp_ui_text(s->ui, 1, line);
    if (s->detail_measured) {
        /* Text row, not imp_ui_value: the detail screen shows the
         * action list, and list/value share one content slot. */
        snprintf(line, sizeof(line), "Size: %lu bytes", (unsigned long)s->detail_size);
        imp_ui_text(s->ui, 2, line);
    }
}

/* Measure by reading: the storage API has no stat call. */
static uint32_t archive_measure(archive_tool_t *s) {
    char path[80];
    snprintf(path, sizeof(path), "%s/%s",
             archive_folders[s->folder_sel], s->files[s->file_sel]);
    imp_file_t *f = imp_storage_open(path, "r");
    if (!f) return 0;
    uint8_t buf[256];
    uint32_t total = 0;
    size_t n;
    while ((n = imp_storage_read(f, buf, sizeof(buf))) > 0) {
        total += (uint32_t)n;
        if (total > 16u * 1024u * 1024u) break; /* sanity cap */
    }
    imp_storage_close(f);
    return total;
}

static void archive_clear_detail(archive_tool_t *s) {
    imp_ui_text(s->ui, 0, "");
    imp_ui_text(s->ui, 1, "");
    imp_ui_text(s->ui, 2, "");
}

static void archive_on_input(void *ctx, imp_input_t ev) {
    archive_tool_t *s = (archive_tool_t *)ctx;
    switch (s->screen) {
        case SCR_NO_SD:
            if (ev == IMP_INPUT_BACK) s->running = false;
            break;
        case SCR_FOLDERS:
            if (ev == IMP_INPUT_ENC_CW) { s->folder_sel = (s->folder_sel + 1) % FOLDER_COUNT; archive_draw(s); }
            else if (ev == IMP_INPUT_ENC_CCW) { s->folder_sel = (s->folder_sel + FOLDER_COUNT - 1) % FOLDER_COUNT; archive_draw(s); }
            else if (ev == IMP_INPUT_BACK) s->running = false;
            else if (ev == IMP_INPUT_OK) {
                s->file_count = 0;
                s->file_sel = 0;
                imp_storage_list(archive_folders[s->folder_sel], collect_file_cb, s);
                s->screen = SCR_FILES;
                archive_draw(s);
            }
            break;
        case SCR_FILES: {
            int n = s->file_count > 0 ? s->file_count : 2;
            if (ev == IMP_INPUT_ENC_CW) { s->file_sel = (s->file_sel + 1) % n; archive_draw(s); }
            else if (ev == IMP_INPUT_ENC_CCW) { s->file_sel = (s->file_sel + n - 1) % n; archive_draw(s); }
            else if (ev == IMP_INPUT_BACK) { s->screen = SCR_FOLDERS; archive_draw(s); }
            else if (ev == IMP_INPUT_OK) {
                if (s->file_count == 0) {
                    if (s->file_sel == 1) { s->screen = SCR_FOLDERS; archive_draw(s); }
                } else {
                    s->action_sel = 0;
                    s->detail_measured = false;
                    s->pending_measure = true;
                    s->screen = SCR_DETAIL;
                    archive_draw(s);
                }
            }
            break;
        }
        case SCR_DETAIL:
            if (ev == IMP_INPUT_ENC_CW) { s->action_sel = (s->action_sel + 1) % DETAIL_ITEM_COUNT; archive_draw(s); }
            else if (ev == IMP_INPUT_ENC_CCW) { s->action_sel = (s->action_sel + DETAIL_ITEM_COUNT - 1) % DETAIL_ITEM_COUNT; archive_draw(s); }
            else if (ev == IMP_INPUT_BACK) { archive_clear_detail(s); s->screen = SCR_FILES; archive_draw(s); }
            else if (ev == IMP_INPUT_OK) {
                if (s->action_sel == 0) s->pending_measure = true;
                else if (s->action_sel == 1) s->pending_delete = true;
                else { archive_clear_detail(s); s->screen = SCR_FILES; archive_draw(s); }
            }
            break;
    }
}

int archive_main(void) {
    archive_tool_t s = {0};
    s.running = true;
    s.ui = imp_ui_open("Archive");
    if (!s.ui) return -1;
    imp_ui_set_input_cb(s.ui, archive_on_input, &s);
    imp_ui_set_pose(s.ui, "pose_archive");
    if (!imp_storage_sd_ready()) {
        s.screen = SCR_NO_SD;
        imp_ui_text(s.ui, 0, "No microSD fitted.");
        imp_ui_text(s.ui, 1, "The archive lives on");
        imp_ui_text(s.ui, 2, "the card - fit one and");
        imp_ui_text(s.ui, 3, "come back. BACK exits.");
        imp_ui_status(s.ui, "No card - BACK only");
    } else {
        s.screen = SCR_FOLDERS;
        imp_ui_status(s.ui, "Pick a folder");
        archive_draw(&s);
    }
    while (s.running) {
        if (s.pending_measure) {
            s.pending_measure = false;
            s.detail_size = archive_measure(&s);
            s.detail_measured = true;
            archive_show_detail_text(&s);
            archive_draw(&s);
        }
        if (s.pending_delete) {
            s.pending_delete = false;
            if (imp_ui_confirm(s.ui, "Delete this file for good?")) {
                char path[80];
                snprintf(path, sizeof(path), "%s/%s",
                         archive_folders[s.folder_sel], s.files[s.file_sel]);
                imp_err_t e = imp_storage_remove(path);
                imp_ui_status(s.ui, e == IMP_OK ? "Deleted" : "Delete failed");
                archive_clear_detail(&s);
                s.file_count = 0;
                s.file_sel = 0;
                imp_storage_list(archive_folders[s.folder_sel], collect_file_cb, &s);
                s.screen = SCR_FILES;
            } else {
                imp_ui_status(s.ui, "Kept it");
            }
            archive_draw(&s);
        }
        imp_delay_ms(30);
    }
    imp_system_set_led(0);
    imp_ui_close(s.ui);
    return 0;
}

static const imp_app_t archive_app = {
    "archive", "Archive", "Tools", "archive_10px", "pose_archive", "sd", 4096, archive_main
};
IMP_APP_REGISTER(archive_app);
