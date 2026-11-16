/* disp_st7789.cpp — Waveshare 1.47" ST7789V3, 172x320 portrait, SPI.
 * The panel GRAM is 240 wide; the 172-px active area is centred, so the
 * column offset is 34. Rendering is strip-based (no full framebuffer):
 * every primitive renders into a small RAM strip and pushes once, which
 * is what makes partial redraw cheap (spec v0.1 section 2). */
#include "imp_internal.h"
#include "imp_font.h"
#include "imp_sprites.h"

/* Display is the fastest device on the bus; 40 MHz is the design clock. */
static SPISettings disp_spi(40000000, MSBFIRST, SPI_MODE0);
static uint8_t bl_pct = 30; /* dim by default — stealth rule */

static void disp_cmd(uint8_t cmd) {
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(disp_spi);
    digitalWrite(IMP_PIN_DISP_CS, LOW);
    digitalWrite(IMP_PIN_DISP_DC, LOW);
    spi.transfer(cmd);
    digitalWrite(IMP_PIN_DISP_CS, HIGH);
    spi.endTransaction();
}

static void disp_cmd_data(uint8_t cmd, const uint8_t *data, size_t len) {
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(disp_spi);
    digitalWrite(IMP_PIN_DISP_CS, LOW);
    digitalWrite(IMP_PIN_DISP_DC, LOW);
    spi.transfer(cmd);
    digitalWrite(IMP_PIN_DISP_DC, HIGH);
    for (size_t i = 0; i < len; i++) spi.transfer(data[i]);
    digitalWrite(IMP_PIN_DISP_CS, HIGH);
    spi.endTransaction();
}

static void disp_set_window(int x, int y, int w, int h) {
    const int X_OFF = 34, Y_OFF = 0;
    uint8_t buf[4];
    buf[0] = (x + X_OFF) >> 8; buf[1] = (x + X_OFF) & 0xFF;
    buf[2] = (x + X_OFF + w - 1) >> 8; buf[3] = (x + X_OFF + w - 1) & 0xFF;
    disp_cmd_data(0x2A, buf, 4); /* CASET */
    buf[0] = (y + Y_OFF) >> 8; buf[1] = (y + Y_OFF) & 0xFF;
    buf[2] = (y + Y_OFF + h - 1) >> 8; buf[3] = (y + Y_OFF + h - 1) & 0xFF;
    disp_cmd_data(0x2B, buf, 4); /* RASET */
    disp_cmd(0x2C);              /* RAMWR */
}

/* Push w*h pixels from a caller buffer in one transaction. */
static void disp_push(int x, int y, int w, int h, const uint16_t *px) {
    if (w <= 0 || h <= 0) return;
    disp_set_window(x, y, w, h);
    SPIClass &spi = imp_bus_spi();
    spi.beginTransaction(disp_spi);
    digitalWrite(IMP_PIN_DISP_CS, LOW);
    digitalWrite(IMP_PIN_DISP_DC, HIGH);
    const uint8_t *b = (const uint8_t *)px;
    size_t n = (size_t)w * h * 2;
    /* ST7789 wants MSB first; pixels are stored big-endian by callers
     * via the swap in the strip renderers below. */
    spi.writeBytes(b, n);
    digitalWrite(IMP_PIN_DISP_CS, HIGH);
    spi.endTransaction();
}

static inline uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

void disp_init(void) {
    pinMode(IMP_PIN_DISP_CS, OUTPUT);
    digitalWrite(IMP_PIN_DISP_CS, HIGH);
    pinMode(IMP_PIN_DISP_DC, OUTPUT);
    pinMode(IMP_PIN_DISP_RST, OUTPUT);
    digitalWrite(IMP_PIN_DISP_RST, LOW);
    delay(20);
    digitalWrite(IMP_PIN_DISP_RST, HIGH);
    delay(120);

    disp_cmd(0x01); delay(120);            /* SWRESET */
    disp_cmd(0x11); delay(120);            /* SLPOUT */
    uint8_t v;
    v = 0x55; disp_cmd_data(0x3A, &v, 1);  /* COLMOD: 16-bit RGB565 */
    v = 0x00; disp_cmd_data(0x36, &v, 1);  /* MADCTL: portrait, no mirror */
    v = 0x0C; disp_cmd_data(0xB0, &v, 1);  /* RAM control (module default) */
    disp_cmd(0x21);                        /* INVON — this panel is inverted */
    disp_cmd(0x13);                        /* NORON */
    disp_cmd(0x29);                        /* DISPON */
    disp_set_backlight(bl_pct);
    disp_fill_rect(0, 0, IMP_DISP_W, IMP_DISP_H, IMP_COLOUR_BG);
}

void disp_set_backlight(uint8_t pct) {
    bl_pct = pct > 100 ? 100 : pct;
    static bool ledc_ready = false;
    if (!ledc_ready) {
        ledcSetup(0, 5000, 8);
        ledcAttachPin(IMP_PIN_DISP_BL, 0);
        ledc_ready = true;
    }
    ledcWrite(0, (uint32_t)bl_pct * 255 / 100);
}

void disp_fill_rect(int x, int y, int w, int h, uint16_t colour) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > IMP_DISP_W) w = IMP_DISP_W - x;
    if (y + h > IMP_DISP_H) h = IMP_DISP_H - y;
    if (w <= 0 || h <= 0) return;
    static uint16_t strip[IMP_DISP_W * 8];
    uint16_t c = be16(colour);
    int chunk = w > IMP_DISP_W ? IMP_DISP_W : w;
    for (int i = 0; i < chunk * 8; i++) strip[i] = c;
    while (h > 0) {
        int rows = h > 8 ? 8 : h;
        disp_push(x, y, w, rows, strip);
        y += rows; h -= rows;
    }
}

int disp_text_width(const char *text, uint8_t scale) {
    return (int)strlen(text) * IMP_FONT_ADV * (scale ? scale : 1);
}

/* Render one text line into a strip and push it once. */
void disp_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg, uint8_t scale) {
    if (scale == 0) scale = 1;
    int tw = disp_text_width(text, scale);
    if (tw <= 0) return;
    if (x + tw > IMP_DISP_W) tw = IMP_DISP_W - x;
    if (tw <= 0) return;
    const int hh = IMP_FONT_H * scale;
    static uint16_t strip[IMP_DISP_W * 7 * 2];
    if (hh > 14) return; /* scale <= 2 supported by the strip */
    uint16_t fgc = be16(fg), bgc = be16(bg);
    for (int row = 0; row < hh; row++) {
        uint16_t *line = strip + (size_t)row * tw;
        int grow = row / scale;
        for (int i = 0; i < tw; i++) {
            int ci = i / (IMP_FONT_ADV * scale);
            int cx = (i / scale) % IMP_FONT_ADV;
            uint16_t px = bgc;
            if (cx < IMP_FONT_W) {
                char ch = text[ci];
                if (ch >= 32 && ch <= 126) {
                    uint8_t col = imp_font_5x7[ch - 32][cx];
                    if (col & (1 << grow)) px = fgc;
                }
            }
            line[i] = px;
        }
    }
    disp_push(x, y, tw, hh, strip);
}

void disp_draw_sprite(int x, int y, const imp_sprite_t *spr, uint16_t fg, uint16_t bg) {
    if (!spr) return;
    static uint16_t strip[96 * 8];
    uint16_t fgc = be16(fg), bgc = be16(bg);
    int w = spr->w, h = spr->h;
    if (x + w > IMP_DISP_W) w = IMP_DISP_W - x;
    for (int y0 = 0; y0 < h; y0 += 8) {
        int rows = (h - y0) > 8 ? 8 : (h - y0);
        for (int r = 0; r < rows; r++) {
            const uint8_t *src = spr->bits + (size_t)(y0 + r) * ((spr->w + 7) / 8);
            for (int i = 0; i < w; i++)
                strip[(size_t)r * w + i] = (src[i >> 3] & (0x80 >> (i & 7))) ? fgc : bgc;
        }
        disp_push(x, y + y0, w, rows, strip);
    }
}

void disp_blank_now(void) {
    disp_set_backlight(0);
    disp_fill_rect(0, 0, IMP_DISP_W, IMP_DISP_H, IMP_COLOUR_BG);
}

void disp_full_repaint(void) {
    disp_fill_rect(0, 0, IMP_DISP_W, IMP_DISP_H, IMP_COLOUR_BG);
}
