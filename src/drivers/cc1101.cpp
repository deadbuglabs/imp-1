/* cc1101.cpp — real CC1101 driver implementing imp_subghz_* (replaces
 * stub_subghz.cpp).
 *
 * HARDWARE LIMITATION (frozen design, read before touching the capture
 * path): the CC1101 GDO0/GDO2 pins are NOT connected on the IMP-1 board,
 * so there is no hardware data/clock line to sample for true OOK edge
 * timing. The honest capture path available on this wiring is packet
 * mode: the chip is configured for OOK/ASK, fixed-length packet FIFO
 * receive at 4.8 kBaud, and timings_us[] is derived from run-lengths of
 * the received bit stream at that known baud (1 bit = 208 us). Edge
 * resolution is therefore one bit period, and a "capture" is an
 * RSSI-gated chunk of the demodulated stream — noise below the gate is
 * drained and discarded. TX is the inverse: the timing list is rebuilt
 * into a bit stream at the same baud and sent as one packet-mode FIFO
 * transmission. RSSI comes from the RSSI status register with the
 * standard CC1101 conversion (offset 74 dB at this data rate).
 *
 * SPI: shared bus via imp_bus_spi(), mode 0, <= 8 MHz, CS = IO18
 * (IMP_PIN_CC_CS), manual CS per the pack. The bus is never held
 * across a wait; every register access is its own transaction.
 *
 * Hardware-absent tolerance: if PARTNUM/VERSION do not read back as a
 * CC1101, every entry point returns IMP_ERR_NO_HW (the firmware is
 * built and run without the board attached).
 */
#include "imp_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------------- CC1101 register / command map ---------------- */
#define CC_REG_IOCFG2    0x00
#define CC_REG_IOCFG1    0x01
#define CC_REG_IOCFG0    0x02
#define CC_REG_FIFOTHR   0x03
#define CC_REG_SYNC1     0x04
#define CC_REG_SYNC0     0x05
#define CC_REG_PKTLEN    0x06
#define CC_REG_PKTCTRL1  0x07
#define CC_REG_PKTCTRL0  0x08
#define CC_REG_ADDR      0x09
#define CC_REG_CHANNR    0x0A
#define CC_REG_FSCTRL1   0x0B
#define CC_REG_FSCTRL0   0x0C
#define CC_REG_FREQ2     0x0D
#define CC_REG_FREQ1     0x0E
#define CC_REG_FREQ0     0x0F
#define CC_REG_MDMCFG4   0x10
#define CC_REG_MDMCFG3   0x11
#define CC_REG_MDMCFG2   0x12
#define CC_REG_MDMCFG1   0x13
#define CC_REG_MDMCFG0   0x14
#define CC_REG_DEVIATN   0x15
#define CC_REG_MCSM2     0x16
#define CC_REG_MCSM1     0x17
#define CC_REG_MCSM0     0x18
#define CC_REG_FOCCFG    0x19
#define CC_REG_BSCFG     0x1A
#define CC_REG_AGCCTRL2  0x1B
#define CC_REG_AGCCTRL1  0x1C
#define CC_REG_AGCCTRL0  0x1D
#define CC_REG_FREND1    0x21
#define CC_REG_FREND0    0x22
#define CC_REG_FSCAL3    0x23
#define CC_REG_FSCAL2    0x24
#define CC_REG_FSCAL1    0x25
#define CC_REG_FSCAL0    0x26
#define CC_REG_TEST2     0x2C
#define CC_REG_TEST1     0x2D
#define CC_REG_TEST0     0x2E
#define CC_REG_PATABLE   0x3E
#define CC_REG_FIFO      0x3F

/* command strobes */
#define CC_SRES          0x30
#define CC_SFSTXON       0x31
#define CC_SXOFF         0x32
#define CC_SCAL          0x33
#define CC_SRX           0x34
#define CC_STX           0x35
#define CC_SIDLE         0x36
#define CC_SPWD          0x39
#define CC_SFRX          0x3A
#define CC_SFTX          0x3B
#define CC_SNOP          0x3D

/* status registers (read with burst bit set) */
#define CC_ST_PARTNUM    0x30
#define CC_ST_VERSION    0x31
#define CC_ST_RSSI       0x34
#define CC_ST_MARCSTATE  0x35
#define CC_ST_TXBYTES    0x3A
#define CC_ST_RXBYTES    0x3B

#define CC_MARC_IDLE          0x01
#define CC_MARC_RX            0x0D
#define CC_MARC_TX            0x13
#define CC_MARC_RX_OVERFLOW   0x11
#define CC_MARC_TX_UNDERFLOW  0x16

#define CC_XTAL_HZ       26000000UL
#define CC_BAUD          4800UL
#define CC_BIT_US        208            /* 1e6 / 4800 = 208.33, truncated */
#define CC_RSSI_GATE_DBM (-88)          /* capture gate, see header note */
#define CC_PKT_LEN       255

static SPISettings cc_spi(8000000, MSBFIRST, SPI_MODE0);

static bool     s_configured = false;   /* registers written for s_freq_hz */
static bool     s_rx_active  = false;
static uint32_t s_freq_hz    = 433920000UL;
static bool     s_hook_done  = false;

/* capture accumulator: one fixed-length packet worth of FIFO bytes */
static uint8_t  s_acc[CC_PKT_LEN];
static int      s_acc_len       = 0;
static uint32_t s_last_sig_ms   = 0;

/* ---------------- low-level SPI ---------------- */

/* After CS falls the CC1101 holds SO high until it is ready. Bounded
 * wait; if it never drops, the readback checks below fail the probe. */
static void cc_wait_so(void) {
    for (int i = 0; i < 200; i++) {
        if (digitalRead(IMP_PIN_SPI_MISO) == LOW) return;
        delayMicroseconds(10);
    }
}

static uint8_t cc_strobe(uint8_t cmd) {
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(cc_spi);
    digitalWrite(IMP_PIN_CC_CS, LOW);
    cc_wait_so();
    uint8_t st = spi.transfer(cmd);
    digitalWrite(IMP_PIN_CC_CS, HIGH);
    spi.endTransaction();
    return st;
}

static void cc_write_reg(uint8_t addr, uint8_t val) {
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(cc_spi);
    digitalWrite(IMP_PIN_CC_CS, LOW);
    cc_wait_so();
    spi.transfer(addr & 0x3F);
    spi.transfer(val);
    digitalWrite(IMP_PIN_CC_CS, HIGH);
    spi.endTransaction();
}

static uint8_t cc_read_reg(uint8_t addr) {           /* config registers */
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(cc_spi);
    digitalWrite(IMP_PIN_CC_CS, LOW);
    cc_wait_so();
    spi.transfer((addr & 0x3F) | 0x80);
    uint8_t v = spi.transfer(0x00);
    digitalWrite(IMP_PIN_CC_CS, HIGH);
    spi.endTransaction();
    return v;
}

static uint8_t cc_read_status(uint8_t addr) {        /* status: burst bit required */
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(cc_spi);
    digitalWrite(IMP_PIN_CC_CS, LOW);
    cc_wait_so();
    spi.transfer((addr & 0x3F) | 0xC0);
    uint8_t v = spi.transfer(0x00);
    digitalWrite(IMP_PIN_CC_CS, HIGH);
    spi.endTransaction();
    return v;
}

static void cc_write_burst(uint8_t addr, const uint8_t *data, int len) {
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(cc_spi);
    digitalWrite(IMP_PIN_CC_CS, LOW);
    cc_wait_so();
    spi.transfer((addr & 0x3F) | 0x40);
    for (int i = 0; i < len; i++) spi.transfer(data[i]);
    digitalWrite(IMP_PIN_CC_CS, HIGH);
    spi.endTransaction();
}

static void cc_read_fifo(uint8_t *data, int len) {
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(cc_spi);
    digitalWrite(IMP_PIN_CC_CS, LOW);
    cc_wait_so();
    spi.transfer(CC_REG_FIFO | 0xC0);
    for (int i = 0; i < len; i++) data[i] = spi.transfer(0x00);
    digitalWrite(IMP_PIN_CC_CS, HIGH);
    spi.endTransaction();
}

static uint8_t cc_marcstate(void) { return cc_read_status(CC_ST_MARCSTATE) & 0x1F; }

/* ---------------- probe / configure ---------------- */

/* True when a CC1101 answers. PARTNUM is 0x00 on every CC1101;
 * VERSION is 0x14 (0x04 on early silicon). A floating/absent bus reads
 * back 0xFF/0x00 noise and fails this check. Read-only: safe to call
 * at any time, it does not disturb a running radio. */
static bool cc_probe(void) {
    pinMode(IMP_PIN_CC_CS, OUTPUT);
    digitalWrite(IMP_PIN_CC_CS, HIGH);
    uint8_t part = cc_read_status(CC_ST_PARTNUM);
    uint8_t ver  = cc_read_status(CC_ST_VERSION);
    return part == 0x00 && (ver == 0x14 || ver == 0x04);
}

static void cc_panic_hook(void) {
    if (s_configured) cc_strobe(CC_SIDLE);
    s_rx_active = false;
    imp_ui_radio_state(IMP_RADIO_SUBGHZ, false);
}

/* OOK/ASK register set (TI SmartRF-class values) for 4.8 kBaud packet
 * FIFO mode. Only FREQ2/1/0 depend on the tuned frequency; they are
 * computed from Hz against the 26 MHz crystal, never hard-coded. */
static void cc_apply_config(void) {
    cc_strobe(CC_SRES);
    delay(1);
    static const struct { uint8_t addr, val; } cfg[] = {
        { CC_REG_IOCFG2,   0x29 },  /* GDO2: CHIP_RDYn (pin not wired) */
        { CC_REG_IOCFG1,   0x2E },  /* GDO1/SO: default (pin not wired) */
        { CC_REG_IOCFG0,   0x06 },  /* GDO0: sync word (pin not wired) */
        { CC_REG_FIFOTHR,  0x47 },  /* RX FIFO threshold 32 bytes */
        { CC_REG_SYNC1,    0xD3 },  /* sync word — unused, sync mode off */
        { CC_REG_SYNC0,    0x91 },
        { CC_REG_PKTLEN,   CC_PKT_LEN },
        { CC_REG_PKTCTRL1, 0x00 },  /* no addr check, no status append */
        { CC_REG_PKTCTRL0, 0x00 },  /* fixed length, no CRC, no whitening, FIFO mode */
        { CC_REG_ADDR,     0x00 },
        { CC_REG_CHANNR,   0x00 },
        { CC_REG_FSCTRL1,  0x06 },  /* IF ~152 kHz */
        { CC_REG_FSCTRL0,  0x00 },
        { CC_REG_MDMCFG4,  0x87 },  /* chan BW ~203 kHz, DRATE_E = 7 */
        { CC_REG_MDMCFG3,  0x83 },  /* DRATE_M = 131 -> 4.806 kBaud */
        { CC_REG_MDMCFG2,  0x30 },  /* MOD_FORMAT = OOK/ASK, no sync word */
        { CC_REG_MDMCFG1,  0x22 },  /* 4 preamble bytes, chan spacing exp */
        { CC_REG_MDMCFG0,  0xF8 },  /* chan spacing mantissa (CHANNR = 0) */
        { CC_REG_DEVIATN,  0x14 },  /* unused for OOK */
        { CC_REG_MCSM2,    0x07 },
        { CC_REG_MCSM1,    0x00 },  /* RX/TX finish -> IDLE */
        { CC_REG_MCSM0,    0x14 },  /* autocal on IDLE -> RX/TX */
        { CC_REG_FOCCFG,   0x16 },
        { CC_REG_BSCFG,    0x6C },
        { CC_REG_AGCCTRL2, 0x03 },
        { CC_REG_AGCCTRL1, 0x00 },
        { CC_REG_AGCCTRL0, 0x91 },
        { CC_REG_FREND1,   0x56 },
        { CC_REG_FREND0,   0x11 },  /* OOK: '1' bits use PATABLE index 1 */
        { CC_REG_FSCAL3,   0xE9 },
        { CC_REG_FSCAL2,   0x2A },
        { CC_REG_FSCAL1,   0x00 },
        { CC_REG_FSCAL0,   0x1F },
        { CC_REG_TEST2,    0x81 },
        { CC_REG_TEST1,    0x35 },
        { CC_REG_TEST0,    0x09 },
    };
    for (unsigned i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++)
        cc_write_reg(cfg[i].addr, cfg[i].val);
    /* FREQ = hz * 2^16 / f_xtal */
    uint32_t freq = (uint32_t)(((uint64_t)s_freq_hz << 16) / CC_XTAL_HZ);
    cc_write_reg(CC_REG_FREQ2, (uint8_t)(freq >> 16));
    cc_write_reg(CC_REG_FREQ1, (uint8_t)(freq >> 8));
    cc_write_reg(CC_REG_FREQ0, (uint8_t)(freq));
    /* PATABLE for OOK: index 0 = carrier off, index 1 = ~+10 dBm class */
    static const uint8_t pa[8] = { 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    cc_write_burst(CC_REG_PATABLE, pa, 8);
    /* config self-check: a ghost bus can accept writes silently, so
     * verify one distinctive register read back before trusting it */
    if (cc_read_reg(CC_REG_MDMCFG2) != 0x30) { s_configured = false; return; }
    s_configured = true;
    if (!s_hook_done) {
        imp_system_register_panic_hook(cc_panic_hook);
        s_hook_done = true;
    }
}

static imp_err_t cc_ensure(void) {
    if (s_configured) return IMP_OK;
    if (!cc_probe()) return IMP_ERR_NO_HW;
    cc_apply_config();
    return s_configured ? IMP_OK : IMP_ERR_IO;
}

static int16_t cc_rssi_raw_convert(uint8_t raw) {
    int v = raw;
    if (v >= 128) v -= 256;
    return (int16_t)(v / 2 - 74);
}

static bool cc_freq_valid(uint32_t hz) {
    return (hz >= 300000000UL && hz <= 348000000UL) ||
           (hz >= 387000000UL && hz <= 464000000UL) ||
           (hz >= 779000000UL && hz <= 928000000UL);
}

/* ---------------- API ---------------- */

imp_err_t imp_subghz_set_freq(uint32_t hz) {
    if (!cc_freq_valid(hz)) return IMP_ERR_RANGE;
    if (!cc_probe()) return IMP_ERR_NO_HW;
    bool was_rx = s_rx_active;
    if (s_configured) cc_strobe(CC_SIDLE);
    s_rx_active = false;
    s_freq_hz = hz;
    s_configured = false;
    imp_err_t e = cc_ensure();
    if (e != IMP_OK) return e;
    if (was_rx) return imp_subghz_rx_start();
    return IMP_OK;
}

imp_err_t imp_subghz_rx_start(void) {
    imp_err_t e = cc_ensure();
    if (e != IMP_OK) return e;
    cc_strobe(CC_SIDLE);
    cc_strobe(CC_SFRX);
    s_acc_len = 0;
    s_last_sig_ms = 0;
    cc_strobe(CC_SRX);
    for (int i = 0; i < 20; i++) {
        if (cc_marcstate() == CC_MARC_RX) {
            s_rx_active = true;
            imp_ui_radio_state(IMP_RADIO_SUBGHZ, true);
            return IMP_OK;
        }
        delay(1);
    }
    return IMP_ERR_IO;
}

/* Turn the accumulated byte stream into mark/space run lengths.
 * Leading and trailing zero runs (demodulated idle) are trimmed.
 * Returns false when the chunk looks like gated noise, not a signal. */
static bool cc_finalize(imp_subghz_capture_t *cap) {
    static uint32_t runs[IMP_SUBGHZ_MAX_TIMINGS];   /* static: keep 2 KB off the task stack */
    int nruns = 0;
    int total_bits = s_acc_len * 8;
    int first_one = -1, last_one = -1;
    for (int b = 0; b < total_bits; b++) {
        if (s_acc[b >> 3] & (0x80 >> (b & 7))) {
            if (first_one < 0) first_one = b;
            last_one = b;
        }
    }
    if (first_one < 0 || last_one - first_one < 16) return false; /* no real marks */
    int cur = -1;
    uint32_t len = 0;
    for (int b = first_one; b <= last_one; b++) {
        int bit = (s_acc[b >> 3] & (0x80 >> (b & 7))) ? 1 : 0;
        if (bit == cur) { len++; continue; }
        if (cur >= 0) {
            if (nruns >= IMP_SUBGHZ_MAX_TIMINGS) break;
            runs[nruns++] = len * CC_BIT_US;
        }
        cur = bit; len = 1;
    }
    if (cur >= 0 && nruns < IMP_SUBGHZ_MAX_TIMINGS) runs[nruns++] = len * CC_BIT_US;
    if (nruns < 2) return false;
    cap->freq_hz = s_freq_hz;
    cap->rssi_dbm = cc_rssi_raw_convert(cc_read_status(CC_ST_RSSI));
    cap->timing_count = (uint32_t)nruns;
    for (int i = 0; i < nruns; i++) cap->timings_us[i] = runs[i];
    return true;
}

imp_err_t imp_subghz_rx_poll(imp_subghz_capture_t *cap) {
    if (!cap) return IMP_ERR_IO;
    if (!s_configured && !cc_probe()) return IMP_ERR_NO_HW;
    if (!s_rx_active) return IMP_ERR_IO;

    uint8_t marc = cc_marcstate();
    uint8_t rxbytes = cc_read_status(CC_ST_RXBYTES);
    if ((rxbytes & 0x80) || marc == CC_MARC_RX_OVERFLOW) {
        cc_strobe(CC_SIDLE);
        cc_strobe(CC_SFRX);
        cc_strobe(CC_SRX);
        s_acc_len = 0;
        return IMP_ERR_BUSY;
    }
    int avail = rxbytes & 0x7F;
    if (avail > 0) {
        int16_t rssi = cc_rssi_raw_convert(cc_read_status(CC_ST_RSSI));
        int chunk = avail > 48 ? 48 : avail;
        uint8_t buf[48];
        cc_read_fifo(buf, chunk);
        if (rssi >= CC_RSSI_GATE_DBM) {
            int room = CC_PKT_LEN - s_acc_len;
            int keep = chunk < room ? chunk : room;
            memcpy(s_acc + s_acc_len, buf, keep);
            s_acc_len += keep;
            s_last_sig_ms = millis();
        }
        /* below the gate: bytes are drained and thrown away */
    }
    bool packet_done = (marc == CC_MARC_IDLE && s_acc_len > 0);
    bool full = s_acc_len >= CC_PKT_LEN;
    bool signal_dropped = s_acc_len >= 16 && s_last_sig_ms &&
                           (millis() - s_last_sig_ms > 60);
    if (packet_done || full || signal_dropped) {
        bool ok = cc_finalize(cap);
        s_acc_len = 0;
        s_last_sig_ms = 0;
        if (marc == CC_MARC_IDLE) {         /* re-arm for the next frame */
            cc_strobe(CC_SFRX);
            cc_strobe(CC_SRX);
        }
        return ok ? IMP_OK : IMP_ERR_BUSY;
    }
    if (marc == CC_MARC_IDLE) {            /* packet ended with nothing gated in */
        cc_strobe(CC_SFRX);
        cc_strobe(CC_SRX);
    }
    return IMP_ERR_BUSY;                    /* still listening — retry */
}

imp_err_t imp_subghz_rx_stop(void) {
    if (s_configured) cc_strobe(CC_SIDLE);
    s_rx_active = false;
    imp_ui_radio_state(IMP_RADIO_SUBGHZ, false);
    return IMP_OK;
}

int16_t imp_subghz_rssi(void) {
    if (!s_configured) {
        if (!cc_probe()) return -127;       /* no chip: honest floor, not 0 */
        return -127;                        /* chip present but radio idle */
    }
    return cc_rssi_raw_convert(cc_read_status(CC_ST_RSSI));
}

/* Save format (text, Flipper-.sub-shaped so captures stay readable and
 * diffable on the card):
 *   Filetype: IMP1 SubGhz Capture
 *   Version: 1
 *   Frequency: 433920000
 *   RSSI: -57
 *   RAW_Data: 350 -700 350 -1400 ...
 * RAW_Data is signed microseconds: positive = mark, negative = space,
 * alternating, starting with a mark. */
imp_err_t imp_subghz_save(const imp_subghz_capture_t *cap, const char *path) {
    if (!cap || !path || cap->timing_count == 0) return IMP_ERR_IO;
    if (cap->timing_count > IMP_SUBGHZ_MAX_TIMINGS) return IMP_ERR_RANGE;
    static char out[6400];
    int pos = snprintf(out, sizeof(out),
        "Filetype: IMP1 SubGhz Capture\nVersion: 1\nFrequency: %lu\nRSSI: %d\nRAW_Data:",
        (unsigned long)cap->freq_hz, (int)cap->rssi_dbm);
    if (pos < 0) return IMP_ERR_IO;
    for (uint32_t i = 0; i < cap->timing_count; i++) {
        long v = (long)cap->timings_us[i];
        if ((i & 1) == 0) { if (v < 0) v = -v; } else { if (v > 0) v = -v; }
        int n = snprintf(out + pos, sizeof(out) - (size_t)pos, " %ld", v);
        if (n < 0 || pos + n >= (int)sizeof(out)) return IMP_ERR_IO;
        pos += n;
    }
    if (pos + 1 < (int)sizeof(out)) out[pos++] = '\n';
    imp_file_t *f = imp_storage_open(path, "w");
    if (!f) return IMP_ERR_IO;
    size_t w = imp_storage_write(f, out, (size_t)pos);
    imp_storage_close(f);
    return w == (size_t)pos ? IMP_OK : IMP_ERR_IO;
}

imp_err_t imp_subghz_load(const char *path, imp_subghz_capture_t *cap) {
    if (!path || !cap) return IMP_ERR_IO;
    imp_file_t *f = imp_storage_open(path, "r");
    if (!f) return IMP_ERR_IO;
    static char buf[8192];
    size_t n = imp_storage_read(f, buf, sizeof(buf) - 1);
    imp_storage_close(f);
    if (n == 0) return IMP_ERR_IO;
    buf[n] = '\0';
    memset(cap, 0, sizeof(*cap));
    const char *p = strstr(buf, "Frequency:");
    if (p) cap->freq_hz = strtoul(p + 10, nullptr, 10);
    p = strstr(buf, "RSSI:");
    if (p) cap->rssi_dbm = (int16_t)strtol(p + 5, nullptr, 10);
    p = strstr(buf, "RAW_Data:");
    if (!p) return IMP_ERR_IO;
    p += 9;
    uint32_t count = 0;
    while (*p && count < IMP_SUBGHZ_MAX_TIMINGS) {
        char *end = nullptr;
        long v = strtol(p, &end, 10);
        if (end == p) { p++; continue; }
        cap->timings_us[count++] = (uint32_t)(v < 0 ? -v : v);
        p = end;
    }
    cap->timing_count = count;
    if (count == 0 || !cc_freq_valid(cap->freq_hz)) return IMP_ERR_IO;
    return IMP_OK;
}

imp_err_t imp_subghz_tx(const imp_subghz_capture_t *cap) {
    if (!cap || cap->timing_count == 0) return IMP_ERR_IO;
    if (cap->timing_count > IMP_SUBGHZ_MAX_TIMINGS) return IMP_ERR_RANGE;
    if (!cc_freq_valid(cap->freq_hz)) return IMP_ERR_RANGE;
    if (!cc_probe()) return IMP_ERR_NO_HW;
    if (cap->freq_hz != s_freq_hz) {
        s_freq_hz = cap->freq_hz;
        s_configured = false;
    }
    imp_err_t e = cc_ensure();
    if (e != IMP_OK) return e;

    /* Rebuild the bit stream at the capture baud: timings alternate
     * mark/space starting with a mark, MSB-first per byte. */
    static uint8_t stream[CC_PKT_LEN];
    memset(stream, 0, sizeof(stream));
    int nbits = 0;
    bool level = true;
    for (uint32_t i = 0; i < cap->timing_count && nbits < CC_PKT_LEN * 8; i++) {
        int reps = (int)((cap->timings_us[i] + CC_BIT_US / 2) / CC_BIT_US);
        if (reps < 1) reps = 1;
        for (int k = 0; k < reps && nbits < CC_PKT_LEN * 8; k++) {
            if (level) stream[nbits >> 3] |= (uint8_t)(0x80 >> (nbits & 7));
            nbits++;
        }
        level = !level;
    }
    int nbytes = (nbits + 7) / 8;
    if (nbytes == 0) return IMP_ERR_IO;

    bool was_rx = s_rx_active;
    s_rx_active = false;
    cc_strobe(CC_SIDLE);
    cc_strobe(CC_SFTX);
    cc_write_reg(CC_REG_PKTLEN, (uint8_t)nbytes);
    imp_ui_radio_state(IMP_RADIO_SUBGHZ, true);

    int off = 0;
    int first = nbytes < 64 ? nbytes : 64;
    cc_write_burst(CC_REG_FIFO, stream, first);
    off = first;
    cc_strobe(CC_STX);
    uint32_t t0 = millis();
    bool failed = false;
    while (off < nbytes) {
        uint8_t marc = cc_marcstate();
        if (marc == CC_MARC_TX_UNDERFLOW) { failed = true; break; }
        int txb = cc_read_status(CC_ST_TXBYTES) & 0x7F;
        int space = 64 - txb;
        if (space > 0) {
            int chunk = nbytes - off < space ? nbytes - off : space;
            cc_write_burst(CC_REG_FIFO, stream + off, chunk);
            off += chunk;
        }
        if (millis() - t0 > 1500) { failed = true; break; }
        delay(1);
    }
    /* wait for the packet to finish (MARCSTATE back to IDLE) */
    while (!failed && cc_marcstate() != CC_MARC_IDLE) {
        if (cc_marcstate() == CC_MARC_TX_UNDERFLOW) { failed = true; break; }
        if (millis() - t0 > 2000) { failed = true; break; }
        delay(1);
    }
    if (failed) cc_strobe(CC_SFTX);
    cc_write_reg(CC_REG_PKTLEN, CC_PKT_LEN);   /* restore RX packet length */
    imp_ui_radio_state(IMP_RADIO_SUBGHZ, false);
    if (was_rx) imp_subghz_rx_start();
    return failed ? IMP_ERR_IO : IMP_OK;
}
