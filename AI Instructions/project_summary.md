# Daft Punk Helmet Light Show — Project Summary

**Status:** Architecture and specifications complete. Two rounds of AI-authored scene validation done. One scene fully clean, three near-clean. Ready to address the remaining registry quality issue, then build the compiler.
**Date of summary:** May 2026 (updated after Round 2 scene validation)
**Hardware target:** Thomas helmet (Guy helmet planned, similar architecture, separate Sparkle Motion)

---

## What this project is

A handheld synchronized music + light show system. Two Adafruit boards:

- **Controller:** ESP32-S3 Reverse TFT Feather (PID 5691) + Music Maker FeatherWing (PID 3357). Plays MP3s from a microSD card. Drives a TFT display for status. Sends commands and animation data over ESP-NOW.
- **Light node:** Adafruit Sparkle Motion (PID 6100). Drives 394 WS2812 LEDs across three data pins. Renders scenes in sync with the music.

The Thomas helmet has 394 LEDs total:
- **Pin 19:** Left ear (16-LED diamond) chained to left visor edge (53 LEDs) = 69 LEDs
- **Pin 21:** Right ear (16-LED diamond) chained to right visor edge (53 LEDs) = 69 LEDs
- **Pin 22:** 32×8 matrix on the front (256 LEDs)

Both boards use ESP-NOW for wireless communication. The Sparkle Motion's onboard I2S microphone (GPIO 33) drives audio-reactive ambient mode when no controller is paired.

---

## Locked architecture (all major decisions made)

### Scenes stream from the controller's SD card at song-start

Each MP3 on the microSD card has a corresponding `<name>.scene` binary bundle alongside it. When the user selects a song:

1. Controller reads + validates the scene bundle
2. Controller streams it to the Sparkle Motion over ESP-NOW (4–7 seconds typical for a 20 KB scene)
3. Sparkle Motion parses, caches in RAM, sends `PKT_SCENE_ACK`
4. Controller starts MP3 playback and begins the sync layer

This replaces an earlier design where all animations lived in the Sparkle Motion's flash. The SD-streaming approach has no library size ceiling (32 GB holds effectively unlimited scenes) and lets you add new songs without reflashing.

### Sync via segment-anchored marks, not continuous time ticks

The light node runs its own local playback clock starting from `PKT_SONG_START`. The controller periodically sends `PKT_SEGMENT_MARK` packets at scene-defined segment boundaries (typically 30–75 second intervals). These resync the light node's clock at musically meaningful moments.

Within a segment, drift between the two boards' clocks is under 100 ms — imperceptible. At segment boundaries, the snap-to-segment is visually masked by the music itself changing.

A `PKT_HEARTBEAT` every 2 seconds keeps the 5-second watchdog fed and provides a soft-resync backstop.

### Data-driven effects, not per-song code

The Sparkle Motion runs an effects engine that interprets a generic `cue_t[]` array against a shared effects library. Scene files are pure data — they reference effect IDs, zone IDs, palette IDs, and parameter bytes. Adding a new song requires authoring a scene JSON; it does not require firmware changes.

### Model B zone addressing

The 394 LEDs are partitioned into named zones:

| Zone | LEDs | Pin | Role |
|---|---|---|---|
| `LEFT_EAR` | 16 | 19 | Ambient/accent (diamond zones: BOT_PEAK, LOWER, FLAT, UPPER, TOP_PEAK) |
| `LEFT_VISOR` | 53 | 19 | Ambient/accent (5×8 grid + beat-flash trio + 10 connector LEDs) |
| `RIGHT_EAR` | 16 | 21 | Mirror of LEFT_EAR |
| `RIGHT_VISOR` | 53 | 21 | Mirror of LEFT_VISOR with reversed col mapping |
| `MATRIX` | 256 | 22 | Main display — text, VU, sprites |

Plus virtual zones (`BOTH_EARS`, `BOTH_VISORS`) and override targets (`LEFT_CONNECTORS`, `RIGHT_CONNECTORS`).

### Firmware-enforced hardware safety

These are baked into the Sparkle Motion firmware as `constexpr` constants. The effects engine applies them automatically; scene authors work in logical 0–255 space:

- `MATRIX_BRIGHTNESS_MAX = 45` (manufacturer hard limit, permanent)
- `EAR_VISOR_BRIGHTNESS_MAX = 185` (tunable)
- `POWER_CAP_GLOBAL_MA = 4500` (revisable when supply upgrades)
- `POWER_CAP_PIN_MA = 2820` (protects JST XH connectors, permanent)
- Gamma correction (2.6 curve) applied engine-wide
- White-point balance: R×1.00, G×1.10, B×0.65 (tunable per-helmet)

Power supply: planning to start with 5V/5A and the firmware-enforced cap; upgrade to a 12V→5V/10A buck converter if real-world brightness needs it.

### Mirror policy

Scene-level `mirror_lr: true` default. Cues targeting `LEFT_*` or `BOTH_*` zones get auto-mirrored at compile time. Per-cue `flags.no_mirror` overrides for explicit asymmetric moments.

---

## Locked design decisions

| Decision | Value | Notes |
|---|---|---|
| Binary format endianness | Little-endian | Matches ESP32 native |
| Integrity check | CRC32 | Also serves as scene cache key |
| Scene RAM ceiling | 64 KB | Revised up from 20 KB to accommodate sprites |
| Scene cache size | 2 LRU | In SRAM |
| Heartbeat cadence | 2 seconds | |
| Watchdog timeout | 5 seconds | |
| Segment-mark max forward jump | 30 seconds | Beyond = reject as glitch |
| Built-in scenes | `__ambient`, `__demo` | Baked into Sparkle Motion firmware |
| Missing `.scene` file | Play `__demo` looped | Lenient fallback |
| Transfer failure | Hard fail with TFT error | Testing-phase policy; production may relax |
| GIFs/sprites | Per-scene sprite library, 24 KB ceiling | Sprites are small regions of matrix, not full-matrix |
| Sprite frame data | 3 bytes per pixel (RGB) | Gamma applied at render time, not pre-baked |
| Text | Pre-rendered glyph subset per scene | 4×6 font, subset computed from text strings |
| VU meters | Fully pre-rendered in scene during songs | Mic-driven only in ambient mode |
| Matrix cycling within songs | Flavor A (cue-driven) | Flavor B (timer-driven) only in ambient mode |
| Connector LEDs | Addressable with defaults | Default: gold beat flash + rainbow ring |

---

## Specs and reference documents (all locked)

Five documents define the system. Together they are the single source of truth.

### 1. `sd_card_animation_architecture.md`
Defines the binary `.scene` bundle format, the ESP-NOW packet protocol, the segment-anchored sync model, scene caching, and validation/error policies. The protocol-level spec.

### 2. `thomas_effects_library_spec.md`
Defines the effects engine for the Thomas helmet specifically: the 5 physical + 2 virtual zones, coordinate models for each zone, the 23 effects with parameter byte layouts, the 11 firmware palettes, the engine's global context, and the firmware safety constants. Will need a parallel `guy_effects_library_spec.md` for the second helmet.

### 3. `scene_json_schema.md`
Defines the JSON format used to author scenes. Has two parts: a reference section (top-level structure, all fields, validation rules, the registry concept, compiler mechanics) and a worked example showing a complete scene from JSON to binary. AI agents and humans both author against this schema.

### 4. `registry.json` (44 KB)
The single source-of-truth name-to-ID mapping for palettes, zones, and effects. Each entry includes the compiler-readable definition (ID, parameter types, allowed zones) and rich human-readable annotations (`description`, `when_to_use`, `when_not_to_use`, `examples`). AI agents read this file alongside the schema doc to draft scenes.

### 5. Hardware context JSON files
`thomas_left_visor_edge_context.json` and `thomas_right_visor_edge_context.json` define the physical LED layout of the visor edge boards (5×8 grid + beat-flash trio + 10 connector LEDs, with the right side using a reversed col mapping). The Sparkle Motion firmware references these layouts when rendering visor effects.

---

## Throwaway firmware (validated hardware path, not production)

Two firmware drafts exist in project knowledge but are **not** the production target:

- `DaftPunkController_main.cpp` — proved the controller's hardware works (VS1053 playback, TFT rendering, ESP-NOW handshake)
- `DaftPunkSparkleMotio_main.cpp` — proved the Sparkle Motion's hardware works (FastLED on three pins, ESP-NOW receiver, mic input)
- `SparkleMotion_SongSketch.ino` — original mode-based renderer for Crescendolls and Aerodynamic, used as design reference

These are reference material. The production firmware will be rewritten against the specs above. The Crescendolls helmet sketch (`crescendolls_helmet.ino`) is also reference material — its segment-based choreography is the prototype for how segments work in the new scene model, and its `DISCOVERY`, `INTRO_WARM`, and `MAGENTA_CYAN` palette colors are sourced from there directly.

---

## AI-authored scenes — validation history

Two rounds of AI-drafted scenes, with iteration between them. The pattern is informative: each round resolves the previous round's issues and reveals one new layer of registry-quality limitation.

### Round 2 results (current, May 2026)

After Round 1 feedback, all four scenes were rewritten by Claude. Current state:

| Scene | Duration | Segments | Cues | Size | Errors | Status |
|---|---|---|---|---|---|---|
| `crescendolls_scene.json` | 211.6s | 11 | 35 | 23.7 KB | **0** | ✓ Ready to compile |
| `harder_better_faster_stronger_scene.json` | 224.3s | 8 | 259 | 32.5 KB | 1 | 1 param rename away from clean |
| `aerodynamic_scene.json` | 207.5s | 8 | 31 | 17.0 KB | 16 | Same 4 cues need param fix |
| `one_more_time_scene.json` | 320.8s | 10 | 25 | 15.7 KB | 16 | Same 4 cues need param fix |

**HBFS shrank by 87%** — from 252 KB / 726 cues in Round 1 to 32.5 KB / 259 cues in Round 2. The per-beat over-authoring problem is solved.

**Crescendolls is fully clean** — zero errors, zero warnings, valid against the registry. This is the first scene that's truly ready for the compiler.

### What worked in Round 2

- **All cue ordering issues resolved.** Zero out-of-order cues across all scenes.
- **The `BEAT_SWEEP` → `BEAT_WIPE` rename was applied everywhere.** No more invented effect names.
- **HBFS is now a sensibly-sized scene.** Per-beat cues replaced with beat-aware effects.
- **Palette/zone/effect compatibility is correct** everywhere except for the one remaining problem.

### Remaining problem (one pattern, two manifestations)

All Round 2 errors are the same issue: **Claude invents plausible-sounding param names rather than using the registry's actual ones.**

**Manifestation 1 — `EFFECT_VISOR_BEAT_WIPE`** (in `aerodynamic` and `one_more_time`, 4 cues each):
- Authored params: `color`, `brightness`, `sweep_direction`, `decay_steepness`
- Registry params: `direction`, `beats_per_wipe`, `wave_color`, `wave_width`, `wave_brightness`, `bg_brightness`
- Zero overlap. Claude formed an internal model of "what BEAT_WIPE needs" from the effect's description and generated names from that model rather than reading the param list.

**Manifestation 2 — `EFFECT_MATRIX_SCROLL_BG`** (in HBFS, 1 cue):
- Authored param: `scroll_direction`
- Registry param: `scroll_speed` (plus a `pattern` enum that includes direction)
- Same pattern: a sensible-sounding name that doesn't actually exist in the spec.

### What this tells us about the registry

The registry's `description` and `when_to_use` text is **sufficient for picking the right effect** but **insufficient for authoring its params correctly**. The agent reads the prose, forms an internal model of "what this effect does," and generates param names from that model.

Two registry improvements would address this:

1. **Add fully-worked example cues for every effect.** Currently only the most common effects (`EFFECT_EAR_BEAT_GRADIENT`, `EFFECT_MATRIX_TEXT`, etc.) have `examples` arrays. When Claude can copy-paste a real example and modify values, invention becomes unnecessary. This is the cheaper fix and should be done first.

2. **Standardize param naming conventions across effects.** If `color` and `brightness` were standard names everywhere, accidental invention would coincidentally produce valid output. This is a registry refactor with downstream consequences — every example, every existing scene needs updating. Defer until after the compiler exists and we can mass-rewrite.

### Round 1 results (historical, May 2026)

The first AI-drafted scenes had three problem categories, two of which are now fully resolved:

- **`EFFECT_VISOR_BEAT_SWEEP` doesn't exist** (5 instances). The correct name is `EFFECT_VISOR_BEAT_WIPE`. ✓ **Fixed in Round 2.**
- **Cues ordered by zone instead of by `at_ms`** (12+ instances). ✓ **Fixed in Round 2.**
- **HBFS over-authored at 726 cues / 252 KB.** ✓ **Fixed in Round 2** (down to 259 cues / 32.5 KB).
- **Invented param names** (1 instance in Round 1). ✗ **Persists in Round 2** (17 instances total across the same pattern).

### What this validation tells us

The workflow is producing **near-deployable scenes**. Crescendolls is fully clean. HBFS is one trivial fix away. The other two scenes have a localized pattern that's the same mistake repeated 4 times each.

The compiler can now reasonably be built. The current AI workflow produces output that's 90%+ valid; the compiler's validation will catch the remaining 10% and the authoring loop will tighten over time. **The original decision to test the AI workflow before building the compiler paid off** — without that test, the registry would have been frozen in its Round 1 state and the compiler would have been validating against scenes that all needed substantial rework.

---

## What's next (in order)

1. **Add worked example cues to the registry for every effect** — particularly `EFFECT_VISOR_BEAT_WIPE` and `EFFECT_MATRIX_SCROLL_BG` which are the source of all current errors. This addresses the "Claude invents param names" issue at the source.

2. **Fix the remaining errors in the four Round 2 scenes:**
   - `aerodynamic_scene.json`: rewrite 4 `EFFECT_VISOR_BEAT_WIPE` cues with correct params
   - `one_more_time_scene.json`: same — rewrite 4 `EFFECT_VISOR_BEAT_WIPE` cues
   - `harder_better_faster_stronger_scene.json`: rename `scroll_direction` to use `scroll_speed` + `pattern`
   - `crescendolls_scene.json`: nothing — already clean

3. **Build the JSON-to-binary compiler** (Python) — the registry is the lookup, the schema doc is the input format, the architecture doc is the output format. Includes:
   - JSON parsing + schema validation
   - Reference resolution (palette/zone/effect names → IDs, sprite/VU/text names → IDs)
   - Mirroring expansion at compile time
   - `--sort-cues` auto-fix flag (already in schema spec)
   - External file references (`frames_from_file` for GIFs, `vu_from_file` for VU data)
   - Binary output matching the architecture doc spec
   - CRC32 computation
   - Validation report output (`<scene>.scene.report.txt`)
   - **Fuzzy-match suggestions in error messages** (registry lessons: when a param name doesn't match, suggest the closest valid one)

4. **Rewrite Sparkle Motion firmware** against the effects library spec — scene bundle parser, effects engine with 23 effect implementations, zone resolver, beat grid, palette table, gamma + white-point correction, per-pin current cap enforcement, ambient mode, demo mode.

5. **Rewrite controller firmware** against the architecture spec — SD card reader, scene validator, ESP-NOW streaming with retry, segment-mark scheduler, heartbeat timer, TFT status display.

6. **Build the analysis pipeline** (Python) — librosa-based extraction of BPM, beat times, downbeats, segment boundaries for new songs. Outputs JSON analysis files that inform scene authoring.

7. **Author real scenes against working firmware** — iterate on the registry as authoring reveals what's missing. Each authored scene becomes a candidate `examples` entry in the registry.

---

## Hardware constraints and notes

### Pin assignments
- Pin 19 (Sparkle Motion): left ear chain → left visor edge chain
- Pin 21 (Sparkle Motion): right ear chain → right visor edge chain
- Pin 22 (Sparkle Motion): 32×8 matrix
- GPIO 33 (Sparkle Motion): I2S mic ws pin (does not conflict with LED pins)

### Power supply
- Target: 5V/5A bench supply initially
- Backup plan: 12V→5V/10A buck converter if brightness is constrained
- Firmware-enforced `POWER_CAP_GLOBAL_MA = 4500` (revisable when upgrading)
- Per-pin `POWER_CAP_PIN_MA = 2820` (permanent, protects JST XH connectors rated for 3A)

### Brightness ceilings
- Matrix: 45/255 hard ceiling (manufacturer: "don't exceed 10–20%")
- Ear + visor edges: 185/255 (current value, tunable)

### STEMMA QT
- Controller's STEMMA QT port is now accessible (headers adjusted)
- Sparkle Motion's STEMMA QT port is accessible
- No I2C peripherals planned for v1, but future options identified: OLED for Sparkle Motion debug visibility, NeoKey 1×4 QT for controller buttons, MAX17048 fuel gauge for accurate battery state

---

## Project context the next instance needs to know

- **The user (Nick) collaborates with AI agents to author scenes.** The workflow is: hand Claude the registry, the schema, and song details (BPM, structure, key moments). Claude drafts a scene JSON. Nick reviews, iterates. Compiler runs when one exists.
- **The user maintains scene files in a GitHub repo (deadcow13)** rather than in project knowledge. To validate scenes, ask the user to upload them as chat attachments.
- **The user is comfortable with Python** and is OK with the compiler being implemented in Python with standard library only (`json`, `struct`).
- **The user has a strong preference for AI-friendly tooling** — registry annotations, error messages, and validation reports should all be designed assuming an AI agent will read them.
- **The user is iterating on the LED layout for Guy's helmet separately.** Thomas is the current focus. Guy's effects library spec will be a future document.
- **The Sparkle Motion has no PSRAM** (classic ESP32 4 MB flash, 520 KB SRAM). Realistic free heap is ~200 KB, which is why the scene cache is 2 LRU at 64 KB each.
- **Documentation discipline matters to this project.** Every major architectural decision has been captured in markdown. The next instance should continue that discipline — when decisions are made, update the relevant spec doc.

---

## All artifacts produced so far

| File | Purpose | Size | Location |
|---|---|---|---|
| `sd_card_animation_architecture.md` | Binary format + ESP-NOW protocol | ~25 KB | Project knowledge |
| `thomas_effects_library_spec.md` | Effects engine spec for Thomas | ~38 KB | Project knowledge |
| `scene_json_schema.md` | Authoring JSON schema | ~35 KB | Project knowledge |
| `registry.json` | Single source-of-truth name→ID lookup with AI annotations | 44 KB | Project knowledge |
| `crescendolls_scene.json` | Round 2 — **fully clean** | 23.7 KB | GitHub: deadcow13 |
| `aerodynamic_scene.json` | Round 2 — 4 cues need param fix | 17.0 KB | GitHub: deadcow13 |
| `one_more_time_scene.json` | Round 2 — 4 cues need param fix | 15.7 KB | GitHub: deadcow13 |
| `harder_better_faster_stronger_scene.json` | Round 2 — 1 param rename needed | 32.5 KB | GitHub: deadcow13 |
| `project_summary.md` | This document | ~17 KB | Project knowledge |

When continuing this work in a new Claude instance, hand it the project knowledge docs and ask the user to upload the current scene files. The user's workflow keeps scenes in version control on GitHub rather than in project knowledge.
