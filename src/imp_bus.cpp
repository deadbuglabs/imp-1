/* imp_bus.cpp — the one shared SPI bus (pack section 4).
 * Display, CC1101, external flash, microSD and ST25R3916B all share
 * MOSI IO9 / SCLK IO10 / MISO IO11. Only one CS is ever asserted; the
 * per-device drivers use SPI transactions, and this lock covers
 * multi-transaction sequences. Never held across a wait. */
#include "imp_internal.h"

static SPIClass bus(FSPI);
static SemaphoreHandle_t bus_mutex = nullptr;
static bool bus_ready = false;

SPIClass &imp_bus_spi(void) { return bus; }

void imp_bus_init(void) {
    if (bus_ready) return;
    bus_mutex = xSemaphoreCreateMutex();
    bus.begin(IMP_PIN_SPI_SCLK, IMP_PIN_SPI_MISO, IMP_PIN_SPI_MOSI, -1);
    bus_ready = true;
}

void imp_bus_lock(void) {
    if (bus_mutex) xSemaphoreTake(bus_mutex, portMAX_DELAY);
}

void imp_bus_unlock(void) {
    if (bus_mutex) xSemaphoreGive(bus_mutex);
}
