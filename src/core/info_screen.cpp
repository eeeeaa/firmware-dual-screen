#include "info_screen.h"

#ifdef HAS_INFO_SCREEN
#include "info_screen_panel.h"
#include "utils.h"
#include <WiFi.h>
#include <globals.h>

static constexpr uint32_t REFRESH_MS = 1000;
static constexpr uint32_t BATTERY_REFRESH_MS = 10000;

static SemaphoreHandle_t s_lock = nullptr;
static bool s_released = false;
static bool s_sleeping = false;

static void fillState(InfoScreenState &st) {
    static int battery = 0;
    static uint32_t lastBatteryMs = 0;
    uint32_t now = millis();
    if (lastBatteryMs == 0 || now - lastBatteryMs >= BATTERY_REFRESH_MS) {
        battery = getBattery();
        lastBatteryMs = now;
    }

    memset(&st, 0, sizeof(st));
    if (clock_set) {
        struct tm t = rtc.getTimeStruct();
        if (bruceConfig.clock24hr) {
            snprintf(st.title, sizeof(st.title), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        } else {
            int h = t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12;
            snprintf(
                st.title,
                sizeof(st.title),
                "%02d:%02d:%02d %s",
                h,
                t.tm_min,
                t.tm_sec,
                t.tm_hour < 12 ? "AM" : "PM"
            );
        }
    } else {
        snprintf(st.title, sizeof(st.title), "BRUCE %s", BRUCE_VERSION);
    }

    // Read from the network stack rather than the wifiIP String, which other tasks rewrite
    wifi_mode_t mode = WiFi.getMode();
    IPAddress ip;
    if (WiFi.status() == WL_CONNECTED) ip = WiFi.localIP();
    else if (mode & WIFI_MODE_AP) ip = WiFi.softAPIP();
    if (ip != IPAddress()) snprintf(st.network, sizeof(st.network), "%s", ip.toString().c_str());

    st.battery = battery;
    st.freeHeapKb = ESP.getFreeHeap() / 1024;
    st.sd = sdcardMounted;
    st.wifi = mode != WIFI_MODE_NULL;
    st.ble = BLEConnected;
    st.webUI = isWebUIActive;
    st.gps = gpsConnected;
    st.fgColor = bruceConfig.priColor;
    st.bgColor = bruceConfig.bgColor;
    st.dimColor = bruceConfig.secColor;
}

static void infoScreenTask(void *) {
    InfoScreenState st;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_released) {
            xSemaphoreGive(s_lock);
            vTaskDelete(nullptr);
        }
        if (!s_sleeping) {
            fillState(st);
            infoPanelDraw(st);
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
    }
}

void infoScreenBegin() {
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    if (!infoPanelBegin()) {
        Serial.println("Info screen: panel init failed");
        s_released = true;
        return;
    }
    xTaskCreate(infoScreenTask, "InfoScreen", 4096, nullptr, 1, nullptr);
}

void infoScreenReleaseBus() {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_released) {
        infoPanelRelease();
        s_released = true;
        Serial.println("Info screen: SPI host handed over, info screen stopped");
    }
    xSemaphoreGive(s_lock);
}

void infoScreenSleep(bool on) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_released && s_sleeping != on) infoPanelSleep(on);
    s_sleeping = on;
    xSemaphoreGive(s_lock);
}
#endif
