# SD-Card-Hosted Animation Bundle Architecture

**Project:** Synchronized Music + Light Show (handheld two-board system)
**Hardware:** ESP32-S3 Reverse TFT Feather + Music Maker FeatherWing (controller) ↔ Sparkle Motion (light node)
**Communication:** ESP-NOW
**Scope of this document:** how animation bundles are stored on the SD card, transferred to the light node, and synchronized to music playback.
**Out of scope (separate document):** effects library specification, LED layout, scene authoring workflow.

---

## 1. Overview

Animation scenes are stored as binary `.scene` bundles on the Music Maker FeatherWing's microSD card, alongside their corresponding `.mp3` files. When the user selects a song, the controller validates the bundle, streams it to the Sparkle Motion over ESP-NOW, waits for parse confirmation, then starts music playback. The light node runs choreography from its own local clock, periodically re-anchored by `PKT_SEGMENT_MARK` packets at musically meaningful boundaries (verse, chorus, bridge, etc.).

This replaces the original design where all scenes lived in the Sparkle Motion's flash and the controller broadcast continuous `PKT_TIME_TICK` packets. The change removes the flash storage ceiling (~30–60 scenes max) and eliminates accumulated timing drift over long songs.

### Key properties

- **Unlimited library size.** SD card capacity is the only ceiling; a 32 GB card holds effectively unlimited scenes.
- **No reflashing for new songs.** Adding a song = drop `<track>.mp3` + `<track>.scene` on the SD card.
- **Drift-resistant sync.** Segment marks at 30–75 second intervals re-anchor the light node's clock; within-segment drift is <100 ms and imperceptible.
- **Hard-fail validation.** Malformed bundles or transfer errors are surfaced explicitly; no silent degradation.
- **Resilient to controller crashes.** Light node continues correct playback to song end if the controller drops out mid-song (sync degrades gracefully, segments still advance on local clock).

---

## 2. File Layout on the microSD Card

Files are placed in the SD card root (or in a known directory — implementation choice).

```
mountain_king.mp3
mountain_king.scene
harder_better.mp3
harder_better.scene
crescendolls.mp3
crescendolls.scene
...
```

**Filename pairing:** an MP3 named `<name>.mp3` looks for `<name>.scene` in the same directory. Case-insensitive (FAT32 behavior).

**Canonical identifier:** the filename. The scene-name field inside the bundle header is metadata only and is not used for matching.

**Missing scene file:** see Section 7.4.

---

## 3. Binary Scene Bundle Format

### 3.1 General rules

- **Endianness:** little-endian throughout.
- **Strings:** null-terminated ASCII, with a declared maximum length. Strings that fail to null-terminate within their maximum are a validation failure.
- **Alignment:** none assumed. All multi-byte fields are read with byte-wise accessors. (ESP32 supports unaligned reads, but writing aligned format-aware parsers is fragile across compilers — byte-wise is robust.)
- **Size budget:** parsed in-RAM form must be ≤ 20 KB. Bundle file on disk is typically smaller due to segment reuse (see 3.4).
- **Integrity check:** CRC32 over all bytes of the bundle except the CRC field itself, computed and stored at the end of the file. Same CRC value serves as the cache key.

### 3.2 Bundle file structure

```
┌──────────────────────────┐
│ Header                   │  fixed 64 bytes
├──────────────────────────┤
│ Palette references       │  variable
├──────────────────────────┤
│ Segment library          │  variable: N unique segments
├──────────────────────────┤
│ Timeline                 │  variable: M references into segment library
├──────────────────────────┤
│ String pool              │  variable: scene name + segment names
├──────────────────────────┤
│ CRC32                    │  4 bytes
└──────────────────────────┘
```

### 3.3 Header (64 bytes, fixed)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `magic` | ASCII bytes `S`, `C`, `N`, `E` (0x53 0x43 0x4E 0x45) |
| 4 | 2 | `format_version` | Currently `1`. Light node firmware declares supported versions. |
| 6 | 2 | `reserved_1` | Set to 0. |
| 8 | 4 | `total_size_bytes` | Total bundle size including CRC, in bytes. |
| 12 | 4 | `crc32` | Placeholder; the actual CRC is the final 4 bytes of the file. This field is 0 during parsing. |
| 16 | 2 | `scene_name_offset` | Byte offset into the string pool where scene name begins. |
| 18 | 1 | `palette_count` | Number of palettes referenced (0–16). |
| 19 | 1 | `segment_lib_count` | Number of unique segments in the segment library (1–64). |
| 20 | 2 | `timeline_count` | Number of entries in the timeline (1–256). |
| 22 | 4 | `total_duration_ms` | Total scene duration. Should match MP3 length within a few seconds. |
| 26 | 2 | `palette_table_offset` | Byte offset to palette references section. |
| 28 | 2 | `segment_lib_offset` | Byte offset to segment library section. |
| 30 | 2 | `timeline_offset` | Byte offset to timeline section. |
| 32 | 2 | `string_pool_offset` | Byte offset to string pool. |
| 34 | 2 | `string_pool_size` | Size of string pool in bytes. |
| 36 | 1 | `base_palette_id` | Default palette ID (see effects spec). |
| 37 | 1 | `base_effect_id` | Default effect when no cue is active. |
| 38 | 26 | `reserved_2` | Reserved for future use. Set to 0. |

### 3.4 Segment library

Unique segments are stored once and referenced from the timeline. A song with structure `intro → verse → chorus → verse → chorus → bridge → chorus → outro` has 5 unique segments (`intro`, `verse`, `chorus`, `bridge`, `outro`) and 8 timeline entries.

Each segment library entry:

```
┌──────────────────────────┐
│ Segment header (12 bytes)│
├──────────────────────────┤
│ Cues (N × 16 bytes)      │
└──────────────────────────┘
```

**Segment header:**

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 2 | `name_offset` | Byte offset into string pool. |
| 2 | 1 | `name_length` | Length of segment name (not including null terminator). Max 15. |
| 3 | 1 | `base_effect_id` | Effect that runs when no cue is currently active in this segment. |
| 4 | 1 | `base_palette_id` | Palette to use for the base effect. |
| 5 | 1 | `cue_count` | Number of cues in this segment (0–255). |
| 6 | 4 | `nominal_duration_ms` | Expected duration of this segment. Used for warnings and timeline validation. |
| 10 | 2 | `reserved` | Set to 0. |

**Cue (16 bytes):**

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `time_ms_in_segment` | Time relative to segment start. Must be < nominal_duration_ms. |
| 4 | 1 | `effect_id` | Effect to invoke. |
| 5 | 1 | `palette_id` | Palette to bind for this cue. |
| 6 | 6 | `params[6]` | Effect-specific parameter bytes. |
| 12 | 4 | `reserved` | Set to 0. |

Cues within a segment are stored in chronological order by `time_ms_in_segment`.

### 3.5 Timeline

Sequence of segment references representing the song's structure. Entries are in chronological order.

Each timeline entry (8 bytes):

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `start_time_ms` | Time from song start when this segment begins. |
| 4 | 1 | `segment_id` | Index into segment library (0-based). |
| 5 | 1 | `flags` | Bit 0: `IS_OUTRO` (this is the final segment; behavior on song-end). Bits 1–7 reserved. |
| 6 | 2 | `reserved` | Set to 0. |

### 3.6 Palette table

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Number of palettes (matches header) |
| 1 | N | Sequence of palette IDs (1 byte each) |

This is just a list of palette IDs the scene declares it will use. The actual palette data lives in the Sparkle Motion firmware, indexed by ID. The list exists so validation can fail fast if a scene references a palette the firmware doesn't support.

### 3.7 String pool

Sequence of null-terminated ASCII strings. Strings are referenced by byte offset from the start of the pool. Order is arbitrary; lookup is by offset.

### 3.8 Trailer

| Offset | Size | Field |
|---|---|---|
| total_size - 4 | 4 | `crc32` over all bytes from offset 0 to total_size - 4 (i.e., everything except this field) |

---

## 4. Validation

The controller validates the bundle after reading it from the SD card, before initiating transfer to the light node. Validation is also performed by the light node after parsing the received bundle.

### 4.1 Hard fails (refuse to load, surface error on TFT)

- Magic bytes are not `SCNE`.
- `format_version` is not supported by the receiving firmware.
- `total_size_bytes` doesn't match the actual file size.
- CRC32 mismatch.
- `segment_lib_count == 0`.
- `timeline_count == 0`.
- Timeline entries are not in chronological order by `start_time_ms`.
- Two timeline entries have identical `start_time_ms`.
- Any `segment_id` in the timeline is ≥ `segment_lib_count`.
- A cue's `time_ms_in_segment` ≥ the segment's `nominal_duration_ms`.
- Cues within a segment are not in chronological order.
- Effect ID references an effect not in the firmware's library.
- Palette ID references a palette not in the firmware's library.
- A string offset points outside the string pool.
- A string in the pool is not null-terminated within its declared maximum length.
- Scene name length > 31 chars (excluding null).
- Segment name length > 15 chars (excluding null).
- Parsed in-RAM size would exceed 20 KB.

### 4.2 Warnings (load and log; playback proceeds)

These are logged to serial console and optionally briefly to the TFT. They suggest authoring bugs but are not unsafe.

- Any segment with `nominal_duration_ms < 5000` (5 seconds). Probably intentional for stings/transitions, but flag in case authoring forgot a real duration.
- Any segment with `nominal_duration_ms > 90000` (90 seconds). Drift accumulates more across long segments. Sync still works but precision degrades mid-segment.
- Any segment in the library with `cue_count == 0` (just the base effect for the whole segment). Legal but probably unintentional.
- `total_duration_ms` differs from the MP3 file's actual duration by more than 5 seconds (when MP3 duration is determinable). Indicates a stale scene.
- Any cue with all 6 `params` bytes equal to 0. Often a sign of forgotten parameter initialization.
- Any cue with `effect_id == 0` and non-zero params (where effect 0 is "off"/no-op). Probably an authoring mistake.

---

## 5. ESP-NOW Packet Protocol

### 5.1 Updated packet table

| Hex | Name | Direction | Payload |
|---|---|---|---|
| `0x10` | `PKT_DISCOVERY` | both | none (unchanged from existing handshake) |
| `0x20` | `PKT_SONG_START` | controller → light | 4 bytes scene CRC32, 4 bytes total duration ms, 1 byte name length, ≤ 31 bytes track filename (no extension), 1 byte flags (bit 0: `NO_SCENE`, run demo instead) |
| `0x21` | `PKT_SONG_STOP` | controller → light | none (user-initiated stop) |
| `0x22` | `PKT_SONG_ENDED` | controller → light | none (natural end of MP3 playback) |
| `0x23` | `PKT_TRACK_SELECTED` | controller → light | track filename (preview-aware; current behavior is no-op) |
| `0x30` | `PKT_SCENE_BEGIN` | controller → light | 4 bytes scene CRC32, 4 bytes total bundle size, 2 bytes chunk count, 2 bytes format version |
| `0x31` | `PKT_SCENE_CHUNK` | controller → light | 2 bytes chunk index, 2 bytes chunk size, payload bytes (max 240 bytes payload to stay under 250 byte ESP-NOW limit) |
| `0x32` | `PKT_SCENE_END` | controller → light | none (signals last chunk has been sent) |
| `0x40` | `PKT_SEGMENT_MARK` | controller → light | 1 byte timeline index, 4 bytes nominal start_time_ms, 1 byte name length, ≤ 15 bytes segment name |
| `0x50` | `PKT_HEARTBEAT` | controller → light | 2 bytes `decode_time_s` (whole seconds from `musicPlayer.decodeTime()`), 1 byte controller state (0=idle, 1=playing, 2=stopped) |
| `0x80` | `PKT_SCENE_ACK` | light → controller | 4 bytes scene CRC32 (echo), 1 byte ack code (0=success, 1=already cached, see Section 7.2 for failure codes), 4 bytes free heap KB |
| `0x81` | `PKT_STATUS` | light → controller | 1 byte state, 1 byte signed RSSI, 1 byte current timeline index, 1 byte current segment library index, 4 bytes local playback clock ms |
| `0x82` | `PKT_ERROR` | light → controller | 1 byte error code, 1 byte message length, ≤ 60 bytes message |

Removed from original design: `PKT_TIME_TICK` (`0x22`). Its responsibilities are split between `PKT_SEGMENT_MARK` (timing anchors) and `PKT_HEARTBEAT` (watchdog + coarse drift detection).

### 5.2 ESP-NOW size limit

All packets must fit within 250 bytes total (ESP-NOW hard limit). Largest packet is `PKT_SCENE_CHUNK` at up to 240 bytes payload + 4 bytes overhead.

---

## 6. Playback Sequence

### 6.1 User initiates playback

1. User selects a song on the controller (button cycling, future encoder, etc.).
2. Controller reads `<track>.scene` from the SD card. If absent, see 7.4.
3. Controller validates the bundle (Section 4). On hard fail, display error on TFT, abort.
4. Controller computes/reads the CRC32 from the bundle.
5. Controller sends `PKT_SONG_START` with track name, CRC32, total duration, flags.
6. Light node checks its scene cache (Section 7.1). If cache hit, light node sends `PKT_SCENE_ACK` with code 1 (already cached); skip to step 11.
7. If cache miss, light node sends `PKT_SCENE_ACK` with code 2 (need transfer), or controller proceeds to `PKT_SCENE_BEGIN` without waiting (implementation choice; recommend the latter for simplicity).
8. Controller sends `PKT_SCENE_BEGIN` with header info.
9. Controller streams `PKT_SCENE_CHUNK` packets in order, using `sendPacketWithRetry()` (max 3 retries per chunk).
10. Controller sends `PKT_SCENE_END`.
11. Light node parses the received bundle, validates (Section 4), populates in-RAM structures, sends `PKT_SCENE_ACK` with code 0 (success) or appropriate failure code (Section 7.2).
12. On any failure code, controller displays error on TFT, aborts. Music does not start.
13. On success: controller records `song_start_millis_local = millis()`, starts `musicPlayer.startPlayingFile(track_name)`, begins the heartbeat and segment-mark schedule (Section 6.3).

### 6.2 During playback

**Controller responsibilities:**

- Maintain `song_start_millis_local`. Compute `playback_elapsed_ms = millis() - song_start_millis_local`.
- Fire `PKT_SEGMENT_MARK` when `playback_elapsed_ms` crosses each timeline entry's `start_time_ms`. Includes segment name and timeline index.
- Send `PKT_HEARTBEAT` every 2 seconds with `musicPlayer.decodeTime()` (whole seconds) and current state.
- Receive `PKT_STATUS` from light node every ~1 second; render relevant fields on TFT (current segment, RSSI, free heap).
- Watch for `!musicPlayer.playingMusic`. When detected, send `PKT_SONG_ENDED`.
- Watch for user pressing stop. Send `PKT_SONG_STOP`, call `musicPlayer.stopPlaying()`.

**Light node responsibilities:**

- Maintain its own `local_playback_clock_ms`, starting at 0 when `PKT_SONG_START` arrives and the scene is loaded.
- Each frame, determine current segment: scan timeline for the entry whose `start_time_ms` is ≤ `local_playback_clock_ms` and whose successor (if any) starts after it. Determine current cue within the segment similarly using `time_ms_in_segment`.
- Render via the effects engine: call the cue's effect function with `(t, params, leds_buffer, global_context)`.
- On `PKT_SEGMENT_MARK` receipt: validate (Section 6.4) and, if accepted, snap `local_playback_clock_ms` to that timeline entry's `start_time_ms`.
- Send `PKT_STATUS` every 1 second with current state.
- Track `last_heartbeat_ms`. If `millis() - last_heartbeat_ms > 5000`, fire watchdog: transition to standby/ambient mode (Section 8.1).
- On `PKT_SONG_STOP`: immediately transition to ambient.
- On `PKT_SONG_ENDED`: play out the current segment to its end (or play `IS_OUTRO` segment if flagged in timeline), then transition to ambient.

### 6.3 Segment mark schedule

At `PKT_SONG_START` acceptance, the controller builds its mark schedule:

```c
for each timeline entry:
    schedule mark to fire at song_start_millis_local + entry.start_time_ms
```

In `loop()`, the controller checks if any scheduled marks are due and fires them. This is millisecond-precision and not tied to `decodeTime()`.

### 6.4 Segment mark acceptance policy

When `PKT_SEGMENT_MARK` arrives at the light node:

| Condition | Action |
|---|---|
| Timeline index matches current segment | Accept (small drift correction). Snap to entry's start_time_ms. |
| Timeline index is the next segment in order | Accept (normal forward advance). Snap. |
| Timeline index is forward by > 1 entry, but the gap is ≤ 30 seconds | Accept (caught up after dropped marks). Snap. |
| Timeline index is forward and the gap is > 30 seconds | Reject (likely glitch). Log to TFT/serial. Do not snap. |
| Timeline index is for an already-played segment | Reject (stale packet). Log. Do not snap. |

The 30-second gap threshold is based on segment lengths being 30–75 seconds in this project — a jump larger than one segment forward indicates a problem, not legitimate sync.

---

## 7. Caching, Errors, and Edge Cases

### 7.1 Light-node scene cache

The light node maintains an LRU cache of the most recently played scenes in SRAM.

- **Cache size:** 3 scenes.
- **Per-scene RAM ceiling:** 20 KB. Parsed scene is rejected at parse time if it would exceed this.
- **Cache key:** scene CRC32 from the bundle (also serves as integrity check).
- **In-RAM representation:** segments are stored once in their library form. The timeline holds `segment_id` references into the library. Cue arrays are not duplicated for repeated segments. Strings are stored in a small string pool.
- **Eviction:** LRU. On cache miss with cache full, evict the least recently used scene before loading the new one.
- **Cache hit behavior:** light node sends `PKT_SCENE_ACK` with code 1 (already cached); transfer is skipped.

### 7.2 Ack codes (`PKT_SCENE_ACK`)

| Code | Meaning |
|---|---|
| 0 | Success: scene received, parsed, and loaded. Ready for playback. |
| 1 | Already cached: scene with this CRC32 is in cache; no transfer needed. |
| 2 | Need transfer: cache miss; controller should proceed with `PKT_SCENE_BEGIN`. |
| 10 | CRC mismatch after transfer. |
| 11 | Format version unsupported. |
| 12 | Validation failure (specific failure type in subsequent `PKT_ERROR`). |
| 13 | Out of RAM (scene exceeds 20 KB or insufficient heap). |
| 14 | Transfer incomplete (missing chunks after `PKT_SCENE_END`). |

### 7.3 Transfer failure policy (hard fail during testing)

- **Per-chunk retry:** controller uses `sendPacketWithRetry()` with up to 3 retries per chunk.
- **Chunk retry exhaustion:** controller aborts transfer, displays "Transfer failed: chunk N" on TFT, does not start playback.
- **Transfer timeout:** if controller has not received `PKT_SCENE_ACK` within 15 seconds of sending `PKT_SCENE_END`, abort with timeout error.
- **Light-node parse failure:** light node sends `PKT_SCENE_ACK` with appropriate failure code, then optionally `PKT_ERROR` with detailed message. Controller displays error.
- **No fallback playback.** Hard fail aborts the operation. User retries or picks a different song. (This policy is for the development/testing phase and may be revisited later for production behavior.)

### 7.4 Missing or invalid scene file (lenient demo fallback)

If the user selects an MP3 for which no `.scene` file exists, or the `.scene` file fails validation:

1. Controller sends `PKT_SONG_START` with the `NO_SCENE` flag (bit 0 of the flags byte) set.
2. Light node ignores any cached scene; plays the built-in `__demo` scene (Section 8.1) instead.
3. Controller starts music playback normally.
4. Demo scene loops cleanly (cut to frame 0 on loop boundary, no fade) for the duration of the MP3.
5. Heartbeats and any (vestigial) segment marks are sent normally so the user can verify the radio link is healthy.
6. Controller's TFT displays "No scene (demo mode)" so the lenient fallback is unambiguous.

This case is lenient by design: it supports testing new songs before scenes are authored, while making it visually obvious that ESP-NOW packets are flowing.

### 7.5 Controller crash mid-playback

The light node's watchdog (5-second timeout on `PKT_HEARTBEAT`) catches this. On timeout, light node transitions to ambient/standby mode. The scene cache is retained.

### 7.6 Light node crash mid-playback

The controller's `PKT_STATUS` timeout indicates this (no status received for > 3 seconds during playback). Controller logs the loss on TFT but continues music playback. When `PKT_STATUS` resumes (light node rebooted, re-paired), the controller resumes sending heartbeats and segment marks; the light node will need to re-receive the scene unless it had cached it pre-crash (it didn't — cache is in RAM, lost on crash).

---

## 8. Built-In Scenes on the Sparkle Motion

Two scenes are baked into the Sparkle Motion's firmware as C structures, not as `.scene` bundles. They do not change without a firmware update.

### 8.1 `__ambient`

The standalone audio-reactive scene that runs when:

- The light node has not yet paired with a controller.
- The watchdog has fired (no heartbeat for 5 seconds).
- `PKT_SONG_STOP` or `PKT_SONG_ENDED` has returned the light node to standby.

Uses the onboard I²S microphone for audio reactivity. Independent of any scene authoring.

### 8.2 `__demo`

A fixed-duration sequence (~3 minutes) that demonstrates every effect in the firmware's effect library. Runs when:

- User selects an MP3 that has no `.scene` file (Section 7.4).
- User triggers a debug command (mechanism TBD; reserved).

Loops cleanly if the song is longer than the demo's duration. Cuts off mid-stream if the song is shorter. The hard restart at the loop boundary is intentional: it makes the loop obvious, which is useful diagnostically.

### 8.3 Built-in scene naming

The double-underscore prefix (`__ambient`, `__demo`) prevents accidental collision with user scenes loaded from the SD card. The light node will refuse to load a SD-streamed scene whose internal name begins with `__`.

---

## 9. Open Questions / Future Work

These are deferred but should be addressed before production use:

- **Encoder support for track selection.** Three buttons (D0/D1/D2) becomes painful at ~10+ songs. STEMMA QT is now accessible on the controller; a future rotary encoder is plausible.
- **OLED display for the Sparkle Motion.** Direct debug visibility into light-node state, independent of the radio link. Plausible future addition via STEMMA QT.
- **Production-mode transfer failure policy.** Current hard-fail behavior is correct for testing; a production version may want graceful degradation (e.g., play demo on transfer failure rather than refusing).
- **Scene preview/seek.** No mechanism currently for skipping to a specific time in a scene. May be useful for scene authoring iteration.
- **Multiple SD card directories.** If the library grows large, organizing songs into folders (by artist, mood, etc.) would help. Currently undefined.

---

## 10. Implementation Checklist

For an AI agent or developer implementing this spec, the work breaks down as:

### Controller firmware (ESP32-S3 Reverse TFT Feather)

1. SD card reading: open and validate `<track>.scene` files.
2. CRC32 computation over bundle bytes.
3. Bundle validation per Section 4.1 and 4.2.
4. ESP-NOW packet serialization for new packet types (`PKT_SCENE_BEGIN`, `_CHUNK`, `_END`, `_SEGMENT_MARK`, `_HEARTBEAT`, `_SONG_ENDED`, updated `_SONG_START`).
5. Streaming chunked transfer with per-chunk retry.
6. Segment-mark scheduler driven by `millis() - song_start_millis_local`.
7. Heartbeat timer (2 second cadence).
8. End-of-song detection via `!musicPlayer.playingMusic`.
9. TFT status display updates: current segment, RSSI, free heap, error messages.
10. Removal of `PKT_TIME_TICK` broadcast loop from original design.

### Light node firmware (Sparkle Motion)

1. ESP-NOW packet deserialization for all new packet types.
2. Scene cache (3-entry LRU keyed by CRC32, 20 KB per scene max).
3. Bundle parser: validate per Section 4.1, build in-RAM segment library + timeline.
4. Local playback clock (`local_playback_clock_ms = millis() - song_start_local_ms`).
5. Segment-mark handler with acceptance policy from Section 6.4.
6. Each-frame logic: locate current timeline entry → current segment → current cue, invoke effect.
7. Heartbeat watchdog (5 second timeout).
8. Status reporting at 1 Hz.
9. Built-in `__ambient` and `__demo` scenes defined as C constants.
10. Effect dispatch table (filled in per the separate effects library spec).

### Authoring tooling (separate document)

11. JSON-to-binary scene compiler.
12. Validation tool (runs the same checks the controller will run, but on the dev machine).
13. Scene preview/visualization tool.

---

## 11. Quick Reference: Decisions Locked

For convenience, the architectural decisions finalized during the design conversation:

- **Endianness:** little-endian.
- **Integrity:** CRC32 over bundle body; also serves as cache key.
- **In-RAM scene size ceiling:** 20 KB.
- **Scene cache:** 3-entry LRU, shared in-RAM representation (segments referenced, not duplicated).
- **Transfer failure:** hard fail with TFT error message (testing phase policy).
- **Missing scene file:** lenient — play built-in `__demo` scene synced to nothing.
- **Demo loop:** clean restart (no fade).
- **Heartbeat cadence:** 2 seconds.
- **Watchdog timeout:** 5 seconds.
- **Segment mark forward-jump limit:** 30 seconds.
- **Built-in scenes:** `__ambient` (standby/watchdog fallback) and `__demo` (no-scene fallback / smoke test).
- **Song end:** controller sends `PKT_SONG_ENDED` on natural completion; outros are normal segments with `IS_OUTRO` flag.
- **Removed from original design:** `PKT_TIME_TICK`. Replaced by `PKT_SEGMENT_MARK` (timing anchors) and `PKT_HEARTBEAT` (watchdog + coarse drift).
