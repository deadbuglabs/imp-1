/* imp_registry.cpp — the launcher's app table (pack section 9).
 * Tools register one imp_app_t each via IMP_APP_REGISTER; the shell
 * enumerates this table for the home menu and checks `requires` before
 * entry. Hardware availability below is build-1 truth: radios fitted are
 * subghz/nfc/ir/flash/wifi; rfid (125 kHz, D-15) and iButton/1-Wire
 * (D-02) are NOT fitted and report unavailable. */
#include "imp_api.h"
#include <string.h>

#define IMP_MAX_APPS 24
static imp_app_t registry[IMP_MAX_APPS];
static int registry_count = 0;

namespace imp {
AppRegistrar::AppRegistrar(const imp_app_t &app) {
    if (registry_count < IMP_MAX_APPS) registry[registry_count++] = app;
}
} /* namespace imp */

int imp_app_count(void) { return registry_count; }

const imp_app_t *imp_app_get(int index) {
    if (index < 0 || index >= registry_count) return nullptr;
    return &registry[index];
}

/* requires is a comma list of tokens; "none" (or empty) always passes. */
bool imp_app_hw_available(const char *requires_token) {
    if (!requires_token) return true;
    static const char *fitted[] = {
        "subghz", "nfc", "ir", "flash", "sd", "wifi", "ble",
        "badusb", "usb", "gpio", "u2f", nullptr
    };
    for (int i = 0; fitted[i]; i++)
        if (strcmp(requires_token, fitted[i]) == 0) return true;
    return false;
}
