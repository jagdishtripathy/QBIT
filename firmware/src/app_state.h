// ==========================================================================
//  QBIT -- Shared application state & RTOS primitives
// ==========================================================================
#ifndef APP_STATE_H
#define APP_STATE_H

#include <Arduino.h>
#include <U8g2lib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

// ==========================================================================
//  Display states (state machine for display task)
// ==========================================================================
enum DisplayState {
    BOOT_ANIM,
    WIFI_SETUP,
    CONNECTED_INFO,
    GIF_PLAYBACK,
    POKE_DISPLAY,
    CLAIM_PROMPT,
    FRIEND_PROMPT,
    HISTORY_TIME,
    HISTORY_POKE,
    MUTE_FEEDBACK,
    OFFLINE_STATUS,
    SETTINGS_MENU,
    SETTINGS_OPTIONS,
    GAME_MENU,
    GAME_CONTRIBUTE,
    WEATHER_SCREEN,
    TIMER_SET,
    TIMER_RUNNING,
    TREX_RUNNING,
    TREX_OVER,
    FLAPPY_RUNNING,
    FLAPPY_OVER,
    CAR_RUNNING,
    CAR_OVER,
    CAM_VIEW,
    PINNED_DISPLAY,
    MUSIC_VIEW
};

// ==========================================================================
//  Gesture types (input task → display task)
// ==========================================================================
enum GestureType {
    GESTURE_NONE,
    TOUCH_DOWN,
    TOUCH_UP,
    SINGLE_TAP,
    DOUBLE_TAP,
    LONG_PRESS
};

struct GestureEvent {
    GestureType type;
    unsigned long timestamp;
};

// ==========================================================================
//  Network events (network task → display task)
// ==========================================================================
    struct NetworkEvent {
    enum Kind {
        POKE,
        POKE_BITMAP,
        CLAIM_REQUEST,
        FRIEND_REQUEST,
        WIFI_STATUS,
        WS_STATUS,
        MQTT_COMMAND,
        CAM_START,
        CAM_STOP,
        PINNED_UPDATE,
        MUSIC_UPDATE
    } kind;

    // Event-specific data
    char sender[33];
    char text[65];
    char title[21];

    // Bitmap poke data (heap pointers, receiver must free)
    uint8_t *senderBmp;
    uint16_t senderBmpWidth;
    size_t   senderBmpLen;
    uint8_t *textBmp;
    uint16_t textBmpWidth;
    size_t   textBmpLen;

    // Status flags
    bool connected;
};

// ==========================================================================
//  Poke history record
// ==========================================================================
struct PokeRecord {
    String sender;
    String text;
    time_t timestamp;

    // Optional bitmap data (copies, owned by this record)
    uint8_t *senderBmp   = nullptr;
    uint16_t senderBmpW  = 0;
    uint16_t senderBmpH  = 0;
    uint8_t *textBmp     = nullptr;
    uint16_t textBmpW    = 0;
    uint16_t textBmpH    = 0;
    bool     hasBitmaps  = false;

    void freeBitmaps() {
        if (senderBmp) { free(senderBmp); senderBmp = nullptr; }
        if (textBmp)   { free(textBmp);   textBmp   = nullptr; }
        hasBitmaps = false;
        senderBmpW = senderBmpH = 0;
        textBmpW   = textBmpH   = 0;
    }
};

// ==========================================================================
//  Connectivity event group bits
// ==========================================================================
#define WIFI_CONNECTED_BIT  (1 << 0)
#define WS_CONNECTED_BIT    (1 << 1)
#define MQTT_CONNECTED_BIT  (1 << 2)
#define PORTAL_ACTIVE_BIT   (1 << 3)

// ==========================================================================
//  RTOS handles (instantiated in app_state.cpp)
// ==========================================================================
extern QueueHandle_t       gestureQueue;
extern QueueHandle_t       networkEventQueue;
extern EventGroupHandle_t  connectivityBits;
extern SemaphoreHandle_t   displayMutex;
extern SemaphoreHandle_t   gifPlayerMutex;

// ==========================================================================
//  Global display object
// ==========================================================================
extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

// ==========================================================================
//  Firmware version
// ==========================================================================
#ifndef QBIT_VERSION
#define QBIT_VERSION "dev-build"
#endif

extern const char *kQbitVersion;

// ==========================================================================
//  Update check (set by network task when latest.json version differs)
// ==========================================================================
#define UPDATE_AVAILABLE_VERSION_LEN 16
extern volatile bool    updateAvailable;
extern char             updateAvailableVersion[UPDATE_AVAILABLE_VERSION_LEN];

// ==========================================================================
//  Wireless OTA state (Cloud Auto-OTA & Browser Local OTA)
// ==========================================================================
extern volatile bool    otaInProgress;
extern volatile int     otaProgressPercent;
extern char             otaStatusMsg[32];
extern volatile bool    otaCloudRequested;
extern volatile bool    otaCancelRequested;
extern volatile bool    otaRebootRequested;

// ==========================================================================
//  Live Music state (updated via MQTT or REST API /api/music)
// ==========================================================================
struct MusicState {
    bool          playing;
    char          title[65];
    char          artist[65];
    uint16_t      progressSec;
    uint16_t      durationSec;
    uint8_t       style;         // 0 = Randomize (Mode 1 & 3), 1 = Mode 1 (Split), 3 = Mode 3 (Auto-Flip)
    unsigned long lastUpdateMs;
};
extern MusicState musicState;
void updateMusicState(bool playing, const char *title, const char *artist,
                      uint16_t progressSec, uint16_t durationSec, uint8_t style);

#endif // APP_STATE_H
