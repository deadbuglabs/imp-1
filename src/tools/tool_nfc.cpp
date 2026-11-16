/* tool_nfc.cpp — NFC tool (ST25R3916B via imp_nfc_*).
 * Idle screen polls for a tag; on a tag it shows type + UID, reads the
 * first pages as hex, dumps a read-only backup to /nfc/, and — NTAG
 * only — can write one fixed test page (page 4, the first user page)
 * after imp_ui_confirm() with a re-read verify. MIFARE Classic data
 * access is not offered: the driver has no Crypto1 auth in this pass,
 * and the screen says so in plain words instead of failing silently.
 * Exit drops the field via the driver's documented
 * imp_nfc_poll(nullptr) end-session call. */
#include "imp_api.h"
#include <string.h>
#include <stdio.h>

enum { NFC_IDLE = 0, NFC_TAG };
enum { ACT_NONE = 0, ACT_DUMP, ACT_WRITE_TEST };

typedef struct {
    imp_ui_t *ui;
    bool running;
    int mode;
    int act;
    bool just_found;     /* main loop: fill the tag screen once */
    imp_nfc_tag_t tag;
    int file_count;
} nfc_state_t;

static nfc_state_t s;

static void list_count_cb(void *ctx, const char *name) {
    nfc_state_t *st = (nfc_state_t *)ctx;
    (void)name;
    st->file_count++;
}

static const char *type_name(imp_nfc_type_t t) {
    switch (t) {
        case IMP_NFC_TYPE_NTAG: return "NTAG";
        case IMP_NFC_TYPE_MIFARE_CLASSIC: return "MIFARE Classic";
        case IMP_NFC_TYPE_ISO14443A_OTHER: return "ISO14443A other";
        default: return "No tag";
    }
}

static void nfc_on_input(void *ctx, imp_input_t ev) {
    nfc_state_t *st = (nfc_state_t *)ctx;
    switch (ev) {
        case IMP_INPUT_OK:
            if (st->mode == NFC_TAG) st->act = ACT_DUMP;
            break;
        case IMP_INPUT_KEY_1:
            if (st->mode == NFC_TAG) st->act = ACT_WRITE_TEST;
            break;
        case IMP_INPUT_BACK:
            if (st->mode == NFC_TAG) {
                st->mode = NFC_IDLE;
                for (int r = 0; r <= 6; r++) imp_ui_text(st->ui, r, "");
                imp_ui_status(st->ui, "Hold a tag to the coil");
            } else st->running = false;
            break;
        default: break;
    }
}

/* Fill the tag screen: identity rows, then pages 0..3 as hex rows. */
static void nfc_show_tag(nfc_state_t *st) {
    char line[40];
    imp_ui_text(st->ui, 0, type_name(st->tag.type));
    char uid[24];
    int pos = 0;
    for (int i = 0; i < st->tag.uid_len && pos < 20; i++)
        pos += snprintf(uid + pos, sizeof(uid) - (size_t)pos, "%02X", st->tag.uid[i]);
    uid[pos] = '\0';
    snprintf(line, sizeof(line), "UID %s", uid);
    imp_ui_text(st->ui, 1, line);
    snprintf(line, sizeof(line), "ATQA %04X SAK %02X",
             (unsigned)st->tag.atqa, (unsigned)st->tag.sak);
    imp_ui_text(st->ui, 2, line);

    if (st->tag.type == IMP_NFC_TYPE_MIFARE_CLASSIC) {
        imp_ui_text(st->ui, 3, "Data needs card keys");
        imp_ui_text(st->ui, 4, "Not in this build");
    } else {
        uint8_t buf[IMP_NFC_BLOCK_SIZE];
        imp_err_t e = imp_nfc_read(&st->tag, 0, buf);
        if (e == IMP_OK) {
            for (int p = 0; p < 4; p++) {
                snprintf(line, sizeof(line), "P%d: %02X%02X%02X%02X", p,
                         buf[p * 4], buf[p * 4 + 1], buf[p * 4 + 2], buf[p * 4 + 3]);
                imp_ui_text(st->ui, 3 + p, line);
            }
        } else {
            imp_ui_text(st->ui, 3, "First pages unreadable");
        }
    }
    imp_ui_status(st->ui, "OK dump - 1 test write");
}

static void nfc_do_dump(nfc_state_t *st) {
    if (st->tag.type == IMP_NFC_TYPE_MIFARE_CLASSIC) {
        imp_ui_status(st->ui, "MIFARE dump not in this build");
        return;
    }
    if (st->tag.type != IMP_NFC_TYPE_NTAG) {
        imp_ui_status(st->ui, "Dump is for NTAG tags");
        return;
    }
    if (!imp_storage_sd_ready()) { imp_ui_status(st->ui, "No microSD - cannot dump"); return; }
    st->file_count = 0;
    imp_storage_list("/nfc", list_count_cb, st);
    char path[48];
    int idx = st->file_count + 1;
    do {
        snprintf(path, sizeof(path), "/nfc/dump%03d.txt", idx++);
    } while (imp_storage_exists(path) && idx < 1000);
    imp_ui_status(st->ui, "Dumping - hold tag still");
    imp_ui_progress(st->ui, 10);
    imp_err_t e = imp_nfc_dump(&st->tag, path);
    imp_ui_progress(st->ui, e == IMP_OK ? 100 : 0);
    if (e == IMP_OK) {
        char msg[40];
        snprintf(msg, sizeof(msg), "Saved %s", path + 5);
        imp_ui_status(st->ui, msg);
    } else if (e == IMP_ERR_NO_HW) {
        imp_ui_status(st->ui, "No NFC reader fitted");
    } else {
        imp_ui_status(st->ui, "Dump failed - hold tag still");
    }
}

static void nfc_do_write_test(nfc_state_t *st) {
    if (st->tag.type != IMP_NFC_TYPE_NTAG) {
        imp_ui_status(st->ui, "Test write is NTAG only");
        return;
    }
    if (!imp_ui_confirm(st->ui, "Write test page 4?")) {
        imp_ui_status(st->ui, "Write cancelled");
        return;
    }
    /* fixed test pattern on page 4 — the first user-data page; the
     * UID/CC pages (0..3) are never written by this tool */
    uint8_t buf[IMP_NFC_BLOCK_SIZE] = { 'I', 'M', 'P', '1' };
    imp_err_t e = imp_nfc_write(&st->tag, 4, buf);
    if (e != IMP_OK) { imp_ui_status(st->ui, "Write failed"); return; }
    uint8_t back[IMP_NFC_BLOCK_SIZE];
    e = imp_nfc_read(&st->tag, 4, back);
    if (e == IMP_OK && memcmp(back, buf, 4) == 0) {
        imp_ui_status(st->ui, "Write verified on page 4");
        char line[40];
        snprintf(line, sizeof(line), "P4: %02X%02X%02X%02X", back[0], back[1], back[2], back[3]);
        imp_ui_text(st->ui, 3, line);
    } else {
        imp_ui_status(st->ui, "Write check failed");
    }
}

int nfc_main(void) {
    memset(&s, 0, sizeof(s));
    s.running = true;
    s.mode = NFC_IDLE;
    s.ui = imp_ui_open("NFC");
    if (!s.ui) return -1;
    imp_ui_set_input_cb(s.ui, nfc_on_input, &s);
    imp_ui_set_pose(s.ui, "pose_nfc");
    imp_ui_status(s.ui, "Hold a tag to the coil");

    bool no_hw_shown = false;
    while (s.running) {
        if (s.act == ACT_DUMP) { s.act = ACT_NONE; nfc_do_dump(&s); }
        else if (s.act == ACT_WRITE_TEST) { s.act = ACT_NONE; nfc_do_write_test(&s); }

        if (s.mode == NFC_IDLE) {
            imp_err_t e = imp_nfc_poll(&s.tag);
            if (e == IMP_OK) {
                s.mode = NFC_TAG;
                s.just_found = true;
                no_hw_shown = false;
            } else if (e == IMP_ERR_NO_HW) {
                if (!no_hw_shown) {
                    imp_ui_status(s.ui, "No NFC reader fitted");
                    no_hw_shown = true;
                }
                imp_delay_ms(200);   /* soldered part — retry slowly */
                continue;
            }
            if (s.just_found) {
                s.just_found = false;
                nfc_show_tag(&s);
                imp_ui_set_pose(s.ui, "pose_success");
            }
            imp_delay_ms(20);
        } else {
            imp_delay_ms(30);
        }
    }

    /* exit: end the session and drop the field (driver convention) */
    imp_nfc_poll(nullptr);
    imp_ui_close(s.ui);
    return 0;
}

static const imp_app_t nfc_app = {
    "nfc", "NFC", "NFC", "nfc_10px", "pose_nfc", "nfc", 8192, nfc_main
};
IMP_APP_REGISTER(nfc_app);
