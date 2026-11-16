/* imp_system.cpp — battery, status LED, buzzer, cooperative delay,
 * deep sleep, and the panic path.
 *
 * Panic (spec v0.1 section 4): the combo is detected in imp_input; the
 * execution runs here, synchronously, the first time any tool loop yields
 * through imp_delay_ms() after the flag sets — worst case one loop slice.
 * Deep sleep is a reset-class operation, so executing it mid-loop is safe
 * and leaves a blank, silent, radio-off device with no software delay. */
#include "imp_internal.h"
#include <WiFi.h>
#include "driver/rmt.h"
#include "esp_sleep.h"
#include "esp_bt.h"

/* ---------------- battery ---------------- */
/* BATT_ADC IO4, divider 300k/100k -> pack mV = pin mV * 4. */
int32_t imp_system_battery_mv(void) {
    uint32_t mv = analogReadMilliVolts(IMP_PIN_BATT_ADC);
    return (int32_t)(mv * 4);
}

/* ---------------- WS2812B status LED (RMT ch 0) ---------------- */
static bool led_ready = false;
static void led_init(void) {
    rmt_config_t cfg = RMT_DEFAULT_CONFIG_TX((gpio_num_t)IMP_PIN_LED_DATA, RMT_CHANNEL_0);
    cfg.clk_div = 2; /* 40 MHz -> 25 ns ticks */
    rmt_config(&cfg);
    rmt_driver_install(RMT_CHANNEL_0, 0, 0);
    led_ready = true;
}

void imp_system_set_led(uint32_t rgb) {
    if (!led_ready) led_init();
    uint8_t g = (rgb >> 8) & 0xFF, r = (rgb >> 16) & 0xFF, b = rgb & 0xFF; /* GRB order */
    rmt_item32_t items[24];
    uint8_t bytes[3] = {g, r, b};
    int n = 0;
    for (int i = 0; i < 3; i++)
        for (int bit = 7; bit >= 0; bit--) {
            bool one = bytes[i] & (1 << bit);
            items[n].duration0 = one ? 32 : 16;  /* T1H/T0H at 25 ns */
            items[n].level0 = 1;
            items[n].duration1 = one ? 18 : 34;  /* T1L/T0L */
            items[n].level1 = 0;
            n++;
        }
    rmt_write_items(RMT_CHANNEL_0, items, n, true);
}

/* ---------------- buzzer (LEDC ch 1 on IO35) ---------------- */
static uint32_t buzzer_stop_at = 0;
void imp_system_buzzer(uint32_t freq_hz, uint32_t ms) {
    static bool ready = false;
    if (!ready) {
        ledcSetup(1, 2000, 10);
        ledcAttachPin(IMP_PIN_BUZZER, 1);
        ready = true;
    }
    if (freq_hz == 0 || ms == 0) {
        ledcWrite(1, 0);
        buzzer_stop_at = 0;
        return;
    }
    ledcWriteTone(1, freq_hz);
    buzzer_stop_at = millis() + ms;
}

/* ---------------- panic ---------------- */
static imp_panic_hook_t panic_hooks[4];
static int panic_hook_count = 0;
void imp_system_register_panic_hook(imp_panic_hook_t fn) {
    if (panic_hook_count < 4) panic_hooks[panic_hook_count++] = fn;
}

void imp_panic_execute(void) {
    for (int i = 0; i < panic_hook_count; i++) panic_hooks[i](); /* radios off */
    WiFi.mode(WIFI_OFF);
    esp_bt_controller_disable();
    imp_system_set_led(0);
    imp_system_buzzer(0, 0);
    digitalWrite(IMP_PIN_A9_PWR, LOW);
    digitalWrite(IMP_PIN_FLASHLED, LOW);
    disp_blank_now();
    imp_system_sleep();
}

/* ---------------- delay / sleep / reboot ---------------- */
void imp_delay_ms(uint32_t ms) {
    uint32_t end = millis() + ms;
    while ((int32_t)(end - millis()) > 0) {
        if (imp_system_panic_requested()) imp_panic_execute(); /* never returns */
        imp_input_pump();
        if (buzzer_stop_at && (int32_t)(millis() - buzzer_stop_at) >= 0) {
            ledcWrite(1, 0);
            buzzer_stop_at = 0;
        }
        uint32_t remain = end - millis();
        delay(remain > 5 ? 5 : remain);
    }
    if (imp_system_panic_requested()) imp_panic_execute();
}

void imp_system_sleep(void) {
    /* Wake on encoder push or BOOT key (both active-low, pulled up). */
    uint64_t mask = (1ULL << IMP_PIN_ENC_PUSH) | (1ULL << IMP_PIN_BOOT);
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

void imp_system_reboot(void) { ESP.restart(); }
