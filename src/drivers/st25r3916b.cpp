/* st25r3916b.cpp — real ST25R3916B driver implementing imp_nfc_*
 * (replaces stub_nfc.cpp).
 *
 * SPI: shared bus via imp_bus_spi(), mode 1 (CPOL=0, CPHA=1 — the
 * ST25R3916B samples MOSI on the falling edge), <= 10 MHz,
 * CS = IO47 (IMP_PIN_ST25_CS), IRQ = IO48 (IMP_PIN_ST25_IRQ, polled —
 * no attachInterrupt; interrupt status is read from the Main Interrupt
 * register, reading it clears it). Every SPI access is its own
 * transaction; the bus is never held across a wait. Frame waits poll
 * in imp_delay_ms(5) slices.
 *
 * SPI framing (datasheet section 4.1.2):
 *   register write  00xxxxxx, register read 01xxxxxx (0x40 | addr)
 *   register space B: prefix byte 0xFB, then a normal read/write frame
 *   FIFO load 0x80, FIFO read 0x9F, direct command = its code byte
 *   (0xC1 SET_DEFAULT, 0xC4 TRANSMIT_WITH_CRC, 0xC5 WITHOUT_CRC,
 *    0xC6 TRANSMIT_REQA, 0xC7 TRANSMIT_WUPA, 0xD0 MASK_RECEIVE_DATA,
 *    0xD1 UNMASK_RECEIVE_DATA, 0xD3 MEASURE_AMPLITUDE,
 *    0xD5 RESET_RX_GAIN, 0xD6 ADJUST_REGULATORS, 0xDB CLEAR_FIFO)
 *
 * FIELD / SESSION POLICY: the field is switched on by imp_nfc_poll()
 * (oscillator + regulators adjusted, Tx/Rx enabled) and stays on while
 * a tag session is live so read/write/dump can follow. A poll that
 * finds no tag switches the field back off before returning, and the
 * next poll always starts from a fresh field cycle (5 ms guard) so a
 * tag left ACTIVE by a previous session answers again. There is no
 * field-off entry point in the frozen API, so by documented driver
 * convention imp_nfc_poll(nullptr) means "end session, field off" —
 * the NFC tool calls it on exit; the panic hook calls the same path.
 *
 * CHIP ID: register 0x3F. The task brief says "0x2A-ish": 0x2A is the
 * plain ST25R3916 (ic_type 5). The fitted part is the B variant, whose
 * identity reads 0x31 (ic_type 6). The probe accepts ic_type 5 or 6
 * ((id >> 3) & 0x1F); anything else is IMP_ERR_NO_HW.
 *
 * MIFARE CLASSIC LIMITATION (bring-up item): poll/anticollision/UID
 * work, but Classic data access needs Crypto1 authentication, which is
 * out of scope for this driver pass. imp_nfc_read/write/dump on a
 * Classic tag return IMP_ERR_IO rather than pretending. NTAG (and
 * Ultralight-family) tags get full READ/WRITE/GET_VERSION access.
 */
#include "imp_internal.h"
#include <string.h>
#include <stdio.h>

/* ---------------- registers (space A unless noted) ---------------- */
#define ST_REG_IO_CONF1        0x00
#define ST_REG_IO_CONF2        0x01
#define ST_REG_OP_CONTROL      0x02
#define ST_REG_MODE_DEF        0x03
#define ST_REG_BITRATE         0x04
#define ST_REG_ISO14443A       0x05
#define ST_REG_AUX_DEF         0x0A
#define ST_REG_MASK_RX_TIMER   0x0F
#define ST_REG_NRT1            0x10
#define ST_REG_NRT2            0x11
#define ST_REG_MASK_MAIN_IRQ   0x16
#define ST_REG_MASK_TIMER_IRQ  0x17
#define ST_REG_MASK_ERROR_IRQ  0x18
#define ST_REG_MAIN_IRQ        0x1A
#define ST_REG_TIMER_IRQ       0x1B
#define ST_REG_ERROR_IRQ       0x1C
#define ST_REG_FIFO_STAT1      0x1E
#define ST_REG_FIFO_STAT2      0x1F
#define ST_REG_NTX1            0x22
#define ST_REG_NTX2            0x23
#define ST_REG_ADC_OUT         0x25
#define ST_REG_TX_DRIVER       0x28
#define ST_REG_REGULATOR       0x2C
#define ST_REG_AUX_DISPLAY     0x31
#define ST_REG_IC_IDENTITY     0x3F

/* direct commands (full first byte, 11xxxxxx pattern) */
#define ST_CMD_SET_DEFAULT         0xC1
#define ST_CMD_STOP_ALL            0xC2
#define ST_CMD_TRANSMIT_WITH_CRC   0xC4
#define ST_CMD_TRANSMIT_WO_CRC     0xC5
#define ST_CMD_TRANSMIT_REQA       0xC6
#define ST_CMD_TRANSMIT_WUPA       0xC7
#define ST_CMD_MASK_RECEIVE_DATA   0xD0
#define ST_CMD_UNMASK_RECEIVE_DATA 0xD1
#define ST_CMD_MEASURE_AMPLITUDE   0xD3
#define ST_CMD_RESET_RX_GAIN       0xD5
#define ST_CMD_ADJUST_REGULATORS   0xD6
#define ST_CMD_CLEAR_FIFO          0xDB

/* Main Interrupt register bits */
#define ST_IRQ_OSC   0x80
#define ST_IRQ_RXS   0x20
#define ST_IRQ_RXE   0x10
#define ST_IRQ_TXE   0x08
/* Timer/NFC Interrupt register bits */
#define ST_IRQ_DCT   0x80
#define ST_IRQ_NRE   0x40
/* Error/Wake-up Interrupt register bits (hard failures only;
 * I_err2 soft framing (0x20) is explicitly non-fatal) */
#define ST_ERR_FATAL 0xD0   /* I_crc | I_par | I_err1 */

#define ST_OP_EN     0x80
#define ST_OP_RX_EN  0x40
#define ST_OP_TX_EN  0x08

static SPISettings st_spi(10000000, MSBFIRST, SPI_MODE1);

static bool s_inited  = false;
static bool s_field_on = false;
static bool s_session  = false;          /* a tag is SELECTed and ACTIVE */
static bool s_hook_done = false;
static imp_nfc_tag_t s_tag;              /* the session tag */

/* ---------------- low-level SPI ---------------- */

static void st_write_reg(uint8_t addr, uint8_t val) {
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(st_spi);
    digitalWrite(IMP_PIN_ST25_CS, LOW);
    spi.transfer(addr & 0x3F);
    spi.transfer(val);
    digitalWrite(IMP_PIN_ST25_CS, HIGH);
    spi.endTransaction();
}

static uint8_t st_read_reg(uint8_t addr) {
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(st_spi);
    digitalWrite(IMP_PIN_ST25_CS, LOW);
    spi.transfer(0x40 | (addr & 0x3F));
    uint8_t v = spi.transfer(0x00);
    digitalWrite(IMP_PIN_ST25_CS, HIGH);
    spi.endTransaction();
    return v;
}

static void st_write_reg_b(uint8_t addr, uint8_t val) {
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(st_spi);
    digitalWrite(IMP_PIN_ST25_CS, LOW);
    spi.transfer(0xFB);                    /* register space B access */
    spi.transfer(addr & 0x3F);
    spi.transfer(val);
    digitalWrite(IMP_PIN_ST25_CS, HIGH);
    spi.endTransaction();
}

static uint8_t st_read_reg_b(uint8_t addr) {
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(st_spi);
    digitalWrite(IMP_PIN_ST25_CS, LOW);
    spi.transfer(0xFB);
    spi.transfer(0x40 | (addr & 0x3F));
    uint8_t v = spi.transfer(0x00);
    digitalWrite(IMP_PIN_ST25_CS, HIGH);
    spi.endTransaction();
    return v;
}

static void st_cmd(uint8_t cmd) {
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(st_spi);
    digitalWrite(IMP_PIN_ST25_CS, LOW);
    spi.transfer(cmd);
    digitalWrite(IMP_PIN_ST25_CS, HIGH);
    spi.endTransaction();
}

static void st_fifo_load(const uint8_t *data, int len) {
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(st_spi);
    digitalWrite(IMP_PIN_ST25_CS, LOW);
    spi.transfer(0x80);                    /* FIFO load */
    for (int i = 0; i < len; i++) spi.transfer(data[i]);
    digitalWrite(IMP_PIN_ST25_CS, HIGH);
    spi.endTransaction();
}

static void st_fifo_read(uint8_t *data, int len) {
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(st_spi);
    digitalWrite(IMP_PIN_ST25_CS, LOW);
    spi.transfer(0x9F);                    /* FIFO read */
    for (int i = 0; i < len; i++) data[i] = spi.transfer(0x00);
    digitalWrite(IMP_PIN_ST25_CS, HIGH);
    spi.endTransaction();
}

static int st_fifo_count(uint8_t *last_bits) {
    uint8_t s1 = st_read_reg(ST_REG_FIFO_STAT1);
    uint8_t s2 = st_read_reg(ST_REG_FIFO_STAT2);
    if (last_bits) *last_bits = (uint8_t)((s2 >> 1) & 0x07);
    return (int)s1 + (((int)(s2 >> 6)) << 8);
}

static void st_drain_irqs(void) {
    (void)st_read_reg(ST_REG_MAIN_IRQ);
    (void)st_read_reg(ST_REG_TIMER_IRQ);
    (void)st_read_reg(ST_REG_ERROR_IRQ);
    (void)st_read_reg(0x1D);
}

/* Wait for end-of-receive / no-response / fatal error. Polls the IRQ
 * pin and the interrupt registers in imp_delay_ms(5) slices; reading
 * the registers clears them, so results are accumulated. */
struct st_wait_res { bool rxe; bool nre; bool err; bool dct; };
static st_wait_res st_wait(uint32_t timeout_ms) {
    st_wait_res r = { false, false, false, false };
    uint32_t waited = 0;
    for (;;) {
        if (digitalRead(IMP_PIN_ST25_IRQ) == HIGH || waited > 0) {
            uint8_t m = st_read_reg(ST_REG_MAIN_IRQ);
            uint8_t t = st_read_reg(ST_REG_TIMER_IRQ);
            uint8_t e = st_read_reg(ST_REG_ERROR_IRQ);
            if (m & ST_IRQ_RXE) r.rxe = true;
            if (t & ST_IRQ_NRE) r.nre = true;
            if (t & ST_IRQ_DCT) r.dct = true;
            if (e & ST_ERR_FATAL) r.err = true;
            if (r.rxe || r.nre || r.err) return r;
        }
        if (waited >= timeout_ms) return r;
        imp_delay_ms(5);
        waited += 5;
    }
}

/* ---------------- field / init ---------------- */

static void st_field_off(void) {
    if (!s_inited) { s_field_on = false; s_session = false; return; }
    if (s_field_on) st_cmd(ST_CMD_MASK_RECEIVE_DATA);   /* stop RX decoder cleanly */
    st_write_reg(ST_REG_OP_CONTROL, 0x00);              /* field + oscillator off */
    s_field_on = false;
    s_session = false;
    imp_ui_radio_state(IMP_RADIO_NFC, false);
}

static void st_panic_hook(void) { st_field_off(); }

static imp_err_t st_field_on(void) {
    if (s_field_on) return IMP_OK;
    st_write_reg(ST_REG_OP_CONTROL, ST_OP_EN);          /* oscillator + regulators */
    /* wait for the crystal to stabilise (osc_ok in Auxiliary display) */
    bool ok = false;
    for (int i = 0; i < 25; i++) {
        if (st_read_reg(ST_REG_AUX_DISPLAY) & 0x10) { ok = true; break; }
        delay(1);
    }
    if (!ok) return IMP_ERR_IO;
    /* Adjust Regulators: reg_s must be toggled 1 -> 0 first (datasheet) */
    uint8_t reg = st_read_reg(ST_REG_REGULATOR);
    st_write_reg(ST_REG_REGULATOR, reg | 0x80);
    st_write_reg(ST_REG_REGULATOR, reg & ~0x80);
    st_drain_irqs();
    st_cmd(ST_CMD_ADJUST_REGULATORS);
    (void)st_wait(15);                                  /* I_dct on completion */
    st_write_reg(ST_REG_OP_CONTROL, ST_OP_EN | ST_OP_RX_EN | ST_OP_TX_EN);
    st_cmd(ST_CMD_UNMASK_RECEIVE_DATA);
    /* field sanity: amplitude measurement drives the antenna briefly
     * and leaves the result in the A/D output register */
    st_drain_irqs();
    st_cmd(ST_CMD_MEASURE_AMPLITUDE);
    (void)st_wait(10);
    (void)st_read_reg(ST_REG_ADC_OUT);
    st_drain_irqs();
    s_field_on = true;
    imp_ui_radio_state(IMP_RADIO_NFC, true);
    delay(5);                                           /* field-on guard before REQA */
    return IMP_OK;
}

static imp_err_t st_ensure_init(void) {
    pinMode(IMP_PIN_ST25_CS, OUTPUT);
    digitalWrite(IMP_PIN_ST25_CS, HIGH);
    pinMode(IMP_PIN_ST25_IRQ, INPUT);
    uint8_t id = st_read_reg(ST_REG_IC_IDENTITY);
    uint8_t type = (uint8_t)((id >> 3) & 0x1F);
    if (type != 5 && type != 6) return IMP_ERR_NO_HW;   /* 3916 / 3916B */
    if (s_inited) return IMP_OK;

    st_cmd(ST_CMD_SET_DEFAULT);
    delay(2);
    st_write_reg(ST_REG_IO_CONF1, 0x06);                /* MCU_CLK output disabled */
    uint8_t io2 = st_read_reg(ST_REG_IO_CONF2);
    st_write_reg(ST_REG_IO_CONF2, io2 | 0x80);          /* sup3V: 3.3 V supply range */
    st_write_reg(ST_REG_MODE_DEF, 0x08);                /* initiator, ISO14443A, OOK (100% ASK) */
    st_write_reg(ST_REG_BITRATE, 0x00);                 /* 106 kbit/s Tx and Rx */
    st_write_reg(ST_REG_ISO14443A, 0x00);               /* parity on, CRC on, antcl off */
    st_write_reg(ST_REG_AUX_DEF, 0x00);
    /* No-response timer: bring-up value, ~10 ms class at 106 kbit/s.
     * Antenna/receiver tuning registers (0x0B-0x0E, Tx driver 0x28)
     * deliberately keep their SET_DEFAULT values in this pass — they
     * are board/antenna specific and are a bench bring-up item; the
     * B variant renumbered the Tx driver register fields, so blind
     * 3916 values would be wrong there. */
    st_write_reg(ST_REG_NRT1, 0x08);
    st_write_reg(ST_REG_NRT2, 0x00);
    st_write_reg(ST_REG_MASK_MAIN_IRQ, 0x00);           /* unmask all */
    st_write_reg(ST_REG_MASK_TIMER_IRQ, 0x00);
    st_write_reg(ST_REG_MASK_ERROR_IRQ, 0x00);
    (void)st_read_reg_b(ST_REG_REGULATOR);              /* space B path exercised: regulator display */
    /* space B overshoot-protection config (B 0x30/0x31): write the
     * datasheet power-on defaults (all patterns off) — a no-op on a
     * healthy chip that also proves the space-B write path */
    st_write_reg_b(0x30, 0x00);
    st_write_reg_b(0x31, 0x00);
    st_drain_irqs();
    s_inited = true;
    if (!s_hook_done) {
        imp_system_register_panic_hook(st_panic_hook);
        s_hook_done = true;
    }
    return IMP_OK;
}

/* ---------------- ISO14443A transceive ---------------- */

/* One frame exchange. rx_last_bits reports valid bits in the final
 * FIFO byte (0 = full byte) — needed for the 4-bit NTAG ACK. */
static imp_err_t st_xceive(const uint8_t *tx, int txlen, bool with_crc, bool antcl,
                           uint8_t *rx, int *rxlen, uint8_t *rx_last_bits,
                           int rxcap, uint32_t timeout_ms) {
    *rxlen = 0;
    if (rx_last_bits) *rx_last_bits = 0;
    st_cmd(ST_CMD_CLEAR_FIFO);
    st_write_reg(ST_REG_ISO14443A, antcl ? 0x01 : 0x00);
    /* number of transmitted bytes: 13-bit byte count + broken bits */
    st_write_reg(ST_REG_NTX1, (uint8_t)((txlen >> 5) & 0xFF));
    st_write_reg(ST_REG_NTX2, (uint8_t)((txlen & 0x1F) << 3));
    st_fifo_load(tx, txlen);
    st_drain_irqs();
    st_cmd(with_crc ? ST_CMD_TRANSMIT_WITH_CRC : ST_CMD_TRANSMIT_WO_CRC);
    st_wait_res w = st_wait(timeout_ms);
    st_cmd(ST_CMD_RESET_RX_GAIN);
    if (!w.rxe || w.err) return IMP_ERR_IO;
    uint8_t lb = 0;
    int n = st_fifo_count(&lb);
    if (n <= 0) return IMP_ERR_IO;
    if (n > rxcap) n = rxcap;
    st_fifo_read(rx, n);
    *rxlen = n;
    if (rx_last_bits) *rx_last_bits = lb;
    return IMP_OK;
}

/* REQA (direct command) or, as fallback, WUPA — a tag left ACTIVE or
 * HALTed by an earlier session ignores REQA but answers WUPA. */
static imp_err_t st_request(uint16_t *atqa) {
    st_cmd(ST_CMD_RESET_RX_GAIN);
    st_drain_irqs();
    st_cmd(ST_CMD_TRANSMIT_REQA);
    st_wait_res w = st_wait(30);
    if (!w.rxe || w.err) {
        st_cmd(ST_CMD_RESET_RX_GAIN);
        st_drain_irqs();
        st_cmd(ST_CMD_TRANSMIT_WUPA);
        w = st_wait(30);
        if (!w.rxe || w.err) return IMP_ERR_IO;
    }
    uint8_t buf[2];
    int n = st_fifo_count(nullptr);
    if (n < 2) return IMP_ERR_IO;
    st_fifo_read(buf, 2);
    *atqa = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    return IMP_OK;
}

/* Full activation: REQA -> anticollision cascade -> SELECT per level.
 * Fills `tag`. Leaves the tag ACTIVE (the driver session). */
static imp_err_t st_activate(imp_nfc_tag_t *tag) {
    memset(tag, 0, sizeof(*tag));
    tag->type = IMP_NFC_TYPE_NONE;
    imp_err_t e = st_field_on();
    if (e != IMP_OK) return e;
    uint16_t atqa = 0;
    e = st_request(&atqa);
    if (e != IMP_OK) return e;
    tag->atqa = atqa;

    static const uint8_t sel_code[3] = { 0x93, 0x95, 0x97 };
    uint8_t sak = 0;
    for (int level = 0; level < 3; level++) {
        uint8_t tx[2] = { sel_code[level], 0x20 };
        uint8_t rx[8];
        int rxlen = 0;
        e = st_xceive(tx, 2, false, true, rx, &rxlen, nullptr, sizeof(rx), 30);
        if (e != IMP_OK || rxlen < 5) return IMP_ERR_IO;
        if (rx[4] != (uint8_t)(rx[0] ^ rx[1] ^ rx[2] ^ rx[3])) return IMP_ERR_IO;  /* BCC */
        int take = (rx[0] == 0x88) ? 3 : 4;             /* 0x88 = cascade tag */
        int off = (rx[0] == 0x88) ? 1 : 0;
        for (int i = 0; i < take && tag->uid_len < 10; i++)
            tag->uid[tag->uid_len++] = rx[off + i];
        uint8_t sel[9] = { sel_code[level], 0x70, rx[0], rx[1], rx[2], rx[3], rx[4], 0, 0 };
        uint8_t srx[4];
        int slen = 0;
        e = st_xceive(sel, 7, true, false, srx, &slen, nullptr, sizeof(srx), 30);
        if (e != IMP_OK || slen < 1) return IMP_ERR_IO;
        sak = srx[0];
        if (!(sak & 0x04)) break;                       /* UID complete */
        if (level == 2) return IMP_ERR_IO;
    }
    tag->sak = sak;
    if (sak == 0x00 && atqa == 0x0044) tag->type = IMP_NFC_TYPE_NTAG;
    else if (sak == 0x08 || sak == 0x09 || sak == 0x18) tag->type = IMP_NFC_TYPE_MIFARE_CLASSIC;
    else if (sak == 0x00) tag->type = IMP_NFC_TYPE_ISO14443A_OTHER;
    else tag->type = IMP_NFC_TYPE_ISO14443A_OTHER;
    s_tag = *tag;
    s_session = true;
    return IMP_OK;
}

static bool st_same_tag(const imp_nfc_tag_t *a, const imp_nfc_tag_t *b) {
    return a->uid_len == b->uid_len && memcmp(a->uid, b->uid, a->uid_len) == 0;
}

/* read/write/dump need the live session from the last poll; if it was
 * lost (tag power-cycled, field cycled), re-activate once and require
 * the same UID before touching data. */
static imp_err_t st_ensure_session(const imp_nfc_tag_t *tag) {
    if (!tag || tag->uid_len == 0) return IMP_ERR_IO;
    if (s_session && st_same_tag(&s_tag, tag)) return IMP_OK;
    imp_nfc_tag_t found;
    imp_err_t e = st_activate(&found);
    if (e != IMP_OK) return e;
    return st_same_tag(&found, tag) ? IMP_OK : IMP_ERR_IO;
}

/* ---------------- NTAG primitives ---------------- */

/* NTAG READ: 16 bytes starting at page `page` (pages page..page+3). */
static imp_err_t st_ntag_read_page16(uint8_t page, uint8_t *buf16) {
    uint8_t tx[2] = { 0x30, page };
    uint8_t rx[20];
    int rxlen = 0;
    imp_err_t e = st_xceive(tx, 2, true, false, rx, &rxlen, nullptr, sizeof(rx), 40);
    if (e != IMP_OK || rxlen < 16) return IMP_ERR_IO;
    memcpy(buf16, rx, 16);
    return IMP_OK;
}

static imp_err_t st_ntag_version(uint8_t *storage_byte) {
    uint8_t tx[1] = { 0x60 };                           /* GET_VERSION */
    uint8_t rx[12];
    int rxlen = 0;
    imp_err_t e = st_xceive(tx, 1, true, false, rx, &rxlen, nullptr, sizeof(rx), 40);
    if (e != IMP_OK || rxlen < 7) return IMP_ERR_IO;
    *storage_byte = rx[6];
    return IMP_OK;
}

/* ---------------- API ---------------- */

imp_err_t imp_nfc_poll(imp_nfc_tag_t *tag) {
    if (!tag) {                                         /* documented: end session, field off */
        if (s_inited) st_field_off();
        return IMP_OK;
    }
    imp_err_t e = st_ensure_init();
    if (e != IMP_OK) return e;
    if (s_session || s_field_on) {                      /* fresh field cycle per poll */
        st_field_off();
        delay(5);
    }
    e = st_activate(tag);
    if (e != IMP_OK) {
        st_field_off();
        tag->type = IMP_NFC_TYPE_NONE;
        return IMP_ERR_IO;                              /* empty field is normal, not a fault */
    }
    return IMP_OK;
}

imp_err_t imp_nfc_read(const imp_nfc_tag_t *tag, uint16_t block, uint8_t *buf) {
    if (!tag || !buf) return IMP_ERR_IO;
    imp_err_t e = st_ensure_init();
    if (e != IMP_OK) return e;
    if (tag->type != IMP_NFC_TYPE_NTAG) return IMP_ERR_IO;  /* Classic: needs Crypto1 auth (see header) */
    if (block > 0xFF) return IMP_ERR_RANGE;
    e = st_ensure_session(tag);
    if (e != IMP_OK) return e;
    /* contract: block is a page index; one call returns the 16 bytes
     * of pages block..block+3 */
    return st_ntag_read_page16((uint8_t)block, buf);
}

imp_err_t imp_nfc_write(const imp_nfc_tag_t *tag, uint16_t block, const uint8_t *buf) {
    if (!tag || !buf) return IMP_ERR_IO;
    imp_err_t e = st_ensure_init();
    if (e != IMP_OK) return e;
    if (tag->type != IMP_NFC_TYPE_NTAG) return IMP_ERR_IO;  /* Classic: needs Crypto1 auth (see header) */
    if (block > 0xFF) return IMP_ERR_RANGE;
    e = st_ensure_session(tag);
    if (e != IMP_OK) return e;
    /* NTAG WRITE (0xA2): one page, the first 4 bytes of buf; the tag
     * answers with a 4-bit ACK (0xA) in the last FIFO byte's LSBs. */
    uint8_t tx[6] = { 0xA2, (uint8_t)block, buf[0], buf[1], buf[2], buf[3] };
    uint8_t rx[4];
    int rxlen = 0;
    uint8_t lb = 0;
    e = st_xceive(tx, 6, true, false, rx, &rxlen, &lb, sizeof(rx), 40);
    if (e != IMP_OK || rxlen < 1) return IMP_ERR_IO;
    if (!((lb == 4 && (rx[0] & 0x0F) == 0x0A) || (lb == 0 && rx[0] == 0x0A)))
        return IMP_ERR_IO;                              /* NAK or silence */
    return IMP_OK;
}

imp_err_t imp_nfc_dump(const imp_nfc_tag_t *tag, const char *path) {
    if (!tag || !path) return IMP_ERR_IO;
    imp_err_t e = st_ensure_init();
    if (e != IMP_OK) return e;
    if (tag->type != IMP_NFC_TYPE_NTAG) return IMP_ERR_IO;  /* Classic: needs Crypto1 auth (see header) */
    e = st_ensure_session(tag);
    if (e != IMP_OK) return e;

    /* Page count from GET_VERSION storage size; fall back to the
     * NTAG213 default (45 pages) and stop at the first read error. */
    int total_pages = 45;
    uint8_t storage = 0;
    if (st_ntag_version(&storage) == IMP_OK) {
        if (storage == 0x0F) total_pages = 45;          /* NTAG213 */
        else if (storage == 0x11) total_pages = 135;    /* NTAG215 */
        else if (storage == 0x13) total_pages = 231;    /* NTAG216 */
    }

    imp_file_t *f = imp_storage_open(path, "w");
    if (!f) return IMP_ERR_IO;
    char line[96];
    int n = snprintf(line, sizeof(line), "IMP-1 NFC DUMP\nTYPE NTAG\nUID ");
    for (int i = 0; i < tag->uid_len; i++)
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%02X", tag->uid[i]);
    n += snprintf(line + n, sizeof(line) - (size_t)n, "\nATQA %04X SAK %02X\nPAGES %d\n",
                  (unsigned)tag->atqa, (unsigned)tag->sak, total_pages);
    imp_storage_write(f, line, (size_t)n);

    imp_err_t result = IMP_OK;
    for (int page = 0; page < total_pages; page += 4) {
        uint8_t buf16[IMP_NFC_BLOCK_SIZE];
        if (st_ntag_read_page16((uint8_t)page, buf16) != IMP_OK) { result = IMP_ERR_IO; break; }
        for (int p = 0; p < 4 && page + p < total_pages; p++) {
            int m = snprintf(line, sizeof(line), "P%03d: %02X %02X %02X %02X\n",
                             page + p, buf16[p * 4], buf16[p * 4 + 1],
                             buf16[p * 4 + 2], buf16[p * 4 + 3]);
            imp_storage_write(f, line, (size_t)m);
        }
    }
    imp_storage_close(f);
    return result;
}
