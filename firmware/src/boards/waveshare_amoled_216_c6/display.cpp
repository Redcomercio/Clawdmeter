#include "../../hal/display_hal.h"
#include "../../hal/imu_hal.h"
#include "../../brightness.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

// C6 AMOLED-2.16 uses an SH8601 panel. Rotation buffer lives in internal
// SRAM (37 KB fits comfortably alongside everything else at ~93 KB used).

#define ROT_BUF_LINES 20   // matches BUF_LINES for non-PSRAM boards in main.cpp
static uint16_t* rot_buf = nullptr;

static Arduino_DataBus* bus = nullptr;
static Arduino_SH8601*  gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(
        LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    gfx = new Arduino_SH8601(
        bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT);
}

// Vendor-specific init commands from the Waveshare C6-2.16 BSP
// (02_Example/Arduino-v3.3.3/09_LVGL_V9_Test/bsp_lvgl_port.cpp in the
// waveshareteam/ESP32-C6-Touch-AMOLED-2.16 repo). The stock Arduino_GFX
// SH8601 init does SLPOUT + NORON + INVOFF + PIXFMT + DISPON + brightness,
// which is enough for the AMOLED-1.8 panel but leaves this 2.16 panel
// dark. The page-switch sequence (0xFE 0x20 ... 0xFE 0x00) writes two
// panel-specific manufacturer registers (0x19 and 0x1C) that gate the
// driving voltages — without them the panel stays black even with the
// rails up and the reset pulse applied.
static void send_vendor_init(Arduino_DataBus* b) {
    b->beginWrite();
    b->writeC8D8(0xFE, 0x20);    // enter manufacturer command page 0x20
    b->writeC8D8(0x19, 0x10);    // panel driving
    b->writeC8D8(0x1C, 0xA0);    // panel driving
    b->writeC8D8(0xFE, 0x00);    // back to user command page
    b->writeC8D8(0xC4, 0x80);    // SPI mode control
    b->writeC8D8(0x36, 0x30);    // MADCTL (BSP value)
    b->writeC8D8(0x53, 0x20);    // CTRL display 1 (brightness control on)
    b->writeC8D8(0x51, 0xFF);    // brightness = max
    b->writeC8D8(0x63, 0xFF);    // HBM brightness = max
    b->writeCommand(0x29);       // DISPON (idempotent — stock init already did this)
    b->endWrite();
    delay(20);
}

void display_hal_begin(void) {
    gfx->begin();
    send_vendor_init(bus);       // patch up panel-specific regs the stock init misses
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);

    rot_buf = (uint16_t*)heap_caps_malloc(LCD_WIDTH * ROT_BUF_LINES * 2, MALLOC_CAP_INTERNAL);
}

void display_hal_set_brightness(uint8_t level) {
    if (gfx) gfx->setBrightness(level);
}

void display_hal_fill_screen(uint16_t color) {
    if (gfx) gfx->fillScreen(color);
}

static void rotate_strip(const uint16_t* src, int32_t w, int32_t h,
                         int32_t sx, int32_t sy, uint8_t r,
                         int32_t* dx, int32_t* dy, int32_t* dw, int32_t* dh) {
    const int S = LCD_WIDTH;
    switch (r) {
    case 1: // 90° CW
        *dw = h; *dh = w;
        *dx = S - sy - h; *dy = sx;
        for (int32_t y = 0; y < h; y++)
            for (int32_t x = 0; x < w; x++)
                rot_buf[x * h + (h - 1 - y)] = src[y * w + x];
        break;
    case 2: // 180°
        *dw = w; *dh = h;
        *dx = S - sx - w; *dy = S - sy - h;
        for (int32_t y = 0; y < h; y++)
            for (int32_t x = 0; x < w; x++)
                rot_buf[(h - 1 - y) * w + (w - 1 - x)] = src[y * w + x];
        break;
    case 3: // 270° CW
        *dw = h; *dh = w;
        *dx = sy; *dy = S - sx - w;
        for (int32_t y = 0; y < h; y++)
            for (int32_t x = 0; x < w; x++)
                rot_buf[(w - 1 - x) * h + y] = src[y * w + x];
        break;
    default:
        *dx = sx; *dy = sy; *dw = w; *dh = h;
        break;
    }
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (!gfx) return;
    uint8_t r = imu_hal_rotation_quadrant();
    if (r == 0 || !rot_buf) {
        gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
        return;
    }
    int32_t dx, dy, dw, dh;
    rotate_strip(pixels, w, h, x, y, r, &dx, &dy, &dw, &dh);
    gfx->draw16bitRGBBitmap(dx, dy, rot_buf, dw, dh);
}

void display_hal_tick(void) {
    static uint8_t  last_rotation = 0;
    static uint8_t  ramp_step = 0;
    static uint32_t ramp_last = 0;

    uint8_t rot = imu_hal_rotation_quadrant();
    if (rot != last_rotation) {
        display_hal_set_brightness(0);
        last_rotation = rot;
        lv_obj_invalidate(lv_screen_active());
        ramp_step = 1;
        return;
    }
    if (ramp_step == 0) return;
    uint32_t now = millis();
    if (now - ramp_last < 25) return;
    ramp_last = now;

    static const uint8_t pct[] = {30, 60, 85, 100};
    uint8_t target = brightness_get();
    display_hal_set_brightness((uint8_t)(((uint16_t)target * pct[ramp_step - 1]) / 100));
    if (ramp_step >= 4) ramp_step = 0;
    else                ramp_step++;
}

// Mirrors the CO5300/SH8601 even-alignment pattern from the other ports.
// Harmless on SH8601, kept for consistency.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    *x1 = *x1 & ~1;
    *y1 = *y1 & ~1;
    *x2 = *x2 | 1;
    *y2 = *y2 | 1;
}
