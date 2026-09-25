#ifndef __INFO_SCREEN_PANEL_H__
#define __INFO_SCREEN_PANEL_H__

// Drawing side of the info screen. Kept free of graphics-library types so it can be included
// next to TFT_eSPI; the implementation is the only file that includes M5GFX.

#include <stdint.h>

struct InfoScreenState {
    char title[24];   // clock, or "BRUCE <version>" until the clock is set
    char network[24]; // IP address, or empty
    int battery;      // 0 hides the battery
    uint32_t freeHeapKb;
    bool sd;
    bool wifi;
    bool ble;
    bool webUI;
    bool gps;
    uint16_t fgColor;
    uint16_t bgColor;
    uint16_t dimColor;
};

bool infoPanelBegin();
void infoPanelDraw(const InfoScreenState &state);
void infoPanelSleep(bool on);
void infoPanelRelease();

#endif
