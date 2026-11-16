/* imp_storage.cpp — microSD over the shared SPI bus (SD CS = IO3, D-10).
 * Captures, payloads, audit reports and tool state live on the card,
 * under each tool's own folder. State never lives in RAM (house rule 3). */
#include "imp_internal.h"
#include <SD.h>
#include <FS.h>

struct imp_file_s { File f; };

static bool sd_mounted = false;
static bool sd_tried = false;

static bool sd_mount(void) {
    if (sd_mounted) return true;
    if (sd_tried && !sd_mounted) {
        /* retry at most once per call site burst; mount is idempotent */
    }
    sd_tried = true;
    imp_bus_lock();
    sd_mounted = SD.begin(IMP_PIN_SD_CS, imp_bus_spi(), 4000000);
    imp_bus_unlock();
    return sd_mounted;
}

bool imp_storage_sd_ready(void) { return sd_mount(); }

imp_file_t *imp_storage_open(const char *path, const char *mode) {
    if (!sd_mount()) return nullptr;
    const char *m = FILE_READ;
    if (mode && mode[0] == 'w') m = FILE_WRITE;
    else if (mode && mode[0] == 'a') m = FILE_APPEND;
    /* create parent dirs on write paths */
    if (m != FILE_READ) {
        String p(path);
        int slash = p.lastIndexOf('/');
        if (slash > 0) SD.mkdir(p.substring(0, slash).c_str());
    }
    File f = SD.open(path, m);
    if (!f) return nullptr;
    imp_file_t *h = (imp_file_t *)malloc(sizeof(imp_file_t));
    if (!h) { f.close(); return nullptr; }
    h->f = f;
    return h;
}

size_t imp_storage_read(imp_file_t *f, void *buf, size_t len) {
    if (!f) return 0;
    return f->f.read((uint8_t *)buf, len);
}

size_t imp_storage_write(imp_file_t *f, const void *buf, size_t len) {
    if (!f) return 0;
    return f->f.write((const uint8_t *)buf, len);
}

void imp_storage_close(imp_file_t *f) {
    if (!f) return;
    f->f.flush();
    f->f.close();
    free(f);
}

bool imp_storage_exists(const char *path) {
    if (!sd_mount()) return false;
    return SD.exists(path);
}

imp_err_t imp_storage_remove(const char *path) {
    if (!sd_mount()) return IMP_ERR_IO;
    return SD.remove(path) ? IMP_OK : IMP_ERR_IO;
}

void imp_storage_list(const char *dir, imp_storage_list_cb_t cb, void *ctx) {
    if (!cb || !sd_mount()) return;
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) return;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
        if (!e.isDirectory()) {
            const char *name = e.name();
            const char *base = strrchr(name, '/');
            cb(ctx, base ? base + 1 : name);
        }
        e.close();
    }
    d.close();
}
