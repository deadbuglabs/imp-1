/* tool_flashlab.cpp — Flash Lab tool for the socketed external NOR.
 *
 * First action on entry is always RDID. A known chip (W25Q64 /
 * MX25L6406E) opens the menu; any other ID is a plain-words stop
 * screen where only BACK works — an unidentified chip is never
 * written to, per the pack.
 *
 * Standing rules honoured here:
 *  - Dump first: Erase and Write refuse until /flash/dump.bin exists.
 *  - Erase and Write both pass through imp_ui_confirm().
 *  - Every write is verified by re-reading it in this tool.
 *  - IMP_ERR_LOCKED is shown as the J4 WP# jumper, in plain words.
 * The dump runs as a chunked imp_flash_read loop in this tool (not
 * imp_flash_dump) so the progress bar is real; the driver-side dump
 * stays in the contract for other callers.
 *
 * As in the IR tool, the input callback only records intent; RDID,
 * confirms and all flash traffic run in the main loop.
 */
#include "imp_api.h"
#include <string.h>
#include <stdio.h>

#define FLASH_SIZE_BYTES (8u * 1024u * 1024u)
#define FLASH_SECTOR_BYTES 4096u
#define DUMP_PATH "/flash/dump.bin"

enum fl_mode_t { FL_MODE_MENU = 0, FL_MODE_PICK, FL_MODE_STOPPED };
enum fl_pick_t { FL_PICK_NONE = 0, FL_PICK_READ, FL_PICK_ERASE, FL_PICK_WRITE };
enum fl_action_t { FL_ACT_NONE = 0, FL_ACT_OK, FL_ACT_BACK };

typedef struct {
    imp_ui_t *ui;
    bool running;
    int mode;
    int sel;         /* menu or picker selection */
    int pending;     /* fl_action_t recorded by the callback */
    int pick;        /* fl_pick_t: what the picker is choosing for */
    bool dump_abort; /* BACK pressed while a dump is running */
    uint8_t id[3];
    const char *chip_name; /* nullptr = unknown ID */
    uint8_t rw_buf[FLASH_SECTOR_BYTES];
} fl_tool_t;

/* Static: the 4 KB chunk buffer plus the rest does not belong on the
 * tool's 8 KB stack next to the UI's own buffers. */
static fl_tool_t s;

static const char *fl_menu_items[] = {
    "Chip info", "Dump to SD", "Read range", "Erase sector", "Write test"
};
#define FL_MENU_COUNT 5

/* The small offset list the pickers choose from. All sector-aligned. */
static const uint32_t fl_offsets[] = {0x000000, 0x001000, 0x7FF000};
static const char *fl_offset_labels[] = {"0x000000", "0x001000", "0x7FF000"};
#define FL_OFFSET_COUNT 3

/* 16-byte write-test pattern: "IMP-1 WRITE TEST" */
static const uint8_t fl_test_pattern[16] = {
    'I', 'M', 'P', '-', '1', ' ', 'W', 'R', 'I', 'T', 'E', ' ', 'T', 'E', 'S', 'T'
};

static void fl_text(fl_tool_t *t, int row, const char *text) { imp_ui_text(t->ui, row, text); }

static void fl_clear_text(fl_tool_t *t) {
    for (int i = 0; i < 4; i++) imp_ui_text(t->ui, i, "");
}

static void fl_show_id(fl_tool_t *t, int row) {
    char buf[28];
    snprintf(buf, sizeof(buf), "ID: %02X %02X %02X", t->id[0], t->id[1], t->id[2]);
    fl_text(t, row, buf);
}

static bool fl_dump_exists(void) { return imp_storage_exists(DUMP_PATH); }

/* Dump-first gate shared by Erase and Write. True = may proceed. */
static bool fl_require_dump(fl_tool_t *t) {
    if (fl_dump_exists()) return true;
    fl_clear_text(t);
    fl_text(t, 0, "Dump the original first.");
    fl_text(t, 1, "Run Dump to SD, then retry.");
    imp_ui_status(t->ui, "No dump yet - refused");
    return false;
}

static void fl_locked_msg(fl_tool_t *t) {
    fl_clear_text(t);
    fl_text(t, 0, "WP# jumper is locked (J4).");
    fl_text(t, 1, "Move the J4 jumper to write.");
    imp_ui_status(t->ui, "Flash is write-locked");
}

/* ------------------------------------------------------------------ */
/* Menu actions (main loop only)                                       */
/* ------------------------------------------------------------------ */

static void fl_chip_info(fl_tool_t *t) {
    fl_clear_text(t);
    fl_text(t, 0, t->chip_name);
    fl_show_id(t, 1);
    fl_text(t, 2, "Size: 8 MB");
    fl_text(t, 3, "Page 256 B, sector 4 KB");
    imp_ui_status(t->ui, "Socketed flash on U2");
}

static void fl_dump(fl_tool_t *t) {
    if (!imp_storage_sd_ready()) {
        fl_clear_text(t);
        fl_text(t, 0, "No microSD card mounted.");
        fl_text(t, 1, "Dump needs the card.");
        imp_ui_status(t->ui, "No microSD - cannot dump");
        return;
    }
    imp_file_t *f = imp_storage_open(DUMP_PATH, "w");
    if (!f) {
        imp_ui_status(t->ui, "Could not open dump file");
        return;
    }
    fl_clear_text(t);
    fl_text(t, 0, "Dumping chip to SD...");
    fl_text(t, 1, "BACK stops the dump.");
    imp_ui_status(t->ui, "Dumping - do not power off");
    t->dump_abort = false;
    imp_err_t err = IMP_OK;
    for (uint32_t addr = 0; addr < FLASH_SIZE_BYTES; addr += FLASH_SECTOR_BYTES) {
        if (t->dump_abort) { err = IMP_ERR_BUSY; break; }
        err = imp_flash_read(addr, t->rw_buf, FLASH_SECTOR_BYTES);
        if (err != IMP_OK) break;
        if (imp_storage_write(f, t->rw_buf, FLASH_SECTOR_BYTES) != FLASH_SECTOR_BYTES) {
            err = IMP_ERR_IO;
            break;
        }
        imp_ui_progress(t->ui, (uint8_t)((addr + FLASH_SECTOR_BYTES) * 100 / FLASH_SIZE_BYTES));
        imp_delay_ms(1); /* yield between chunks; also pumps input for BACK */
    }
    imp_storage_close(f);
    t->pending = FL_ACT_NONE; /* presses during the dump belonged to the dump */
    if (err == IMP_OK) {
        fl_clear_text(t);
        fl_text(t, 0, "Dump saved:");
        fl_text(t, 1, "flash/dump.bin, 8 MB");
        imp_ui_status(t->ui, "Dump complete");
    } else {
        /* A partial dump must not satisfy the dump-first rule. */
        imp_storage_remove(DUMP_PATH);
        imp_ui_progress(t->ui, 0);
        fl_clear_text(t);
        fl_text(t, 0, t->dump_abort ? "Dump stopped." : "Dump failed.");
        fl_text(t, 1, "Partial file was removed.");
        imp_ui_status(t->ui, t->dump_abort ? "Dump stopped" : "Dump failed - card or chip");
    }
}

static void fl_open_pick(fl_tool_t *t, int pick) {
    if (pick != FL_PICK_READ && !fl_require_dump(t)) return;
    t->pick = pick;
    t->mode = FL_MODE_PICK;
    t->sel = 0;
    imp_ui_status(t->ui, pick == FL_PICK_READ ? "Pick a read offset"
                        : pick == FL_PICK_ERASE ? "Pick a sector to erase"
                                                : "Pick a write offset");
}

static void fl_read_range(fl_tool_t *t, uint32_t addr) {
    uint8_t buf[16];
    if (imp_flash_read(addr, buf, sizeof(buf)) != IMP_OK) {
        fl_clear_text(t);
        fl_text(t, 0, "Read failed.");
        fl_text(t, 1, "Check the flash chip.");
        imp_ui_status(t->ui, "Read failed");
        return;
    }
    char row[28];
    fl_clear_text(t);
    snprintf(row, sizeof(row), "Addr 0x%06lX", (unsigned long)addr);
    fl_text(t, 0, row);
    snprintf(row, sizeof(row), "%02X %02X %02X %02X %02X %02X %02X %02X",
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
    fl_text(t, 1, row);
    snprintf(row, sizeof(row), "%02X %02X %02X %02X %02X %02X %02X %02X",
             buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
    fl_text(t, 2, row);
    imp_ui_status(t->ui, "Read 16 bytes");
}

static void fl_erase_sector(fl_tool_t *t, uint32_t addr) {
    char q[48];
    snprintf(q, sizeof(q), "Erase 4K sector at 0x%06lX?", (unsigned long)addr);
    if (!imp_ui_confirm(t->ui, q)) {
        imp_ui_status(t->ui, "Erase cancelled");
        return;
    }
    imp_ui_status(t->ui, "Erasing...");
    imp_err_t e = imp_flash_erase(addr, FLASH_SECTOR_BYTES);
    if (e == IMP_ERR_LOCKED) { fl_locked_msg(t); return; }
    fl_clear_text(t);
    if (e == IMP_OK) {
        char row[28];
        snprintf(row, sizeof(row), "Erased sector 0x%06lX", (unsigned long)addr);
        fl_text(t, 0, row);
        fl_text(t, 1, "It now reads back as FF.");
        imp_ui_status(t->ui, "Sector erased");
    } else {
        fl_text(t, 0, "Erase failed.");
        fl_text(t, 1, "Check the flash chip.");
        imp_ui_status(t->ui, "Erase failed");
    }
}

static void fl_write_test(fl_tool_t *t, uint32_t addr) {
    char q[48];
    snprintf(q, sizeof(q), "Write test pattern at 0x%06lX?", (unsigned long)addr);
    if (!imp_ui_confirm(t->ui, q)) {
        imp_ui_status(t->ui, "Write cancelled");
        return;
    }
    imp_ui_status(t->ui, "Writing...");
    imp_err_t e = imp_flash_write(addr, fl_test_pattern, sizeof(fl_test_pattern));
    if (e == IMP_ERR_LOCKED) { fl_locked_msg(t); return; }
    fl_clear_text(t);
    if (e != IMP_OK) {
        fl_text(t, 0, "Write failed.");
        fl_text(t, 1, "Check the flash chip.");
        imp_ui_status(t->ui, "Write failed");
        return;
    }
    /* Pack rule: every write is verified by re-reading it here. */
    uint8_t back[16];
    bool pass = imp_flash_read(addr, back, sizeof(back)) == IMP_OK &&
                memcmp(back, fl_test_pattern, sizeof(back)) == 0;
    char row[28];
    snprintf(row, sizeof(row), "Write test 0x%06lX", (unsigned long)addr);
    fl_text(t, 0, row);
    fl_text(t, 1, pass ? "Verify: PASS" : "Verify: FAIL");
    if (!pass) fl_text(t, 2, "Erase the sector, retry.");
    imp_ui_status(t->ui, pass ? "Write verified" : "Write did not verify");
}

/* ------------------------------------------------------------------ */
/* Input + main loop                                                   */
/* ------------------------------------------------------------------ */

static void fl_handle_action(fl_tool_t *t, int act) {
    if (t->mode == FL_MODE_STOPPED) {
        if (act == FL_ACT_BACK) t->running = false; /* the only way out */
        return;
    }
    if (act == FL_ACT_BACK) {
        if (t->mode == FL_MODE_PICK) {
            t->mode = FL_MODE_MENU;
            t->pick = FL_PICK_NONE;
            t->sel = 0;
            imp_ui_status(t->ui, "Pick a Flash Lab action");
        } else {
            t->running = false;
        }
        return;
    }
    if (act != FL_ACT_OK) return;
    if (t->mode == FL_MODE_PICK) {
        uint32_t addr = fl_offsets[t->sel];
        int pick = t->pick;
        t->mode = FL_MODE_MENU;
        t->pick = FL_PICK_NONE;
        t->sel = 0;
        if (pick == FL_PICK_READ) fl_read_range(t, addr);
        else if (pick == FL_PICK_ERASE) fl_erase_sector(t, addr);
        else if (pick == FL_PICK_WRITE) fl_write_test(t, addr);
        return;
    }
    switch (t->sel) {
        case 0: fl_chip_info(t); break;
        case 1: fl_dump(t); break;
        case 2: fl_open_pick(t, FL_PICK_READ); break;
        case 3: fl_open_pick(t, FL_PICK_ERASE); break;
        case 4: fl_open_pick(t, FL_PICK_WRITE); break;
        default: break;
    }
}

static void fl_on_input(void *ctx, imp_input_t ev) {
    fl_tool_t *t = (fl_tool_t *)ctx;
    int n = (t->mode == FL_MODE_PICK) ? FL_OFFSET_COUNT : FL_MENU_COUNT;
    switch (ev) {
        case IMP_INPUT_ENC_CW:
            if (t->mode != FL_MODE_STOPPED) t->sel = (t->sel + 1) % n;
            break;
        case IMP_INPUT_ENC_CCW:
            if (t->mode != FL_MODE_STOPPED) t->sel = (t->sel + n - 1) % n;
            break;
        case IMP_INPUT_OK:
            t->pending = FL_ACT_OK;
            break;
        case IMP_INPUT_BACK:
            t->pending = FL_ACT_BACK;
            t->dump_abort = true; /* a dump in progress sees this first */
            break;
        default: break;
    }
}

static void fl_refresh(fl_tool_t *t) {
    if (t->mode == FL_MODE_MENU)
        imp_ui_list(t->ui, fl_menu_items, FL_MENU_COUNT, t->sel);
    else if (t->mode == FL_MODE_PICK)
        imp_ui_list(t->ui, fl_offset_labels, FL_OFFSET_COUNT, t->sel);
}

int flashlab_main(void) {
    memset(&s, 0, sizeof(s));
    s.running = true;
    s.mode = FL_MODE_MENU;
    s.ui = imp_ui_open("Flash Lab");
    if (!s.ui) return -1;
    imp_ui_set_input_cb(s.ui, fl_on_input, &s);
    imp_ui_set_pose(s.ui, "pose_archive");

    /* First action: RDID. The chip decides whether the menu opens. */
    imp_flash_rdid(s.id);
    if (s.id[0] == 0xEF && s.id[1] == 0x40 && s.id[2] == 0x17) s.chip_name = "Winbond W25Q64";
    else if (s.id[0] == 0xC2 && s.id[1] == 0x20 && s.id[2] == 0x17) s.chip_name = "Macronix MX25L6406E";

    if (!s.chip_name) {
        s.mode = FL_MODE_STOPPED;
        fl_text(&s, 0, "Unknown flash ID - stopped");
        fl_show_id(&s, 1);
        fl_text(&s, 2, "Expected W25Q64 or");
        fl_text(&s, 3, "MX25L6406E. Check U2.");
        imp_ui_status(s.ui, "Only BACK works");
    } else {
        char msg[40];
        snprintf(msg, sizeof(msg), "%s found", s.chip_name);
        imp_ui_status(s.ui, msg);
        fl_refresh(&s);
    }

    while (s.running) {
        if (s.pending != FL_ACT_NONE) {
            int act = s.pending;
            s.pending = FL_ACT_NONE;
            /* During a dump the loop is inside fl_dump(), not here, so a
             * pending BACK at this level is a real navigation press. */
            s.dump_abort = false;
            fl_handle_action(&s, act);
        }
        fl_refresh(&s);
        imp_delay_ms(30);
    }
    imp_system_set_led(0);
    imp_ui_close(s.ui);
    return 0;
}

static const imp_app_t flashlab_app = {
    "flashlab", "Flash Lab", "Tools", "flash_10px", "pose_archive", "flash", 8192, flashlab_main
};
IMP_APP_REGISTER(flashlab_app);
