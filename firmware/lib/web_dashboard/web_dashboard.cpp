#include "web_dashboard.h"
#include "gif_player.h"
#include "../../src/settings.h"
#include "../../src/app_state.h"
#include "../../src/network_task.h"
#include "../../src/time_manager.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#if defined(ESP32) || defined(ESP8266)
#include <WiFi.h>
#endif
#if defined(ESP32)
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

// ==========================================================================
//  Upload state
// ==========================================================================

static File   _uploadFile;
static bool   _uploadOk    = false;
static String _uploadError;

// ==========================================================================
//  Path sanitization (prevent path traversal)
// ==========================================================================

#define MAX_BASENAME_LEN 64

// Returns a safe basename for a file under "/", or empty string if invalid.
// Rejects "..", "/", "\\", NUL, and limits length.
static String sanitizeFileBasename(const String &input) {
    if (input.length() == 0 || input.length() > MAX_BASENAME_LEN)
        return "";
    for (size_t i = 0; i < input.length(); i++) {
        char c = input[i];
        if (c == '\0' || c == '/' || c == '\\')
            return "";
    }
    if (input.indexOf("..") >= 0)
        return "";
    return input;
}

// Normalize request path to a single segment under root for .qgif serving.
// Returns path like "/foo.qgif" or empty if invalid.
static String normalizeQgifPath(const String &url) {
    String path = url;
    path.trim();
    if (path.length() == 0) return "";
    if (path.startsWith("/")) path = path.substring(1);
    if (path.length() == 0 || path.indexOf("..") >= 0 || path.indexOf('/') >= 0)
        return "";
    if (!path.endsWith(".qgif")) return "";
    if (path.length() > MAX_BASENAME_LEN) return "";
    return "/" + path;
}

// ==========================================================================
//  Helpers
// ==========================================================================

// Returns request parameter from URL query first, then POST body.
static bool getParamValue(AsyncWebServerRequest *request, const char *name, String &out) {
    const AsyncWebParameter *p = request->getParam(name, false);
    if (!p) p = request->getParam(name, true);
    if (!p) return false;
    out = p->value();
    return true;
}

// Serve a file from LittleFS with the given content type.
static void serveFile(AsyncWebServerRequest *request,
                      const char *path, const char *contentType) {
    if (LittleFS.exists(path)) {
        request->send(LittleFS, path, contentType);
    } else {
        request->send(404, "text/plain", "File not found");
    }
}

// ==========================================================================
//  Handlers -- static assets
// ==========================================================================

static void handleRoot(AsyncWebServerRequest *request) {
    serveFile(request, "/index.html", "text/html");
}

static void handleCSS(AsyncWebServerRequest *request) {
    serveFile(request, "/style.css", "text/css");
}

static void handleScript(AsyncWebServerRequest *request) {
    serveFile(request, "/script.js", "application/javascript");
}

static void handleJszip(AsyncWebServerRequest *request) {
    serveFile(request, "/jszip.min.js", "application/javascript");
}

static void handleFont(AsyncWebServerRequest *request) {
    serveFile(request, "/inter-latin.woff2", "font/woff2");
}

static void handleIcon(AsyncWebServerRequest *request) {
    serveFile(request, "/icon.svg", "image/svg+xml");
}

static void handleFavicon(AsyncWebServerRequest *request) {
    // Browsers auto-request /favicon.ico; redirect to SVG icon
    request->redirect("/icon.svg");
}

// ==========================================================================
//  Handlers -- REST API
// ==========================================================================

static void handleList(AsyncWebServerRequest *request) {
    StaticJsonDocument<2048> doc;
    JsonArray arr = doc.to<JsonArray>();
    File root = LittleFS.open("/");
    if (root && root.isDirectory()) {
        String current = gifPlayerGetCurrentFile();
        File f = root.openNextFile();
        while (f) {
            String name = String(f.name());
            size_t sz   = f.size();
            f.close();
            if (name.startsWith("/")) name = name.substring(1);
            if (name.endsWith(".qgif")) {
                JsonObject obj = arr.add<JsonObject>();
                obj["name"]    = name;
                obj["size"]    = sz;
                obj["playing"] = (name == current);
            }
            f = root.openNextFile();
        }
        root.close();
    }
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

static void handleStorage(AsyncWebServerRequest *request) {
    StaticJsonDocument<128> doc;
    doc["total"] = LittleFS.totalBytes();
    doc["used"]  = LittleFS.usedBytes();
    doc["free"]  = LittleFS.totalBytes() - LittleFS.usedBytes();
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

static void handleUploadDone(AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    if (_uploadOk) {
        doc["ok"] = true;
    } else {
        doc["error"] = _uploadError;
    }
    String json;
    serializeJson(doc, json);
    request->send(_uploadOk ? 200 : 507, "application/json", json);
}

// Called for each chunk of the multipart file upload.
//   filename -- original file name from the client
//   index    -- byte offset of this chunk within the upload stream
//   data/len -- current chunk payload
//   final    -- true when this is the last chunk
static void handleUploadData(AsyncWebServerRequest *request,
                             const String &filename, size_t index,
                             uint8_t *data, size_t len, bool final) {
    // --- Start of upload (first chunk, index == 0) ---
    if (index == 0) {
        _uploadOk    = true;
        _uploadError = "";

        // Validate extension
        if (!filename.endsWith(".qgif")) {
            _uploadOk    = false;
            _uploadError = "Only .qgif files are accepted";
            return;
        }

        // Path traversal: use basename only and sanitize
        int lastSlash = filename.lastIndexOf('/');
        String basename = lastSlash >= 0 ? filename.substring(lastSlash + 1) : filename;
        basename = sanitizeFileBasename(basename);
        if (basename.length() == 0 || !basename.endsWith(".qgif")) {
            _uploadOk    = false;
            _uploadError = "Invalid filename";
            return;
        }

        // Rough free-space check (exact size unknown at this point)
        size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
        if (freeBytes < 2048) {
            _uploadOk    = false;
            _uploadError = "Insufficient storage -- delete some files first";
            return;
        }

        _uploadFile = LittleFS.open("/" + basename, "w");
        if (!_uploadFile) {
            _uploadOk    = false;
            _uploadError = "Failed to create file";
        }
    }

    // --- Write data ---
    if (_uploadOk && _uploadFile && len > 0) {
        if (_uploadFile.write(data, len) != len) {
            _uploadOk    = false;
            _uploadError = "Write failed -- storage may be full";
        }
    }

    // --- End of upload (last chunk) ---
    if (final) {
        if (_uploadFile) _uploadFile.close();

        int lastSlash = filename.lastIndexOf('/');
        String basename = lastSlash >= 0 ? filename.substring(lastSlash + 1) : filename;
        basename = sanitizeFileBasename(basename);
        if (basename.length() == 0) {
            _uploadOk = false;
            _uploadError = "Invalid filename";
            return;
        }
        String path = "/" + basename;

        if (!_uploadOk) {
            LittleFS.remove(path);
            return;
        }

        // Validate .qgif header
        File vf = LittleFS.open(path, "r");
        if (!vf) {
            _uploadOk = false;
            _uploadError = "Cannot reopen file";
        } else {
            uint8_t hdr[QGIF_HEADER_SIZE];
            if (vf.read(hdr, QGIF_HEADER_SIZE) != QGIF_HEADER_SIZE) {
                _uploadOk = false;
                _uploadError = "File too small";
            } else {
                uint8_t  fc = hdr[0];
                uint16_t w  = hdr[1] | ((uint16_t)hdr[2] << 8);
                uint16_t h  = (hdr[3] | ((uint16_t)hdr[4] << 8)) & ~QGIF_RLE_FLAG;
                if (fc == 0 || w != QGIF_FRAME_WIDTH || h != QGIF_FRAME_HEIGHT) {
                    _uploadOk    = false;
                    _uploadError = "Invalid .qgif format (bad header)";
                }
            }
            vf.close();
        }

        if (!_uploadOk) {
            LittleFS.remove(path);
            return;
        }

        gifPlayerBuildShuffleBag();

        if (gifPlayerGetCurrentFile().length() == 0)
            gifPlayerSetFile(basename);
    }
}

// Serve a single .qgif file by name (for backup download; ensures correct binary response)
static void handleGetFile(AsyncWebServerRequest *request) {
    if (!request->hasParam("name")) {
        request->send(400, "text/plain", "Missing name");
        return;
    }
    String name = sanitizeFileBasename(request->getParam("name")->value());
    if (name.length() == 0 || !name.endsWith(".qgif")) {
        request->send(400, "text/plain", "Invalid name");
        return;
    }
    String path = "/" + name;
    if (!LittleFS.exists(path)) {
        request->send(404, "text/plain", "Not found");
        return;
    }
    request->send(LittleFS, path, "application/octet-stream");
}

static void handleDelete(AsyncWebServerRequest *request) {
    if (!request->hasParam("name")) {
        request->send(400, "application/json", "{\"error\":\"Missing name\"}");
        return;
    }
    String name = sanitizeFileBasename(request->getParam("name")->value());
    if (name.length() == 0) {
        request->send(400, "application/json", "{\"error\":\"Invalid name\"}");
        return;
    }

    String path = "/" + name;
    if (!LittleFS.exists(path)) {
        request->send(404, "application/json", "{\"error\":\"File not found\"}");
        return;
    }

    // If the file being deleted is currently playing, stop/close it first so LittleFS can remove it
    bool wasPlaying = (gifPlayerGetCurrentFile() == name);
    if (wasPlaying) {
        gifPlayerStop();
        vTaskDelay(pdMS_TO_TICKS(30)); // Allow display task tick to release file handle
    }

    // If deleted file was pinned, reset pinned mode
    if (getPinnedMode() == 1 && getPinnedTarget() == name) {
        setPinnedMode(0);
        setPinnedTarget("");
        saveSettings();
        if (networkEventQueue) {
            NetworkEvent evt;
            memset(&evt, 0, sizeof(evt));
            evt.kind = NetworkEvent::PINNED_UPDATE;
            xQueueSend(networkEventQueue, &evt, 0);
        }
    }

    if (!LittleFS.remove(path)) {
        Serial.printf("web: failed to remove %s\n", path.c_str());
        request->send(500, "application/json", "{\"error\":\"Failed to delete file\"}");
        return;
    }

    // Rebuild shuffle bag immediately without the deleted file
    gifPlayerBuildShuffleBag();

    if (wasPlaying) {
        if (gifPlayerHasFiles()) {
            String next = gifPlayerNextShuffle();
            if (next.length() > 0) {
                gifPlayerSetFile(next);
            }
        } else {
            gifPlayerStartIdleIfNoUserGifs();
        }
    }

    request->send(200, "application/json", "{\"ok\":true}");
}

// ==========================================================================
//  Handlers -- Settings API
// ==========================================================================

static void handleGetSettings(AsyncWebServerRequest *request) {
    StaticJsonDocument<128> doc;
    doc["speed"]      = getPlaybackSpeed();
    doc["brightness"] = getDisplayBrightness();
    doc["volume"]     = getBuzzerVolume();
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

static void handlePostSettings(AsyncWebServerRequest *request) {
    if (request->hasParam("speed")) {
        int v = request->getParam("speed")->value().toInt();
        if (v >= 1 && v <= 10) setPlaybackSpeed((uint16_t)v);
    }
    if (request->hasParam("brightness")) {
        int v = request->getParam("brightness")->value().toInt();
        if (v >= 0 && v <= 255) setDisplayBrightness((uint8_t)v);
    }
    if (request->hasParam("volume")) {
        int v = request->getParam("volume")->value().toInt();
        if (v >= 0 && v <= 100) setBuzzerVolume((uint8_t)v);
    }
    // If save=1 is passed, persist to NVS
    if (request->hasParam("save")) {
        saveSettings();
    }

    // Echo back the current state
    handleGetSettings(request);
}

// ==========================================================================
//  Handlers -- Play API
// ==========================================================================

static void handlePlay(AsyncWebServerRequest *request) {
    if (!request->hasParam("name")) {
        request->send(400, "application/json", "{\"error\":\"Missing name\"}");
        return;
    }
    String name = sanitizeFileBasename(request->getParam("name")->value());
    if (name.length() == 0) {
        request->send(400, "application/json", "{\"error\":\"Invalid name\"}");
        return;
    }

    String path = "/" + name;
    if (!LittleFS.exists(path)) {
        request->send(404, "application/json", "{\"error\":\"File not found\"}");
        return;
    }

    // If device was in Pinned Mode, clear pinned mode so it can cycle or play normally
    if (getPinnedMode() != 0) {
        setPinnedMode(0);
        saveSettings();
        if (networkEventQueue) {
            NetworkEvent evt;
            memset(&evt, 0, sizeof(evt));
            evt.kind = NetworkEvent::PINNED_UPDATE;
            xQueueSend(networkEventQueue, &evt, 0);
        }
    }
    gifPlayerSetAutoAdvance(1);
    gifPlayerSetFile(name);
    request->send(200, "application/json", "{\"ok\":true}");
}

// ==========================================================================
//  Handlers -- WiFi reset and Reboot (extern from network_task / ESP)
// ==========================================================================

extern void networkWifiReset();

static void handleWifiReset(AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true}");
    networkWifiReset();
}

static void handleReboot(AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
#if defined(ESP32)
    esp_restart();
#elif defined(ESP8266)
    ESP.restart();
#else
    (void)0;
#endif
}

// ==========================================================================
//  Handlers -- Wireless OTA (Cloud Auto-OTA & Browser Local .bin Upload)
// ==========================================================================

static bool   _otaUploadOk = false;
static String _otaUploadError;

static void handleGetOtaStatus(AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["in_progress"]      = (bool)otaInProgress;
    doc["progress"]         = (int)otaProgressPercent;
    doc["status"]           = String((const char *)otaStatusMsg);
    doc["update_available"] = (bool)updateAvailable;
    doc["latest_version"]   = String(updateAvailableVersion);
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

static void handlePostOtaCloud(AsyncWebServerRequest *request) {
    if (otaInProgress) {
        request->send(409, "application/json", "{\"ok\":false,\"error\":\"OTA already in progress\"}");
        return;
    }
    otaCancelRequested = false;
    otaInProgress      = true;
    otaProgressPercent = 0;
    strncpy(otaStatusMsg, "Checking version...", sizeof(otaStatusMsg) - 1);
    otaCloudRequested  = true;
    request->send(200, "application/json", "{\"ok\":true}");
}

static void handlePostOtaCancel(AsyncWebServerRequest *request) {
    if (otaInProgress) {
        otaCancelRequested = true;
        strncpy(otaStatusMsg, "Cancelling...", sizeof(otaStatusMsg) - 1);
    }
    request->send(200, "application/json", "{\"ok\":true}");
}

static void handleOtaUploadDone(AsyncWebServerRequest *request) {
    if (_otaUploadOk) {
        request->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
        otaRebootRequested = true;
    } else {
        StaticJsonDocument<256> doc;
        doc["ok"]    = false;
        doc["error"] = _otaUploadError.length() ? _otaUploadError : "Firmware update failed";
        String json;
        serializeJson(doc, json);
        request->send(400, "application/json", json);
    }
}

static void handleOtaUploadData(AsyncWebServerRequest *request,
                                const String &filename, size_t index,
                                uint8_t *data, size_t len, bool final) {
    if (index == 0) {
        _otaUploadOk    = true;
        _otaUploadError = "";
        if (!filename.endsWith(".bin")) {
            _otaUploadOk    = false;
            _otaUploadError = "Only .bin firmware files are accepted";
            return;
        }
        otaInProgress      = true;
        otaProgressPercent = 0;
        String lowerName = filename;
        lowerName.toLowerCase();
        bool isFs = (lowerName.indexOf("littlefs") >= 0 || lowerName.indexOf("spiffs") >= 0);
        if (isFs) {
            strncpy(otaStatusMsg, "Uploading LittleFS...", sizeof(otaStatusMsg) - 1);
            gifPlayerStop();
            vTaskDelay(pdMS_TO_TICKS(30));
            LittleFS.end();
        } else {
            strncpy(otaStatusMsg, "Uploading firmware...", sizeof(otaStatusMsg) - 1);
        }
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, isFs ? U_SPIFFS : U_FLASH)) {
            _otaUploadOk    = false;
            _otaUploadError = "Update.begin failed";
            strncpy(otaStatusMsg, "OTA init failed", sizeof(otaStatusMsg) - 1);
            otaInProgress = false;
            return;
        }
    }

    if (_otaUploadOk && otaCancelRequested) {
        _otaUploadOk    = false;
        _otaUploadError = "Update cancelled";
        Update.abort();
        otaCancelRequested = false;
        strncpy(otaStatusMsg, "Update cancelled", sizeof(otaStatusMsg) - 1);
        otaInProgress = false;
        return;
    }

    if (_otaUploadOk && len > 0) {
        if (Update.write(data, len) != len) {
            _otaUploadOk    = false;
            _otaUploadError = "Flash write failed";
            Update.abort();
            strncpy(otaStatusMsg, "Write failed!", sizeof(otaStatusMsg) - 1);
            otaInProgress = false;
            return;
        }
        size_t total = request->contentLength();
        if (total > 0) {
            int pct = (int)(((index + len) * 100ULL) / total);
            if (pct > 99) pct = 99;
            otaProgressPercent = pct;
        }
    }

    if (final) {
        if (_otaUploadOk) {
            if (Update.end(true) && Update.isFinished()) {
                otaProgressPercent = 100;
                strncpy(otaStatusMsg, "Success! Rebooting", sizeof(otaStatusMsg) - 1);
            } else {
                _otaUploadOk    = false;
                _otaUploadError = "Firmware verification failed";
                Update.abort();
                strncpy(otaStatusMsg, "Verify failed!", sizeof(otaStatusMsg) - 1);
                otaInProgress = false;
            }
        } else {
            Update.abort();
            otaInProgress = false;
        }
    }
}

// ==========================================================================
//  Handlers -- Device identity API
// ==========================================================================

static void handleGetDevice(AsyncWebServerRequest *request) {
    StaticJsonDocument<512> doc;
    doc["id"]               = getDeviceId();
    doc["name"]             = getDeviceName();
    doc["uptime_s"]         = (uint32_t)networkGetBootUptimeSeconds();
    doc["server_connected"] = networkIsCloudWsConnected();
    doc["server_uptime_s"]  = (uint32_t)networkGetCloudWsUptimeSeconds();
    EventBits_t bits        = xEventGroupGetBits(connectivityBits);
    doc["mqtt_connected"]   = (bits & MQTT_CONNECTED_BIT) != 0;
    doc["mqtt_enabled"]     = getMqttEnabled();
    doc["firmware"]         = kQbitVersion;
    bool ua = updateAvailable;
    doc["update_available"] = ua;
    doc["latest_version"]   =
        (ua && updateAvailableVersion[0] != '\0') ? String(updateAvailableVersion) : String("");
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

static void handlePostDevice(AsyncWebServerRequest *request) {
    if (request->hasParam("name")) {
        String name = request->getParam("name")->value();
        if (name.length() > 0 && name.length() <= 32) {
            setDeviceName(name);
        }
    }
    if (request->hasParam("save")) {
        saveSettings();
    }
    handleGetDevice(request);
}

// ==========================================================================
//  Handlers -- Local MQTT settings API
// ==========================================================================

static void handleGetMqtt(AsyncWebServerRequest *request) {
    StaticJsonDocument<512> doc;
    doc["enabled"] = getMqttEnabled();
    doc["host"]    = getMqttHost();
    doc["port"]    = getMqttPort();
    doc["user"]    = getMqttUser();
    doc["pass"]    = getMqttPass();
    doc["prefix"]  = getMqttPrefix();
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

static void handlePostMqtt(AsyncWebServerRequest *request) {
    String  host    = request->hasParam("host")    ?  request->getParam("host")->value()            : getMqttHost();
    int     portVal = request->hasParam("port")    ?  request->getParam("port")->value().toInt()    : (int)getMqttPort();
    String  user    = request->hasParam("user")    ?  request->getParam("user")->value()            : getMqttUser();
    String  pass    = request->hasParam("pass")    ?  request->getParam("pass")->value()            : getMqttPass();
    String  prefix  = request->hasParam("prefix")  ?  request->getParam("prefix")->value()          : getMqttPrefix();
    bool    enabled = request->hasParam("enabled") ? (request->getParam("enabled")->value() == "1") : getMqttEnabled();

    host.trim();
    prefix.trim();
    if (prefix.length() == 0) prefix = "qbit";
    if (portVal < 1 || portVal > 65535) portVal = 1883;
    uint16_t port = (uint16_t)portVal;

    setMqttConfig(host, port, user, pass, prefix, enabled);

    if (request->hasParam("save")) {
        saveSettings();
    }

    handleGetMqtt(request);
}

// ==========================================================================
//  Handlers -- GPIO Pin Configuration API
// ==========================================================================

// Valid GPIOs for ESP32-C3 Super Mini
static const uint8_t VALID_PINS[] = {0,1,2,3,4,5,6,7,8,9,10,20,21};
static const uint8_t VALID_PINS_COUNT = sizeof(VALID_PINS) / sizeof(VALID_PINS[0]);

static bool isValidPin(uint8_t pin) {
    for (uint8_t i = 0; i < VALID_PINS_COUNT; i++) {
        if (VALID_PINS[i] == pin) return true;
    }
    return false;
}

static void handleGetPins(AsyncWebServerRequest *request) {
    StaticJsonDocument<128> doc;
    doc["touch"]  = getPinTouch();
    doc["buzzer"] = getPinBuzzer();
    doc["sda"]    = getPinSDA();
    doc["scl"]    = getPinSCL();
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

static void handlePostPins(AsyncWebServerRequest *request) {
    if (!request->hasParam("touch") || !request->hasParam("buzzer") ||
        !request->hasParam("sda")   || !request->hasParam("scl")) {
        request->send(400, "application/json",
                      "{\"error\":\"Missing pin parameters (touch, buzzer, sda, scl)\"}");
        return;
    }

    uint8_t touch  = (uint8_t)request->getParam("touch")->value().toInt();
    uint8_t buzzer = (uint8_t)request->getParam("buzzer")->value().toInt();
    uint8_t sda    = (uint8_t)request->getParam("sda")->value().toInt();
    uint8_t scl    = (uint8_t)request->getParam("scl")->value().toInt();

    // Validate: all pins must be in the allowed set
    if (!isValidPin(touch) || !isValidPin(buzzer) ||
        !isValidPin(sda)   || !isValidPin(scl)) {
        request->send(400, "application/json",
                      "{\"error\":\"Invalid GPIO pin number\"}");
        return;
    }

    // Validate: all 4 pins must be distinct
    if (touch == buzzer || touch == sda || touch == scl ||
        buzzer == sda   || buzzer == scl || sda == scl) {
        request->send(400, "application/json",
                      "{\"error\":\"All four pins must be different\"}");
        return;
    }

    // Send response before reboot
    request->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");

    // Save and reboot (setPinConfig writes NVS then calls ESP.restart)
    setPinConfig(touch, buzzer, sda, scl);
}

// ==========================================================================
//  Handlers -- Current playing file
// ==========================================================================

static void handleCurrent(AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["name"]   = gifPlayerGetCurrentFile();
    doc["idle"]   = gifPlayerIsIdle();
    doc["pinned"] = (getPinnedMode() != 0);
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

// ==========================================================================
//  Handlers -- Timezone API
// ==========================================================================

static void handleGetTimezone(AsyncWebServerRequest *request) {
    StaticJsonDocument<128> doc;
    doc["timezone"] = getTimezoneIANA();
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

static void handlePostTimezone(AsyncWebServerRequest *request) {
    // Accept both "tz" and "iana" param names for the timezone
    String tz;
    if (request->hasParam("tz")) {
        tz = request->getParam("tz")->value();
    } else if (request->hasParam("iana")) {
        tz = request->getParam("iana")->value();
    }
    if (tz.length() > 0) {
        setTimezoneIANA(tz);
        timeManagerSetTimezone(tz);
    } else {
        // Empty value = auto-detect: clear saved timezone
        setTimezoneIANA("");
    }
    saveSettings();
    handleGetTimezone(request);
}

// ==========================================================================
//  Handlers -- Pinned Display API
// ==========================================================================

static void handleGetPinned(AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["active"]      = (getPinnedMode() != 0);
    doc["mode"]        = getPinnedMode();
    doc["target"]      = getPinnedTarget();
    doc["duration"]    = getPinnedDuration();
    doc["remaining"]   = getPinnedRemainingSeconds();
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

static void handlePostPinned(AsyncWebServerRequest *request) {
    uint8_t mode = 0;
    String target = "";
    uint32_t duration = 0;

    String val;
    if (getParamValue(request, "mode", val)) {
        mode = (uint8_t)val.toInt();
    }
    if (getParamValue(request, "target", val)) {
        target = val;
        target.trim();
    }
    if (getParamValue(request, "duration", val)) {
        duration = (uint32_t)val.toInt();
    }

    if (mode > 2) mode = 0;

    if (mode == 1) {
        if (target.startsWith("/")) target = target.substring(1);
        // GIF target: sanitize basename
        target = sanitizeFileBasename(target);
        if (!target.endsWith(".qgif") || !LittleFS.exists("/" + target)) {
            request->send(400, "application/json", "{\"error\":\"File not found or invalid .qgif\"}");
            return;
        }
    } else if (mode == 2) {
        // Text banner: limit length
        if (target.length() == 0) target = "QBIT";
        if (target.length() > 64) target = target.substring(0, 64);
    }

    setPinnedMode(mode);
    setPinnedTarget(target);
    setPinnedDuration(duration);
    uint32_t nowSec = (uint32_t)(timeManagerNow() > 100000 ? timeManagerNow() : (millis() / 1000));
    setPinnedStartEpoch(nowSec);
    saveSettings();

    // Signal display task via networkEventQueue
    if (networkEventQueue) {
        NetworkEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.kind = NetworkEvent::PINNED_UPDATE;
        xQueueSend(networkEventQueue, &evt, 0);
    }

    handleGetPinned(request);
}

// ==========================================================================
//  Handlers -- Live Music API (/api/music)
// ==========================================================================

static void handleGetMusicConfig(AsyncWebServerRequest *request);
static void handlePostMusicConfig(AsyncWebServerRequest *request);

static void handleGetMusic(AsyncWebServerRequest *request) {
    if (request->url() == "/api/music/config") {
        handleGetMusicConfig(request);
        return;
    }
    StaticJsonDocument<256> doc;
    doc["playing"]  = musicState.playing;
    doc["title"]    = musicState.title;
    doc["artist"]   = musicState.artist;
    doc["progress"] = musicState.progressSec;
    doc["duration"] = musicState.durationSec;
    doc["style"]    = musicState.style;
    doc["provider"] = getMusicProvider();
    String json;
    serializeJson(doc, json);
    AsyncWebServerResponse *res = request->beginResponse(200, "application/json", json);
    res->addHeader("Access-Control-Allow-Origin", "*");
    request->send(res);
}

static void handlePostMusic(AsyncWebServerRequest *request) {
    if (request->url() == "/api/music/config") {
        handlePostMusicConfig(request);
        return;
    }
    bool     playing  = musicState.playing;
    String   title    = musicState.title;
    String   artist   = musicState.artist;
    uint16_t progress = musicState.progressSec;
    uint16_t duration = musicState.durationSec;
    uint8_t  style    = musicState.style;

    String val;
    if (getParamValue(request, "playing", val)) {
        playing = (val == "1" || val.equalsIgnoreCase("true") || val.equalsIgnoreCase("on"));
    }
    if (getParamValue(request, "title", val)) {
        val.trim();
        if (val.length() > 0) title = val;
    }
    if (getParamValue(request, "artist", val)) {
        val.trim();
        if (val.length() > 0) artist = val;
    }
    if (getParamValue(request, "progress", val)) {
        progress = (uint16_t)val.toInt();
    }
    if (getParamValue(request, "duration", val)) {
        duration = (uint16_t)val.toInt();
    }
    if (getParamValue(request, "style", val)) {
        style = (uint8_t)val.toInt();
    }

    updateMusicState(playing, title.c_str(), artist.c_str(), progress, duration, style);
    handleGetMusic(request);
}

static void handleGetMusicConfig(AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["provider"]  = getMusicProvider();
    doc["user"]      = getMusicUser();
    doc["has_key"]   = (getMusicApiKey().length() > 0);
    doc["has_token"] = (getMusicToken().length() > 0);
    doc["status"]    = getCloudMusicStatus();
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

static void handlePostMusicConfig(AsyncWebServerRequest *request) {
    uint8_t provider = getMusicProvider();
    String  user     = getMusicUser();
    String  apiKey   = "";
    String  token    = "";

    String val;
    if (getParamValue(request, "provider", val)) {
        provider = (uint8_t)val.toInt();
    }
    if (getParamValue(request, "user", val)) {
        val.trim();
        user = val;
    }
    if (getParamValue(request, "api_key", val) || getParamValue(request, "key", val)) {
        val.trim();
        apiKey = val;
    }
    if (getParamValue(request, "token", val)) {
        val.trim();
        token = val;
    }

    setMusicCloudConfig(provider, user, apiKey, token);
    if (provider == 1) {
        setCloudMusicStatus("Checking Last.fm...");
    } else if (provider == 2) {
        setCloudMusicStatus("Checking Spotify...");
    } else {
        setCloudMusicStatus("Browser Extension / MQTT");
    }
    handleGetMusicConfig(request);
}

// ==========================================================================
//  Web Cam WebSocket (/ws_cam)
// ==========================================================================

static AsyncWebSocket      _camWs("/ws_cam");
static uint8_t             _camBuf[QGIF_FRAME_SIZE];
static volatile bool       _camFrameNew       = false;
static SemaphoreHandle_t   _camMutex          = nullptr;
static volatile int        _camClientCount    = 0;
static uint32_t            _camActiveClientId = 0;
static uint32_t            _camLastFrameMs    = 0;
static void              (*_onCamStart)()     = nullptr;
static void              (*_onCamStop)()      = nullptr;

void webCamSetCallbacks(void (*onStart)(), void (*onStop)()) {
    _onCamStart = onStart;
    _onCamStop  = onStop;
}

bool webCamHasNewFrame() {
    return _camFrameNew;
}

void webCamConsumeFrame(uint8_t *dst) {
    if (!_camMutex) return;
    if (xSemaphoreTake(_camMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        memcpy(dst, _camBuf, QGIF_FRAME_SIZE);
        _camFrameNew = false;
        xSemaphoreGive(_camMutex);
    }
}

void webCamDisconnectAll() {
    _camWs.closeAll();
}

static void onCamWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                         AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT: {
            // Allow only one active Web Cam client at a time. Reject new connection
            // if someone is already streaming; do not touch the existing client.
            // Basic IP subnet check: only allow same /24 as device STA IP.
#if defined(ESP32) || defined(ESP8266)
            IPAddress remote = client->remoteIP();
            IPAddress local  = WiFi.localIP();
            if ((remote[0] != local[0]) || (remote[1] != local[1]) || (remote[2] != local[2])) {
                client->close();
                break;
            }
#endif
            if (_camActiveClientId != 0) {
                client->text("{\"error\":\"busy\",\"message\":\"Web Cam is in use by another client\"}");
                client->close();
                break;
            }
            _camClientCount++;
            _camActiveClientId = client->id();
            if (_onCamStart) _onCamStart();
            break;
        }
        case WS_EVT_DISCONNECT:
            if (client->id() == _camActiveClientId) {
                _camActiveClientId = 0;
                _camFrameNew = false;
                _camLastFrameMs = 0;
                if (_onCamStop) _onCamStop();
                if (_camClientCount > 0) _camClientCount--;
            }
            _camWs.cleanupClients();
            break;
        case WS_EVT_DATA: {
            AwsFrameInfo *info = (AwsFrameInfo *)arg;
            // Accept frames only from the active client
            if (client->id() != _camActiveClientId) {
                client->text("{\"error\":\"busy\",\"message\":\"Web Cam is in use by another client\"}");
                client->close();
                break;
            }
            // Rate-limit and accept only a complete, unfragmented binary message of exactly 1024 bytes
            uint32_t nowMs = millis();
            if (info->final && info->index == 0 &&
                info->len == QGIF_FRAME_SIZE && info->opcode == WS_BINARY &&
                len == QGIF_FRAME_SIZE) {
                if (_camLastFrameMs != 0 && (nowMs - _camLastFrameMs) < 50) {
                    break;
                }
                if (_camMutex && xSemaphoreTake(_camMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    memcpy(_camBuf, data, QGIF_FRAME_SIZE);
                    _camFrameNew = true;
                    _camLastFrameMs = nowMs;
                    xSemaphoreGive(_camMutex);
                }
            }
            break;
        }
        default:
            break;
    }
}

// ==========================================================================
//  Handlers -- Weather location API
// ==========================================================================

static void handleWeatherSearch(AsyncWebServerRequest *request);

// GET /api/weather → {city, lat, lon, displayName}
static void handleGetWeather(AsyncWebServerRequest *request) {
    // Defensive guard: some router versions/plugins may do prefix matching.
    // If /api/weather/search lands here, forward to the proper handler.
    if (request->url() == "/api/weather/search") {
        handleWeatherSearch(request);
        return;
    }
    StaticJsonDocument<256> doc;
    doc["city"]        = getWeatherCity();
    doc["lat"]         = getWeatherLat();
    doc["lon"]         = getWeatherLon();
    doc["displayName"] = getWeatherDisplayName();
    // Keep snake_case alias for backward compatibility across UI versions.
    doc["display_name"] = getWeatherDisplayName();
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}


// POST /api/weather?lat=&lon=&display_name=&city=&save=1  → updated JSON
static void handlePostWeather(AsyncWebServerRequest *request) {
    String latStr, lonStr;
    if (getParamValue(request, "lat", latStr) && getParamValue(request, "lon", lonStr)) {
        float lat = latStr.toFloat();
        float lon = lonStr.toFloat();
        // Basic range validation
        if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
            request->send(400, "application/json", "{\"error\":\"lat/lon out of range\"}");
            return;
        }
        String city;
        if (!getParamValue(request, "city", city)) city = getWeatherCity();

        String displayName;
        if (!getParamValue(request, "display_name", displayName)) {
            // Also accept camelCase from any older/newer UI variants.
            if (!getParamValue(request, "displayName", displayName)) {
                displayName = getWeatherDisplayName();
            }
        }
        // Reject suspiciously long or empty values
        if (city.length() == 0 || city.length() > WEATHER_CITY_MAX_LEN)
            city = city.length() > WEATHER_CITY_MAX_LEN ? city.substring(0, WEATHER_CITY_MAX_LEN) : getWeatherCity();
        if (displayName.length() > WEATHER_NAME_MAX_LEN)
            displayName = displayName.substring(0, WEATHER_NAME_MAX_LEN);
        // Persist immediately (setWeatherLocation also writes to NVS)
        setWeatherLocation(lat, lon, city, displayName);
        setWeatherManual(true);
        // Fetch fresh weather now so active weather screen doesn't show "No data".
        (void)weatherScreenRefreshNow();
    }
    handleGetWeather(request);
}

// Percent-encode a string for use as a URL query value
static String urlEncodeParam(const String &s) {
    String out;
    out.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char enc[4];
            snprintf(enc, sizeof(enc), "%%%02X", (unsigned char)c);
            out += enc;
        }
    }
    return out;
}

// GET /api/weather/search?q=CityName
// · Geocode via Open-Meteo Geocoding API (proxied by the device)
// · Returns JSON array: [{name, country, lat, lon}] (max 5 results)
static void handleWeatherSearch(AsyncWebServerRequest *request) {
    if (!request->hasParam("q")) {
        request->send(400, "application/json", "{\"error\":\"Missing q\"}");
        return;
    }
    String q = request->getParam("q")->value();
    q.trim();
    if (q.length() == 0 || q.length() > 64) {
        request->send(400, "application/json", "{\"error\":\"q must be 1-64 chars\"}");
        return;
    }
    // Build URL using plain HTTP to avoid cert overhead on ESP32-C3
    char url[256];
    String qEnc = urlEncodeParam(q);
    snprintf(url, sizeof(url),
        "http://geocoding-api.open-meteo.com/v1/search"
        "?name=%s&count=5&language=en&format=json",
        qEnc.c_str());

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(8000);
    http.begin(url);
    int code = http.GET();
    if (code < 200 || code >= 300) {
        String errMsg = "{\"error\":\"Geocoding unavailable (HTTP " + String(code) + ")\"}";
        http.end();
        request->send(502, "application/json", errMsg);
        return;
    }
    String body = http.getString();
    http.end();

    // Parse and re-emit a compact array
    JsonDocument inDoc;
    if (deserializeJson(inDoc, body) || !inDoc["results"].is<JsonArray>()) {
        request->send(200, "application/json", "[]");
        return;
    }
    JsonArray results = inDoc["results"].as<JsonArray>();
    JsonDocument outDoc;
    JsonArray arr = outDoc.to<JsonArray>();
    for (JsonObject r : results) {
        JsonObject item = arr.add<JsonObject>();
        item["name"]    = r["name"].as<const char *>();
        item["country"] = r["country_code"].as<const char *>();
        item["lat"]     = r["latitude"].as<float>();
        item["lon"]     = r["longitude"].as<float>();
    }
    String out;
    serializeJson(outDoc, out);
    request->send(200, "application/json", out);
}

// ==========================================================================
//  Init
// ==========================================================================

void webDashboardInit(AsyncWebServer &server) {
    // Cam WebSocket: create mutex, register event handler, and add to server
    _camMutex = xSemaphoreCreateMutex();
    _camWs.onEvent(onCamWsEvent);
    server.addHandler(&_camWs);

    // Dashboard at "/" only when STA is connected; when in AP mode (e.g. after WiFi lost
    // and portal restarted), "/" is left for NetWizard so opening 192.168.4.1/ shows WiFi setup.
    server.on("/", HTTP_GET, handleRoot).setFilter(ON_STA_FILTER);
    // Static assets (served from LittleFS data/ partition)
    server.on("/icon.svg",          HTTP_GET,  handleIcon);
    server.on("/favicon.ico",       HTTP_GET,  handleFavicon);
    server.on("/style.css",         HTTP_GET,  handleCSS);
    server.on("/script.js",         HTTP_GET,  handleScript);
    server.on("/jszip.min.js",      HTTP_GET,  handleJszip);
    server.on("/inter-latin.woff2", HTTP_GET,  handleFont);

    // API endpoints
    server.on("/api/list",          HTTP_GET,  handleList);
    server.on("/api/storage",       HTTP_GET,  handleStorage);
    server.on("/api/upload",        HTTP_POST, handleUploadDone, handleUploadData);
    server.on("/api/delete",        HTTP_POST, handleDelete);
    server.on("/api/play",          HTTP_POST, handlePlay);
    server.on("/api/current",       HTTP_GET,  handleCurrent);
    server.on("/api/file",          HTTP_GET,  handleGetFile);
    server.on("/api/settings",      HTTP_GET,  handleGetSettings);
    server.on("/api/settings",      HTTP_POST, handlePostSettings);
    server.on("/api/device",        HTTP_GET,  handleGetDevice);
    server.on("/api/device",        HTTP_POST, handlePostDevice);
    server.on("/api/wifi-reset",    HTTP_POST, handleWifiReset);
    server.on("/api/reboot",        HTTP_POST, handleReboot);
    server.on("/api/mqtt",          HTTP_GET,  handleGetMqtt);
    server.on("/api/mqtt",          HTTP_POST, handlePostMqtt);
    server.on("/api/pins",          HTTP_GET,  handleGetPins);
    server.on("/api/pins",          HTTP_POST, handlePostPins);
    server.on("/api/timezone",      HTTP_GET,  handleGetTimezone);
    server.on("/api/timezone",      HTTP_POST, handlePostTimezone);
    // Register more specific weather route first to avoid accidental prefix captures.
    server.on("/api/weather/search",HTTP_GET,  handleWeatherSearch);
    server.on("/api/weather",       HTTP_GET,  handleGetWeather);
    server.on("/api/weather",       HTTP_POST, handlePostWeather);
    server.on("/api/pinned",        HTTP_GET,  handleGetPinned);
    server.on("/api/pinned",        HTTP_POST, handlePostPinned);
    server.on("/api/music/config",  HTTP_GET,  handleGetMusicConfig);
    server.on("/api/music/config",  HTTP_POST, handlePostMusicConfig);
    server.on("/api/music",         HTTP_GET,  handleGetMusic);
    server.on("/api/music",         HTTP_POST, handlePostMusic);
    server.on("/api/ota/status",    HTTP_GET,  handleGetOtaStatus);
    server.on("/api/ota/cloud",     HTTP_POST, handlePostOtaCloud);
    server.on("/api/ota/cancel",    HTTP_POST, handlePostOtaCancel);
    server.on("/api/ota/upload",    HTTP_POST, handleOtaUploadDone, handleOtaUploadData);

    // Catch-all: serve .qgif files from LittleFS for browser preview (path-normalized)
    server.onNotFound([](AsyncWebServerRequest *request) {
        if (request->method() != HTTP_GET) {
            request->send(404, "text/plain", "Not found");
            return;
        }
        String path = normalizeQgifPath(request->url());
        if (path.length() > 0 && LittleFS.exists(path)) {
            request->send(LittleFS, path, "application/octet-stream");
        } else {
            request->send(404, "text/plain", "Not found");
        }
    });
}
