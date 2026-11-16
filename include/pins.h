/*
 * pins.h — IMP-1 frozen pin map, V5-FREEZE-2026-11-15.
 *
 * Source: IMP-1 Tool Builder's Pack section 3 (41-pad sign-off matrix).
 * If firmware disagrees with this file, the firmware is wrong.
 * GPIO numbers are what the ESP32-S3-WROOM-1-N8R2 presents to software.
 */
#ifndef IMP_PINS_H
#define IMP_PINS_H

/* ---- Shared SPI bus (one bus, five chip selects — driver enforces) ---- */
#define IMP_PIN_SPI_MOSI   9
#define IMP_PIN_SPI_SCLK   10
#define IMP_PIN_SPI_MISO   11

/* ---- Chip selects (only one asserted at a time) ---- */
#define IMP_PIN_DISP_CS    17   /* display, J3 pin 2 */
#define IMP_PIN_CC_CS      18   /* CC1101 CSn, U9 pin 7 */
#define IMP_PIN_SD_CS      3    /* microSD, J5 — strapping pin, D-10 accepted */
#define IMP_PIN_FLASH_CS   21   /* external NOR flash, U2 pin 1 */
#define IMP_PIN_ST25_CS    47   /* ST25R3916B, via J11 pin 6 */

/* ---- Display control (ST7789V3 module on J3) ---- */
#define IMP_PIN_DISP_DC    16   /* J3 pin 3 */
#define IMP_PIN_DISP_RST   15   /* J3 pin 4 */
#define IMP_PIN_DISP_BL    7    /* J3 pin 7 — PWM backlight, dim by default */

/* ---- NFC ---- */
#define IMP_PIN_ST25_IRQ   48   /* ST25R3916B interrupt, via J11 pin 7 */

/* ---- Infrared (IR daughterboard on J12) ---- */
#define IMP_PIN_IR_RX      46   /* TSOP4838 output — INPUT-ONLY pad, D-02 */
#define IMP_PIN_IR_TX      2    /* TSAL6100 drive, via J12 pin 4 */

/* ---- Native USB (fixed by silicon — never reassign) ---- */
#define IMP_PIN_USB_DN     19
#define IMP_PIN_USB_DP     20

/* ---- Human input ---- */
#define IMP_PIN_ENC_A      5    /* SW1 phase A */
#define IMP_PIN_ENC_B      36   /* SW1 phase B (octal-PSRAM pad, N8R2 only) */
#define IMP_PIN_ENC_PUSH   37   /* SW1 push = OK (octal-PSRAM pad, N8R2 only) */
#define IMP_PIN_BOOT       0    /* SW2 momentary to GND. Never repurpose. */
/* Keypad matrix on J7: rows 1-4, then columns 1-4 */
#define IMP_PIN_KP_ROW1    12
#define IMP_PIN_KP_ROW2    38
#define IMP_PIN_KP_ROW3    39
#define IMP_PIN_KP_ROW4    40
#define IMP_PIN_KP_COL1    41
#define IMP_PIN_KP_COL2    42
#define IMP_PIN_KP_COL3    14
#define IMP_PIN_KP_COL4    13

/* ---- Switched outputs (all default OFF — stealth rule) ---- */
#define IMP_PIN_LED_DATA   6    /* WS2812B status LED (D1) */
#define IMP_PIN_A9_PWR     8    /* A9 camera 5 V high-side switch */
#define IMP_PIN_BUZZER     35   /* piezo via MMBT3904 low-side (octal pad) */
#define IMP_PIN_FLASHLED   1    /* flash LED gate, AO3400A low-side, J15 */

/* ---- Sensing + debug ---- */
#define IMP_PIN_BATT_ADC   4    /* ADC1_CH3, divider 300k/100k: 1.05 V @ 4.2 V */
#define IMP_PIN_UART_TX    43   /* TP1 — debug only, never a tool pin */
#define IMP_PIN_UART_RX    44   /* TP2 — debug only, never a tool pin */

/* ---- Never touch ---- */
/* IO45: VDD_SPI voltage-select strap. Clean NC, no load ever. This is why
 * iButton has no home in build 1 (D-02). No constant is defined on purpose:
 * code that needs a number for IO45 does not get one from this file. */

#endif /* IMP_PINS_H */
