// ==========================================================================
//  QBIT -- RTOS primitive instantiation & global objects
// ==========================================================================
#include "app_state.h"

// RTOS handles (created in main.cpp setup())
QueueHandle_t       gestureQueue      = nullptr;
QueueHandle_t       networkEventQueue = nullptr;
EventGroupHandle_t  connectivityBits  = nullptr;
SemaphoreHandle_t   displayMutex      = nullptr;
SemaphoreHandle_t   gifPlayerMutex    = nullptr;

// Global display object (reconstructed via placement new in setup())
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
    U8G2_R0, /* reset= */ U8X8_PIN_NONE,
    /* clock= */ 21, /* data= */ 20);

const char *kQbitVersion = (QBIT_VERSION[0] != '\0')
                           ? QBIT_VERSION
                           : "dev-build";

volatile bool updateAvailable = false;
char         updateAvailableVersion[UPDATE_AVAILABLE_VERSION_LEN] = "";

volatile bool otaInProgress      = false;
volatile int  otaProgressPercent = 0;
char          otaStatusMsg[32]   = "Idle";
volatile bool otaCloudRequested  = false;
volatile bool otaCancelRequested = false;
volatile bool otaRebootRequested = false;

MusicState musicState = {
    false,
    "Not Playing",
    "QBIT Music",
    0,
    180,
    0,
    0
};

void updateMusicState(bool playing, const char *title, const char *artist,
                      uint16_t progressSec, uint16_t durationSec, uint8_t style) {
    musicState.playing = playing;
    if (title && title[0] != '\0') {
        strncpy(musicState.title, title, sizeof(musicState.title) - 1);
        musicState.title[sizeof(musicState.title) - 1] = '\0';
    }
    if (artist && artist[0] != '\0') {
        strncpy(musicState.artist, artist, sizeof(musicState.artist) - 1);
        musicState.artist[sizeof(musicState.artist) - 1] = '\0';
    }
    musicState.progressSec  = progressSec;
    musicState.durationSec  = (durationSec > 0) ? durationSec : 180;
    musicState.style        = style;
    musicState.lastUpdateMs = millis();

    if (networkEventQueue) {
        NetworkEvent evt = {};
        evt.kind = NetworkEvent::MUSIC_UPDATE;
        evt.connected = playing;
        xQueueSend(networkEventQueue, &evt, pdMS_TO_TICKS(50));
    }
}
