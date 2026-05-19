/*
 * Daft Punk Thomas Controller
 * Hardware: Adafruit ESP32-S3 Reverse TFT Feather (PID 5691)
 *         + Music Maker FeatherWing (PID 3357)
 *
 * Songs screen (stopped):   D1/D2 scroll  |  D0 short = play  |  D0 long = back
 * Songs screen (playing):   D1 = stop     |  D0 long = back (stops)
 * Main screen:              D1/D2 scroll  |  D0 short = select
 * Wake from deep sleep:     hold D0 2 s
 */

#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>
SdFat SD;   // must be before VS1053 include when PREFER_SDFAT_LIBRARY is set
#include <Adafruit_VS1053.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sleep.h>

#include "config.h"
#include "protocol.h"

// ── Extra color constants ─────────────────────────────────────────────────────
#define ST77XX_DARKGREY  0x7BEF
#define ST77XX_ORANGE    0xFD20

// ── Screens ───────────────────────────────────────────────────────────────────
enum Screen : uint8_t { SCREEN_MAIN, SCREEN_SONGS, SCREEN_SHUTDOWN_CONFIRM };

enum MainItem : uint8_t { MAIN_SONGS = 0, MAIN_STOP, MAIN_SHUTDOWN, MAIN_ITEM_COUNT };
const char* MAIN_LABELS[] = { "SONGS", "STOP", "SHUT DOWN" };

// ── Catalog ───────────────────────────────────────────────────────────────────
struct Song {
    char filename[MAX_FILENAME_LEN];
    char displayName[MAX_FILENAME_LEN];
    bool hasScene;
};

// ── Segment mark schedule ─────────────────────────────────────────────────────
struct SegMark {
    unsigned long fireMs;
    uint8_t       timelineIdx;
    uint32_t      startTimeMs;
};

// ── Application state ─────────────────────────────────────────────────────────
struct AppState {
    Screen screen       = SCREEN_MAIN;
    int    mainSel      = 0;
    int    songSel      = 0;
    int    songScrollTop = 0;
    int    volIdx       = 1;

    bool         playing        = false;
    int          playingSongIdx = -1;
    unsigned long songStartMs   = 0;

    uint8_t peerAddr[6]   = {0};
    char    peerMacStr[20] = "(none)";
    bool    peerPaired    = false;

    uint8_t  nodeState    = 0;
    int8_t   nodeRssi     = 0;
    uint8_t  nodeSegIdx   = 0;
    uint32_t nodeClockMs  = 0;
    unsigned long lastStatusMs = 0;

    int   batPercent    = -1;
    float batVolts      = 0.0f;
    unsigned long lastBatPollMs = 0;
    unsigned long lastHeartbeatMs = 0;

    bool dirtyAll     = true;
    bool dirtyBattery = false;
    bool dirtyStatus  = false;
};

struct ButtonState {
    bool d0Prev      = HIGH;
    bool d1Prev      = LOW;
    bool d2Prev      = LOW;
    unsigned long d0PressStart = 0;
    bool d0LongFired = false;
};

// ── Hardware ──────────────────────────────────────────────────────────────────
Adafruit_VS1053_FilePlayer musicPlayer(VS1053_RESET, VS1053_CS, VS1053_DCS, VS1053_DREQ, CARDCS);
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ── Globals ───────────────────────────────────────────────────────────────────
AppState    st;
ButtonState bs;

Song songs[MAX_SONGS];
int  songCount = 0;

SegMark segSchedule[MAX_TIMELINE];
int     segScheduleCount = 0;
int     segScheduleNext  = 0;

uint8_t* sceneBuf     = nullptr;
uint32_t sceneBufSize = 0;

volatile bool    sceneAckReceived = false;
volatile uint8_t sceneAckCode     = 0xFF;

uint8_t broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Forward declarations ──────────────────────────────────────────────────────
void drawUI();
void stopPlayback();
void startPlayback(int songIdx);

// ── Battery ───────────────────────────────────────────────────────────────────
struct VPoint { float v; int pct; };
const VPoint LIPO_CURVE[] = {
    { 4.20f, 100 }, { 4.10f, 90 }, { 4.00f, 80 }, { 3.90f, 65 },
    { 3.80f, 50  }, { 3.70f, 30 }, { 3.60f, 15 }, { 3.50f,  5 },
    { 3.30f, 0   }
};
const int LIPO_CURVE_LEN = sizeof(LIPO_CURVE) / sizeof(LIPO_CURVE[0]);

static int voltsToPercent(float v) {
    if (v >= LIPO_CURVE[0].v)               return 100;
    if (v <= LIPO_CURVE[LIPO_CURVE_LEN-1].v) return 0;
    for (int i = 0; i < LIPO_CURVE_LEN - 1; i++) {
        float vHi = LIPO_CURVE[i].v, vLo = LIPO_CURVE[i+1].v;
        if (v <= vHi && v >= vLo) {
            float frac = (v - vLo) / (vHi - vLo);
            return (int)(LIPO_CURVE[i+1].pct + frac * (LIPO_CURVE[i].pct - LIPO_CURVE[i+1].pct));
        }
    }
    return 0;
}

static void pollBattery() {
    int raw = analogRead(VBAT_PIN);
    float v = (raw / 4095.0f) * 3.3f * 2.0f;
    st.batVolts = v;
    int pct = voltsToPercent(v);
    if (pct != st.batPercent) { st.batPercent = pct; st.dirtyBattery = true; }
    st.lastBatPollMs = millis();
}

// ── CRC32 (IEEE 802.3, 0xEDB88320) ───────────────────────────────────────────
static uint32_t crc32buf(const uint8_t* buf, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

// ── Song catalog ──────────────────────────────────────────────────────────────
static void makeDisplayName(const char* path, char* out, int outLen) {
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;

    char tmp[MAX_FILENAME_LEN];
    strncpy(tmp, base, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char* dot = strrchr(tmp, '.');
    if (dot) *dot = '\0';

    char* p = tmp;
    while (*p && isdigit((unsigned char)*p)) p++;
    if (*p && !isalpha((unsigned char)*p)) p++;

    int i = 0;
    while (*p && i < outLen - 1)
        out[i++] = (*p++ == '_') ? ' ' : *(p - 1);
    out[i] = '\0';
}

static void scanSongs() {
    songCount = 0;
    File root = SD.open("/");
    if (!root) { Serial.println("SD root open failed"); return; }

    File f;
    while ((f = root.openNextFile()) && songCount < MAX_SONGS) {
        if (f.isDirectory()) { f.close(); continue; }

        char longName[MAX_FILENAME_LEN] = {0};
        f.getName(longName, sizeof(longName));
        int len = strlen(longName);

        if (len > 4 && strcasecmp(longName + len - 4, ".mp3") == 0) {
            char path[MAX_FILENAME_LEN];
            snprintf(path, sizeof(path), "/%s", longName);

            strncpy(songs[songCount].filename,    path, MAX_FILENAME_LEN - 1);
            makeDisplayName(path, songs[songCount].displayName, MAX_FILENAME_LEN);

            char scenePath[MAX_FILENAME_LEN];
            strncpy(scenePath, path, sizeof(scenePath) - 1);
            char* dotPos = strrchr(scenePath, '.');
            if (dotPos) strcpy(dotPos, ".scene");
            songs[songCount].hasScene = SD.exists(scenePath);

            Serial.printf("Song %d: %s  scene=%s\n",
                songCount, path, songs[songCount].hasScene ? "yes" : "no");
            songCount++;
        }
        f.close();
    }
    root.close();
    Serial.printf("Found %d songs\n", songCount);
}

// ── Scene file ────────────────────────────────────────────────────────────────
static bool loadScene(int songIdx) {
    if (sceneBuf) { free(sceneBuf); sceneBuf = nullptr; sceneBufSize = 0; }

    char scenePath[MAX_FILENAME_LEN];
    strncpy(scenePath, songs[songIdx].filename, sizeof(scenePath) - 1);
    char* dot = strrchr(scenePath, '.');
    if (dot) strcpy(dot, ".scene");

    File f = SD.open(scenePath);
    if (!f) { Serial.printf("No scene: %s\n", scenePath); return false; }

    uint32_t sz = f.size();
    if (sz < 68) { f.close(); return false; }   // must have at least the header + CRC trailer

    sceneBuf = (uint8_t*)malloc(sz);
    if (!sceneBuf) { Serial.println("Scene malloc failed"); f.close(); return false; }

    if (f.read(sceneBuf, sz) != sz) {
        free(sceneBuf); sceneBuf = nullptr; f.close(); return false;
    }
    f.close();

    if (sceneBuf[0] != 'S' || sceneBuf[1] != 'C' ||
        sceneBuf[2] != 'N' || sceneBuf[3] != 'E') {
        Serial.println("Scene: bad magic"); free(sceneBuf); sceneBuf = nullptr; return false;
    }

    sceneBufSize = sz;
    Serial.printf("Scene loaded: %u bytes\n", sz);
    return true;
}

// ── Segment mark schedule (reads from loaded scene bundle) ────────────────────
static void buildSegmentSchedule(unsigned long songStartMs) {
    segScheduleCount = 0;
    segScheduleNext  = 0;
    if (!sceneBuf || sceneBufSize < 64) return;

    uint16_t timelineOff;
    uint16_t timelineCount;
    memcpy(&timelineOff,   sceneBuf + 30, 2);
    memcpy(&timelineCount, sceneBuf + 20, 2);
    if (timelineCount > MAX_TIMELINE) timelineCount = MAX_TIMELINE;

    for (uint16_t i = 0; i < timelineCount; i++) {
        uint32_t base = timelineOff + i * 8u;
        if (base + 8 > sceneBufSize) break;

        uint32_t startMs;
        memcpy(&startMs, sceneBuf + base, 4);

        segSchedule[segScheduleCount].fireMs      = songStartMs + startMs;
        segSchedule[segScheduleCount].timelineIdx = (uint8_t)i;
        segSchedule[segScheduleCount].startTimeMs = startMs;
        segScheduleCount++;
    }
    Serial.printf("Segment schedule: %d marks\n", segScheduleCount);
}

// ── ESP-NOW callbacks ─────────────────────────────────────────────────────────
static void onDataSent(const esp_now_send_info_t*, esp_now_send_status_t) {}

static void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len < 1) return;

    switch (data[0]) {
        case PKT_STATUS:
            if (len >= (int)sizeof(PktStatus)) {
                const PktStatus* p = (const PktStatus*)data;
                st.nodeState   = p->state;
                st.nodeRssi    = p->rssi;
                st.nodeSegIdx  = p->segment_index;
                st.nodeClockMs = p->local_clock_ms;
                st.lastStatusMs = millis();
                st.dirtyStatus = true;

                // First STATUS (from discovery response) → learn peer MAC
                if (!st.peerPaired) {
                    memcpy(st.peerAddr, info->src_addr, 6);
                    st.peerPaired = true;
                    snprintf(st.peerMacStr, sizeof(st.peerMacStr),
                             "%02X:%02X:%02X:%02X:%02X:%02X",
                             st.peerAddr[0], st.peerAddr[1], st.peerAddr[2],
                             st.peerAddr[3], st.peerAddr[4], st.peerAddr[5]);
                    Serial.printf("Peer discovered: %s\n", st.peerMacStr);
                }
            }
            break;

        case PKT_SCENE_ACK:
            if (len >= (int)(offsetof(PktSceneAck, ack_code) + 1)) {
                const PktSceneAck* p = (const PktSceneAck*)data;
                sceneAckCode     = p->ack_code;
                sceneAckReceived = true;
            }
            break;

        case PKT_ERROR:
            if (len >= 3) {
                uint8_t msgLen = data[2];
                char msg[64] = {0};
                if (msgLen > 0 && len >= 3 + msgLen)
                    memcpy(msg, data + 3, min((int)msgLen, 63));
                Serial.printf("Helmet error %u: %s\n", data[1], msg);
            }
            break;
    }
}

// ── ESP-NOW helpers ───────────────────────────────────────────────────────────
static void sendRaw(const void* pkt, int len) {
    esp_now_send(st.peerAddr, (const uint8_t*)pkt, len);
}

static bool sendWithRetry(const void* pkt, int len) {
    for (int i = 0; i < MAX_CHUNK_RETRY; i++) {
        if (esp_now_send(st.peerAddr, (const uint8_t*)pkt, len) == ESP_OK) return true;
        delay(CHUNK_RETRY_DELAY_MS);
    }
    return false;
}

static void sendDiscoveryBeacon() {
    PktDiscovery pkt{ PKT_DISCOVERY };
    esp_now_send(broadcastAddr, (uint8_t*)&pkt, sizeof(pkt));
}

static void sendHeartbeat() {
    if (!st.peerPaired) return;
    PktHeartbeat pkt;
    pkt.type             = PKT_HEARTBEAT;
    pkt.decode_time_s    = (uint16_t)musicPlayer.decodeTime();
    pkt.controller_state = st.playing ? 1 : 0;
    sendRaw(&pkt, sizeof(pkt));
    st.lastHeartbeatMs = millis();
}

static void sendSongStop() {
    if (!st.peerPaired) return;
    PktSongStop pkt{ PKT_SONG_STOP };
    sendRaw(&pkt, sizeof(pkt));
}

// ── TFT error helper ──────────────────────────────────────────────────────────
static void showError(const char* line1, const char* line2 = nullptr) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_RED);
    tft.setTextSize(2);
    tft.setCursor(4, 20);
    tft.print(line1);
    if (line2) { tft.setCursor(4, 44); tft.setTextColor(ST77XX_WHITE); tft.print(line2); }
    delay(2000);
}

// ── Discovery (blocking) ──────────────────────────────────────────────────────
static void waitForPeer() {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);  tft.setCursor(4, 4);  tft.print("ESP-NOW");
    tft.setTextSize(1);  tft.setCursor(4, 28); tft.print("Searching for helmet...");
    tft.setCursor(4, 42); tft.print("My MAC:");
    tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(4, 54); tft.print(WiFi.macAddress());
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(4, 66); tft.printf("Channel: %d", ESPNOW_CHANNEL);

    esp_now_peer_info_t bcast = {};
    memcpy(bcast.peer_addr, broadcastAddr, 6);
    bcast.channel = ESPNOW_CHANNEL;
    bcast.encrypt = false;
    esp_now_add_peer(&bcast);

    int dotCount = 0;
    unsigned long lastBeacon = 0, lastDot = 0;

    while (!st.peerPaired) {
        if (millis() - lastBeacon > 500) { sendDiscoveryBeacon(); lastBeacon = millis(); }
        if (millis() - lastDot > DOT_INTERVAL_MS) {
            tft.fillRect(4, 82, SCREEN_W - 8, 12, ST77XX_BLACK);
            tft.setCursor(4, 82);
            tft.setTextColor(ST77XX_YELLOW);
            tft.print("Listening");
            for (int i = 0; i < (dotCount % 4); i++) tft.print(".");
            dotCount++;
            lastDot = millis();
        }
        delay(10);
    }

    tft.fillRect(4, 82, SCREEN_W - 8, 30, ST77XX_BLACK);
    tft.setCursor(4, 82);  tft.setTextColor(ST77XX_GREEN);  tft.print("Paired!");
    tft.setCursor(4, 96);  tft.setTextColor(ST77XX_WHITE);  tft.print(st.peerMacStr);

    // Grace beacons so helmet can register us as a peer
    for (int i = 0; i < 30; i++) { sendDiscoveryBeacon(); delay(100); }

    if (esp_now_is_peer_exist(broadcastAddr)) esp_now_del_peer(broadcastAddr);

    if (!esp_now_is_peer_exist(st.peerAddr)) {
        esp_now_peer_info_t uni = {};
        memcpy(uni.peer_addr, st.peerAddr, 6);
        uni.channel = ESPNOW_CHANNEL;
        uni.encrypt = false;
        esp_now_add_peer(&uni);
    }

    Serial.printf("Paired with %s\n", st.peerMacStr);
    delay(400);
}

// ── Scene transfer ────────────────────────────────────────────────────────────
static bool transferScene(int songIdx) {
    if (!sceneBuf || sceneBufSize == 0) return false;

    // CRC is computed over the entire bundle with the header CRC field zeroed.
    // The compile_scene.py tool writes zeros at [12..15] in the file and then
    // appends the real CRC as the last 4 bytes. We can compute it the same way:
    // zero [12..15], crc32 over [0..total_size-4], then restore.
    uint32_t savedCrcField;
    memcpy(&savedCrcField, sceneBuf + 12, 4);
    memset(sceneBuf + 12, 0, 4);
    uint32_t crc = crc32buf(sceneBuf, sceneBufSize - 4);
    memcpy(sceneBuf + 12, &savedCrcField, 4);

    uint32_t totalDurMs = 0;
    if (sceneBufSize >= 26) memcpy(&totalDurMs, sceneBuf + 22, 4);

    // Track name from filename (no path, no extension)
    const char* fn   = songs[songIdx].filename;
    const char* base = strrchr(fn, '/');
    base = base ? base + 1 : fn;
    char trackName[32] = {0};
    strncpy(trackName, base, sizeof(trackName) - 1);
    char* dot = strrchr(trackName, '.');
    if (dot) *dot = '\0';

    // ── PKT_SONG_START ────────────────────────────────────────────────────────
    PktSongStart songStart;
    memset(&songStart, 0, sizeof(songStart));
    songStart.type             = PKT_SONG_START;
    songStart.crc32            = crc;
    songStart.total_duration_ms = totalDurMs;
    songStart.name_len         = (uint8_t)strlen(trackName);
    songStart.flags            = 0;
    memcpy(songStart.track_name, trackName, songStart.name_len);
    sendRaw(&songStart, sizeof(songStart));

    // Wait for ACK_CACHED or ACK_NEED_TRANSFER
    sceneAckReceived = false;
    unsigned long deadline = millis() + SONG_START_ACK_TIMEOUT;
    while (!sceneAckReceived && millis() < deadline) delay(10);

    if (!sceneAckReceived) {
        Serial.println("Song start: no ACK from helmet");
        showError("No ACK");
        return false;
    }
    if (sceneAckCode == ACK_CACHED) {
        Serial.println("Scene cache hit");
        return true;
    }
    if (sceneAckCode != ACK_NEED_TRANSFER) {
        Serial.printf("Song start unexpected ACK: %u\n", sceneAckCode);
        showError("Scene err", "unexpected ACK");
        return false;
    }

    // ── PKT_SCENE_BEGIN ───────────────────────────────────────────────────────
    uint16_t totalChunks = (uint16_t)((sceneBufSize + CHUNK_PAYLOAD - 1) / CHUNK_PAYLOAD);

    PktSceneBegin begin;
    begin.type           = PKT_SCENE_BEGIN;
    begin.crc32          = crc;
    begin.total_size     = sceneBufSize;
    begin.chunk_count    = totalChunks;
    begin.format_version = 1;
    if (!sendWithRetry(&begin, sizeof(begin))) {
        Serial.println("PKT_SCENE_BEGIN failed");
        showError("Transfer fail", "SCENE_BEGIN");
        return false;
    }

    // ── PKT_SCENE_CHUNK stream ────────────────────────────────────────────────
    tft.fillRect(4, SCREEN_H - 14, SCREEN_W - 8, 10, ST77XX_DARKGREY);

    PktSceneChunk chunk;
    chunk.type = PKT_SCENE_CHUNK;

    for (uint16_t ci = 0; ci < totalChunks; ci++) {
        uint32_t offset    = (uint32_t)ci * CHUNK_PAYLOAD;
        uint16_t chunkSize = (uint16_t)min((uint32_t)CHUNK_PAYLOAD, sceneBufSize - offset);

        chunk.chunk_index = ci;
        chunk.chunk_size  = chunkSize;
        memcpy(chunk.payload, sceneBuf + offset, chunkSize);

        int pktLen = (int)(offsetof(PktSceneChunk, payload) + chunkSize);
        bool ok = false;
        for (int retry = 0; retry < MAX_CHUNK_RETRY && !ok; retry++) {
            if (esp_now_send(st.peerAddr, (uint8_t*)&chunk, pktLen) == ESP_OK) ok = true;
            else delay(CHUNK_RETRY_DELAY_MS);
        }
        if (!ok) {
            Serial.printf("Chunk %u failed\n", ci);
            showError("Transfer fail", "chunk lost");
            return false;
        }

        // Progress bar
        int barW = (int)((float)(ci + 1) / totalChunks * (SCREEN_W - 8));
        tft.fillRect(4, SCREEN_H - 14, barW, 10, ST77XX_CYAN);

        delay(CHUNK_INTER_DELAY_MS);
    }

    // ── PKT_SCENE_END ─────────────────────────────────────────────────────────
    PktSceneEnd end{ PKT_SCENE_END };
    sendRaw(&end, sizeof(end));

    // ── Wait for final ACK ────────────────────────────────────────────────────
    sceneAckReceived = false;
    deadline = millis() + SCENE_ACK_TIMEOUT_MS;
    while (!sceneAckReceived && millis() < deadline) delay(10);

    if (!sceneAckReceived) {
        Serial.println("Scene ACK timeout after END");
        showError("ACK timeout");
        return false;
    }
    if (sceneAckCode != ACK_SUCCESS) {
        Serial.printf("Scene rejected: code %u\n", sceneAckCode);
        char msg[24];
        snprintf(msg, sizeof(msg), "code %u", sceneAckCode);
        showError("Scene rejected", msg);
        return false;
    }

    Serial.println("Scene transfer OK");
    return true;
}

// ── Playback ──────────────────────────────────────────────────────────────────
void stopPlayback() {
    if (musicPlayer.playingMusic) musicPlayer.stopPlaying();
    sendSongStop();
    st.playing        = false;
    st.playingSongIdx = -1;
    segScheduleCount  = 0;
    segScheduleNext   = 0;
    if (sceneBuf) { free(sceneBuf); sceneBuf = nullptr; sceneBufSize = 0; }
    st.dirtyAll = true;
    Serial.println("Playback stopped");
}

void startPlayback(int songIdx) {
    if (songIdx < 0 || songIdx >= songCount) return;
    if (st.playing) stopPlayback();

    Serial.printf("Starting: %s\n", songs[songIdx].displayName);

    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2);
    tft.setCursor(4, 20); tft.print("Loading...");
    tft.setTextSize(1);   tft.setCursor(4, 44); tft.print(songs[songIdx].displayName);

    bool haveScene = songs[songIdx].hasScene && loadScene(songIdx);

    if (haveScene) {
        tft.setCursor(4, 60);
        tft.setTextColor(ST77XX_CYAN); tft.print("Sending scene...");
        if (!transferScene(songIdx)) {
            st.screen   = SCREEN_SONGS;
            st.dirtyAll = true;
            return;
        }
    } else {
        // No scene: tell helmet to play demo
        const char* fn   = songs[songIdx].filename;
        const char* base = strrchr(fn, '/');
        base = base ? base + 1 : fn;
        char trackName[32] = {0};
        strncpy(trackName, base, sizeof(trackName) - 1);
        char* dot2 = strrchr(trackName, '.');
        if (dot2) *dot2 = '\0';

        PktSongStart pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type    = PKT_SONG_START;
        pkt.flags   = FLAG_NO_SCENE;
        pkt.name_len = (uint8_t)strlen(trackName);
        memcpy(pkt.track_name, trackName, pkt.name_len);
        sendRaw(&pkt, sizeof(pkt));

        tft.setCursor(4, 60);
        tft.setTextColor(ST77XX_ORANGE); tft.print("No scene (demo mode)");
        delay(600);
    }

    if (!musicPlayer.startPlayingFile(songs[songIdx].filename)) {
        Serial.println("MP3 open failed");
        showError("MP3 open fail");
        st.dirtyAll = true;
        return;
    }

    st.playing        = true;
    st.playingSongIdx = songIdx;
    st.songStartMs    = millis();

    if (haveScene) buildSegmentSchedule(st.songStartMs);

    // Send an immediate heartbeat so the helmet's watchdog is reset at the
    // exact moment music starts (prevents 5-s gap from scene-ACK to first HB).
    sendHeartbeat();

    st.screen   = SCREEN_SONGS;
    st.dirtyAll = true;
    Serial.printf("Playing: %s\n", songs[songIdx].filename);
}

// ── Segment marks ─────────────────────────────────────────────────────────────
static void tickSegmentMarks() {
    if (!st.playing || !st.peerPaired) return;
    if (segScheduleNext >= segScheduleCount) return;

    unsigned long now = millis();
    while (segScheduleNext < segScheduleCount &&
           now >= segSchedule[segScheduleNext].fireMs) {

        SegMark& sm = segSchedule[segScheduleNext];

        // Read segment name from bundle via the timeline → segment library
        char segName[17] = "?";
        uint8_t segNameLen = 1;
        if (sceneBuf && sceneBufSize > 64) {
            uint16_t tlOff;
            memcpy(&tlOff, sceneBuf + 30, 2);
            uint32_t entry = tlOff + sm.timelineIdx * 8u;
            uint8_t segId = (entry + 8 <= sceneBufSize) ? sceneBuf[entry + 4] : 0;

            uint16_t libOff;
            memcpy(&libOff, sceneBuf + 28, 2);
            uint32_t seOff = libOff;
            for (int s = 0; s < segId && seOff + 12 <= sceneBufSize; s++) {
                uint8_t cueCount = sceneBuf[seOff + 5];
                seOff += 12 + cueCount * 16u;
            }
            if (seOff + 12 <= sceneBufSize) {
                uint16_t nameOff;
                uint8_t  nameLen;
                memcpy(&nameOff, sceneBuf + seOff, 2);
                nameLen = sceneBuf[seOff + 2];

                uint16_t spOff;
                memcpy(&spOff, sceneBuf + 32, 2);
                uint32_t strAbs = spOff + nameOff;
                if (nameLen > 0 && nameLen <= 16 && strAbs + nameLen < sceneBufSize) {
                    memcpy(segName, sceneBuf + strAbs, nameLen);
                    segName[nameLen] = '\0';
                    segNameLen = nameLen;
                }
            }
        }

        PktSegmentMark pkt;
        pkt.type          = PKT_SEGMENT_MARK;
        pkt.timeline_index = sm.timelineIdx;
        pkt.start_time_ms = sm.startTimeMs;
        pkt.name_len      = segNameLen;
        memset(pkt.name, 0, sizeof(pkt.name));
        memcpy(pkt.name, segName, segNameLen);

        int pktLen = (int)(offsetof(PktSegmentMark, name) + segNameLen);
        sendRaw(&pkt, pktLen);

        Serial.printf("SegMark %u -> %s\n", sm.timelineIdx, segName);
        segScheduleNext++;
    }
}

// ── Power off ─────────────────────────────────────────────────────────────────
static void powerOff() {
    if (st.playing) stopPlayback();
    Serial.println("Deep sleep");

    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2);
    tft.setCursor(20, SCREEN_H / 2 - 16); tft.print("Powering off");
    tft.setTextSize(1); tft.setTextColor(ST77XX_DARKGREY);
    tft.setCursor(20, SCREEN_H / 2 + 8);  tft.print("Hold D0 to wake");
    delay(1200);

    digitalWrite(TFT_BACKLITE, LOW);
    tft.fillScreen(ST77XX_BLACK);

    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    Serial.flush();
    esp_deep_sleep_start();
}

// ── UI: battery indicator (top-right) ─────────────────────────────────────────
static void drawBatteryIndicator() {
    const int bx = SCREEN_W - 52, by = 2, bw = 42, bh = 12;
    tft.fillRect(bx - 30, by, bw + 34, bh + 2, ST77XX_BLACK);

    bool low = (st.batPercent >= 0 && st.batPercent < BAT_LOW_PERCENT);
    uint16_t color;
    if (low)
        color = ((millis() / BAT_LOW_BLINK_MS) & 1) ? ST77XX_RED : ST77XX_DARKGREY;
    else if (st.batPercent < 30) color = ST77XX_ORANGE;
    else                         color = ST77XX_GREEN;

    tft.drawRect(bx, by, bw, bh, ST77XX_WHITE);
    tft.fillRect(bx + bw, by + 3, 3, bh - 6, ST77XX_WHITE);
    int fillW = (st.batPercent < 0) ? 0 : (bw - 2) * st.batPercent / 100;
    if (fillW > 0) tft.fillRect(bx + 1, by + 1, fillW, bh - 2, color);

    tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(bx - 28, by + 2);
    if (st.batPercent >= 0) tft.printf("%3d%%", st.batPercent);
    else                    tft.print("  ?%");
}

// ── UI: node status strip (bottom of songs screen) ───────────────────────────
static void drawNodeStatus() {
    const int y = SCREEN_H - 14;
    tft.fillRect(0, y, SCREEN_W, 14, ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setCursor(4, y + 2);
    if (!st.peerPaired) {
        tft.setTextColor(ST77XX_RED); tft.print("No peer");
        return;
    }
    if (st.playing) {
        tft.setTextColor(ST77XX_GREEN);
        tft.printf("PLAYING  seg:%u  %s", st.nodeSegIdx, st.peerMacStr + 9);
    } else {
        tft.setTextColor(ST77XX_DARKGREY);
        tft.printf("Ready  %s", st.peerMacStr + 9);
    }
}

// ── UI: main screen ───────────────────────────────────────────────────────────
static void drawMain() {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextSize(1); tft.setTextColor(ST77XX_DARKGREY);
    tft.setCursor(4, 4); tft.print("[MAIN]  D1/D2=scroll  D0=select");

    const int itemH = 34, listTop = 20;
    for (int i = 0; i < MAIN_ITEM_COUNT; i++) {
        int y   = listTop + i * itemH;
        bool sel = (i == st.mainSel);
        if (sel) {
            tft.fillRect(2, y - 1, SCREEN_W - 56, itemH - 4, ST77XX_WHITE);
            tft.setTextColor(ST77XX_BLACK);
        } else {
            tft.setTextColor(ST77XX_WHITE);
        }
        tft.setTextSize(2); tft.setCursor(8, y + 4);
        tft.print(MAIN_LABELS[i]);
    }
    drawBatteryIndicator();
}

// ── UI: songs screen ──────────────────────────────────────────────────────────
static const int SONGS_VISIBLE = 6;

static void drawSongs() {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextSize(1); tft.setTextColor(ST77XX_DARKGREY);
    tft.setCursor(4, 4);
    tft.print(st.playing ? "[SONGS]  D1=stop  D0-long=back"
                         : "[SONGS]  D0=play  D0-long=back");

    if (songCount == 0) {
        tft.setCursor(4, 30); tft.setTextColor(ST77XX_RED);
        tft.print("No MP3 files on SD");
        drawBatteryIndicator(); return;
    }

    const int listTop = 16, rowH = 18;
    for (int i = 0; i < SONGS_VISIBLE; i++) {
        int idx = st.songScrollTop + i;
        if (idx >= songCount) break;
        int y       = listTop + i * rowH;
        bool sel     = (idx == st.songSel);
        bool playing = (st.playing && idx == st.playingSongIdx);

        if (sel) {
            tft.fillRect(2, y - 1, SCREEN_W - 12, rowH - 2, ST77XX_WHITE);
            tft.setTextColor(ST77XX_BLACK);
        } else if (playing) {
            tft.setTextColor(ST77XX_GREEN);
        } else {
            tft.setTextColor(ST77XX_WHITE);
        }
        tft.setTextSize(1); tft.setCursor(8, y + 4);
        tft.print(playing ? "> " : (songs[idx].hasScene ? "* " : "  "));
        tft.print(songs[idx].displayName);
    }

    if (songCount > SONGS_VISIBLE) {
        int barH  = SCREEN_H - 28 - 14;
        int thumbH = max(6, barH * SONGS_VISIBLE / songCount);
        int thumbY = 16 + barH * st.songScrollTop / songCount;
        tft.drawRect(SCREEN_W - 8, 16, 6, barH, ST77XX_DARKGREY);
        tft.fillRect(SCREEN_W - 7, thumbY, 4, thumbH, ST77XX_WHITE);
    }

    drawNodeStatus();
    drawBatteryIndicator();
}

// ── UI: shutdown confirm ──────────────────────────────────────────────────────
static void drawShutdownConfirm() {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextSize(2); tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(4, 20); tft.print("Shut down?");
    tft.setTextSize(1); tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(4, 52); tft.print("D0 again to confirm");
    tft.setTextColor(ST77XX_DARKGREY);
    tft.setCursor(4, 66); tft.print("Any other key cancels");
    drawBatteryIndicator();
}

void drawUI() {
    switch (st.screen) {
        case SCREEN_MAIN:             drawMain();            break;
        case SCREEN_SONGS:            drawSongs();           break;
        case SCREEN_SHUTDOWN_CONFIRM: drawShutdownConfirm(); break;
    }
}

// ── Button handling ───────────────────────────────────────────────────────────
static void handleButtons() {
    bool d0 = digitalRead(BTN_D0);
    bool d1 = digitalRead(BTN_D1);
    bool d2 = digitalRead(BTN_D2);

    if (bs.d0Prev == HIGH && d0 == LOW) {
        bs.d0PressStart = millis(); bs.d0LongFired = false;
    }
    bool d0LongNow = (d0 == LOW && !bs.d0LongFired &&
                      millis() - bs.d0PressStart >= LONG_PRESS_MS);
    if (d0LongNow) bs.d0LongFired = true;

    bool d0Short = (bs.d0Prev == LOW && d0 == HIGH && !bs.d0LongFired);
    bool d1Rise  = (bs.d1Prev == LOW && d1 == HIGH);
    bool d2Rise  = (bs.d2Prev == LOW && d2 == HIGH);

    switch (st.screen) {

        case SCREEN_MAIN:
            if (d1Rise)  { st.mainSel = (st.mainSel + 1) % MAIN_ITEM_COUNT; st.dirtyAll = true; }
            if (d2Rise)  { st.mainSel = (st.mainSel - 1 + MAIN_ITEM_COUNT) % MAIN_ITEM_COUNT; st.dirtyAll = true; }
            if (d0Short) {
                switch ((MainItem)st.mainSel) {
                    case MAIN_SONGS:    st.screen = SCREEN_SONGS; st.dirtyAll = true; break;
                    case MAIN_STOP:     if (st.playing) stopPlayback(); st.dirtyAll = true; break;
                    case MAIN_SHUTDOWN: st.screen = SCREEN_SHUTDOWN_CONFIRM; st.dirtyAll = true; break;
                    default: break;
                }
            }
            break;

        case SCREEN_SONGS:
            if (d0LongNow) {
                if (st.playing) stopPlayback();
                st.screen = SCREEN_MAIN; st.dirtyAll = true;
            } else if (d0Short && !st.playing) {
                startPlayback(st.songSel);
            }
            if (d1Rise) {
                if (st.playing) {
                    stopPlayback();
                } else {
                    if (st.songSel < songCount - 1) st.songSel++;
                    if (st.songSel >= st.songScrollTop + SONGS_VISIBLE)
                        st.songScrollTop = st.songSel - SONGS_VISIBLE + 1;
                    st.dirtyAll = true;
                }
            }
            if (d2Rise && !st.playing) {
                if (st.songSel > 0) st.songSel--;
                if (st.songSel < st.songScrollTop)
                    st.songScrollTop = st.songSel;
                st.dirtyAll = true;
            }
            break;

        case SCREEN_SHUTDOWN_CONFIRM:
            if (d0Short)                 powerOff();
            if (d1Rise || d2Rise || d0LongNow) { st.screen = SCREEN_MAIN; st.dirtyAll = true; }
            break;
    }

    bs.d0Prev = d0; bs.d1Prev = d1; bs.d2Prev = d2;
}

// ── Audio tick ────────────────────────────────────────────────────────────────
static void tickAudio() {
    if (musicPlayer.playingMusic) musicPlayer.feedBuffer();

    static bool wasPlaying = false;
    if (wasPlaying && !musicPlayer.playingMusic && st.playing) {
        Serial.println("Track ended");
        PktSongEnded pkt{ PKT_SONG_ENDED };
        sendRaw(&pkt, sizeof(pkt));
        st.playing        = false;
        st.playingSongIdx = -1;
        segScheduleCount  = 0;
        segScheduleNext   = 0;
        if (sceneBuf) { free(sceneBuf); sceneBuf = nullptr; sceneBufSize = 0; }
        st.dirtyAll = true;
    }
    wasPlaying = musicPlayer.playingMusic;
}

// ── Display tick ──────────────────────────────────────────────────────────────
static void tickDisplay() {
    if (!st.lastBatPollMs || millis() - st.lastBatPollMs > BAT_POLL_MS)
        pollBattery();

    if (st.dirtyAll) {
        drawUI(); st.dirtyAll = st.dirtyBattery = st.dirtyStatus = false; return;
    }
    if (st.dirtyBattery) { drawBatteryIndicator(); st.dirtyBattery = false; }
    if (st.dirtyStatus && st.screen == SCREEN_SONGS) { drawNodeStatus(); st.dirtyStatus = false; }

    if (st.batPercent >= 0 && st.batPercent < BAT_LOW_PERCENT) {
        static unsigned long lastBlink = 0;
        if (millis() - lastBlink > BAT_LOW_BLINK_MS) { drawBatteryIndicator(); lastBlink = millis(); }
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\nDaft Punk Controller boot (wake=%d)\n", esp_sleep_get_wakeup_cause());

    pinMode(TFT_I2C_POWER, OUTPUT); digitalWrite(TFT_I2C_POWER, HIGH); delay(10);
    pinMode(TFT_BACKLITE,  OUTPUT); digitalWrite(TFT_BACKLITE,  HIGH);

    tft.init(135, 240);
    tft.setRotation(3);
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextWrap(false);
    tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
    tft.setCursor(4, 4); tft.print("Booting...");

    pinMode(BTN_D0, INPUT_PULLUP);
    pinMode(BTN_D1, INPUT_PULLDOWN);
    pinMode(BTN_D2, INPUT_PULLDOWN);

    analogReadResolution(12);
    analogSetPinAttenuation(VBAT_PIN, ADC_11db);

    tft.setCursor(4, 16); tft.print("VS1053...");
    if (!musicPlayer.begin()) {
        tft.fillScreen(ST77XX_RED); tft.setTextSize(2);
        tft.setCursor(4, 4); tft.print("VS1053 FAIL");
        Serial.println("VS1053 FAIL"); while (1) delay(100);
    }
    musicPlayer.setVolume(VOL_LEVELS[st.volIdx], VOL_LEVELS[st.volIdx]);
    Serial.println("VS1053 OK");

    tft.setCursor(4, 28); tft.print("SD card...");
    if (!SD.begin(CARDCS)) {
        tft.fillScreen(ST77XX_RED); tft.setTextSize(2);
        tft.setCursor(4, 4); tft.print("SD FAIL");
        Serial.println("SD FAIL"); while (1) delay(100);
    }
    Serial.println("SD OK");

    tft.setCursor(4, 40); tft.print("Scanning...");
    scanSongs();
    tft.setCursor(4, 52); tft.printf("Found %d songs", songCount);

    tft.setCursor(4, 64); tft.print("ESP-NOW...");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    if (esp_now_init() != ESP_OK) {
        tft.fillScreen(ST77XX_RED); tft.setTextSize(2);
        tft.setCursor(4, 4); tft.print("ESPNOW FAIL");
        Serial.println("ESP-NOW init failed"); while (1) delay(100);
    }
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);

    waitForPeer();
    pollBattery();

    st.dirtyAll = true;
    Serial.println("Ready.");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    tickAudio();
    tickSegmentMarks();
    handleButtons();

    if (st.playing && millis() - st.lastHeartbeatMs > HEARTBEAT_INTERVAL_MS)
        sendHeartbeat();

    tickDisplay();
    delay(1);
}
