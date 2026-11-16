/* imp_input.cpp — encoder (SW1), BOOT key (SW2), 4x4 keypad (J7), panic combo.
 *
 * Encoder quadrature is decoded in ISR into a detent accumulator; buttons
 * and the keypad are polled. Events are dispatched by imp_input_pump(),
 * which runs from imp_delay_ms() and the shell loop — callbacks never run
 * in ISR context (pack section 9, house rule 1).
 *
 * Panic combo (spec v0.1 section 4): BOOT + encoder push held together.
 * The ISR timestamps both edges; the pump confirms the overlap and sets
 * the panic flag. Execution (blank + deep sleep) happens in imp_system.
 */
#include "imp_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static QueueHandle_t ev_queue = nullptr;
static volatile int enc_accum = 0;
static volatile uint8_t enc_state = 0;
static volatile uint32_t boot_down_ms = 0, push_down_ms = 0;
static volatile bool boot_held = false, push_held = false;
static volatile bool panic_flag = false;
static uint32_t last_activity = 0;

static imp_input_cb_t active_cb = nullptr;
static void *active_ctx = nullptr;
static imp_input_cb_t modal_cb = nullptr;
static void *modal_ctx = nullptr;

void imp_input_set_active(imp_input_cb_t cb, void *ctx) { active_cb = cb; active_ctx = ctx; }
void imp_input_set_modal(imp_input_cb_t cb, void *ctx) { modal_cb = cb; modal_ctx = ctx; }
void imp_input_clear_modal(void) { modal_cb = nullptr; modal_ctx = nullptr; }
uint32_t imp_input_last_activity_ms(void) { return last_activity; }
bool imp_system_panic_requested(void) { return panic_flag; }

static void post_event(imp_input_t ev) {
    if (ev_queue) xQueueSend(ev_queue, &ev, 0);
}

/* Quadrature: full-step table, one event per detent (4 transitions). */
static void IRAM_ATTR enc_isr(void) {
    uint8_t s = (uint8_t)((digitalRead(IMP_PIN_ENC_A) << 1) | digitalRead(IMP_PIN_ENC_B));
    static const int8_t tbl[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
    enc_accum += tbl[(enc_state << 2) | s];
    enc_state = s;
    if (enc_accum >= 4)  { enc_accum = 0; post_event(IMP_INPUT_ENC_CW); }
    if (enc_accum <= -4) { enc_accum = 0; post_event(IMP_INPUT_ENC_CCW); }
}

static void IRAM_ATTR boot_isr(void) {
    if (digitalRead(IMP_PIN_BOOT) == LOW) { boot_held = true; boot_down_ms = millis(); }
    else boot_held = false;
}

static void IRAM_ATTR push_isr(void) {
    if (digitalRead(IMP_PIN_ENC_PUSH) == LOW) { push_held = true; push_down_ms = millis(); }
    else push_held = false;
}

/* Keypad matrix: rows driven, columns pulled up. Scan is skipped for any
 * pin the GPIO tool has claimed (imp_gpio owns the claim table). */
static const uint8_t kp_rows[4] = {IMP_PIN_KP_ROW1, IMP_PIN_KP_ROW2, IMP_PIN_KP_ROW3, IMP_PIN_KP_ROW4};
static const uint8_t kp_cols[4] = {IMP_PIN_KP_COL1, IMP_PIN_KP_COL2, IMP_PIN_KP_COL3, IMP_PIN_KP_COL4};
static const imp_input_t kp_map[4][4] = {
    {IMP_INPUT_KEY_1, IMP_INPUT_KEY_2, IMP_INPUT_KEY_3, IMP_INPUT_KEY_A},
    {IMP_INPUT_KEY_4, IMP_INPUT_KEY_5, IMP_INPUT_KEY_6, IMP_INPUT_KEY_B},
    {IMP_INPUT_KEY_7, IMP_INPUT_KEY_8, IMP_INPUT_KEY_9, IMP_INPUT_KEY_C},
    {IMP_INPUT_KEY_STAR, IMP_INPUT_KEY_0, IMP_INPUT_KEY_HASH, IMP_INPUT_KEY_D},
};
static uint16_t kp_prev = 0;

static void keypad_scan(void) {
    uint16_t now_mask = 0;
    for (int r = 0; r < 4; r++) {
        if (imp_input_gpio_claimed(kp_rows[r])) continue;
        pinMode(kp_rows[r], OUTPUT);
        digitalWrite(kp_rows[r], LOW);
        delayMicroseconds(30);
        for (int c = 0; c < 4; c++) {
            if (imp_input_gpio_claimed(kp_cols[c])) continue;
            if (digitalRead(kp_cols[c]) == LOW) now_mask |= (uint16_t)(1u << (r * 4 + c));
        }
        digitalWrite(kp_rows[r], HIGH);
        pinMode(kp_rows[r], INPUT);
    }
    uint16_t pressed = now_mask & (uint16_t)~kp_prev;
    for (int i = 0; i < 16; i++)
        if (pressed & (1u << i)) post_event(kp_map[i / 4][i % 4]);
    kp_prev = now_mask;
}

void imp_input_init(void) {
    ev_queue = xQueueCreate(16, sizeof(imp_input_t));
    pinMode(IMP_PIN_ENC_A, INPUT_PULLUP);
    pinMode(IMP_PIN_ENC_B, INPUT_PULLUP);
    pinMode(IMP_PIN_ENC_PUSH, INPUT_PULLUP);
    pinMode(IMP_PIN_BOOT, INPUT_PULLUP);
    for (int c = 0; c < 4; c++) pinMode(kp_cols[c], INPUT_PULLUP);
    for (int r = 0; r < 4; r++) pinMode(kp_rows[r], INPUT);
    enc_state = (uint8_t)((digitalRead(IMP_PIN_ENC_A) << 1) | digitalRead(IMP_PIN_ENC_B));
    attachInterrupt(IMP_PIN_ENC_A, enc_isr, CHANGE);
    attachInterrupt(IMP_PIN_ENC_B, enc_isr, CHANGE);
    attachInterrupt(IMP_PIN_BOOT, boot_isr, CHANGE);
    attachInterrupt(IMP_PIN_ENC_PUSH, push_isr, CHANGE);
    last_activity = millis();
}

static bool push_prev = false, boot_prev = false;
static uint32_t boot_release_deadline = 0;

void imp_input_pump(void) {
    /* Button edges -> OK / BACK (debounced by 25 ms hold). */
    uint32_t now = millis();
    if (push_held && !push_prev && now - push_down_ms > 5) {
        /* wait for release to distinguish click from panic hold */
    }
    if (!push_held && push_prev && now - push_down_ms < 800 && !panic_flag) {
        post_event(IMP_INPUT_OK);
    }
    if (!boot_held && boot_prev && now - boot_down_ms < 800 && !panic_flag) {
        post_event(IMP_INPUT_BACK);
    }
    push_prev = push_held;
    boot_prev = boot_held;

    /* Panic combo: both held, overlapping, for 300 ms. */
    if (!panic_flag && boot_held && push_held) {
        uint32_t both_since = boot_down_ms > push_down_ms ? boot_down_ms : push_down_ms;
        if (now - both_since > 300) panic_flag = true;
    }

    keypad_scan();

    imp_input_t ev;
    while (xQueueReceive(ev_queue, &ev, 0) == pdTRUE) {
        last_activity = now;
        if (modal_cb) modal_cb(modal_ctx, ev);
        else if (active_cb) active_cb(active_ctx, ev);
    }
}
