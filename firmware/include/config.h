#pragma once

// ── Music Maker FeatherWing ──────────────────────────────────────────────────
#define VS1053_RESET  -1
#define VS1053_CS      6
#define VS1053_DCS    10
#define CARDCS         5
#define VS1053_DREQ    9

// ── Battery sense ───────────────────────────────────────────────────────────
#define VBAT_PIN       4

// ── TFT (pins defined by board variant: TFT_CS, TFT_DC, TFT_RST,
//         TFT_BACKLITE, TFT_I2C_POWER) ──────────────────────────────────────
#define SCREEN_W 240
#define SCREEN_H 135

// ── Buttons (active-LOW for D0, active-HIGH for D1/D2) ──────────────────────
#define BTN_D0 0
#define BTN_D1 1
#define BTN_D2 2

// ── ESP-NOW ──────────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL 1

// ── Timing ───────────────────────────────────────────────────────────────────
static const unsigned long HEARTBEAT_INTERVAL_MS   = 2000;   // ≤5000 required
static const unsigned long SONG_START_ACK_TIMEOUT  = 3000;   // wait for CACHED/NEED_TRANSFER
static const unsigned long SCENE_ACK_TIMEOUT_MS    = 15000;  // wait for SUCCESS after END
static const unsigned long LONG_PRESS_MS            = 800;
static const unsigned long BAT_POLL_MS              = 5000;
static const unsigned long BAT_LOW_BLINK_MS         = 500;
static const unsigned long DOT_INTERVAL_MS          = 250;
static const unsigned long CHUNK_INTER_DELAY_MS     = 5;     // breathe between chunks

// ── Scene transfer ────────────────────────────────────────────────────────────
static const int CHUNK_PAYLOAD    = 240;   // bytes per PKT_SCENE_CHUNK payload
static const int MAX_CHUNK_RETRY  = 3;
static const int CHUNK_RETRY_DELAY_MS = 20;

// ── Catalog ───────────────────────────────────────────────────────────────────
static const int MAX_SONGS        = 64;
static const int MAX_FILENAME_LEN = 64;
static const int MAX_TIMELINE     = 256;

// ── Battery ───────────────────────────────────────────────────────────────────
static const int BAT_LOW_PERCENT  = 10;

// ── Volume: VS1053 lower = louder ─────────────────────────────────────────────
static const uint8_t VOL_LEVELS[] = { 60, 40, 20, 10 };
