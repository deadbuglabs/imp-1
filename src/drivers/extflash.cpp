/* extflash.cpp — Flash Lab driver: external SPI NOR on the shared bus.
 *
 * Socketed chip on U2, CS = IO21, SPI mode 0. Two candidate chips, one
 * geometry (IMP-1 pack, Flash Lab):
 *   W25Q64      JEDEC EF 40 17
 *   MX25L6406E  JEDEC C2 20 17
 * Both: 8 MB, 256-byte pages, 4 KB sectors, 3.3 V only, never 5 V.
 * Any other RDID value: imp_flash_rdid still returns the raw ID (the
 * tool shows it and stops), but write/erase refuse with IMP_ERR_IO.
 *
 * Clock is 20 MHz, not the 40 MHz the chips allow — margin for the
 * socket, the pigtail-class routing and whichever chip lands in U2.
 *
 * WP# lock (J4): the jumper ties WP# so the block-protect bits engage.
 * Detection is the status register: SR1 bits 2-4 (BP0-BP2, mask 0x1C)
 * set at read time means protected ranges exist, and a WREN that fails
 * to set WEL (SR1 bit 1) is the same fact seen from the other side.
 * write/erase check SR1 first and return IMP_ERR_LOCKED.
 *
 * Bus discipline (pack section 4): imp_bus_lock covers one transaction
 * or one wait-free command pair (WREN + program/erase). It is never
 * held across the WIP poll — the poll takes and drops the lock per
 * RDSR and waits in imp_delay_ms slices.
 */
#include "imp_internal.h"
#include <string.h>

#define FLASH_SIZE_BYTES (8u * 1024u * 1024u)
#define FLASH_PAGE_BYTES 256u
#define FLASH_SECTOR_BYTES 4096u

#define FLASH_CMD_RDID 0x9F
#define FLASH_CMD_READ 0x03
#define FLASH_CMD_PAGE_PROGRAM 0x02
#define FLASH_CMD_WREN 0x06
#define FLASH_CMD_RDSR 0x05
#define FLASH_CMD_SECTOR_ERASE 0x20

#define FLASH_SR_WIP 0x01
#define FLASH_SR_WEL 0x02
#define FLASH_SR_BP_MASK 0x1C /* SR1 bits 2-4: BP0, BP1, BP2 */

static SPISettings flash_spi(20000000, MSBFIRST, SPI_MODE0);
static bool flash_cs_ready = false;

static void flash_cs_init(void) {
    if (flash_cs_ready) return;
    pinMode(IMP_PIN_FLASH_CS, OUTPUT);
    digitalWrite(IMP_PIN_FLASH_CS, HIGH);
    flash_cs_ready = true;
}

/* Raw single transactions. Caller holds imp_bus_lock. */
static void flash_txn_begin(void) {
    flash_cs_init();
    imp_bus_spi().beginTransaction(flash_spi);
    digitalWrite(IMP_PIN_FLASH_CS, LOW);
}
static void flash_txn_end(void) {
    digitalWrite(IMP_PIN_FLASH_CS, HIGH);
    imp_bus_spi().endTransaction();
}

static void flash_cmd_only(uint8_t cmd) {
    flash_txn_begin();
    imp_bus_spi().transfer(cmd);
    flash_txn_end();
}

static uint8_t flash_read_sr1_locked(void) {
    flash_txn_begin();
    imp_bus_spi().transfer(FLASH_CMD_RDSR);
    uint8_t sr = imp_bus_spi().transfer(0xFF);
    flash_txn_end();
    return sr;
}

static uint8_t flash_read_sr1(void) {
    imp_bus_lock();
    uint8_t sr = flash_read_sr1_locked();
    imp_bus_unlock();
    return sr;
}

static void flash_rdid_locked(uint8_t id[3]) {
    flash_txn_begin();
    imp_bus_spi().transfer(FLASH_CMD_RDID);
    for (int i = 0; i < 3; i++) id[i] = imp_bus_spi().transfer(0xFF);
    flash_txn_end();
}

static bool flash_id_known(const uint8_t id[3]) {
    return (id[0] == 0xEF && id[1] == 0x40 && id[2] == 0x17) || /* W25Q64 */
           (id[0] == 0xC2 && id[1] == 0x20 && id[2] == 0x17);   /* MX25L6406E */
}

/* Poll WIP in imp_delay_ms slices until the chip is ready or the
 * budget runs out. The bus lock is dropped between polls. */
static bool flash_wait_ready(uint32_t timeout_ms) {
    uint32_t start = millis();
    for (;;) {
        if ((flash_read_sr1() & FLASH_SR_WIP) == 0) return true;
        if (millis() - start >= timeout_ms) return false;
        imp_delay_ms(2);
    }
}

/* Gate for write/erase: known chip, and no block protection set. */
static imp_err_t flash_check_writable(void) {
    uint8_t id[3];
    imp_bus_lock();
    flash_rdid_locked(id);
    imp_bus_unlock();
    if (!flash_id_known(id)) return IMP_ERR_IO; /* unknown chip: stop and report */
    if (flash_read_sr1() & FLASH_SR_BP_MASK) return IMP_ERR_LOCKED;
    return IMP_OK;
}

/* WREN, then confirm WEL actually latched. If it did not, protection
 * is the reason: BP bits set -> LOCKED, anything else -> IO.
 * Leaves the bus lock RELEASED; the caller re-takes it for the
 * program/erase command itself. */
static imp_err_t flash_write_enable(void) {
    imp_bus_lock();
    flash_cmd_only(FLASH_CMD_WREN);
    uint8_t sr = flash_read_sr1_locked();
    imp_bus_unlock();
    if (sr & FLASH_SR_WEL) return IMP_OK;
    if (sr & FLASH_SR_BP_MASK) return IMP_ERR_LOCKED;
    return IMP_ERR_IO;
}

static bool flash_range_ok(uint32_t addr, size_t len) {
    if (addr >= FLASH_SIZE_BYTES) return len == 0 && addr <= FLASH_SIZE_BYTES;
    return len <= (size_t)(FLASH_SIZE_BYTES - addr);
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

imp_err_t imp_flash_rdid(uint8_t id[3]) {
    if (!id) return IMP_ERR_RANGE;
    imp_bus_lock();
    flash_rdid_locked(id);
    imp_bus_unlock();
    return IMP_OK; /* raw ID returned even when unknown — the tool decides */
}

imp_err_t imp_flash_read(uint32_t addr, uint8_t *buf, size_t len) {
    if (!buf && len > 0) return IMP_ERR_RANGE;
    if (!flash_range_ok(addr, len)) return IMP_ERR_RANGE;
    if (len == 0) return IMP_OK;
    imp_bus_lock();
    flash_txn_begin();
    SPIClass &spi = imp_bus_spi();
    spi.transfer(FLASH_CMD_READ);
    spi.transfer((addr >> 16) & 0xFF);
    spi.transfer((addr >> 8) & 0xFF);
    spi.transfer(addr & 0xFF);
    spi.transferBytes(nullptr, buf, (uint32_t)len); /* read-only: data-in may be NULL */
    flash_txn_end();
    imp_bus_unlock();
    return IMP_OK;
}

imp_err_t imp_flash_write(uint32_t addr, const uint8_t *buf, size_t len) {
    if (!buf && len > 0) return IMP_ERR_RANGE;
    if (!flash_range_ok(addr, len)) return IMP_ERR_RANGE;
    if (len == 0) return IMP_OK;
    imp_err_t gate = flash_check_writable();
    if (gate != IMP_OK) return gate;

    size_t done = 0;
    while (done < len) {
        uint32_t cur = addr + (uint32_t)done;
        /* Never cross a 256-byte page boundary in one program command:
         * the chip's address counter wraps inside the page. */
        size_t chunk = FLASH_PAGE_BYTES - (cur % FLASH_PAGE_BYTES);
        if (chunk > len - done) chunk = len - done;
        imp_err_t we = flash_write_enable();
        if (we != IMP_OK) return we;
        imp_bus_lock();
        flash_txn_begin();
        SPIClass &spi = imp_bus_spi();
        spi.transfer(FLASH_CMD_PAGE_PROGRAM);
        spi.transfer((cur >> 16) & 0xFF);
        spi.transfer((cur >> 8) & 0xFF);
        spi.transfer(cur & 0xFF);
        spi.writeBytes(buf + done, (uint32_t)chunk);
        flash_txn_end();
        imp_bus_unlock();
        if (!flash_wait_ready(500)) return IMP_ERR_IO; /* page program: ~3 ms worst case */
        done += chunk;
    }
    return IMP_OK;
}

imp_err_t imp_flash_erase(uint32_t addr, size_t len) {
    if (len == 0) return IMP_OK;
    if (!flash_range_ok(addr, len)) return IMP_ERR_RANGE;
    imp_err_t gate = flash_check_writable(); /* WP# lock checked first */
    if (gate != IMP_OK) return gate;

    /* Every 4 KB sector the range touches gets a sector erase. */
    uint32_t first = addr & ~(FLASH_SECTOR_BYTES - 1);
    uint32_t last = (addr + (uint32_t)len - 1) & ~(FLASH_SECTOR_BYTES - 1);
    for (uint32_t sector = first;; sector += FLASH_SECTOR_BYTES) {
        imp_err_t we = flash_write_enable();
        if (we != IMP_OK) return we;
        imp_bus_lock();
        flash_txn_begin();
        SPIClass &spi = imp_bus_spi();
        spi.transfer(FLASH_CMD_SECTOR_ERASE);
        spi.transfer((sector >> 16) & 0xFF);
        spi.transfer((sector >> 8) & 0xFF);
        spi.transfer(sector & 0xFF);
        flash_txn_end();
        imp_bus_unlock();
        if (!flash_wait_ready(3000)) return IMP_ERR_IO; /* sector erase: ~400 ms worst case */
        if (sector == last) break;
    }
    return IMP_OK;
}

imp_err_t imp_flash_dump(const char *path) {
    if (!path) return IMP_ERR_RANGE;
    imp_file_t *f = imp_storage_open(path, "w");
    if (!f) return IMP_ERR_IO;
    static uint8_t chunk[FLASH_SECTOR_BYTES];
    for (uint32_t addr = 0; addr < FLASH_SIZE_BYTES; addr += FLASH_SECTOR_BYTES) {
        imp_err_t e = imp_flash_read(addr, chunk, sizeof(chunk));
        if (e != IMP_OK) { imp_storage_close(f); return e; }
        if (imp_storage_write(f, chunk, sizeof(chunk)) != sizeof(chunk)) {
            imp_storage_close(f);
            return IMP_ERR_IO;
        }
        imp_delay_ms(1); /* yield so the UI thread stays alive between chunks */
    }
    imp_storage_close(f);
    return IMP_OK;
}
