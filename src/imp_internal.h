/*
 * imp_internal.h — cross-module internals. NOT part of the tool contract;
 * tools include imp_api.h only. Drivers and the shell live below the API.
 */
#ifndef IMP_INTERNAL_H
#define IMP_INTERNAL_H

#include <Arduino.h>
#include <SPI.h>
#include "imp_api.h"
#include "imp_sprites.h"
#include "pins.h"

/* ---- shared SPI bus (imp_bus.cpp) ---- */
SPIClass &imp_bus_spi(void);
void imp_bus_init(void);
/* Cross-task politeness lock: take before a multi-transaction sequence,
 * give immediately after. Never hold across a wait (pack section 4). */
void imp_bus_lock(void);
void imp_bus_unlock(void);

/* ---- display (drivers/disp_st7789.cpp) ---- */
#define IMP_DISP_W 172
#define IMP_DISP_H 320
#define IMP_COLOUR_BG     0x0000  /* black */
#define IMP_COLOUR_IMP    0xF800  /* placeholder — set from IMP_RGB565_IMP */
#define IMP_RGB565(r, g, b) ((uint16_t)(((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define IMP_COLOUR_RED    IMP_RGB565(0xF8, 0x0C, 0x00)  /* the imp: #f80c00 */
#define IMP_COLOUR_FG     0xFFFF
#define IMP_COLOUR_DIM    IMP_RGB565(0x80, 0x80, 0x80)
#define IMP_COLOUR_ACCENT IMP_RGB565(0x00, 0xC8, 0xFF)
#define IMP_COLOUR_OK     IMP_RGB565(0x20, 0xE0, 0x60)
#define IMP_COLOUR_WARN   IMP_RGB565(0xFF, 0xB0, 0x00)
#define IMP_COLOUR_ERR    IMP_RGB565(0xFF, 0x30, 0x30)

void disp_init(void);
void disp_fill_rect(int x, int y, int w, int h, uint16_t colour);
void disp_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg, uint8_t scale);
int  disp_text_width(const char *text, uint8_t scale);
void disp_draw_sprite(int x, int y, const imp_sprite_t *spr, uint16_t fg, uint16_t bg);
void disp_set_backlight(uint8_t pct);
void disp_blank_now(void);            /* panic path: black frame + BL off */
void disp_full_repaint(void);         /* wake path only (spec: full frame) */

/* ---- input (imp_input.cpp) ---- */
/* The active UI's callback receives events from the pump. The pump runs
 * inside imp_delay_ms() and the shell loop — never in ISR context. */
void imp_input_init(void);
void imp_input_pump(void);
void imp_input_set_active(imp_input_cb_t cb, void *ctx); /* the screen owner */
void imp_input_set_modal(imp_input_cb_t cb, void *ctx); /* confirm dialogs */
void imp_input_clear_modal(void);
uint32_t imp_input_last_activity_ms(void);
bool imp_input_gpio_claimed(uint8_t pin); /* keypad scan respects GPIO claims */

/* ---- UI internals (imp_ui.cpp) ---- */
void imp_ui_refresh_header(void);

/* ---- panic hooks (imp_system.cpp) ---- */
typedef void (*imp_panic_hook_t)(void);
void imp_system_register_panic_hook(imp_panic_hook_t fn);
void imp_panic_execute(void); /* blank, silence, radios off, deep sleep */

/* ---- layout geometry (spec v0.1 + approved mock) ---- */
#define IMP_HDR_H        18                 /* header strip */
#define IMP_FOOT_Y       304                /* footer legend line */
#define IMP_MASCOT_X     2
#define IMP_MASCOT_Y     64
#define IMP_MASCOT_W     82
#define IMP_CONTENT_X    88                 /* content slot / list column */
#define IMP_CONTENT_W    (IMP_DISP_W - IMP_CONTENT_X - 2)
#define IMP_LIST_ROW_H   23
#define IMP_LIST_Y       (IMP_HDR_H + 4)
#define IMP_LIST_MAX     12                 /* visible rows: 12 * 23px fits 320 */
#define IMP_LIST_CACHE_MAX 16               /* lists scroll a 12-row window */

#endif /* IMP_INTERNAL_H */
