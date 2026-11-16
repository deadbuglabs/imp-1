/* net.cpp — imp_wifi_* / imp_ble_* : 2.4 GHz inventory + AP watch.
 *
 * Measurement only. Scans are the standard active scans the radios
 * already do; the watch repeats them against one BSSID and reports
 * RSSI / presence per sweep. No association is attempted except by
 * imp_wifi_connect(), which joins a network the user names.
 *
 * Radio lifetime: WiFi STA is enabled lazily and left on while a watch
 * is active; imp_wifi_watch_stop() disconnects and powers the radio
 * down. A plain scan leaves STA mode on (cheap, and the audit tool
 * chains scan -> watch). BLE is initialised lazily once and left up:
 * BLE + Wi-Fi coexistence on this stack is fine, and repeated
 * init/deinit cycles are the fragile path. The panic hook stops the
 * watch and kills both radios.
 */
#include "imp_internal.h"
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEAddress.h>
#include <string.h>

static void net_ensure_hook(void); /* defined with the panic hook, below */

/* ------------------------------------------------------------------ */
/* Wi-Fi                                                               */
/* ------------------------------------------------------------------ */
static bool wifi_sta_on = false;

static void wifi_ensure_sta(void) {
    net_ensure_hook();
    if (wifi_sta_on) return;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    wifi_sta_on = true;
}

int imp_wifi_scan(imp_wifi_result_t *results, int max) {
    if (!results || max <= 0) return -1;
    if (max > IMP_WIFI_MAX_RESULTS) max = IMP_WIFI_MAX_RESULTS;
    wifi_ensure_sta();
    imp_ui_radio_state(IMP_RADIO_WIFI, true);
    /* The one sanctioned blocking call in this family: the audit tool
     * invokes it once per user action. */
    int n = WiFi.scanNetworks();
    if (n < 0) {
        imp_ui_radio_state(IMP_RADIO_WIFI, false);
        return -1;
    }
    int out = n < max ? n : max;
    for (int i = 0; i < out; i++) {
        imp_wifi_result_t *r = &results[i];
        String ssid = WiFi.SSID(i);
        snprintf(r->ssid, sizeof(r->ssid), "%s", ssid.c_str());
        uint8_t *b = WiFi.BSSID(i);
        if (b) memcpy(r->bssid, b, 6); else memset(r->bssid, 0, 6);
        r->channel = (uint8_t)WiFi.channel(i);
        r->rssi_dbm = (int16_t)WiFi.RSSI(i);
        r->encrypted = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        r->network_reported = false; /* RF-measured, here, by this radio */
    }
    WiFi.scanDelete();
    imp_ui_radio_state(IMP_RADIO_WIFI, false);
    return out;
}

/* ---------------- watch one AP ---------------- */
typedef struct {
    bool active;
    uint8_t bssid[6];
    imp_wifi_log_cb_t cb;
    void *ctx;
    bool scan_in_flight;
    uint32_t last_sweep_ms;
} wifi_watch_t;

static wifi_watch_t watch;

imp_err_t imp_wifi_watch(const uint8_t bssid[6], imp_wifi_log_cb_t cb, void *ctx) {
    if (!bssid || !cb) return IMP_ERR_RANGE;
    wifi_ensure_sta();
    memcpy(watch.bssid, bssid, 6);
    watch.cb = cb;
    watch.ctx = ctx;
    watch.active = true;
    watch.scan_in_flight = false;
    watch.last_sweep_ms = 0; /* first poll sweeps immediately */
    imp_ui_radio_state(IMP_RADIO_WIFI, true);
    return IMP_OK;
}

imp_err_t imp_wifi_watch_poll(void) {
    if (!watch.active) return IMP_OK;
    uint32_t now = millis();
    if (watch.scan_in_flight) {
        int c = WiFi.scanComplete();
        if (c == -1) return IMP_OK; /* still running */
        watch.scan_in_flight = false;
        watch.last_sweep_ms = now;
        if (c >= 0) {
            bool present = false;
            int16_t rssi = 0;
            for (int i = 0; i < c; i++) {
                uint8_t *b = WiFi.BSSID(i);
                if (b && memcmp(b, watch.bssid, 6) == 0) {
                    present = true;
                    rssi = (int16_t)WiFi.RSSI(i);
                    break;
                }
            }
            WiFi.scanDelete();
            /* present=false is the dropout record the audit wants. */
            watch.cb(watch.ctx, rssi, present, now);
        } else {
            WiFi.scanDelete(); /* failed sweep: no callback, retry next cycle */
        }
        return IMP_OK;
    }
    if (now - watch.last_sweep_ms >= 2000) {
        /* Async scan keeps the caller's loop (and panic) live. */
        if (WiFi.scanNetworks(true) == -2 /* WIFI_SCAN_FAILED */) {
            watch.last_sweep_ms = now; /* back off one cycle */
        } else {
            watch.scan_in_flight = true;
        }
    }
    return IMP_OK;
}

void imp_wifi_watch_stop(void) {
    if (watch.scan_in_flight) {
        WiFi.scanDelete();
        watch.scan_in_flight = false;
    }
    watch.active = false;
    watch.cb = nullptr;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifi_sta_on = false;
    imp_ui_radio_state(IMP_RADIO_WIFI, false);
}

imp_err_t imp_wifi_connect(const char *ssid) {
    if (!ssid || !*ssid) return IMP_ERR_RANGE;
    wifi_ensure_sta();
    /* Open-network join. Credentialed join is provisioned out-of-band
     * via Settings in a later pass — credentials are never stored in
     * tool or driver source, and this signature carries no PSK. */
    WiFi.begin(ssid);
    imp_ui_radio_state(IMP_RADIO_WIFI, true);
    uint32_t start = millis();
    while (millis() - start < 10000) {
        if (WiFi.status() == WL_CONNECTED) return IMP_OK;
        if (imp_system_panic_requested()) return IMP_ERR_IO;
        imp_delay_ms(100);
    }
    imp_ui_radio_state(IMP_RADIO_WIFI, false);
    return IMP_ERR_IO;
}

/* ------------------------------------------------------------------ */
/* BLE                                                                 */
/* ------------------------------------------------------------------ */
static bool ble_ready = false;

int imp_ble_scan(imp_ble_result_t *results, int max) {
    if (!results || max <= 0) return -1;
    if (max > IMP_BLE_MAX_RESULTS) max = IMP_BLE_MAX_RESULTS;
    if (!ble_ready) {
        net_ensure_hook();
        BLEDevice::init("");
        ble_ready = true; /* left initialised; see file header */
    }
    BLEScan *scan = BLEDevice::getScan();
    if (!scan) return -1;
    scan->setActiveScan(true);
    imp_ui_radio_state(IMP_RADIO_BLE, true);
    BLEScanResults found = scan->start(3, false); /* 3 s sweep */
    int count = found.getCount();
    int out = count < max ? count : max;
    for (int i = 0; i < out; i++) {
        BLEAdvertisedDevice dev = found.getDevice(i);
        imp_ble_result_t *r = &results[i];
        if (dev.haveName())
            snprintf(r->name, sizeof(r->name), "%s", dev.getName().c_str());
        else
            r->name[0] = '\0';
        BLEAddress addr = dev.getAddress();
        uint8_t(*native)[6] = addr.getNative();
        if (native) memcpy(r->addr, native, 6); else memset(r->addr, 0, 6);
        r->rssi_dbm = (int16_t)dev.getRSSI();
    }
    scan->clearResults();
    imp_ui_radio_state(IMP_RADIO_BLE, false);
    return out;
}

/* ------------------------------------------------------------------ */
/* Panic: radios off, watch forgotten                                  */
/* ------------------------------------------------------------------ */
static void net_panic_hook(void) {
    imp_wifi_watch_stop();
    if (ble_ready) {
        BLEScan *scan = BLEDevice::getScan();
        if (scan) scan->stop();
    }
    imp_ui_radio_state(IMP_RADIO_BLE, false);
}

static bool panic_hook_registered = false;
static void net_ensure_hook(void) {
    if (!panic_hook_registered) {
        imp_system_register_panic_hook(net_panic_hook);
        panic_hook_registered = true;
    }
}
