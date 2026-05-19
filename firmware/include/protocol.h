#pragma once
#include <stdint.h>

// ── Packet type IDs ───────────────────────────────────────────────────────────
// Controller → helmet
#define PKT_DISCOVERY       0x10
#define PKT_SONG_START      0x20
#define PKT_SONG_STOP       0x21
#define PKT_SONG_ENDED      0x22
#define PKT_TRACK_SELECTED  0x23
#define PKT_SCENE_BEGIN     0x30
#define PKT_SCENE_CHUNK     0x31
#define PKT_SCENE_END       0x32
#define PKT_SEGMENT_MARK    0x40
#define PKT_HEARTBEAT       0x50
// Helmet → controller
#define PKT_SCENE_ACK       0x80
#define PKT_STATUS          0x81
#define PKT_ERROR           0x82

// ── ACK codes (PktSceneAck.ack_code) ─────────────────────────────────────────
#define ACK_SUCCESS         0
#define ACK_CACHED          1
#define ACK_NEED_TRANSFER   2
#define ACK_CRC_MISMATCH    10
#define ACK_BAD_VERSION     11
#define ACK_VALIDATION_ERR  12
#define ACK_OOM             13
#define ACK_INCOMPLETE      14

// ── Song-start flags ─────────────────────────────────────────────────────────
#define FLAG_NO_SCENE       0x01   // helmet plays demo; no transfer needed

// ── Packet structures (all little-endian, #pragma pack(1)) ───────────────────
#pragma pack(push, 1)

struct PktDiscovery {
    uint8_t type;       // 0x10
};

struct PktSongStart {
    uint8_t  type;              // 0x20
    uint32_t crc32;
    uint32_t total_duration_ms;
    uint8_t  name_len;
    uint8_t  flags;
    char     track_name[32];
};

struct PktSongStop {
    uint8_t type;       // 0x21
};

struct PktSongEnded {
    uint8_t type;       // 0x22
};

struct PktSceneBegin {
    uint8_t  type;          // 0x30
    uint32_t crc32;
    uint32_t total_size;
    uint16_t chunk_count;
    uint16_t format_version; // must be 1
};

struct PktSceneChunk {
    uint8_t  type;          // 0x31
    uint16_t chunk_index;   // 0-based
    uint16_t chunk_size;    // ≤ 240; last chunk may be shorter
    uint8_t  payload[240];
};

struct PktSceneEnd {
    uint8_t type;           // 0x32
};

struct PktSegmentMark {
    uint8_t  type;          // 0x40
    uint8_t  timeline_index;
    uint32_t start_time_ms;
    uint8_t  name_len;
    char     name[16];
};

struct PktHeartbeat {
    uint8_t  type;              // 0x50
    uint16_t decode_time_s;
    uint8_t  controller_state;  // 0=idle 1=playing 2=stopped
};

struct PktSceneAck {
    uint8_t  type;      // 0x80
    uint32_t crc32;
    uint8_t  ack_code;
    uint32_t free_heap;
};

struct PktStatus {
    uint8_t  type;          // 0x81
    uint8_t  state;         // 0=AMBIENT 1=LOADING 2=PLAYING 3=STOPPING
    int8_t   rssi;
    uint8_t  timeline_index;
    uint8_t  segment_index;
    uint32_t local_clock_ms;
};

#pragma pack(pop)
