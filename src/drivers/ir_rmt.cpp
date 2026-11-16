/* ir_rmt.cpp — IR driver: TSOP4838 RX on IO46, TSAL6100 TX on IO2.
 *
 * Legacy IDF RMT driver (Arduino-ESP32 2.0.x). Channel map is frozen
 * project-wide: ch 0 = WS2812 LED (imp_system), ch 1 = IR TX, ch 2 = IR RX.
 * Both channels run clk_div 80 on the 80 MHz APB clock, so one RMT tick
 * is exactly 1 us and durations move between RMT items and
 * imp_ir_signal_t.timings_us with no scaling.
 *
 * RX: the TSOP4838 output is active-low (idle HIGH, a burst pulls it
 * LOW), so a mark is a LOW half-period in the RMT items. The RMT RX idle
 * threshold (20 ms) ends a reception; the driver then hands the whole
 * item block to the ring buffer, which is how learn_poll detects the
 * end of a signal — a block in the ring buffer IS a complete signal.
 *
 * .ir file format (text, Flipper .ir spirit, human-readable):
 *   protocol: NEC            <- or RAW, RC5, SIRC
 *   carrier: 38000
 *   9000 4500 560 560 560 1690 ...
 * Line 3 is the timings in us, space-separated, mark first, mark/space
 * alternating. The loader also tolerates a "timings:" prefix on line 3
 * and timings split over several lines.
 */
#include "imp_internal.h"
#include "driver/rmt.h"
#include "freertos/ringbuf.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define IR_TX_CHANNEL RMT_CHANNEL_1
#define IR_RX_CHANNEL RMT_CHANNEL_2
#define IR_RX_RING_BYTES 4096 /* 1024 RMT items; a full 512-timing signal is 256 */
#define IR_DEFAULT_CARRIER_HZ 38000

static bool ir_learning = false;
static RingbufHandle_t ir_rb = nullptr;
static imp_ir_signal_t ir_pending; /* signal being assembled by learn_poll */

/* ------------------------------------------------------------------ */
/* RX assembly                                                         */
/* ------------------------------------------------------------------ */

/* Append one half-period. Index parity is the mark/space identity:
 * even indices are marks. Split RMT items that repeat the parity of
 * the last entry merge into it instead of breaking the alternation. */
static void ir_append(imp_ir_signal_t *sig, bool is_mark, uint32_t dur_us) {
    if (dur_us == 0) return;
    if (sig->timing_count == 0) {
        if (!is_mark) return; /* signals start with a mark */
        sig->timings_us[0] = dur_us;
        sig->timing_count = 1;
        return;
    }
    bool last_is_mark = ((sig->timing_count - 1) % 2) == 0;
    if (is_mark == last_is_mark) {
        sig->timings_us[sig->timing_count - 1] += dur_us;
    } else if (sig->timing_count < IMP_IR_MAX_TIMINGS) {
        sig->timings_us[sig->timing_count++] = dur_us;
    } else {
        sig->timings_us[sig->timing_count - 1] += dur_us; /* over-long: fold tail */
    }
}

static void ir_parse_items(imp_ir_signal_t *sig, const rmt_item32_t *items, size_t count) {
    for (size_t i = 0; i < count; i++) {
        /* TSOP4838 is active-low: level 0 = mark (burst), level 1 = space. */
        ir_append(sig, items[i].level0 == 0, items[i].duration0);
        if (items[i].duration1 == 0 && items[i].duration0 == 0) break; /* terminator */
        ir_append(sig, items[i].level1 == 0, items[i].duration1);
    }
}

/* ------------------------------------------------------------------ */
/* NEC decode                                                          */
/* ------------------------------------------------------------------ */

static bool ir_in(uint32_t v, uint32_t lo, uint32_t hi) { return v >= lo && v <= hi; }

/* NEC frame: 9 ms mark + 4.5 ms space, then 32 bits LSB-first as
 * addr, ~addr, cmd, ~cmd, each bit a ~560 us mark plus a 560 us (0) or
 * 1690 us (1) space, then a final mark. True only if both complement
 * checks pass — anything else stays RAW. */
static bool ir_nec_decode(const imp_ir_signal_t *sig) {
    if (sig->timing_count < 67) return false;
    if (!ir_in(sig->timings_us[0], 7000, 11000)) return false; /* leader mark */
    if (!ir_in(sig->timings_us[1], 3500, 5500)) return false;  /* leader space */
    uint8_t bytes[4] = {0, 0, 0, 0};
    for (int i = 0; i < 32; i++) {
        uint32_t mark = sig->timings_us[2 + 2 * i];
        uint32_t space = sig->timings_us[3 + 2 * i];
        if (!ir_in(mark, 350, 900)) return false;
        int bit;
        if (ir_in(space, 350, 900)) bit = 0;
        else if (ir_in(space, 1300, 2100)) bit = 1;
        else return false;
        if (bit) bytes[i / 8] |= (uint8_t)(1u << (i % 8));
    }
    return (uint8_t)(bytes[0] ^ bytes[1]) == 0xFF &&
           (uint8_t)(bytes[2] ^ bytes[3]) == 0xFF;
}

/* ------------------------------------------------------------------ */
/* Learn (RX)                                                          */
/* ------------------------------------------------------------------ */

imp_err_t imp_ir_learn_start(void) {
    if (ir_learning) return IMP_ERR_BUSY;
    memset(&ir_pending, 0, sizeof(ir_pending));
    ir_pending.carrier_hz = IR_DEFAULT_CARRIER_HZ;
    ir_pending.protocol = IMP_IR_PROTO_RAW;

    rmt_config_t cfg = RMT_DEFAULT_CONFIG_RX((gpio_num_t)IMP_PIN_IR_RX, IR_RX_CHANNEL);
    cfg.clk_div = 80;                              /* 1 tick = 1 us */
    cfg.rx_config.idle_threshold = 20000;          /* ~20 ms idle ends a signal */
    cfg.rx_config.filter_en = true;
    cfg.rx_config.filter_ticks_thresh = 100;       /* ~100 tick glitch filter */
    if (rmt_config(&cfg) != ESP_OK) return IMP_ERR_IO;
    if (rmt_driver_install(IR_RX_CHANNEL, IR_RX_RING_BYTES, 0) != ESP_OK) return IMP_ERR_IO;
    if (rmt_get_ringbuf_handle(IR_RX_CHANNEL, &ir_rb) != ESP_OK || !ir_rb) {
        rmt_driver_uninstall(IR_RX_CHANNEL);
        return IMP_ERR_IO;
    }
    if (rmt_rx_start(IR_RX_CHANNEL, true) != ESP_OK) {
        rmt_driver_uninstall(IR_RX_CHANNEL);
        ir_rb = nullptr;
        return IMP_ERR_IO;
    }
    ir_learning = true;
    imp_ui_radio_state(IMP_RADIO_IR, true);
    return IMP_OK;
}

imp_err_t imp_ir_learn_poll(imp_ir_signal_t *sig) {
    if (!sig) return IMP_ERR_RANGE;
    if (!ir_learning || !ir_rb) return IMP_ERR_BUSY;
    /* Drain every block the RMT driver has delivered. A block only
     * appears after the idle threshold fired, so a drained block is a
     * complete, end-detected signal. */
    bool got = false;
    size_t item_size = 0;
    rmt_item32_t *items;
    while ((items = (rmt_item32_t *)xRingbufferReceive(ir_rb, &item_size, 0)) != nullptr) {
        ir_parse_items(&ir_pending, items, item_size / sizeof(rmt_item32_t));
        vRingbufferReturnItem(ir_rb, items);
        got = true;
    }
    if (!got || ir_pending.timing_count == 0) return IMP_ERR_BUSY; /* nothing yet — retry */
    *sig = ir_pending;
    sig->protocol = ir_nec_decode(sig) ? IMP_IR_PROTO_NEC : IMP_IR_PROTO_RAW;
    if (sig->carrier_hz == 0) sig->carrier_hz = IR_DEFAULT_CARRIER_HZ;
    memset(&ir_pending, 0, sizeof(ir_pending));
    ir_pending.carrier_hz = IR_DEFAULT_CARRIER_HZ;
    return IMP_OK;
}

imp_err_t imp_ir_learn_stop(void) {
    if (!ir_learning) return IMP_OK; /* idempotent: safe on every exit path */
    rmt_rx_stop(IR_RX_CHANNEL);
    rmt_driver_uninstall(IR_RX_CHANNEL);
    ir_rb = nullptr;
    ir_learning = false;
    imp_ui_radio_state(IMP_RADIO_IR, false);
    return IMP_OK;
}

/* ------------------------------------------------------------------ */
/* Send (TX)                                                           */
/* ------------------------------------------------------------------ */

imp_err_t imp_ir_send(const imp_ir_signal_t *sig) {
    if (!sig || sig->timing_count == 0 || sig->timing_count > IMP_IR_MAX_TIMINGS)
        return IMP_ERR_RANGE;
    uint32_t carrier = sig->carrier_hz ? sig->carrier_hz : IR_DEFAULT_CARRIER_HZ;
    if (carrier < 10000 || carrier > 100000) return IMP_ERR_RANGE;

    /* Flatten timings into RMT half-periods, splitting any duration
     * past the 15-bit RMT field (32767 us) into same-level chunks —
     * consecutive same-level halves play as one continuous level. */
    uint16_t *half_level = (uint16_t *)malloc(sizeof(uint16_t) * IMP_IR_MAX_TIMINGS * 4);
    uint16_t *half_dur = (uint16_t *)malloc(sizeof(uint16_t) * IMP_IR_MAX_TIMINGS * 4);
    if (!half_level || !half_dur) { free(half_level); free(half_dur); return IMP_ERR_IO; }
    const int half_cap = IMP_IR_MAX_TIMINGS * 4;
    int halves = 0;
    uint32_t total_us = 0;
    for (uint32_t i = 0; i < sig->timing_count; i++) {
        uint32_t remain = sig->timings_us[i];
        total_us += remain;
        uint16_t level = (i % 2 == 0) ? 1 : 0; /* mark = LED drive high */
        while (remain > 0) {
            if (halves >= half_cap) { /* absurd timing from a hand-edited file */
                free(half_level); free(half_dur);
                return IMP_ERR_RANGE;
            }
            uint32_t chunk = remain > 32767 ? 32767 : remain;
            half_level[halves] = level;
            half_dur[halves] = (uint16_t)chunk;
            halves++;
            remain -= chunk;
        }
    }
    int item_count = (halves + 1) / 2;
    rmt_item32_t *items = (rmt_item32_t *)calloc(item_count + 1, sizeof(rmt_item32_t));
    if (!items) { free(half_level); free(half_dur); return IMP_ERR_IO; }
    for (int i = 0; i < halves; i++) {
        rmt_item32_t *it = &items[i / 2];
        if (i % 2 == 0) { it->level0 = half_level[i]; it->duration0 = half_dur[i]; }
        else            { it->level1 = half_level[i]; it->duration1 = half_dur[i]; }
    }
    /* items[item_count] stays all-zero: the transmission terminator. */
    free(half_level);
    free(half_dur);

    rmt_config_t cfg = RMT_DEFAULT_CONFIG_TX((gpio_num_t)IMP_PIN_IR_TX, IR_TX_CHANNEL);
    cfg.clk_div = 80; /* 1 tick = 1 us */
    cfg.tx_config.carrier_en = true;
    cfg.tx_config.carrier_freq_hz = carrier;
    cfg.tx_config.carrier_duty_percent = 33;
    cfg.tx_config.carrier_level = RMT_CARRIER_LEVEL_HIGH;
    cfg.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    cfg.tx_config.idle_output_en = true;
    cfg.tx_config.loop_en = false;
    imp_err_t result = IMP_OK;
    if (rmt_config(&cfg) != ESP_OK || rmt_driver_install(IR_TX_CHANNEL, 0, 0) != ESP_OK) {
        free(items);
        return IMP_ERR_IO;
    }
    imp_ui_radio_state(IMP_RADIO_IR, true);
    if (rmt_write_items(IR_TX_CHANNEL, items, item_count + 1, false) != ESP_OK) {
        result = IMP_ERR_IO;
    } else {
        /* Bounded poll: never wait meaningfully longer than the signal
         * itself, and yield through imp_delay_ms so the UI stays alive. */
        uint32_t budget_ms = total_us / 1000 + 100;
        uint32_t start = millis();
        while (rmt_wait_tx_done(IR_TX_CHANNEL, 0) != ESP_OK) {
            if (millis() - start > budget_ms) { result = IMP_ERR_IO; break; }
            imp_delay_ms(2);
        }
    }
    rmt_driver_uninstall(IR_TX_CHANNEL);
    imp_ui_radio_state(IMP_RADIO_IR, ir_learning);
    pinMode(IMP_PIN_IR_TX, OUTPUT);
    digitalWrite(IMP_PIN_IR_TX, LOW); /* LED drive off — restore on every path */
    free(items);
    return result;
}

/* ------------------------------------------------------------------ */
/* Save / load (.ir text format, documented at the top of this file)   */
/* ------------------------------------------------------------------ */

static const char *ir_proto_name(uint8_t proto) {
    switch (proto) {
        case IMP_IR_PROTO_NEC:  return "NEC";
        case IMP_IR_PROTO_RC5:  return "RC5";
        case IMP_IR_PROTO_SIRC: return "SIRC";
        default:                return "RAW";
    }
}

static uint8_t ir_proto_from_name(const char *name) {
    if (strcmp(name, "NEC") == 0) return IMP_IR_PROTO_NEC;
    if (strcmp(name, "RC5") == 0) return IMP_IR_PROTO_RC5;
    if (strcmp(name, "SIRC") == 0) return IMP_IR_PROTO_SIRC;
    return IMP_IR_PROTO_RAW;
}

imp_err_t imp_ir_save(const imp_ir_signal_t *sig, const char *path) {
    if (!sig || !path || sig->timing_count == 0) return IMP_ERR_RANGE;
    imp_file_t *f = imp_storage_open(path, "w");
    if (!f) return IMP_ERR_IO;
    char line[1024]; /* timings are flushed in chunks; this stays stack-cheap */
    int n = snprintf(line, sizeof(line), "protocol: %s\ncarrier: %lu\n",
                     ir_proto_name(sig->protocol),
                     (unsigned long)(sig->carrier_hz ? sig->carrier_hz : IR_DEFAULT_CARRIER_HZ));
    bool ok = n > 0 && imp_storage_write(f, line, (size_t)n) == (size_t)n;
    /* Timings line(s): chunked so the line buffer stays small. */
    size_t off = 0;
    line[0] = '\0';
    for (uint32_t i = 0; ok && i < sig->timing_count; i++) {
        char num[16];
        int m = snprintf(num, sizeof(num), "%lu ", (unsigned long)sig->timings_us[i]);
        if (off + (size_t)m >= sizeof(line) - 2) {
            ok = imp_storage_write(f, line, off) == off;
            off = 0;
        }
        if (ok) { memcpy(line + off, num, (size_t)m); off += (size_t)m; }
    }
    if (ok && off > 0) ok = imp_storage_write(f, line, off) == off;
    if (ok) ok = imp_storage_write(f, "\n", 1) == 1;
    imp_storage_close(f);
    return ok ? IMP_OK : IMP_ERR_IO;
}

imp_err_t imp_ir_load(const char *path, imp_ir_signal_t *sig) {
    if (!path || !sig) return IMP_ERR_RANGE;
    imp_file_t *f = imp_storage_open(path, "r");
    if (!f) return IMP_ERR_IO;
    static char buf[8192]; /* 512 timings worst case is ~3.2 KB of text */
    size_t len = 0, r;
    while (len < sizeof(buf) - 1 && (r = imp_storage_read(f, buf + len, sizeof(buf) - 1 - len)) > 0)
        len += r;
    imp_storage_close(f);
    buf[len] = '\0';

    memset(sig, 0, sizeof(*sig));
    sig->protocol = IMP_IR_PROTO_RAW;
    sig->carrier_hz = IR_DEFAULT_CARRIER_HZ;
    /* Line by line: header lines set protocol/carrier; every other
     * line contributes its numbers as timings (a "timings:" prefix is
     * skipped by the digit scan, as is any other label text). */
    for (char *line = strtok(buf, "\n"); line; line = strtok(nullptr, "\n")) {
        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (strncmp(p, "protocol:", 9) == 0) {
            p += 9;
            while (*p == ' ') p++;
            char name[16];
            int i = 0;
            while (p[i] && !isspace((unsigned char)p[i]) && i < 15) { name[i] = p[i]; i++; }
            name[i] = '\0';
            sig->protocol = ir_proto_from_name(name);
        } else if (strncmp(p, "carrier:", 8) == 0) {
            sig->carrier_hz = strtoul(p + 8, nullptr, 10);
        } else {
            while (*p && sig->timing_count < IMP_IR_MAX_TIMINGS) {
                if (isdigit((unsigned char)*p)) {
                    char *end;
                    sig->timings_us[sig->timing_count++] = strtoul(p, &end, 10);
                    p = end;
                } else {
                    p++;
                }
            }
        }
    }
    if (sig->carrier_hz == 0) sig->carrier_hz = IR_DEFAULT_CARRIER_HZ;
    return sig->timing_count > 0 ? IMP_OK : IMP_ERR_IO;
}
