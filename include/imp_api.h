/*
 * imp_api.h — the IMP-1 tool API. DESIGN CONTRACT, frozen.
 *
 * Source: IMP-1 Tool Builder's Pack sections 7-9 (V5-FREEZE-2026-11-15).
 * Drivers own the buses, chip selects and rails; a tool calls this API and
 * never touches a radio pin directly. If a tool disagrees with this header,
 * the tool is wrong.
 *
 * Conventions (pack section 7, binding on every family):
 *  - Returns are imp_err_t. Handles are opaque; close releases everything
 *    the tool held (buses, CS lines, outputs).
 *  - Long operations are start + poll, never one blocking call. The UI
 *    thread must stay free to repaint and read the encoder.
 *  - Units: frequencies in Hz, voltages in mV, times in ms, signal
 *    strength in dBm. No exceptions, no floats for frequency.
 *  - Confirm before consequence: erase, write, transmit and payload-run
 *    all pass through imp_ui_confirm().
 *  - Restore what you touched at exit, including error exits.
 */
#ifndef IMP_API_H
#define IMP_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */
typedef enum {
    IMP_OK = 0,
    IMP_ERR_BUSY,    /* bus or device held — retry, do not force */
    IMP_ERR_NO_HW,   /* manifest claimed hardware the build does not have */
    IMP_ERR_LOCKED,  /* hardware lock, e.g. the flash WP# jumper (J4) */
    IMP_ERR_RANGE,   /* value outside the hardware's honest range */
    IMP_ERR_IO       /* transfer / media failure */
} imp_err_t;

/* ------------------------------------------------------------------ */
/* Input events (imp_input_t)                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    IMP_INPUT_NONE = 0,
    IMP_INPUT_ENC_CW,    /* encoder turn clockwise */
    IMP_INPUT_ENC_CCW,   /* encoder turn counter-clockwise */
    IMP_INPUT_OK,        /* encoder push */
    IMP_INPUT_BACK,      /* dedicated BACK key (BOOT key short-press in shell) */
    /* Keypad matrix (J7) — bonus input, never required by any screen */
    IMP_INPUT_KEY_1, IMP_INPUT_KEY_2, IMP_INPUT_KEY_3, IMP_INPUT_KEY_A,
    IMP_INPUT_KEY_4, IMP_INPUT_KEY_5, IMP_INPUT_KEY_6, IMP_INPUT_KEY_B,
    IMP_INPUT_KEY_7, IMP_INPUT_KEY_8, IMP_INPUT_KEY_9, IMP_INPUT_KEY_C,
    IMP_INPUT_KEY_STAR, IMP_INPUT_KEY_0, IMP_INPUT_KEY_HASH, IMP_INPUT_KEY_D
} imp_input_t;

typedef void (*imp_input_cb_t)(void *ctx, imp_input_t ev);

/* ------------------------------------------------------------------ */
/* UI — imp_ui_*                                                       */
/* One tool owns the screen at a time. Partial redraws only: a call    */
/* repaints the strip that changed, never the whole panel.             */
/* ------------------------------------------------------------------ */
typedef struct imp_ui_s imp_ui_t;

imp_ui_t *imp_ui_open(const char *title);
void      imp_ui_close(imp_ui_t *ui);
void      imp_ui_set_input_cb(imp_ui_t *ui, imp_input_cb_t cb, void *ctx);

/* Selectable list — partial redraw of the changed rows only. */
void imp_ui_list(imp_ui_t *ui, const char *const *items, int n, int sel);
/* One labelled live value (frequency, RSSI, voltage). The workhorse. */
void imp_ui_value(imp_ui_t *ui, const char *label, int32_t value, const char *unit);
/* Status line / progress bar for dumps, scans and writes. */
void imp_ui_status(imp_ui_t *ui, const char *text);
void imp_ui_progress(imp_ui_t *ui, uint8_t pct);
/* Free-text line in the content slot (plain-words errors live here). */
void imp_ui_text(imp_ui_t *ui, int row, const char *text);
/* Encoder-driven yes/no. Required before any erase, write or transmit. */
bool imp_ui_confirm(imp_ui_t *ui, const char *question);
/* PWM backlight, percent. Dim by default; restore on exit. */
void imp_ui_set_backlight(imp_ui_t *ui, uint8_t pct);
/* Swap the imp to a pose for the current state (working/success/error/idle). */
void imp_ui_set_pose(imp_ui_t *ui, const char *pose_id);

/* Radio-state header icons (spec v0.1 §4): the shell owns the strip;
 * drivers report state through these so the header stays truthful. */
typedef enum {
    IMP_RADIO_SUBGHZ = 0, IMP_RADIO_NFC, IMP_RADIO_IR,
    IMP_RADIO_WIFI, IMP_RADIO_BLE, IMP_RADIO_COUNT
} imp_radio_t;
void imp_ui_radio_state(imp_radio_t radio, bool active);

/* ------------------------------------------------------------------ */
/* Storage — imp_storage_* (microSD; state lives here, not in RAM)     */
/* ------------------------------------------------------------------ */
typedef struct imp_file_s imp_file_t;

/* True when a card is mounted. Tools must run degraded, with a clear
 * on-screen note, when it is not. */
bool imp_storage_sd_ready(void);
imp_file_t *imp_storage_open(const char *path, const char *mode); /* "r","w","a" */
size_t imp_storage_read(imp_file_t *f, void *buf, size_t len);
size_t imp_storage_write(imp_file_t *f, const void *buf, size_t len);
void   imp_storage_close(imp_file_t *f);
bool   imp_storage_exists(const char *path);
imp_err_t imp_storage_remove(const char *path);
/* Enumerate saved items for picker screens (captures, remotes, payloads). */
typedef void (*imp_storage_list_cb_t)(void *ctx, const char *name);
void imp_storage_list(const char *dir, imp_storage_list_cb_t cb, void *ctx);

/* ------------------------------------------------------------------ */
/* System — imp_system_*                                               */
/* ------------------------------------------------------------------ */
int32_t  imp_system_battery_mv(void);                 /* via BATT_ADC, IO4 */
void     imp_system_set_led(uint32_t rgb);            /* 0 = off (default) */
void     imp_system_buzzer(uint32_t freq_hz, uint32_t ms);
void     imp_delay_ms(uint32_t ms);                   /* the only wait a tool may make */
void     imp_system_sleep(void);                      /* deep sleep; save state first */
void     imp_system_reboot(void);
/* True once the panic combo has fired. Tools see their loops unwind via
 * imp_delay_ms(); the shell performs the blank + deep sleep. */
bool     imp_system_panic_requested(void);

/* ------------------------------------------------------------------ */
/* GPIO — imp_gpio_*                                                   */
/* Driver-owned pins (every radio, display and bus pin in the frozen   */
/* map) refuse with IMP_ERR_BUSY. That refusal is correct behaviour.   */
/* ------------------------------------------------------------------ */
typedef enum { IMP_GPIO_INPUT = 0, IMP_GPIO_OUTPUT, IMP_GPIO_INPUT_PULLUP } imp_gpio_mode_t;
imp_err_t imp_gpio_claim(uint8_t pin, imp_gpio_mode_t mode);
void      imp_gpio_release(uint8_t pin);
imp_err_t imp_gpio_write(uint8_t pin, bool level);
bool      imp_gpio_read(uint8_t pin);

/* ------------------------------------------------------------------ */
/* Sub-GHz — imp_subghz_* (CC1101)                                     */
/* ------------------------------------------------------------------ */
#define IMP_SUBGHZ_MAX_TIMINGS 512
typedef struct {
    uint32_t freq_hz;
    int16_t  rssi_dbm;
    uint32_t timing_count;
    uint32_t timings_us[IMP_SUBGHZ_MAX_TIMINGS]; /* mark/space durations */
} imp_subghz_capture_t;

imp_err_t imp_subghz_set_freq(uint32_t hz);  /* 315/433/868/915 bands; else RANGE */
imp_err_t imp_subghz_rx_start(void);
imp_err_t imp_subghz_rx_poll(imp_subghz_capture_t *cap); /* IMP_OK when a capture is ready */
imp_err_t imp_subghz_rx_stop(void);
imp_err_t imp_subghz_save(const imp_subghz_capture_t *cap, const char *path);
imp_err_t imp_subghz_load(const char *path, imp_subghz_capture_t *cap);
imp_err_t imp_subghz_tx(const imp_subghz_capture_t *cap); /* confirm first, in the tool */
int16_t   imp_subghz_rssi(void);             /* live RSSI, dBm */

/* ------------------------------------------------------------------ */
/* NFC — imp_nfc_* (ST25R3916B, 13.56 MHz, near-contact by design)     */
/* ------------------------------------------------------------------ */
#define IMP_NFC_BLOCK_SIZE 16
typedef enum {
    IMP_NFC_TYPE_NONE = 0, IMP_NFC_TYPE_NTAG, IMP_NFC_TYPE_MIFARE_CLASSIC,
    IMP_NFC_TYPE_ISO14443A_OTHER
} imp_nfc_type_t;
typedef struct {
    imp_nfc_type_t type;
    uint8_t  uid[10];
    uint8_t  uid_len;
    uint8_t  sak;
    uint16_t atqa;
} imp_nfc_tag_t;

imp_err_t imp_nfc_poll(imp_nfc_tag_t *tag);  /* one poll for a tag in the field */
imp_err_t imp_nfc_read(const imp_nfc_tag_t *tag, uint16_t block, uint8_t *buf /*16*/);
imp_err_t imp_nfc_write(const imp_nfc_tag_t *tag, uint16_t block, const uint8_t *buf);
imp_err_t imp_nfc_dump(const imp_nfc_tag_t *tag, const char *path);

/* ------------------------------------------------------------------ */
/* Infrared — imp_ir_* (TSAL6100 TX on IO2, TSOP4838 RX on IO46)       */
/* ------------------------------------------------------------------ */
#define IMP_IR_MAX_TIMINGS 512
typedef struct {
    uint8_t  protocol;      /* 0 = raw/unknown; else IMP_IR_PROTO_* */
    uint32_t carrier_hz;    /* nominal 38000 */
    uint32_t timing_count;
    uint32_t timings_us[IMP_IR_MAX_TIMINGS];
} imp_ir_signal_t;
#define IMP_IR_PROTO_RAW   0
#define IMP_IR_PROTO_NEC   1
#define IMP_IR_PROTO_RC5   2
#define IMP_IR_PROTO_SIRC  3

imp_err_t imp_ir_learn_start(void);
imp_err_t imp_ir_learn_poll(imp_ir_signal_t *sig); /* IMP_OK when a signal is ready */
imp_err_t imp_ir_learn_stop(void);
imp_err_t imp_ir_save(const imp_ir_signal_t *sig, const char *path);
imp_err_t imp_ir_load(const char *path, imp_ir_signal_t *sig);
imp_err_t imp_ir_send(const imp_ir_signal_t *sig); /* ~110 mA pulsed; one send per call */

/* ------------------------------------------------------------------ */
/* BadUSB — imp_badusb_* (native USB, IO19/IO20)                       */
/* ------------------------------------------------------------------ */
typedef enum { IMP_BADUSB_KEYBOARD = 0, IMP_BADUSB_STORAGE, IMP_BADUSB_SERIAL } imp_badusb_mode_t;
imp_err_t imp_badusb_present(imp_badusb_mode_t mode); /* one mode at a time */
imp_err_t imp_badusb_type(const char *text);          /* keyboard mode */
imp_err_t imp_badusb_key(const char *combo);          /* e.g. "GUI+R", "CTRL+ALT+DELETE" */
imp_err_t imp_badusb_run(const char *path);           /* DuckyScript-class payload from SD */

/* ------------------------------------------------------------------ */
/* Wi-Fi / BLE — imp_wifi_* / imp_ble_* (2.4 GHz only)                 */
/* ------------------------------------------------------------------ */
#define IMP_WIFI_MAX_RESULTS 32
typedef struct {
    char     ssid[33];
    uint8_t  bssid[6];
    uint8_t  channel;
    int16_t  rssi_dbm;
    bool     encrypted;
    bool     network_reported; /* true = seen over a network, never RF-measured */
} imp_wifi_result_t;

int imp_wifi_scan(imp_wifi_result_t *results, int max); /* count, or -1 on error */
/* Watch one network/device over time: RSSI as you move, every dropout
 * timestamped. Measurement only — the audit tool's stopwatch. */
typedef void (*imp_wifi_log_cb_t)(void *ctx, int16_t rssi_dbm, bool present, uint32_t now_ms);
imp_err_t imp_wifi_watch(const uint8_t bssid[6], imp_wifi_log_cb_t cb, void *ctx);
imp_err_t imp_wifi_watch_poll(void);   /* call from the tool loop; fires cb */
void      imp_wifi_watch_stop(void);
imp_err_t imp_wifi_connect(const char *ssid); /* open or Settings-provisioned nets */

#define IMP_BLE_MAX_RESULTS 32
typedef struct {
    char     name[32];
    uint8_t  addr[6];
    int16_t  rssi_dbm;
} imp_ble_result_t;
int imp_ble_scan(imp_ble_result_t *results, int max); /* count, or -1 on error */

/* ------------------------------------------------------------------ */
/* Flash Lab — imp_flash_* (external NOR, 3.3 V only, never 5 V)       */
/* ------------------------------------------------------------------ */
imp_err_t imp_flash_rdid(uint8_t id[3]); /* EF 40 17 = W25Q64, C2 20 17 = MX25L6406E */
imp_err_t imp_flash_read(uint32_t addr, uint8_t *buf, size_t len);
imp_err_t imp_flash_dump(const char *path);            /* full 8 MB to microSD */
imp_err_t imp_flash_write(uint32_t addr, const uint8_t *buf, size_t len);
imp_err_t imp_flash_erase(uint32_t addr, size_t len);  /* checks WP# lock first */

/* ------------------------------------------------------------------ */
/* App contract (pack section 9)                                       */
/* name = folder = _main prefix. Manifest fields are checked by the    */
/* launcher before entry; a false `requires` claim is how              */
/* IMP_ERR_NO_HW happens.                                              */
/* ------------------------------------------------------------------ */
typedef struct imp_app_s {
    const char *name;      /* lowercase, underscores */
    const char *title;     /* menu label — must fit the 172 px menu */
    const char *category;  /* Tools, RF, NFC, IR, USB, System */
    const char *icon;      /* icon id */
    const char *pose;      /* imp pose shown while selected — must exist */
    const char *requires;  /* subghz,nfc,ir,flash,sd,wifi — or "none" */
    uint32_t    stack;     /* task stack bytes; start at 4096 */
    int       (*main_fn)(void);
} imp_app_t;

/* Registry access for the launcher/shell. */
int                imp_app_count(void);
const imp_app_t   *imp_app_get(int index);
bool               imp_app_hw_available(const char *requires_token);

#ifdef __cplusplus
} /* extern "C" */

/* Registration: a tool defines one imp_app_t and registers it.
 *   static const imp_app_t app = { "my_tool", "My Tool", ... , my_tool_main };
 *   IMP_APP_REGISTER(app);
 * The descriptor is copied into the registry at static-init time. */
namespace imp {
struct AppRegistrar {
    explicit AppRegistrar(const imp_app_t &app);
};
} /* namespace imp */
#define IMP_APP_REGISTER(app_desc) \
    static ::imp::AppRegistrar imp_app_registrar_instance(app_desc)

#endif /* __cplusplus */

#endif /* IMP_API_H */
