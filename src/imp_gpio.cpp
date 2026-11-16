/* imp_gpio.cpp — tool GPIO claims.
 * Policy (pack sections 3 + 11): every radio, bus, display and output pin
 * is driver-owned and refuses with IMP_ERR_BUSY — that refusal is correct
 * behaviour, not a bug. UART test pads TP1/TP2 stay reserved for debug.
 * IO45 (VDD_SPI strap) and pads the WROOM-1-N8R2 does not break out
 * refuse with IMP_ERR_NO_HW. The keypad matrix pins are the honest free
 * GPIO: claimable, and the keypad scanner respects live claims. */
#include "imp_internal.h"

static uint64_t claimed_mask = 0;

static bool pin_exists(uint8_t pin) {
    /* WROOM-1-N8R2 broken-out pads: 0-21, 35-42, 44-48 (43/44 = UART0). */
    return pin <= 21 || (pin >= 35 && pin <= 42) || (pin >= 44 && pin <= 48);
}

static bool pin_driver_owned(uint8_t pin) {
    switch (pin) {
        case IMP_PIN_SPI_MOSI: case IMP_PIN_SPI_SCLK: case IMP_PIN_SPI_MISO:
        case IMP_PIN_DISP_CS: case IMP_PIN_CC_CS: case IMP_PIN_SD_CS:
        case IMP_PIN_FLASH_CS: case IMP_PIN_ST25_CS:
        case IMP_PIN_DISP_DC: case IMP_PIN_DISP_RST: case IMP_PIN_DISP_BL:
        case IMP_PIN_ST25_IRQ:
        case IMP_PIN_IR_RX: case IMP_PIN_IR_TX:
        case IMP_PIN_USB_DN: case IMP_PIN_USB_DP:
        case IMP_PIN_ENC_A: case IMP_PIN_ENC_B: case IMP_PIN_ENC_PUSH:
        case IMP_PIN_BOOT:
        case IMP_PIN_LED_DATA: case IMP_PIN_A9_PWR: case IMP_PIN_BUZZER:
        case IMP_PIN_FLASHLED: case IMP_PIN_BATT_ADC:
        case IMP_PIN_UART_TX: case IMP_PIN_UART_RX:
            return true;
        default:
            return false;
    }
}

bool imp_input_gpio_claimed(uint8_t pin) {
    return pin < 64 && (claimed_mask & (1ULL << pin));
}

imp_err_t imp_gpio_claim(uint8_t pin, imp_gpio_mode_t mode) {
    if (pin == 45 || !pin_exists(pin)) return IMP_ERR_NO_HW; /* strap / not fitted */
    if (pin_driver_owned(pin)) return IMP_ERR_BUSY;
    if (imp_input_gpio_claimed(pin)) return IMP_ERR_BUSY;
    switch (mode) {
        case IMP_GPIO_OUTPUT: pinMode(pin, OUTPUT); break;
        case IMP_GPIO_INPUT_PULLUP: pinMode(pin, INPUT_PULLUP); break;
        default: pinMode(pin, INPUT); break;
    }
    claimed_mask |= (1ULL << pin);
    return IMP_OK;
}

void imp_gpio_release(uint8_t pin) {
    if (pin >= 64) return;
    claimed_mask &= ~(1ULL << pin);
    pinMode(pin, INPUT);
}

imp_err_t imp_gpio_write(uint8_t pin, bool level) {
    if (!imp_input_gpio_claimed(pin)) return IMP_ERR_BUSY;
    digitalWrite(pin, level ? HIGH : LOW);
    return IMP_OK;
}

bool imp_gpio_read(uint8_t pin) {
    return digitalRead(pin) == HIGH;
}
