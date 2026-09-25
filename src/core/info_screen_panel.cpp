#ifdef HAS_INFO_SCREEN
// Only file that includes M5GFX, so its names never meet TFT_eSPI's.
#include "info_screen_panel.h"

#include <lgfx/v1/LGFXBase.hpp>
#include <lgfx/v1/panel/Panel_ST7789.hpp>
#include <lgfx/v1/platforms/esp32/Bus_SPI.hpp>
#include <stdio.h>
#include <string.h>

// Built-in Cardputer ST7789, configured as M5GFX's Cardputer autodetect does.
// The backlight (GPIO 38) stays with Bruce's setBrightness().
static lgfx::Bus_SPI s_bus;
static lgfx::Panel_ST7789 s_panel;
static lgfx::LGFX_Device s_lcd;
static bool s_ready = false;

bool infoPanelBegin() {
    auto bus_cfg = s_bus.config();
    bus_cfg.spi_host = SPI3_HOST;
    bus_cfg.spi_mode = 0;
    bus_cfg.freq_write = 40000000;
    bus_cfg.freq_read = 16000000;
    bus_cfg.spi_3wire = true;
    bus_cfg.use_lock = true;
    bus_cfg.dma_channel = SPI_DMA_CH_AUTO;
    bus_cfg.pin_sclk = INFO_SCREEN_SCLK;
    bus_cfg.pin_mosi = INFO_SCREEN_MOSI;
    bus_cfg.pin_miso = -1;
    bus_cfg.pin_dc = INFO_SCREEN_DC;
    s_bus.config(bus_cfg);
    s_panel.setBus(&s_bus);

    auto cfg = s_panel.config();
    cfg.pin_cs = INFO_SCREEN_CS;
    cfg.pin_rst = INFO_SCREEN_RST;
    cfg.panel_width = 135;
    cfg.panel_height = 240;
    cfg.offset_x = 52;
    cfg.offset_y = 40;
    cfg.offset_rotation = 0;
    cfg.readable = false;
    cfg.invert = true;
    cfg.rgb_order = false;
    cfg.bus_shared = false;
    s_panel.config(cfg);
    s_lcd.setPanel(&s_panel);

    s_ready = s_lcd.init();
    if (!s_ready) return false;
    s_lcd.setRotation(1);
    s_lcd.setColorDepth(16);
    s_lcd.setTextWrap(false, false);
    s_lcd.fillScreen(0);
    return true;
}

static void drawFlag(int x, int y, const char *label, bool on, const InfoScreenState &s) {
    uint16_t color = on ? s.fgColor : s.dimColor;
    s_lcd.drawRoundRect(x, y, 54, 20, 4, color);
    s_lcd.setTextColor(color, s.bgColor);
    s_lcd.drawCenterString(label, x + 27, y + 6);
}

static void drawTitleRow(const InfoScreenState &s) {
    const int w = s_lcd.width();
    s_lcd.fillRect(0, 0, w, 32, s.bgColor);
    s_lcd.setTextSize(2);
    s_lcd.setTextColor(s.fgColor, s.bgColor);
    s_lcd.drawString(s.title, 6, 8);
    if (s.battery > 0) {
        const int bx = w - 46, by = 8;
        s_lcd.drawRect(bx, by, 36, 16, s.fgColor);
        s_lcd.fillRect(bx + 36, by + 5, 3, 6, s.fgColor);
        s_lcd.fillRect(bx + 2, by + 2, (32 * s.battery) / 100, 12, s.fgColor);
    }
}

static void drawHeapLine(const InfoScreenState &s) {
    char line[32];
    snprintf(line, sizeof(line), "Free heap  %lu KB", (unsigned long)s.freeHeapKb);
    s_lcd.setTextSize(1);
    s_lcd.fillRect(0, 94, s_lcd.width(), 8, s.bgColor);
    s_lcd.setTextColor(s.fgColor, s.bgColor);
    s_lcd.drawString(line, 8, 94);
}

void infoPanelDraw(const InfoScreenState &s) {
    if (!s_ready) return;
    static InfoScreenState last;
    static bool drawn = false;

    const bool titleChanged = !drawn || strcmp(s.title, last.title) != 0 || s.battery != last.battery;
    // Everything except the title row
    const bool bodyChanged = !drawn || s.fgColor != last.fgColor || s.bgColor != last.bgColor ||
                             s.dimColor != last.dimColor || s.sd != last.sd || s.wifi != last.wifi ||
                             s.ble != last.ble || s.webUI != last.webUI || s.gps != last.gps ||
                             strcmp(s.network, last.network) != 0;
    const bool heapChanged = !drawn || s.freeHeapKb != last.freeHeapKb;
    if (!titleChanged && !bodyChanged && !heapChanged) return;
    last = s;
    drawn = true;

    const int w = s_lcd.width();
    s_lcd.startWrite();
    s_lcd.setFont(&lgfx::fonts::Font0);
    if (bodyChanged) s_lcd.fillScreen(s.bgColor);
    if (bodyChanged || titleChanged) drawTitleRow(s);
    if (bodyChanged || heapChanged) drawHeapLine(s);
    if (!bodyChanged) {
        s_lcd.endWrite();
        return;
    }
    s_lcd.drawFastHLine(4, 32, w - 8, s.fgColor);

    // Status flags
    s_lcd.setTextSize(1);
    const int fy = 42, gap = 5, fw = 54;
    const int fx = (w - (4 * fw + 3 * gap)) / 2;
    drawFlag(fx + 0 * (fw + gap), fy, "SD", s.sd, s);
    drawFlag(fx + 1 * (fw + gap), fy, "WiFi", s.wifi, s);
    drawFlag(fx + 2 * (fw + gap), fy, "BLE", s.ble, s);
    drawFlag(fx + 3 * (fw + gap), fy, "GPS", s.gps, s);

    // Network and memory
    s_lcd.setTextSize(1);
    s_lcd.setTextColor(s.fgColor, s.bgColor);
    char line[48];
    if (s.network[0]) {
        snprintf(line, sizeof(line), "IP  %s%s", s.network, s.webUI ? "  WebUI" : "");
        s_lcd.drawString(line, 8, 76);
    } else if (s.webUI) {
        s_lcd.drawString("WebUI on", 8, 76);
    }

    s_lcd.setTextColor(s.dimColor, s.bgColor);
    s_lcd.drawString("Main UI on external screen", 8, 118);
    s_lcd.endWrite();
}

void infoPanelSleep(bool on) {
    if (!s_ready) return;
    if (on) s_lcd.sleep();
    else s_lcd.wakeup();
}

void infoPanelRelease() {
    if (!s_ready) return;
    s_lcd.waitDisplay();
    s_lcd.releaseBus();
    s_ready = false;
}
#endif
