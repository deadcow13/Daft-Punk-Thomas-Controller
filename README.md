# Daft Punk Thomas Helmet — Controller & Scene Authoring

Scene authoring tools, compiled scene binaries, and specification documents for the Daft Punk Thomas helmet light show system.

Part of a two-board synchronized music + light show:
- **This repo** — Scene compiler, authored scenes, spec docs
- **[daft-punk-thomas-sparkle-motion](https://github.com/deadcow13/Daft-Punk-Thomas-Sparkle-Motion)** — Sparkle Motion firmware: effects engine, ESP-NOW receiver

## What this system does

An ESP32-S3 controller plays MP3s from a microSD card and streams pre-compiled light show scenes to an Adafruit Sparkle Motion over ESP-NOW. The Sparkle Motion drives 394 WS2812 LEDs across the Thomas helmet in sync with the music.

## Hardware

| Board | Role |
|---|---|
| [Adafruit ESP32-S3 Reverse TFT Feather](https://www.adafruit.com/product/5691) (PID 5691) | Controller — MP3 playback, SD card, TFT display, ESP-NOW |
| [Adafruit Music Maker FeatherWing](https://www.adafruit.com/product/3357) (PID 3357) | VS1053 MP3 decoder + microSD slot |
| [Adafruit Sparkle Motion](https://www.adafruit.com/product/6100) (PID 6100) | Light node — 394 LEDs across 3 pins |

**LED layout (Thomas helmet, 394 total):**
- Pin 19: Left ear (16 LEDs) → Left visor edge (53 LEDs)
- Pin 21: Right ear (16 LEDs) → Right visor edge (53 LEDs)
- Pin 22: 32×8 front matrix (256 LEDs)

## Repo structure

```
AI Instructions/
  project_summary.md              ← Project status and what's next (read this first)
  sd_card_animation_architecture.md  ← Binary .scene format + ESP-NOW protocol
  thomas_effects_library_spec.md  ← 23 effects, zone model, palettes
  scene_json_schema.md            ← JSON authoring format reference
  registry.json                   ← Single source-of-truth name→ID lookup

Scenes/
  crescendolls.scene.json         ← Authored scene (Crescendolls)
  aerodynamic.scene.json          ← Authored scene (Aerodynamic)
  one_more_time.scene.json        ← Authored scene (One More Time)
  harder_better_faster_stronger.scene.json  ← Authored scene (HBFS)
  *.scene                         ← Compiled binary bundles (SD card ready)
  *.scene.report.txt              ← Compiler output reports

compile_scene.py                  ← JSON-to-binary scene compiler (Python stdlib)
```

## Compiling a scene

```bash
python compile_scene.py Scenes/crescendolls.scene.json
# Output: Scenes/crescendolls.scene + Scenes/crescendolls.scene.report.txt
```

**Options:**
```
--registry PATH     Registry file (default: AI Instructions/registry.json)
--output PATH       Output .scene file path
--sort-cues         Auto-sort cues by at_ms before compiling
--strict            Treat warnings as errors
--dump-binary       Hex-dump the output bundle to stdout
```

The compiler validates all effect names, zone names, palette names, and parameter names against `registry.json`. Unknown parameter names produce an error with fuzzy-match suggestions — e.g.:

```
EFFECT_VISOR_BEAT_WIPE: unrecognised param(s): 'sweep_direction' (did you mean 'direction'?)
Valid params: ['direction', 'beats_per_wipe', 'wave_color', 'wave_width', 'wave_brightness', 'bg_brightness']
```

## Authoring a new scene

1. Read `AI Instructions/scene_json_schema.md` — the JSON format reference
2. Read `AI Instructions/registry.json` — copy param names and values from the `examples` arrays; do not invent param names
3. Author `<song>.scene.json` in the `Scenes/` folder
4. Run `compile_scene.py` to validate and produce the binary bundle
5. Copy `<song>.scene` and `<song>.mp3` to the microSD card

**Common authoring mistakes** (documented in `registry.json`):
- Inventing plausible-sounding param names instead of copying from the registry `examples`
- Using matrix brightness > 45 (firmware clamps it; 45 is the hardware ceiling)
- Authoring per-beat cues instead of using beat-aware effects like `EFFECT_EAR_BEAT_GRADIENT`

## Scenes status

| Scene | Duration | Segments | Cues | Binary | Status |
|---|---|---|---|---|---|
| `crescendolls` | 211.6s | 11 | 35 | 949 B | ✓ Clean |
| `aerodynamic` | 207.5s | 8 | 31 | 799 B | ✓ Clean |
| `one_more_time` | 320.8s | 10 | 25 | 773 B | ✓ Clean |
| `harder_better_faster_stronger` | 224.3s | 8 | 259 | 4.6 KB | ✓ Clean |

## Binary format overview

Each `.scene` file is a little-endian binary bundle:

```
64-byte header (magic SCNE, format version, section offsets, beat grid)
Palette table
Segment library (unique segments stored once, referenced from timeline)
Timeline (ordered segment references)
Text / VU / sprite resource sections (if used)
String pool
CRC32 trailer
```

Full spec: `AI Instructions/sd_card_animation_architecture.md`
