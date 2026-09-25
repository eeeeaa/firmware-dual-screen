#ifndef __INFO_SCREEN_H__
#define __INFO_SCREEN_H__

// Secondary status display, used when an external screen runs the main UI (HAS_INFO_SCREEN).
// All functions are no-ops on other boards.

#ifdef HAS_INFO_SCREEN
// Starts the panel and its low-priority refresh task. Call after begin_tft().
void infoScreenBegin();
// Stops drawing for good and frees its SPI host for another user.
void infoScreenReleaseBus();
// Mirrors powerSave: puts the panel to sleep and pauses refreshes.
void infoScreenSleep(bool on);
#else
inline void infoScreenBegin() {}
inline void infoScreenReleaseBus() {}
inline void infoScreenSleep(bool) {}
#endif

#endif
