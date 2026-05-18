# Scene JSON Schema Specification

**Project:** Synchronized Music + Light Show — Thomas helmet
**Purpose:** Defines the JSON format used to author scenes. A scene is authored as `<song>.scene.json`, then compiled to a binary `<song>.scene` bundle (per `sd_card_animation_architecture.md`) by a Python build script.
**Companion documents:**
- `sd_card_animation_architecture.md` — defines the binary bundle format and ESP-NOW transfer protocol
- `thomas_effects_library_spec.md` — defines effect IDs, zone IDs, palette IDs, and all parameter semantics

**Reading guide:** Part I is a reference document — field-by-field tables, validation rules, and binary mapping. Part II is a complete worked example showing every section of the schema in context.

---

# Part I: Reference

## 1. Top-Level Schema

A scene JSON file is a single JSON object with this top-level structure:

```json
{
  "schema_version": 1,
  "scene_name": "string",
  "total_duration_ms": <integer>,
  "beat_grid": { ... },
  "base_palette": "PAL_NAME",
  "base_effect": "EFFECT_NAME",
  "mirror_lr": true,
  "segments": { ... },
  "timeline": [ ... ],
  "resources": {
    "sprites": [ ... ],
    "vu_sequences": [ ... ],
    "text_strings": [ ... ]
  }
}
```

### 1.1 Top-level fields

| Field | Type | Required | Description |
|---|---|---|---|
| `schema_version` | integer | yes | Currently `1`. Bumps when this schema changes incompatibly. |
| `scene_name` | string | yes | Max 31 chars. Metadata only; the SD filename is the canonical identifier. Cannot begin with `__` (reserved for built-in scenes). |
| `total_duration_ms` | integer | yes | Total scene duration in milliseconds. Should match the MP3's actual duration within ~5 seconds. |
| `beat_grid` | object | yes | See Section 2. |
| `base_palette` | string | yes | Palette name (see effects spec Section 6.2). Used when no cue's palette is active. |
| `base_effect` | string | yes | Effect name applied to all zones when no cue overrides. Typically `EFFECT_OFF`. |
| `mirror_lr` | boolean | no | Default `true`. Scene-wide left/right mirror policy. Individual cues can override via `no_mirror` flag. |
| `segments` | object | yes | Segment library: dictionary of segment name → segment definition. See Section 3. |
| `timeline` | array | yes | Ordered list of `(start_time_ms, segment_name)` entries. See Section 4. |
| `resources` | object | no | Optional. Sprite, VU, and text resources. See Sections 6–8. |

### 1.2 Binary mapping (top-level)

Maps to the header in the binary bundle:

| JSON field | Binary header field |
|---|---|
| `scene_name` | `scene_name_offset` → string pool entry |
| `total_duration_ms` | `total_duration_ms` |
| `beat_grid.first_downbeat_ms` | `beat_first_downbeat_ms` |
| `beat_grid.period_ms` | `beat_period_ms_x1000` (× 1000) |
| `beat_grid.beats_per_bar` | `beats_per_bar` |
| `base_palette` (name → ID) | `base_palette_id` |
| `base_effect` (name → ID) | `base_effect_id` |
| `mirror_lr` | not a binary field; expanded at compile time into per-cue mirror flags |

---

## 2. Beat Grid

```json
"beat_grid": {
  "first_downbeat_ms": <integer>,
  "period_ms": <number>,
  "beats_per_bar": <integer>
}
```

| Field | Type | Description |
|---|---|---|
| `first_downbeat_ms` | integer | When beat 1 of bar 1 occurs in the song, in ms from song start. From librosa analysis or hand-tuned. |
| `period_ms` | number | Beat period in milliseconds. Float allowed for sub-ms precision (will be multiplied by 1000 and stored as int32). |
| `beats_per_bar` | integer | Typically `4`. Range 2–16. |

**Example:** A 124 BPM song with the first downbeat at 1.62 seconds:
```json
"beat_grid": {
  "first_downbeat_ms": 1620,
  "period_ms": 483.871,
  "beats_per_bar": 4
}
```

The engine exposes `beat_phase`, `beat_index`, and `beat_in_bar` derived from these values. Effects use them for tempo-locked rendering without needing per-beat cues.

---

## 3. Segment Library

The `segments` object is a dictionary mapping segment names to segment definitions. Segment names appear in the timeline by reference.

```json
"segments": {
  "intro": { ... },
  "verse": { ... },
  "chorus": { ... },
  "bridge": { ... },
  "outro": { ... }
}
```

### 3.1 Segment definition

```json
{
  "nominal_duration_ms": <integer>,
  "base_effect": "EFFECT_NAME",
  "base_palette": "PAL_NAME",
  "is_outro": false,
  "cues": [ ... ]
}
```

| Field | Type | Required | Description |
|---|---|---|---|
| `nominal_duration_ms` | integer | yes | Expected duration. Cue timestamps must fall within this. |
| `base_effect` | string | yes | Effect that renders when no cue is active in this segment. Usually `EFFECT_OFF` or a low-intensity fill effect. |
| `base_palette` | string | yes | Palette bound to the base effect. |
| `is_outro` | boolean | no | Default `false`. Setting `true` flags this segment as the song's natural-end segment (sets `IS_OUTRO` flag in binary timeline entry). |
| `cues` | array | yes | Array of cue objects in chronological order. Can be empty (just runs base effect). |

### 3.2 Segment naming rules

- Max 15 characters, ASCII only
- Cannot begin with `__`
- Must be unique within the segment library
- Conventional names: `intro`, `verse`, `verse_1`, `verse_2`, `chorus`, `chorus_1`, `bridge`, `breakdown`, `drop`, `outro`. Use what describes the music.

### 3.3 Reuse

A segment defined once can appear multiple times in the timeline. The binary format stores the segment once in the library; the timeline references it by ID. This is the storage optimization that makes long, repetitive songs fit within the scene size budget.

---

## 4. Timeline

The timeline is an ordered array of segment activations. Each entry says "at time T, start playing segment S."

```json
"timeline": [
  { "at_ms": 0,      "play": "intro" },
  { "at_ms": 18000,  "play": "verse" },
  { "at_ms": 48000,  "play": "chorus" },
  { "at_ms": 78000,  "play": "verse" },
  { "at_ms": 108000, "play": "chorus" },
  { "at_ms": 138000, "play": "outro" }
]
```

### 4.1 Entry fields

| Field | Type | Required | Description |
|---|---|---|---|
| `at_ms` | integer | yes | Start time relative to song start. |
| `play` | string | yes | Name of a segment defined in the `segments` object. |

### 4.2 Timeline validation rules

- Entries must be in strictly chronological order by `at_ms`
- No two entries may share the same `at_ms`
- Every `play` value must reference a segment defined in `segments`
- The first entry's `at_ms` is typically `0` (a scene that starts with darkness can begin with `EFFECT_OFF` as the intro segment's base effect)
- The last entry, plus its segment's `nominal_duration_ms`, should approximate `total_duration_ms`

### 4.3 Binary mapping

Each timeline entry becomes one 8-byte binary timeline entry:

| JSON field | Binary field |
|---|---|
| `at_ms` | `start_time_ms` (uint32) |
| `play` (segment name → ID) | `segment_id` (uint8) |
| segment's `is_outro` | flags bit 0 (`IS_OUTRO`) |

---

## 5. Cues

Cues live inside a segment's `cues` array. Each cue defines an effect that runs at a specific moment within the segment, on a specific zone.

```json
{
  "at_ms": <integer>,
  "effect": "EFFECT_NAME",
  "zone": "ZONE_NAME",
  "palette": "PAL_NAME",
  "params": { ... },
  "duration_ms": <integer>,
  "flags": {
    "no_mirror": false,
    "additive": false
  }
}
```

### 5.1 Cue fields

| Field | Type | Required | Description |
|---|---|---|---|
| `at_ms` | integer | yes | Time relative to segment start (NOT song start). Must be < segment's `nominal_duration_ms`. |
| `effect` | string | yes | Effect name from effects library spec Section 5. |
| `zone` | string | yes | Zone name (see Section 9 below). |
| `palette` | string | no | Palette to bind for this cue. Defaults to the segment's `base_palette` if omitted. |
| `params` | object | no | Named parameters for the effect. See Section 5.2. Defaults to all zeros if omitted. |
| `duration_ms` | integer | no | How long the cue stays active. Default `0` means "until the next cue in this segment overrides this zone." |
| `flags.no_mirror` | boolean | no | Default `false`. If `true`, this cue is not auto-mirrored to the opposite side even if scene's `mirror_lr` is `true`. |
| `flags.additive` | boolean | no | Default `false`. If `true`, this cue layers on top of the underlying effect instead of replacing it. |

### 5.2 Named parameters

Effects in the effects library spec describe their parameters as `params[0]` through `params[5]` with specific meanings (e.g. "params[0] = palette color index, params[1] = brightness scale"). For authoring, the JSON uses **named parameters** that the compiler maps to byte positions.

The compiler reads parameter names from a registry (see Section 10). For each effect, the registry defines:

```
EFFECT_EAR_BEAT_GRADIENT:
  params[0] -> "decay_steepness"      uint8
  params[1] -> "bottom_color"         palette_index
  params[2] -> "bottom_brightness"    uint8
  params[3] -> "top_brightness"       uint8
  params[4] -> "downbeat_flash"       uint8
  params[5] -> "subbeat_sparkle"      uint8
```

So a cue authored as:

```json
{
  "at_ms": 0,
  "effect": "EFFECT_EAR_BEAT_GRADIENT",
  "zone": "BOTH_EARS",
  "params": {
    "decay_steepness": 180,
    "bottom_color": 2,
    "bottom_brightness": 40,
    "top_brightness": 200,
    "downbeat_flash": 255,
    "subbeat_sparkle": 0
  }
}
```

Compiles to:

```
params: [180, 2, 40, 200, 255, 0]
```

**Parameter types in the registry:**

| Type | JSON representation | Binary representation |
|---|---|---|
| `uint8` | integer 0–255 | 1 byte |
| `int8` | integer -128 to 127 | 1 byte (two's complement) |
| `palette_index` | integer 0–15 (palette gradient index) | 1 byte |
| `bool` | boolean | 1 byte (0 or 1) |
| `enum` | string from a list defined in registry | 1 byte (registry-defined value) |

Unused parameter bytes can be omitted from JSON; the compiler fills them with 0.

### 5.3 Cue validation rules

- `at_ms` must be ≥ 0 and < the enclosing segment's `nominal_duration_ms`
- `effect` must exist in the effects registry
- `zone` must exist in the zone registry
- `effect` must be compatible with `zone` (the registry declares each effect's allowed zones)
- All named parameters must match the effect's registry definition
- Parameter values must fit their declared type (e.g. `uint8` rejects 300)
- If `flags.no_mirror` is set on a zone that has no mirror partner (`MATRIX`, the connector ID's), the compiler emits a warning (the flag is harmless but indicates author confusion)
- If `palette` is named, it must exist in the palette registry

### 5.4 Cue ordering and overlap

Cues within a segment must be sorted by `at_ms` ascending. The compiler enforces this (and will sort them automatically if you set a `--sort-cues` compiler flag, but warns).

Multiple cues at the same time targeting the **same** zone are an error. Multiple cues at the same time targeting **different** zones are normal and expected — that's how you orchestrate multi-zone moments.

A cue with `duration_ms: 0` is active until another cue in the same segment targeting the same zone overrides it. A cue with `duration_ms > 0` is active for that span; after it ends, the zone reverts to the segment's `base_effect` until the next cue.

### 5.5 Mirroring behavior

When the scene has `mirror_lr: true`:

- A cue targeting `LEFT_EAR` is automatically duplicated to `RIGHT_EAR` at compile time
- A cue targeting `LEFT_VISOR` is duplicated to `RIGHT_VISOR` with column reversal applied
- A cue targeting `LEFT_CONNECTORS` is duplicated to `RIGHT_CONNECTORS`
- Cues targeting `BOTH_EARS`, `BOTH_VISORS`, `MATRIX`, or cross-zone IDs do not duplicate (they already cover both sides or are inherently centered)
- A cue with `flags.no_mirror: true` is NOT duplicated regardless of scene setting

The compiler outputs the duplicated cues in the binary. Scene author sees `LEFT_EAR` cues; binary contains both `LEFT_EAR` and `RIGHT_EAR` cues.

---

## 6. Sprite Resources

Optional. Sprites are small rectangular animations for the matrix, referenced by cues using `EFFECT_MATRIX_SPRITE`.

```json
"resources": {
  "sprites": [
    {
      "name": "spinning_globe",
      "width": 8,
      "height": 8,
      "fps": 15,
      "loop": true,
      "frames": [
        [ [r, g, b], [r, g, b], ... ],
        [ [r, g, b], [r, g, b], ... ],
        ...
      ]
    }
  ]
}
```

### 6.1 Sprite fields

| Field | Type | Required | Description |
|---|---|---|---|
| `name` | string | yes | Used to reference this sprite from cues. Max 31 chars. |
| `width` | integer | yes | Pixels wide (1–32). |
| `height` | integer | yes | Pixels tall (1–8). |
| `fps` | integer | yes | Authored frame rate. Cues can override at playback time. |
| `loop` | boolean | no | Default play behavior hint. Cues can override. |
| `frames` | array | yes | Array of frames. Each frame is a `width × height` array of `[r, g, b]` triplets. |

### 6.2 Frame format

A frame is an array of rows (top to bottom), where each row is an array of `[r, g, b]` pixels (left to right). Each color value is 0–255.

For an 8×8 sprite:
```json
"frames": [
  [
    [0,0,0], [50,50,100], [60,80,120], ..., [0,0,0]
  ],
  ...
]
```

Hand-authoring full frame data in JSON is tedious for non-trivial sprites. Two practical workflows:

- **Authoring from PNG/GIF:** the compiler accepts a sprite definition with `"frames_from_file": "earth_globe.gif"` instead of inline `frames`. The compiler reads the GIF/PNG sequence and produces the frame array.
- **Authoring from Python helper:** a future helper script generates the JSON frames array from any image format the user has.

For now, document both options in the schema; the compiler implements `frames_from_file` first.

### 6.3 Sprite size validation

- Total bytes across all sprites in a scene: ≤ 24 KB. Per-sprite formula: `width × height × frame_count × 3` bytes
- Each sprite individually: ≤ 24 KB (the per-scene total IS the per-sprite max in practice)
- Compile-time error if exceeded

### 6.4 Binary mapping

JSON sprites become entries in the binary scene's sprite library section (effects spec Section 8). Sprite IDs are assigned in array order (sprite 0 is the first in the JSON array, sprite 1 the second, etc.).

A cue's `params.sprite_id` is filled in by the compiler when it sees a sprite name reference:

```json
{
  "effect": "EFFECT_MATRIX_SPRITE",
  "params": { "sprite_name": "spinning_globe", "x_offset": 12, "y_offset": 0, ... }
}
```

The compiler resolves `"sprite_name": "spinning_globe"` to `"sprite_id": 0` in the binary. The JSON's named reference is just authoring convenience.

---

## 7. VU Sequence Resources

Optional. Pre-rendered VU meter sequences played back by `EFFECT_VISOR_VU_PRERENDERED` or `EFFECT_MATRIX_VU_PRERENDERED`.

```json
"resources": {
  "vu_sequences": [
    {
      "name": "main_vu_5band",
      "band_count": 5,
      "fps": 20,
      "data": [
        [120, 80, 200, 90, 40],
        [130, 90, 210, 100, 50],
        ...
      ]
    }
  ]
}
```

### 7.1 VU sequence fields

| Field | Type | Required | Description |
|---|---|---|---|
| `name` | string | yes | Reference name. |
| `band_count` | integer | yes | 5 for visor VU, 32 for matrix VU. |
| `fps` | integer | yes | Sample rate. 10–30 typical. |
| `data` | array | yes | Array of frames. Each frame is an array of `band_count` integers (0–255). |

### 7.2 VU duration

`duration_ms` of the sequence is computed from `data.length` and `fps`. The compiler emits this in the binary header.

### 7.3 VU size validation

Total VU data size per scene: ≤ 32 KB (warning at 24 KB, error at 32 KB).

- 5-band, 20 fps, 4-minute song: `5 × 20 × 240 = 24,000 bytes`. Acceptable.
- 32-band, 20 fps, 4-minute song: `32 × 20 × 240 = 153,600 bytes`. Rejected.
- 32-band, 10 fps, 60-second segment: `32 × 10 × 60 = 19,200 bytes`. Acceptable.

Matrix VU should cover only segments where it's active, not full song duration.

### 7.4 Authoring from audio

Hand-authoring VU data is impractical. Realistic workflow:

- Run librosa FFT analysis on the MP3 file
- Output a JSON file like `vu_main.json` matching the schema above
- Reference it from the scene JSON via `"vu_from_file": "vu_main.json"`

The compiler implements `vu_from_file` to read external JSON files relative to the scene JSON's directory.

### 7.5 Binary mapping

VU sequences are assigned IDs in array order. Cues reference them by name; compiler resolves to ID.

---

## 8. Text Resources

Optional. Text strings displayed by `EFFECT_MATRIX_TEXT`.

```json
"resources": {
  "text_strings": [
    "HARDER",
    "BETTER",
    "FASTER",
    "STRONGER",
    "DAFT PUNK"
  ]
}
```

### 8.1 Text fields

`text_strings` is a flat array of strings. Position in array determines the string's binary ID (string 0 = first, etc.).

### 8.2 Constraints

- Each string: max 63 characters
- Total characters across all strings: ≤ 1024
- ASCII only (chars 32–126)
- Total strings: ≤ 64

### 8.3 Glyph subset

The compiler computes the glyph subset automatically. It scans all `text_strings`, finds the unique set of characters, and emits a glyph table containing only those characters' bitmaps from a built-in font.

The font is a 4×6 pixel font compiled into the build tool. Authors don't pick the font (yet — a future enhancement could allow font selection).

### 8.4 Binary mapping

The `text_strings` array becomes the binary scene's text strings table. The glyph subset is computed and emitted as a separate binary section. Cues reference strings by ID; compiler resolves text-name references to IDs.

```json
{
  "effect": "EFFECT_MATRIX_TEXT",
  "params": {
    "text": "HARDER",
    "x_position": -8,
    "y_position": 1,
    "color": 0,
    "brightness": 200,
    "animation": "scroll_left_slow"
  }
}
```

The compiler resolves `"text": "HARDER"` to the string's ID and writes it as `params[0]`.

---

## 9. Zone Names

Cues reference zones by name. Recognized names (from effects spec Section 3):

| Name | Effects spec ID | Notes |
|---|---|---|
| `LEFT_EAR` | 0 | Mirrors to `RIGHT_EAR` if mirror_lr |
| `LEFT_VISOR` | 1 | Mirrors to `RIGHT_VISOR` if mirror_lr |
| `RIGHT_EAR` | 2 | Use only for explicitly asymmetric scenes |
| `RIGHT_VISOR` | 3 | Use only for explicitly asymmetric scenes |
| `MATRIX` | 4 | No mirroring |
| `BOTH_EARS` | 10 | Virtual zone — renders to both ears |
| `BOTH_VISORS` | 11 | Virtual zone — renders to both visors with col reversal on right |
| `LEFT_CONNECTORS` | 80 | Mirrors to `RIGHT_CONNECTORS` if mirror_lr |
| `RIGHT_CONNECTORS` | 81 | Use only for explicitly asymmetric scenes |

**Authoring guidance:** for mirrored scenes (the default), prefer `BOTH_EARS` / `BOTH_VISORS` / `LEFT_*` over explicit right-side targeting. Use `RIGHT_*` zones only when intentionally creating asymmetric moments.

---

## 10. The Effects Registry

The compiler needs a single source-of-truth file mapping effect/zone/palette names to IDs and parameter layouts. This file is called `registry.json` and lives alongside the compiler.

### 10.1 Registry structure

```json
{
  "schema_version": 1,
  "palettes": {
    "PAL_DISCOVERY": 0,
    "PAL_FIRE": 1,
    "PAL_OCEAN": 2,
    ...
  },
  "zones": {
    "LEFT_EAR": 0,
    "LEFT_VISOR": 1,
    ...
  },
  "effects": {
    "EFFECT_OFF": {
      "id": 0,
      "zones": ["*"],
      "params": []
    },
    "EFFECT_EAR_BEAT_GRADIENT": {
      "id": 3,
      "zones": ["LEFT_EAR", "RIGHT_EAR", "BOTH_EARS"],
      "params": [
        { "name": "decay_steepness", "type": "uint8" },
        { "name": "bottom_color", "type": "palette_index" },
        { "name": "bottom_brightness", "type": "uint8" },
        { "name": "top_brightness", "type": "uint8" },
        { "name": "downbeat_flash", "type": "uint8" },
        { "name": "subbeat_sparkle", "type": "uint8" }
      ]
    },
    ...
  }
}
```

### 10.2 Registry maintenance

The registry is the authoritative mapping. If you add a palette or effect to the firmware, you update the registry. The firmware should ideally generate `registry.json` from its own constants (e.g. a Python script that parses the C++ headers and emits the registry), but a hand-maintained registry is acceptable for v1.

### 10.3 Validation rules using the registry

When compiling a scene, the compiler checks every reference against the registry and reports specific errors for mismatches. This is the primary validation surface.

---

## 11. Compilation Validation

The compiler runs every check below before emitting the binary. Hard fails block compilation; warnings are logged and compilation continues.

### 11.1 Hard fails

- Invalid JSON syntax
- Missing required top-level fields
- `schema_version` unsupported
- `scene_name` length > 31 or begins with `__`
- `total_duration_ms` ≤ 0
- `beat_grid.period_ms` ≤ 0 or `beats_per_bar` not in 2–16
- `base_palette` or `base_effect` not in registry
- Empty `segments` object
- Empty `timeline` array
- Timeline entry references undefined segment
- Timeline entries not in chronological order
- Two timeline entries with identical `at_ms`
- Any segment definition missing required fields
- Segment name > 15 chars, contains non-ASCII, or begins with `__`
- Segment `nominal_duration_ms` ≤ 0
- Cue `at_ms` ≥ enclosing segment's `nominal_duration_ms`
- Cue `at_ms` < 0
- Cues within a segment not in chronological order
- Two cues at same `at_ms` targeting same zone
- Effect name not in registry
- Zone name not in registry
- Palette name not in registry
- Effect not compatible with target zone (per registry)
- Parameter name not in effect's registry definition
- Parameter value doesn't match declared type
- Sprite name referenced by cue but not defined in resources
- VU sequence name referenced by cue but not defined in resources
- Text string referenced by cue but not in `text_strings` array
- Sprite frame dimensions don't match declared `width` × `height`
- Sprite total size > 24 KB
- VU total size > 32 KB
- Text string > 63 chars
- Total text characters > 1024
- More than 64 strings in `text_strings`
- Parsed scene RAM estimate > 64 KB

### 11.2 Warnings

- Segment `nominal_duration_ms` < 5000 (very short segment)
- Segment `nominal_duration_ms` > 90000 (very long segment — drift within accumulates)
- Segment with no cues (just base effect for full duration — legal but flag)
- `total_duration_ms` differs from last timeline entry's natural end by > 5 seconds
- Cue with all-zero params (often a forgotten parameter init)
- VU data > 24 KB (approaching ceiling)
- `flags.no_mirror` set on a zone with no mirror partner

### 11.3 Compiler output

Successful compilation emits:
- `<song>.scene` — binary bundle, ready for SD card
- `<song>.scene.report.txt` — human-readable summary: scene size, RAM usage estimate, per-resource breakdown, warnings encountered

---

## 12. Compilation Mechanics

### 12.1 Invocation

```bash
python compile_scene.py mountain_king.scene.json
# outputs: mountain_king.scene + mountain_king.scene.report.txt
```

### 12.2 Compiler options

| Flag | Effect |
|---|---|
| `--registry path/to/registry.json` | Override default registry path |
| `--sort-cues` | Auto-sort cues within segments by `at_ms` (and warn) |
| `--strict` | Treat all warnings as errors |
| `--output path` | Specify output binary path |
| `--dump-binary` | Also emit a hex dump for debugging |

### 12.3 External file references

Some resources (sprite frames, VU data) are impractical to inline in JSON. The compiler supports:

- `"frames_from_file": "globe.gif"` in a sprite definition — compiler reads the GIF and produces frame data
- `"vu_from_file": "main_vu.json"` in a VU sequence — compiler reads the external JSON

Paths are resolved relative to the scene JSON file's directory.

---

# Part II: Worked Example

This section walks through a complete scene from JSON to binary, exercising every section of the schema. The scene is intentionally simple: a 60-second test song with three segments, a few cues across all five zones, one tiny sprite, and one text string.

## 13. Example Setup

We'll write a scene for a hypothetical 60-second track called `test_song.mp3` at 120 BPM, with this structure:

| Time | Section | Description |
|---|---|---|
| 0–10 s | intro | Slow, calm, gradient breathing on ears and visor, dark matrix |
| 10–40 s | groove | Beat-driven palette cycling, VU bars on matrix |
| 40–60 s | outro | Spinning globe sprite, "TEST" text, fade out |

Beat grid: 120 BPM = 500 ms per beat. First downbeat at 500 ms (after a half-beat lead-in).

## 14. Complete Scene JSON

```json
{
  "schema_version": 1,
  "scene_name": "test_song",
  "total_duration_ms": 60000,
  "beat_grid": {
    "first_downbeat_ms": 500,
    "period_ms": 500.0,
    "beats_per_bar": 4
  },
  "base_palette": "PAL_DISCOVERY",
  "base_effect": "EFFECT_OFF",
  "mirror_lr": true,

  "segments": {
    "intro": {
      "nominal_duration_ms": 10000,
      "base_effect": "EFFECT_OFF",
      "base_palette": "PAL_DISCOVERY",
      "cues": [
        {
          "at_ms": 0,
          "effect": "EFFECT_EAR_BREATHE",
          "zone": "BOTH_EARS",
          "palette": "PAL_INTRO_WARM",
          "params": {
            "bottom_color": 1,
            "top_color": 3,
            "breath_period_100ms": 40,
            "min_brightness": 30,
            "max_brightness": 120
          }
        },
        {
          "at_ms": 0,
          "effect": "EFFECT_VISOR_GRADIENT_V",
          "zone": "BOTH_VISORS",
          "palette": "PAL_INTRO_WARM",
          "params": {
            "bottom_color": 0,
            "top_color": 2,
            "bottom_brightness": 20,
            "top_brightness": 80
          }
        }
      ]
    },

    "groove": {
      "nominal_duration_ms": 30000,
      "base_effect": "EFFECT_OFF",
      "base_palette": "PAL_DISCOVERY",
      "cues": [
        {
          "at_ms": 0,
          "effect": "EFFECT_EAR_BEAT_GRADIENT",
          "zone": "BOTH_EARS",
          "palette": "PAL_DISCOVERY",
          "params": {
            "decay_steepness": 180,
            "bottom_color": 2,
            "bottom_brightness": 40,
            "top_brightness": 200,
            "downbeat_flash": 220,
            "subbeat_sparkle": 100
          }
        },
        {
          "at_ms": 0,
          "effect": "EFFECT_VISOR_VU_PRERENDERED",
          "zone": "BOTH_VISORS",
          "palette": "PAL_DISCOVERY",
          "params": {
            "vu_name": "groove_visor_vu",
            "bar_color_mode": 0,
            "single_color": 0,
            "bar_brightness": 180,
            "bg_fill_mode": 0,
            "bg_color": 0
          }
        },
        {
          "at_ms": 0,
          "effect": "EFFECT_MATRIX_VU_PRERENDERED",
          "zone": "MATRIX",
          "palette": "PAL_DISCOVERY",
          "params": {
            "vu_name": "groove_matrix_vu",
            "bar_color_mode": 0,
            "single_color": 0,
            "bar_brightness": 45,
            "bg_fill_mode": 0,
            "bg_color": 0
          }
        }
      ]
    },

    "outro": {
      "nominal_duration_ms": 20000,
      "base_effect": "EFFECT_OFF",
      "base_palette": "PAL_DISCOVERY",
      "is_outro": true,
      "cues": [
        {
          "at_ms": 0,
          "effect": "EFFECT_AMBIENT_BREATHE",
          "zone": "BOTH_VISORS",
          "palette": "PAL_OCEAN",
          "params": {
            "color": 2,
            "period_100ms": 30,
            "min_brightness": 20,
            "max_brightness": 100
          }
        },
        {
          "at_ms": 2000,
          "effect": "EFFECT_MATRIX_SPRITE",
          "zone": "MATRIX",
          "params": {
            "sprite_name": "tiny_globe",
            "x_offset": 12,
            "y_offset": 0,
            "loop_mode": 1,
            "brightness": 40,
            "fps_override": 0
          }
        },
        {
          "at_ms": 5000,
          "effect": "EFFECT_MATRIX_TEXT",
          "zone": "MATRIX",
          "params": {
            "text": "TEST",
            "x_position": 22,
            "y_position": 1,
            "color": 0,
            "brightness": 40,
            "animation": 0
          },
          "duration_ms": 15000
        }
      ]
    }
  },

  "timeline": [
    { "at_ms": 0,     "play": "intro"  },
    { "at_ms": 10000, "play": "groove" },
    { "at_ms": 40000, "play": "outro"  }
  ],

  "resources": {
    "sprites": [
      {
        "name": "tiny_globe",
        "width": 8,
        "height": 8,
        "fps": 12,
        "loop": true,
        "frames_from_file": "tiny_globe.gif"
      }
    ],
    "vu_sequences": [
      {
        "name": "groove_visor_vu",
        "band_count": 5,
        "fps": 20,
        "vu_from_file": "test_song_visor_vu.json"
      },
      {
        "name": "groove_matrix_vu",
        "band_count": 32,
        "fps": 15,
        "vu_from_file": "test_song_matrix_vu.json"
      }
    ],
    "text_strings": [
      "TEST"
    ]
  }
}
```

## 15. What the Compiler Does With This

Step by step, what `compile_scene.py test_song.scene.json` produces:

### 15.1 Parse and validate

1. Load the JSON.
2. Verify `schema_version: 1` is supported.
3. Look up `PAL_DISCOVERY`, `PAL_INTRO_WARM`, `PAL_OCEAN` → palette IDs 0, 8, 2 (from registry).
4. Look up `EFFECT_OFF`, `EFFECT_EAR_BREATHE`, `EFFECT_VISOR_GRADIENT_V`, `EFFECT_EAR_BEAT_GRADIENT`, `EFFECT_VISOR_VU_PRERENDERED`, `EFFECT_MATRIX_VU_PRERENDERED`, `EFFECT_AMBIENT_BREATHE`, `EFFECT_MATRIX_SPRITE`, `EFFECT_MATRIX_TEXT` → IDs 0, 4, 21, 3, 23, 41, 61, 42, 43.
5. Look up zone names → IDs 10, 11, 4 (and resolve `BOTH_EARS`, `BOTH_VISORS` virtual zones).
6. Validate every cue's parameters against the registry's parameter definitions.
7. Validate that sprite name `tiny_globe` appears in `resources.sprites`.
8. Validate VU sequence names match.
9. Validate text string `"TEST"` is in `text_strings`.

### 15.2 Resolve external files

1. Read `tiny_globe.gif` from the same directory as the scene JSON. Decode 12 frames. Verify dimensions are 8×8. Generate frame data: 8 × 8 × 12 × 3 = 2,304 bytes.
2. Read `test_song_visor_vu.json`. Verify it has 5 bands × (60 sec × 20 fps - whatever) entries. For the `groove` segment (30 sec): 5 × 20 × 30 = 3,000 bytes.
3. Read `test_song_matrix_vu.json`. 32 bands × 15 fps × 30 sec = 14,400 bytes.

### 15.3 Build segment library

Three unique segments: `intro`, `groove`, `outro`. Indexed 0, 1, 2.

### 15.4 Apply mirroring

Since `mirror_lr: true`:
- Cues targeting `BOTH_EARS` already cover both — no duplication needed
- Cues targeting `BOTH_VISORS` already cover both — no duplication needed
- Cues targeting `MATRIX` are inherently single-zone — no duplication

In this scene, no cues use `LEFT_*` or `RIGHT_*` zones explicitly, so mirroring is a no-op. (In a real scene that did use `LEFT_EAR` etc., the compiler would emit matching `RIGHT_EAR` cues.)

### 15.5 Build timeline

Three entries:
- `at_ms: 0`, segment_id: 0 (intro)
- `at_ms: 10000`, segment_id: 1 (groove)
- `at_ms: 40000`, segment_id: 2 (outro), IS_OUTRO flag set

### 15.6 Build string pool

Strings written: `"test_song"`, `"intro"`, `"groove"`, `"outro"`, `"tiny_globe"`, `"groove_visor_vu"`, `"groove_matrix_vu"`, `"TEST"`. Pool size ~100 bytes.

### 15.7 Compute glyph subset

Scan `text_strings: ["TEST"]` for unique characters: `T`, `E`, `S`. Three glyphs needed. Subset size: 4 (header) + 3 × 4 = 16 bytes.

### 15.8 Compute CRC32

CRC over everything except the trailer CRC field itself.

### 15.9 Emit binary

Final binary layout, approximately:

| Section | Size (bytes) |
|---|---|
| Header | 64 |
| Palette references | ~5 |
| Segment library (3 segments + cues) | ~200 |
| Timeline | 24 (3 entries × 8 bytes) |
| Sprite library (1 sprite + frames) | ~2,320 |
| VU sequences (2 sequences + data) | ~17,400 |
| Text strings table | ~10 |
| Glyph subset | 16 |
| String pool | ~100 |
| Trailer (CRC) | 4 |
| **Total** | **~20,150 bytes (~20 KB)** |

Well under the 64 KB ceiling. Most of the size is the matrix VU sequence (14.4 KB).

### 15.10 Emit report

The compiler writes `test_song.scene.report.txt`:

```
Compiled test_song.scene successfully.
  Scene: test_song
  Duration: 60.0 sec
  Segments: 3 (intro, groove, outro)
  Timeline entries: 3
  Total cues: 9
  Sprites: 1 (2,304 bytes)
  VU sequences: 2 (17,400 bytes)
  Text strings: 1 ("TEST")
  Glyph subset: 3 characters (T, E, S)
  Binary size: 20,150 bytes
  Estimated parsed RAM: 20.1 KB / 64 KB ceiling
  Warnings: 0
```

## 16. What This Worked Example Validates About the Schema

This example exercises:

- All five physical zones (LEFT_EAR + RIGHT_EAR via BOTH_EARS, LEFT_VISOR + RIGHT_VISOR via BOTH_VISORS, MATRIX)
- Both virtual zones (BOTH_EARS, BOTH_VISORS)
- Multiple palettes within one scene (DISCOVERY, INTRO_WARM, OCEAN)
- Mirroring policy at scene level
- Sprite resource with `frames_from_file`
- VU resources at two different band counts and fps
- Text resource with automatic glyph subset
- Cues with `duration_ms: 0` (run until next override) and explicit `duration_ms`
- Segment with `is_outro` flag
- Multiple cues at `at_ms: 0` within a segment targeting different zones (legal)
- Named parameters mapping to byte positions via registry
- Binary size well within budgets

It does not exercise: `flags.no_mirror`, `flags.additive`, explicit `LEFT_*`/`RIGHT_*` zone targeting, the connector LED overrides, or matrix scroll background effects. Those are valid but didn't fit the example's musical structure. The schema supports them; future scenes will exercise them.

---

# Part III: Next Steps After This Spec

This document defines **what the JSON looks like** and **what the compiler must do with it**. Three artifacts implement and exercise it:

1. **`registry.json`** — the effect/zone/palette name-to-ID mapping. Needs to be authored from the effects library spec. ~200 lines.
2. **`compile_scene.py`** — the Python compiler. Reads JSON + registry, emits binary `.scene`. ~500 lines.
3. **A first hand-authored test scene** — like the worked example above, but simpler. Used to verify the compiler produces a binary the (future) firmware can parse correctly.

These come in that order. The registry is mechanical work — populating tables from the effects spec. The compiler is the substantive work. The test scene is the smoke test.

After these three exist, the next step shifts to firmware: the Sparkle Motion needs to parse and render the binary the compiler produced. That's where we'll pick up.
