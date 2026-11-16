/* badusb.cpp — imp_badusb_* : native USB personalities on IO19/IO20.
 *
 * One personality at a time (keyboard / storage / serial). The Arduino
 * TinyUSB stack enumerates ONCE per boot: every device object below is
 * file-static so its interface registers at static-init, before any
 * USB.begin(). imp_badusb_present() begins the requested device and
 * calls USB.begin(); the first call enumerates, later mode switches
 * re-run begin() but the host usually keeps the first enumeration —
 * a re-plug (or reboot) may be needed on a mode change. That is USB
 * reality on this framework, not a driver shortcut.
 *
 * Storage mode is a 64 KB RAM disk (see msc_alloc). An SD-backed MSC
 * needs block-device plumbing shared with imp_storage's SPI session;
 * that lands at bring-up, not here.
 */
#include "imp_internal.h"
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBMSC.h"
#include "esp_heap_caps.h"
#include <ctype.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Devices (static: interface registration happens at static-init)     */
/* ------------------------------------------------------------------ */
static USBHIDKeyboard Keyboard;
static USBMSC Msc;

static imp_badusb_mode_t cur_mode = IMP_BADUSB_SERIAL;
static bool usb_begun = false;
static bool kbd_begun = false;
static bool msc_begun = false;

/* ------------------------------------------------------------------ */
/* RAM disk for storage mode: 128 x 512 B = 64 KB                      */
/* ------------------------------------------------------------------ */
#define MSC_SECTORS 128
#define MSC_SECTOR_SIZE 512
static uint8_t *msc_disk = nullptr; /* PSRAM first, DRAM fallback */

static int32_t imp_msc_read_fn(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    if (!msc_disk) return -1;
    uint32_t addr = lba * MSC_SECTOR_SIZE + offset;
    if (addr + bufsize > MSC_SECTORS * MSC_SECTOR_SIZE) return -1;
    memcpy(buffer, msc_disk + addr, bufsize);
    return (int32_t)bufsize;
}

static int32_t imp_msc_write_fn(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    if (!msc_disk) return -1;
    uint32_t addr = lba * MSC_SECTOR_SIZE + offset;
    if (addr + bufsize > MSC_SECTORS * MSC_SECTOR_SIZE) return -1;
    memcpy(msc_disk + addr, buffer, bufsize);
    return (int32_t)bufsize;
}

/* Minimal valid FAT12 so hosts mount instead of offering to format:
 * sector 0 boot, sector 1 FAT, sector 2 root dir, data from sector 3
 * (cluster 2 = README.TXT). Built in code, not a giant literal. */
static void msc_format(void) {
    memset(msc_disk, 0, MSC_SECTORS * MSC_SECTOR_SIZE);
    uint8_t *b = msc_disk;
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b + 3, "MSDOS5.0", 8);
    b[11] = MSC_SECTOR_SIZE & 0xFF; b[12] = MSC_SECTOR_SIZE >> 8;
    b[13] = 1;                              /* sectors per cluster */
    b[14] = 1; b[15] = 0;                   /* reserved sectors */
    b[16] = 1;                              /* FAT count */
    b[17] = 16; b[18] = 0;                  /* root entries */
    b[19] = MSC_SECTORS & 0xFF; b[20] = MSC_SECTORS >> 8;
    b[21] = 0xF8;                           /* media */
    b[22] = 1; b[23] = 0;                   /* sectors per FAT */
    b[24] = 1; b[25] = 0;                   /* sectors per track */
    b[26] = 1; b[27] = 0;                   /* heads */
    b[38] = 0x29;                           /* extended boot sig */
    b[39] = 0x34; b[40] = 0x12;             /* serial 0x1234 */
    memcpy(b + 43, "IMP-1      ", 11);
    memcpy(b + 54, "FAT12   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    uint8_t *fat = msc_disk + MSC_SECTOR_SIZE;
    fat[0] = 0xF8; fat[1] = 0xFF; fat[2] = 0xFF; /* cluster 2 = EOF */
    static const char readme[] =
        "IMP-1 BadUSB storage mode.\r\n"
        "This is a small RAM disk (64 KB) - it is wiped on reboot.\r\n"
        "Payloads live on the microSD under /badusb/, not here.\r\n";
    uint8_t *root = msc_disk + 2 * MSC_SECTOR_SIZE;
    memcpy(root, "README  TXT", 11);
    root[11] = 0x20;                        /* archive */
    root[26] = 2; root[27] = 0;             /* first cluster */
    uint32_t sz = sizeof(readme) - 1;
    root[28] = sz & 0xFF; root[29] = (sz >> 8) & 0xFF;
    memcpy(msc_disk + 3 * MSC_SECTOR_SIZE, readme, sz);
}

static bool msc_alloc(void) {
    if (msc_disk) return true;
    msc_disk = (uint8_t *)heap_caps_malloc(MSC_SECTORS * MSC_SECTOR_SIZE,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!msc_disk)
        msc_disk = (uint8_t *)heap_caps_malloc(MSC_SECTORS * MSC_SECTOR_SIZE,
                                               MALLOC_CAP_8BIT);
    if (!msc_disk) return false;
    msc_format();
    return true;
}

/* ------------------------------------------------------------------ */
/* Key combos                                                          */
/* ------------------------------------------------------------------ */
static bool token_is_modifier(const char *t, uint8_t *code) {
    if (!strcmp(t, "CTRL") || !strcmp(t, "CONTROL")) { *code = KEY_LEFT_CTRL; return true; }
    if (!strcmp(t, "SHIFT")) { *code = KEY_LEFT_SHIFT; return true; }
    if (!strcmp(t, "ALT")) { *code = KEY_LEFT_ALT; return true; }
    if (!strcmp(t, "GUI") || !strcmp(t, "WIN") || !strcmp(t, "WINDOWS") ||
        !strcmp(t, "CMD") || !strcmp(t, "COMMAND") || !strcmp(t, "META")) {
        *code = KEY_LEFT_GUI; return true;
    }
    return false;
}

static bool token_key_code(const char *t, uint8_t *code) {
    if (strlen(t) == 1) { *code = (uint8_t)tolower((unsigned char)t[0]); return true; }
    if (!strcmp(t, "ENTER") || !strcmp(t, "RETURN")) { *code = KEY_RETURN; return true; }
    if (!strcmp(t, "TAB")) { *code = KEY_TAB; return true; }
    if (!strcmp(t, "ESC") || !strcmp(t, "ESCAPE")) { *code = KEY_ESC; return true; }
    if (!strcmp(t, "DELETE") || !strcmp(t, "DEL")) { *code = KEY_DELETE; return true; }
    if (!strcmp(t, "BACKSPACE")) { *code = KEY_BACKSPACE; return true; }
    if (!strcmp(t, "SPACE")) { *code = ' '; return true; }
    if (!strcmp(t, "UP") || !strcmp(t, "UPARROW")) { *code = KEY_UP_ARROW; return true; }
    if (!strcmp(t, "DOWN") || !strcmp(t, "DOWNARROW")) { *code = KEY_DOWN_ARROW; return true; }
    if (!strcmp(t, "LEFT") || !strcmp(t, "LEFTARROW")) { *code = KEY_LEFT_ARROW; return true; }
    if (!strcmp(t, "RIGHT") || !strcmp(t, "RIGHTARROW")) { *code = KEY_RIGHT_ARROW; return true; }
    if (!strcmp(t, "HOME")) { *code = KEY_HOME; return true; }
    if (!strcmp(t, "END")) { *code = KEY_END; return true; }
    if (!strcmp(t, "INSERT")) { *code = KEY_INSERT; return true; }
    if (!strcmp(t, "PAGEUP")) { *code = KEY_PAGE_UP; return true; }
    if (!strcmp(t, "PAGEDOWN")) { *code = KEY_PAGE_DOWN; return true; }
    if (!strcmp(t, "CAPSLOCK")) { *code = KEY_CAPS_LOCK; return true; }
    if (t[0] == 'F' && t[1]) {
        int n = atoi(t + 1);
        if (n >= 1 && n <= 12) { *code = (uint8_t)(KEY_F1 + (n - 1)); return true; }
    }
    return false;
}

/* Press a set of already-split, upper-cased tokens together. */
static imp_err_t press_tokens(char tokens[][16], int n) {
    if (n <= 0) return IMP_ERR_RANGE;
    bool any = false;
    for (int i = 0; i < n; i++) {
        uint8_t code;
        if (token_is_modifier(tokens[i], &code) || token_key_code(tokens[i], &code)) {
            Keyboard.press(code);
            any = true;
        }
    }
    if (!any) return IMP_ERR_RANGE;
    imp_delay_ms(30);
    Keyboard.releaseAll();
    return IMP_OK;
}

/* Split a combo on '+' and/or spaces into upper-cased tokens. */
static int split_combo(const char *combo, char tokens[][16], int max_tokens) {
    int n = 0;
    const char *p = combo;
    while (*p && n < max_tokens) {
        while (*p == '+' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int len = 0;
        while (*p && *p != '+' && *p != ' ' && *p != '\t' && len < 15) {
            tokens[n][len++] = (char)toupper((unsigned char)*p);
            p++;
        }
        while (*p && *p != '+' && *p != ' ' && *p != '\t') p++; /* overlong tail */
        tokens[n][len] = '\0';
        if (len) n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */
imp_err_t imp_badusb_present(imp_badusb_mode_t mode) {
    if (mode != IMP_BADUSB_KEYBOARD && mode != IMP_BADUSB_STORAGE &&
        mode != IMP_BADUSB_SERIAL)
        return IMP_ERR_RANGE;
    cur_mode = mode;
    if (mode == IMP_BADUSB_KEYBOARD) {
        if (!kbd_begun) { Keyboard.begin(); kbd_begun = true; }
    } else if (mode == IMP_BADUSB_STORAGE) {
        if (!msc_begun) {
            if (!msc_alloc()) return IMP_ERR_IO;
            Msc.vendorID("IMP-1");
            Msc.productID("BadUSB RAM disk");
            Msc.productRevision("1.0");
            Msc.onRead(imp_msc_read_fn);
            Msc.onWrite(imp_msc_write_fn);
            if (!Msc.begin(MSC_SECTORS, MSC_SECTOR_SIZE)) return IMP_ERR_IO;
            Msc.mediaPresent(true);
            msc_begun = true;
        }
    }
    /* Serial mode needs no device: the default CDC is already the
     * framework's boot console. present() just records the mode. */
    USB.begin();
    usb_begun = true;
    imp_delay_ms(50); /* let enumeration start before the tool types */
    return IMP_OK;
}

imp_err_t imp_badusb_type(const char *text) {
    if (!text) return IMP_ERR_RANGE;
    if (cur_mode != IMP_BADUSB_KEYBOARD || !usb_begun) return IMP_ERR_IO;
    Keyboard.print(text);
    return IMP_OK;
}

imp_err_t imp_badusb_key(const char *combo) {
    if (!combo || !*combo) return IMP_ERR_RANGE;
    if (cur_mode != IMP_BADUSB_KEYBOARD || !usb_begun) return IMP_ERR_IO;
    char tokens[8][16];
    int n = split_combo(combo, tokens, 8);
    return press_tokens(tokens, n);
}

/* ------------------------------------------------------------------ */
/* DuckyScript-class payload interpreter                               */
/* ------------------------------------------------------------------ */
/* Streaming line reader over imp_storage (payloads are small; a
 * 256-byte window keeps RAM flat). */
typedef struct {
    imp_file_t *f;
    char buf[256];
    size_t len, pos;
    bool eof;
} line_reader_t;

static bool lr_fill(line_reader_t *r) {
    if (r->pos < r->len) return true;
    if (r->eof) return false;
    r->len = imp_storage_read(r->f, r->buf, sizeof(r->buf));
    r->pos = 0;
    if (r->len == 0) { r->eof = true; return false; }
    return true;
}

/* Next line without trailing CR/LF. Returns false at EOF. */
static bool lr_next(line_reader_t *r, char *out, size_t outsz) {
    size_t n = 0;
    bool any = false;
    while (lr_fill(r)) {
        char c = r->buf[r->pos++];
        any = true;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n + 1 < outsz) out[n++] = c;
    }
    out[n] = '\0';
    return any;
}

static char *skip_ws(char *p) { while (*p == ' ' || *p == '\t') p++; return p; }

/* First word upper-cased into kw; returns pointer past it. */
static char *split_keyword(char *line, char *kw, size_t kwsz) {
    char *p = skip_ws(line);
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t' && n + 1 < kwsz) {
        kw[n++] = (char)toupper((unsigned char)*p);
        p++;
    }
    kw[n] = '\0';
    return skip_ws(p);
}

static bool line_is_key_combo(char *line) {
    /* Every token a modifier/key name, at least one modifier present:
     * "GUI r", "CTRL ALT DELETE", "ALT F4". */
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s", line);
    char tokens[8][16];
    int n = split_combo(tmp, tokens, 8);
    if (n < 2) return false;
    bool has_mod = false;
    for (int i = 0; i < n; i++) {
        uint8_t code;
        if (token_is_modifier(tokens[i], &code)) { has_mod = true; continue; }
        if (!token_key_code(tokens[i], &code)) return false;
    }
    return has_mod;
}

typedef struct {
    uint32_t default_delay_ms;
    char prev[256];
    bool have_prev;
} ducky_t;

static void ducky_exec_line(ducky_t *d, char *line, int depth);

static void ducky_exec_line(ducky_t *d, char *line, int depth) {
    char *p = skip_ws(line);
    if (!*p) return;
    char kw[24];
    char *rest = split_keyword(p, kw, sizeof(kw));

    if (!strcmp(kw, "REM")) return;
    if (!strcmp(kw, "STRING")) { Keyboard.print(rest); return; }
    if (!strcmp(kw, "STRINGLN")) {
        Keyboard.print(rest);
        Keyboard.press(KEY_RETURN);
        imp_delay_ms(20);
        Keyboard.releaseAll();
        return;
    }
    if (!strcmp(kw, "DELAY")) {
        uint32_t ms = (uint32_t)strtoul(rest, nullptr, 10);
        if (ms > 60000) ms = 60000;
        imp_delay_ms(ms);
        return;
    }
    if (!strcmp(kw, "DEFAULT_DELAY") || !strcmp(kw, "DEFAULTDELAY")) {
        d->default_delay_ms = (uint32_t)strtoul(rest, nullptr, 10);
        return;
    }
    if (!strcmp(kw, "REPEAT")) {
        if (depth > 0 || !d->have_prev) return; /* no nested REPEAT */
        int n = atoi(rest);
        if (n < 0) n = 0;
        if (n > 500) n = 500;
        for (int i = 0; i < n; i++) {
            char copy[256];
            snprintf(copy, sizeof(copy), "%s", d->prev);
            ducky_exec_line(d, copy, depth + 1);
            imp_delay_ms(d->default_delay_ms ? d->default_delay_ms : 10);
            if (imp_system_panic_requested()) return;
        }
        return;
    }
    /* Bare key name (ENTER, TAB, F4...) or a modifier combo line. */
    {
        uint8_t code;
        char one[24];
        snprintf(one, sizeof(one), "%s", kw);
        char *tail = skip_ws(rest);
        if (!*tail && token_key_code(one, &code)) {
            Keyboard.press(code);
            imp_delay_ms(20);
            Keyboard.releaseAll();
            return;
        }
        if (line_is_key_combo(p)) {
            char tokens[8][16];
            int n = split_combo(p, tokens, 8);
            press_tokens(tokens, n);
            return;
        }
    }
    /* Unknown line: skipped by design, never fatal. */
}

imp_err_t imp_badusb_run(const char *path) {
    if (!path) return IMP_ERR_RANGE;
    if (cur_mode != IMP_BADUSB_KEYBOARD || !usb_begun) return IMP_ERR_IO;
    imp_file_t *f = imp_storage_open(path, "r");
    if (!f) return IMP_ERR_IO;
    line_reader_t r = {f, {0}, 0, 0, false};
    ducky_t d = {0, {0}, false};
    char line[256];
    while (lr_next(&r, line, sizeof(line))) {
        char kw[24];
        char probe[256];
        snprintf(probe, sizeof(probe), "%s", line);
        split_keyword(probe, kw, sizeof(kw));
        bool is_repeat = !strcmp(kw, "REPEAT");
        ducky_exec_line(&d, line, 0);
        if (!is_repeat && skip_ws(line)[0]) {
            snprintf(d.prev, sizeof(d.prev), "%s", line);
            d.have_prev = true;
        }
        /* Yield between lines: input pump + panic stay live. */
        imp_delay_ms(d.default_delay_ms ? d.default_delay_ms : 10);
        if (imp_system_panic_requested()) break;
    }
    imp_storage_close(f);
    Keyboard.releaseAll();
    return IMP_OK;
}
