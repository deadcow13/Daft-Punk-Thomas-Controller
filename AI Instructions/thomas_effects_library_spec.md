# Thomas Helmet Effects Library Specification

**Project:** Synchronized Music + Light Show — Thomas helmet
**Hardware target:** Adafruit Sparkle Motion (classic ESP32) driving 394 LEDs across 3 data pins
**Companion document:** `sd_card_animation_architecture.md` (defines the `.scene` binary bundle format and ESP-NOW transfer protocol)
**Scope:** the effects engine running on Thomas's Sparkle Motion, the zone addressing model, the per-effect parameter layouts, and the firmware constants that shape rendering.
**Out of scope:** the controller-side authoring workflow, JSON-to-binary build script details, individual scene files.

---

## 1. Overview

The effects engine renders each frame by:

1. Determining the current cue (or cues) active at the current `local_playback_clock_ms`.
2. For each active cue, invoking its effect function against a target zone with the cue's parameters.
3. Applying engine-wide post-processing (gamma correction, per-zone brightness ceilings, per-pin current caps).
4. Calling `FastLED.show()`.

Cues are **layered, not exclusive**: a single moment in time can have one cue rendering the visor edge while another renders the matrix. Cues addressing the same zone in the same moment are resolved by ordering — later cues in the segment override earlier ones for overlapping LEDs.

The Thomas helmet uses **Model B zone addressing**: the 394 LEDs are partitioned into named zones, and every cue declares which zone it targets. Effects know how to render within their zone's logical coordinate system; the engine maps logical coordinates to physical LED indices.

---

## 2. Hardware Constants (firmware-enforced)

These are baked into the firmware as `constexpr` constants and applied automatically by the effects engine. Scene authors work in logical 0–255 brightness space and trust the engine to handle enforcement.

| Constant | Value | Purpose |
|---|---|---|
| `POWER_CAP_GLOBAL_MA` | 4500 | Total current ceiling across all three pins. Revisable when supply is upgraded. |
| `POWER_CAP_PIN_MA` | 2820 | Per-pin current cap. Protects JST XH connectors rated at 3 A. Permanent. |
| `MATRIX_BRIGHTNESS_MAX` | 45 | Maximum brightness on the matrix zone (out of 255). Permanent. Manufacturer guidance: "do not exceed 10–20%." |
| `EAR_VISOR_BRIGHTNESS_MAX` | 185 | Maximum brightness on ear and visor edge zones. Tunable per build. |
| `NUM_LEDS_TOTAL` | 394 | Sanity constant. |
| `FRAME_INTERVAL_MS` | 16 | Target frame interval (~60 fps). |

### 2.1 Pre-show enforcement pipeline

Before every `FastLED.show()`:

1. Engine clamps each zone's pixel buffer to that zone's brightness ceiling.
2. Engine applies gamma correction (Section 6.1) to each pixel.
3. Engine projects each pin's current draw (sum of channel values × per-LED current coefficient).
4. If any pin's projected current exceeds `POWER_CAP_PIN_MA`, that pin's buffer is scaled proportionally before show.
5. FastLED's global `setMaxPowerInVoltsAndMilliamps(5, POWER_CAP_GLOBAL_MA)` provides a final backstop.

This means a scene authoring full-white peaks will be silently dimmed during those peaks. By design — protects hardware.

---

## 3. Zone Map

Thomas has **5 logical zones** corresponding to physical regions of the helmet. Cues address zones by ID.

| ID | Name | Pin | LED range | Count | Coordinate model |
|---|---|---|---|---|---|
| 0 | `LEFT_EAR` | 19 | 0–15 | 16 | Diamond zones (`BOT_PEAK`, `LOWER`, `FLAT`, `UPPER`, `TOP_PEAK`) |
| 1 | `LEFT_VISOR` | 19 | 16–68 | 53 | 5×8 grid + beat-flash trio + 10 connector LEDs |
| 2 | `RIGHT_EAR` | 21 | 0–15 | 16 | Diamond zones (mirrored or independent per scene) |
| 3 | `RIGHT_VISOR` | 21 | 16–68 | 53 | 5×8 grid (mirrored col mapping) + beat-flash trio + 10 connector LEDs |
| 4 | `MATRIX` | 22 | 0–255 | 256 | 32×8 matrix, vertical serpentine |

Two **virtual zones** exist as authoring conveniences and are expanded by the engine at parse time:

| ID | Name | Expands to | Purpose |
|---|---|---|---|
| 10 | `BOTH_EARS` | `LEFT_EAR` + `RIGHT_EAR` | Mirror an ear effect across both ears in one cue |
| 11 | `BOTH_VISORS` | `LEFT_VISOR` + `RIGHT_VISOR` | Mirror a visor effect across both visor edges |

### 3.1 Mirroring

A scene's header declares a default mirroring policy:

- `mirror_lr: true` — cues targeting `LEFT_EAR` are automatically duplicated to `RIGHT_EAR`, cues targeting `LEFT_VISOR` are duplicated to `RIGHT_VISOR` with column reversal applied. Default for most scenes.
- `mirror_lr: false` — left and right are addressed independently. Author must explicitly write cues for both sides.

A per-cue `no_mirror` flag overrides the scene default for that cue only. This lets a scene be 95% mirrored with a few intentional asymmetric moments.

When `RIGHT_VISOR` content comes from a mirrored `LEFT_VISOR` cue, the engine applies the column reversal formula from `thomas_right_visor_edge_context.json`: `col_right = (NUM_COLS - 1) - col_left`. Hue stays tied to the band (frequency), not the column, so VU bars look correct on both sides.

### 3.2 Ear coordinate model

The diamond ear has 5 named zones based on the EKG-style layout:

| Zone | LED indices | Count | Role |
|---|---|---|---|
| `BOT_PEAK` | 0 | 1 | Bottom spike |
| `LOWER` | 1–3 | 3 | Lower roll |
| `FLAT` | 4–11 | 8 | Flat baseline |
| `UPPER` | 12–14 | 3 | Upper roll |
| `TOP_PEAK` | 15 | 1 | Top spike |

Effects targeting ear zones address these named regions. The `verticalGradient(bottom_color, top_color)` primitive from `SparkleMotion_SongSketch.ino` is the foundational ear rendering function.

### 3.3 Visor edge coordinate model

The visor edge has three sub-regions:

| Sub-region | LED count | Address by |
|---|---|---|
| `GRID` | 40 | `(col, row)` where col is 1–5, row is 0–7 |
| `BEAT_FLASH` | 3 | Treated as a single 3-LED unit (U24, U25, U26) |
| `CONNECTORS` | 10 | Treated as a single ring (U6, U17, U18, U27–29, U50–53) |

Grid `(col, row)` to chain index is resolved via the lookup tables in `thomas_left_visor_edge_context.json`. The engine ships this table as a constant.

For the right visor edge, the chain layout is identical but the col-to-band mapping is reversed (`col_right = 6 - col_left`) so that bass-frequency content appears on the inner column of both visors regardless of helmet side.

### 3.4 Matrix coordinate model

The matrix is addressed as `(x, y)` where x is 0–31 (column) and y is 0–7 (row).

Vertical serpentine wiring: physical chain index is computed as
- For even x: `idx = x * 8 + y`
- For odd x: `idx = x * 8 + (7 - y)`

Origin `(0, 0)` is the top-left from the wearer's perspective. The HBFS sketch's `XY(x, y)` function defines this mapping; the engine ships an equivalent.

### 3.5 Connector LEDs (left and right visor)

Each visor has 13 connector LEDs split into two roles:

- **Beat flash trio** (U24, U25, U26): a 3-LED horizontal bar between rows 4 and 5
- **Rainbow ring** (10 LEDs in vertical columns on the outer side of each visor)

Default behavior when no cue addresses them:

| LEDs | Default effect |
|---|---|
| Beat flash trio | Gold flash on bass beat (CHSV(42, 255, brightness), decay 0.75) |
| Rainbow ring | Slow rainbow cycle (CHSV(rainbowHue + i × 25, 255, 200)) |

These defaults run automatically when no cue is overriding them, giving the visor a baseline liveness even in unauthored gaps. Scene cues can explicitly address `BEAT_FLASH` or `CONNECTORS` to override the defaults for specific moments.

---

## 4. The Cue Type, Revisited for Thomas

The base `cue_t` from the architecture doc is 16 bytes. Thomas's effects engine reads it as:

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `time_ms_in_segment` | When this cue fires within its segment |
| 4 | 1 | `effect_id` | Effect to invoke (see Section 5) |
| 5 | 1 | `zone_id` | Target zone (see Section 3) |
| 6 | 1 | `palette_id` | Palette to bind for this cue |
| 7 | 1 | `flags` | Bit 0: `no_mirror`. Bit 1: `additive` (layer on top of base effect instead of replacing). Bits 2–7 reserved. |
| 8 | 6 | `params[6]` | Effect-specific parameters |
| 14 | 2 | `duration_ms` | How long this cue stays active (0 = until next cue overrides) |

Note: `params[6]` is unchanged from the architecture doc, but the meaning of each byte depends on `effect_id`. Each effect documents its own parameter layout below.

---

## 5. Effects

Effects are grouped by which zones they support. The engine's effect dispatch table maps `effect_id` → function pointer. Effects targeting incompatible zones are rejected at scene validation time.

### 5.0 Special: `EFFECT_OFF`

| Effect ID | Name | Zones |
|---|---|---|
| 0 | `EFFECT_OFF` | All |

All LEDs in the target zone go to black. No parameters used. Used to silence a zone in a scene that's otherwise busy elsewhere.

---

### 5.1 Ear effects (target `LEFT_EAR`, `RIGHT_EAR`, or `BOTH_EARS`)

#### `EFFECT_EAR_SOLID` (ID 1)

Fill the entire ear with one color from the palette.

| Param byte | Meaning |
|---|---|
| `params[0]` | Palette color index (0–15) |
| `params[1]` | Brightness scale (0–255) applied to the palette color |
| `params[2]–[5]` | Reserved (set to 0) |

#### `EFFECT_EAR_GRADIENT` (ID 2)

`verticalGradient(bottom, top)` — bottom color at `BOT_PEAK`, blends through `LOWER`, `FLAT`, `UPPER`, to top color at `TOP_PEAK`.

| Param byte | Meaning |
|---|---|
| `params[0]` | Palette color index for bottom |
| `params[1]` | Palette color index for top |
| `params[2]` | Bottom brightness scale (0–255) |
| `params[3]` | Top brightness scale (0–255) |
| `params[4]–[5]` | Reserved |

#### `EFFECT_EAR_BEAT_GRADIENT` (ID 3)

The Crescendolls-style render. Vertical gradient with the top color drawn from a beat-cycled palette, with a brightness envelope that decays each beat. Optional downbeat flash on `TOP_PEAK`.

| Param byte | Meaning |
|---|---|
| `params[0]` | Decay envelope steepness (0–255, where 0 = no decay, 255 = sharp decay over one beat) |
| `params[1]` | Bottom color palette index |
| `params[2]` | Bottom brightness scale (0–255) |
| `params[3]` | Top brightness scale (0–255) |
| `params[4]` | Downbeat flash brightness on `TOP_PEAK` (0 = disabled, 1–255 = peak flash brightness) |
| `params[5]` | Sub-beat sparkle brightness on `BOT_PEAK` (0 = disabled, 1–255) |

Reads the global beat grid from the scene header for timing.

#### `EFFECT_EAR_BREATHE` (ID 4)

Slow breathing brightness over a held gradient. The Aerodynamic intro.

| Param byte | Meaning |
|---|---|
| `params[0]` | Bottom palette color |
| `params[1]` | Top palette color |
| `params[2]` | Breath period in 100ms units (e.g., 40 = 4 seconds per breath) |
| `params[3]` | Min brightness (0–255) at trough of breath |
| `params[4]` | Max brightness (0–255) at peak of breath |
| `params[5]` | Reserved |

#### `EFFECT_EAR_EKG_PULSE` (ID 5)

A bright pulse travels through the diamond from `BOT_PEAK` → `LOWER` → `FLAT` → `UPPER` → `TOP_PEAK` and back, like an EKG waveform. Anchored to the beat grid: one full traversal per beat (or per N beats).

| Param byte | Meaning |
|---|---|
| `params[0]` | Palette color for the pulse |
| `params[1]` | Beats per traversal (1 = one pulse per beat, 2 = one per half-note, 4 = one per measure) |
| `params[2]` | Pulse width in LEDs (1–6) |
| `params[3]` | Tail decay (0–255) |
| `params[4]` | Base brightness (0–255) for non-pulse LEDs |
| `params[5]` | Reserved |

---

### 5.2 Visor edge effects (target `LEFT_VISOR`, `RIGHT_VISOR`, or `BOTH_VISORS`)

These effects render to the 5×8 grid sub-region. The connector LEDs are rendered separately by their default effects unless a cue explicitly targets `BEAT_FLASH` or `CONNECTORS` (see Section 5.5).

#### `EFFECT_VISOR_SOLID` (ID 20)

Fill all 40 grid LEDs with one palette color.

| Param byte | Meaning |
|---|---|
| `params[0]` | Palette color index |
| `params[1]` | Brightness scale (0–255) |
| `params[2]–[5]` | Reserved |

#### `EFFECT_VISOR_GRADIENT_V` (ID 21)

Vertical gradient: row 0 (bottom) is `params[0]` color, row 7 (top) is `params[1]` color, intermediate rows blend.

| Param byte | Meaning |
|---|---|
| `params[0]` | Bottom row palette color |
| `params[1]` | Top row palette color |
| `params[2]` | Bottom brightness scale (0–255) |
| `params[3]` | Top brightness scale (0–255) |
| `params[4]–[5]` | Reserved |

#### `EFFECT_VISOR_GRADIENT_H` (ID 22)

Horizontal gradient: col 1 (inner edge) is `params[0]`, col 5 (outer edge) is `params[1]`, intermediate cols blend. When mirroring to right visor, the col mapping reverses, which preserves the "inner edge = inner edge" semantic.

Same param layout as `EFFECT_VISOR_GRADIENT_V`.

#### `EFFECT_VISOR_VU_PRERENDERED` (ID 23)

Plays a fully pre-rendered VU meter sequence from the scene's VU data section (see Section 7). Each band's height over time is sampled at the scene's authored framerate and the engine renders 5 vertical bars accordingly.

| Param byte | Meaning |
|---|---|
| `params[0]` | VU sequence ID (0–15, indexes into scene's VU data table) |
| `params[1]` | Bar color mode: 0 = per-band hue from scene palette, 1 = single color, 2 = rainbow per col |
| `params[2]` | Single color palette index (used when mode=1) |
| `params[3]` | Bar brightness scale (0–255) |
| `params[4]` | Background fill: 0 = black, 1 = palette index in `params[5]` |
| `params[5]` | Background palette color (used when `params[4]` = 1) |
| | |

#### `EFFECT_VISOR_BEAT_WIPE` (ID 24)

A color washes across the grid in sync with the beat. Direction is parameter-controlled. Picture: every beat, a new column lights up bright then fades; the wave moves left-to-right or bottom-to-top.

| Param byte | Meaning |
|---|---|
| `params[0]` | Direction: 0 = bottom-to-top, 1 = top-to-bottom, 2 = inner-to-outer, 3 = outer-to-inner |
| `params[1]` | Beats per full wipe (1, 2, 4, etc.) |
| `params[2]` | Palette color for the wave |
| `params[3]` | Wave width in rows/cols (1–4) |
| `params[4]` | Wave brightness (0–255) |
| `params[5]` | Background brightness (0–255) for the non-wave area |

#### `EFFECT_VISOR_HALO_ROTATE` (ID 25)

A bright spot rotates around the perimeter of the 5×8 grid over a configurable period. Used for slow background motion that maintains a sense of life without escalating.

| Param byte | Meaning |
|---|---|
| `params[0]` | Halo palette color |
| `params[1]` | Floor palette color (dim base color across whole grid) |
| `params[2]` | Halo brightness (0–255) |
| `params[3]` | Floor brightness (0–255) |
| `params[4]` | Rotation period in beats (e.g., 16 = one rotation per 4 bars) |
| `params[5]` | Halo width in perimeter LEDs (3–8) |

#### `EFFECT_VISOR_SPARKLE` (ID 26)

Sparse twinkle effect. Random LEDs in the grid briefly light up, decay quickly.

| Param byte | Meaning |
|---|---|
| `params[0]` | Sparkle palette color |
| `params[1]` | Density (0–255: how many LEDs sparkle per second on average) |
| `params[2]` | Decay rate (0–255: how quickly each sparkle fades) |
| `params[3]` | Background palette color |
| `params[4]` | Background brightness (0–255) |
| `params[5]` | Sparkle peak brightness (0–255) |

Uses deterministic hash-based randomness so the same scene time always produces the same sparkle pattern (reproducible).

---

### 5.3 Matrix effects (target `MATRIX` only)

The matrix is Thomas's "main display" zone. Its effects are richer and include sprite playback and text rendering.

#### `EFFECT_MATRIX_SOLID` (ID 40)

Fill the entire matrix with one palette color.

| Param byte | Meaning |
|---|---|
| `params[0]` | Palette color index |
| `params[1]` | Brightness scale (0–255) |
| `params[2]–[5]` | Reserved |

#### `EFFECT_MATRIX_VU_PRERENDERED` (ID 41)

Pre-rendered VU meter on the matrix. Renders as 32 columns of 8-LED-tall bars (one bar per matrix column). The pre-rendered VU data must contain 32 bands (not 5).

| Param byte | Meaning |
|---|---|
| `params[0]` | VU sequence ID (0–15) |
| `params[1]` | Bar color mode: 0 = rainbow per col, 1 = single palette color, 2 = frequency-mapped hue |
| `params[2]` | Single color palette index (used when mode=1) |
| `params[3]` | Bar brightness scale (0–255) — clamped to `MATRIX_BRIGHTNESS_MAX` |
| `params[4]` | Background fill: 0 = black, 1 = palette index in `params[5]` |
| `params[5]` | Background palette color |

#### `EFFECT_MATRIX_SPRITE` (ID 42)

Play a sprite from the scene's sprite library (see Section 8) at a fixed position on the matrix. Sprite plays once or loops.

| Param byte | Meaning |
|---|---|
| `params[0]` | Sprite ID (0–15, indexes into scene's sprite library) |
| `params[1]` | X offset (0–31) |
| `params[2]` | Y offset (0–7) |
| `params[3]` | Loop mode: 0 = play once and hold last frame, 1 = loop, 2 = play once and clear, 3 = ping-pong |
| `params[4]` | Brightness scale (0–255), clamped to `MATRIX_BRIGHTNESS_MAX` |
| `params[5]` | Frame rate override: 0 = use sprite's authored fps, else fps value |

The sprite renders on top of whatever else the matrix is showing (via the `additive` cue flag) or replaces a rectangular region of the matrix (default behavior).

#### `EFFECT_MATRIX_TEXT` (ID 43)

Display text from the scene's text library on the matrix. The text engine uses the glyph subset loaded with the scene (Section 9).

| Param byte | Meaning |
|---|---|
| `params[0]` | Text string ID (0–63, indexes into scene's text strings table) |
| `params[1]` | X position (signed 8-bit, -128 to 127) — negative values start text off-screen-left |
| `params[2]` | Y position (0–7) |
| `params[3]` | Text palette color |
| `params[4]` | Brightness (0–255), clamped to `MATRIX_BRIGHTNESS_MAX` |
| `params[5]` | Animation mode: 0 = static, 1 = scroll left at 8 px/sec, 2 = scroll left at 16 px/sec, 3 = blink at beat |

For scrolling, the engine animates the X offset over the cue's `duration_ms`. The cue's duration controls how long the text remains in motion.

#### `EFFECT_MATRIX_SCROLL_BG` (ID 44)

A background pattern that scrolls under text or sprites. Useful as a base layer.

| Param byte | Meaning |
|---|---|
| `params[0]` | Pattern type: 0 = horizontal stripes, 1 = vertical stripes, 2 = diagonal, 3 = checkerboard |
| `params[1]` | Palette color A |
| `params[2]` | Palette color B |
| `params[3]` | Scroll speed in pixels per second |
| `params[4]` | Brightness (0–255), clamped to `MATRIX_BRIGHTNESS_MAX` |
| `params[5]` | Pattern period in pixels (e.g., 4 = stripes 4 LEDs wide) |

---

### 5.4 Cross-zone effects (target multiple zones simultaneously)

These effects use special zone IDs that span multiple physical zones.

#### `EFFECT_FULL_FLASH` (ID 60)

Bright flash across all five physical zones at once. Used sparingly for downbeat punctuation. Subject to all brightness/current caps.

| Param byte | Meaning |
|---|---|
| `params[0]` | Palette color |
| `params[1]` | Peak brightness (0–255) |
| `params[2]` | Decay time in 10ms units (e.g., 20 = 200ms decay) |
| `params[3]` | Include matrix: 0 = ears/visors only, 1 = include matrix |
| `params[4]–[5]` | Reserved |

#### `EFFECT_AMBIENT_BREATHE` (ID 61)

Slow synchronized breathing across all ear and visor zones. Used in quiet scene segments. Matrix is not affected.

| Param byte | Meaning |
|---|---|
| `params[0]` | Palette color |
| `params[1]` | Period in 100ms units |
| `params[2]` | Min brightness |
| `params[3]` | Max brightness |
| `params[4]–[5]` | Reserved |

---

### 5.5 Connector LED effects (target `LEFT_CONNECTORS` ID 80, `RIGHT_CONNECTORS` ID 81)

By default, connector LEDs run their built-in behaviors (gold beat flash on the trio, rainbow cycle on the ring). Scenes can override either ring with explicit cues.

#### `EFFECT_CONN_SOLID` (ID 70)

Override all 13 connector LEDs on one side with a single color.

| Param byte | Meaning |
|---|---|
| `params[0]` | Palette color |
| `params[1]` | Brightness (0–255) |
| `params[2]–[5]` | Reserved |

#### `EFFECT_CONN_BEAT_FLASH_OVERRIDE` (ID 71)

Override the default gold beat flash with a custom color/timing.

| Param byte | Meaning |
|---|---|
| `params[0]` | Flash palette color |
| `params[1]` | Flash brightness (0–255) |
| `params[2]` | Beats between flashes (1 = every beat, 2 = every other, 4 = every measure) |
| `params[3]` | Decay rate (0–255) |
| `params[4]` | Apply to: 0 = beat flash trio only, 1 = full connector set |
| `params[5]` | Reserved |

#### `EFFECT_CONN_RAINBOW_OVERRIDE` (ID 72)

Adjust the default rainbow cycle (speed, palette, brightness) without disabling it.

| Param byte | Meaning |
|---|---|
| `params[0]` | Cycle speed in 0.1 Hz units (e.g., 5 = 0.5 Hz, full cycle every 2 sec) |
| `params[1]` | Brightness (0–255) |
| `params[2]` | Hue step per LED (0–255, default ~25) |
| `params[3]` | Saturation (0–255) |
| `params[4]–[5]` | Reserved |

---

## 6. Engine Services Available to Effects

Effects render against a global context the engine provides. This is what makes data-driven effects expressive without per-song custom code.

### 6.1 Global context fields

```
struct GlobalContext {
  uint32_t now_ms;              // local playback clock
  uint32_t cue_start_ms;        // when the current cue started rendering
  uint32_t segment_start_ms;    // when the current segment started

  float    beat_phase;          // 0..1 within current beat (from scene beat grid)
  uint32_t beat_index;          // total beat count since song start
  uint8_t  beat_in_bar;         // 0..3, position within 4-beat bar

  uint32_t segment_idx;         // which segment in the timeline
  uint8_t  segment_lib_idx;     // which library entry the current segment references

  const CRGBPalette16* palette; // active palette (set by cue's palette_id)

  bool     mirror_active;       // true if this cue is the mirrored copy on right side
};
```

### 6.2 Palette table

The engine ships 16 hardcoded palettes in firmware. Scene cues reference them by ID. Each palette is 16 colors deep, indexable from 0–255 with smooth interpolation via FastLED's `ColorFromPalette`.

| Palette ID | Name | Description |
|---|---|---|
| 0 | `PAL_DISCOVERY` | Daft Punk / Interstella 5555 inspired: gold, cyan, magenta, green, orange, purple. Workhorse for most scenes. |
| 1 | `PAL_FIRE` | Black → red → orange → yellow → white. For energy peaks. |
| 2 | `PAL_OCEAN` | Black → deep blue → cyan → light blue → white. For calm passages. |
| 3 | `PAL_FOREST` | Black → dark green → green → yellow-green → cream. Earthy. |
| 4 | `PAL_NEON` | Hot pink, cyan, lime, yellow, purple. High-saturation pop. |
| 5 | `PAL_GOLD_WARM` | Helmet gold variations: deep gold, bright gold, amber, cream. |
| 6 | `PAL_COOL_SWEEP` | Cyan-to-violet sweep used in Aerodynamic groove sections. |
| 7 | `PAL_RED_BREAKDOWN` | Deep red, bright red, orange-red, with black gaps. Aerodynamic breakdown. |
| 8 | `PAL_INTRO_WARM` | Warm amber/orange used for Aerodynamic intro breath. |
| 9 | `PAL_MONO_WHITE` | Grayscale: black → gray → white. Useful as a brightness palette. |
| 10 | `PAL_MAGENTA_CYAN` | Just magenta and cyan, hard transitions. Crescendolls beat colors. |
| 11–15 | Reserved | Future palettes; scenes referencing 11–15 fail validation until defined. |

Gamma correction (Section 6.4) is applied after palette lookup, so palette RGB values are in "linear ideal space."

### 6.3 Beat grid

Scene bundles declare a beat grid in their header:

```
beat_first_downbeat_ms : uint32_t   // when beat 1 of bar 1 lands
beat_period_ms_x1000   : uint32_t   // beat period × 1000 for sub-ms precision
beats_per_bar          : uint8_t    // typically 4
```

The engine exposes `beat_phase`, `beat_index`, and `beat_in_bar` on every frame, computed from `local_playback_clock_ms`. Effects use these for tempo-locked rendering without authoring per-beat cues.

### 6.4 Gamma + white-point correction

Engine applies gamma correction (gamma 2.6 LUT) and white-point balance (per-channel multipliers) as a final pre-show stage. Current defaults:

- Red multiplier: 1.00
- Green multiplier: 1.10
- Blue multiplier: 0.65
- Gamma curve: 2.6

These are firmware constants and can be retuned per build. Scenes do not see or interact with this layer.

### 6.5 Deterministic randomness

For sparkle/twinkle effects, the engine provides `engine_hash(uint32_t seed) → uint32_t`, a deterministic hash function. Effects requesting "random" pixels at time `t` should hash `(led_index, t / time_quantum)` so the same `t` produces the same output (reproducibility).

---

## 7. Pre-Rendered VU Data Format

A scene can include up to 16 pre-rendered VU sequences. Each sequence is a 2D array of band intensities sampled at the scene's authored framerate.

### 7.1 VU sequence header (12 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `name_offset` (into string pool) |
| 2 | 1 | `band_count` (5 for visor VU, 32 for matrix VU) |
| 3 | 1 | `fps` (10–60 typical; 20 recommended) |
| 4 | 4 | `duration_ms` |
| 8 | 4 | `data_offset` (byte offset to sample data) |

### 7.2 Sample data

Packed as `(band_count × frame_count)` bytes, row-major by frame: frame 0 bands 0..N, frame 1 bands 0..N, etc. Each byte is band intensity 0–255.

### 7.3 Playback

When a cue invokes `EFFECT_VISOR_VU_PRERENDERED` or `EFFECT_MATRIX_VU_PRERENDERED`, the engine reads the appropriate frame based on `(now_ms - cue_start_ms) × fps / 1000`. If playback runs past the sequence's `duration_ms`, the last frame is held.

### 7.4 Size budget

A 5-band, 20 fps, 4-minute VU sequence is `5 × 20 × 240 = 24,000` bytes ≈ 24 KB. A 32-band, 20 fps, 4-minute matrix VU sequence is `32 × 20 × 240 = 153,600` bytes ≈ 150 KB.

**The 150 KB matrix VU sequence exceeds the scene RAM ceiling.** Authoring guidance: matrix VU sequences should either run at lower fps (10 fps = ~75 KB) or cover only the segments where the VU is active, not the whole song. The scene compiler must check this during validation.

---

## 8. Sprite Library Format

A scene can include up to 16 sprites. Each sprite is a region of the matrix animated by frame.

### 8.1 Sprite header (16 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `name_offset` |
| 2 | 1 | `width_px` (1–32) |
| 3 | 1 | `height_px` (1–8) |
| 4 | 1 | `frame_count` |
| 5 | 1 | `fps` |
| 6 | 4 | `frame_data_offset` |
| 10 | 4 | `frame_data_size_bytes` |
| 14 | 2 | Reserved |

### 8.2 Frame data

Tightly packed RGB triplets, row-major within each frame, frame-major within the sequence:
- Frame 0: pixel (0,0) R,G,B; (1,0) R,G,B; ...; (w-1,h-1) R,G,B
- Frame 1: same layout
- ...

Each pixel: 3 bytes. Frame size: `width × height × 3`. Total size: `width × height × frame_count × 3`.

### 8.3 Size budget

| Sprite | Size |
|---|---|
| 8×8, 30 frames | 5,760 bytes |
| 16×8, 30 frames | 11,520 bytes |
| 32×8, 30 frames | 23,040 bytes |
| 8×8, 60 frames | 11,520 bytes |

**Hard ceiling per scene's total sprite data: 24 KB.** Validation rejects scenes exceeding this. Authors should prefer small region sprites (8×8 or 16×8) and reuse them across cues rather than authoring large unique sprites.

### 8.4 Color correction

Sprite frame data is stored in "ideal RGB" (uncorrected). The engine applies gamma + white-point correction during rendering, same as palette colors. Authors design sprites against the linear color space.

---

## 9. Text Glyph Subset Format

A scene can include up to 64 text strings and a glyph subset covering only the characters those strings use.

### 9.1 Glyph subset table

Header:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `glyph_count` |
| 1 | 1 | `glyph_width` (typically 4) |
| 2 | 1 | `glyph_height` (typically 6) |
| 3 | 1 | Reserved |

Followed by `glyph_count` entries:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | ASCII character code |
| 1 | N | Glyph bitmap, packed (width × height) / 8 bytes |

For a 4×6 font, each glyph is `(4 × 6) / 8 = 3` bytes packed. So each entry is 4 bytes total. For a typical Daft Punk song with 20 unique characters, the table is 80 bytes.

### 9.2 Text strings table

Header:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `string_count` (0–64) |
| 1 | N | Sequence of `(2 bytes length, length bytes ASCII)` |

Strings reference characters that must exist in the glyph subset. Validation rejects scenes whose strings contain characters not in the subset.

### 9.3 Rendering

`EFFECT_MATRIX_TEXT` renders one string at a time. The engine looks up each character in the glyph subset, draws the bitmap at the cue's X/Y position, advances X by `glyph_width + 1` for spacing. Word spacing handled by the space character (which has an entry in the subset with an all-zeros bitmap if visual space is needed).

### 9.4 Size budget

A typical scene with 20 unique characters + 30 strings averaging 8 characters each:
- Glyph subset: `4 + 20 × 4 = 84` bytes
- Strings: `1 + 30 × (2 + 8) = 301` bytes
- **Total: ~400 bytes**

Tiny.

---

## 10. Scene RAM Budget Recap

The architecture doc's 20 KB scene ceiling is too tight for Thomas's matrix-heavy scenes. Updated budget:

| Component | Typical | Max |
|---|---|---|
| Header + segment library + timeline + cues | 4–6 KB | 8 KB |
| Palette references | < 1 KB | < 1 KB |
| Text strings + glyph subset | < 1 KB | < 1 KB |
| VU sequences (5-band visor: 24 KB; 32-band matrix at 10 fps: 75 KB) | 0–24 KB | 24 KB practical (visor only) |
| Sprite library | 0–24 KB | 24 KB |
| String pool | < 1 KB | < 1 KB |
| Padding/alignment | < 1 KB | < 1 KB |
| **Total** | **5–55 KB** | **~60 KB** |

**Updated scene RAM ceiling: 64 KB.** Scenes exceeding this fail validation. Cache size: 2 scenes LRU (down from 3 in the architecture doc, to fit in available SRAM with 64 KB scenes).

**Authors should use sprite + matrix VU sparingly.** Most scenes will land at 10–20 KB total. Scenes using both a sprite library and matrix VU at full song length will push toward the ceiling.

---

## 11. Ambient Mode

When the watchdog fires or no controller is paired, the Sparkle Motion runs `__ambient`. Architecture:

- I²S microphone on GPIO 33 reads room audio
- 5-band FFT runs at 30 Hz, drives visor VU bars (using the `EFFECT_VISOR_VU_PRERENDERED` path but with real-time band values instead of pre-rendered data)
- Ear effects: slow audio-reactive breathing using overall RMS
- Matrix: cycles through a small set of baked-in attract-mode animations (timer-driven, ~20 sec each):
  - Slow scrolling "DAFT PUNK" text
  - Discovery palette gradient sweep
  - Spinning record sprite
  - Mic-driven full-matrix VU bars
- Connector LEDs run their defaults

Ambient mode does not use the scene format. It's pure firmware, defined as C constants and rendering functions.

---

## 12. Demo Mode

When a `.scene` file is missing for a played MP3, the Sparkle Motion runs `__demo`. Architecture:

- ~3 minute fixed sequence demonstrating every effect ID in the library
- Each effect gets ~6 seconds with descriptive matrix text identifying the effect by name
- Loops cleanly (cut to frame 0) if music outlasts the demo
- Used as both a no-scene fallback and a smoke test after firmware flashes

Defined as C constants in firmware, not as a `.scene` bundle.

---

## 13. Validation Rules Specific to This Library

In addition to the architecture doc's validation rules, the scene compiler must check:

- Every `effect_id` exists in the dispatch table for the addressed `zone_id` (e.g., `EFFECT_MATRIX_SPRITE` on `LEFT_EAR` is invalid)
- All palette IDs are 0–10 (palettes 11–15 reserved)
- All sprite IDs referenced by `EFFECT_MATRIX_SPRITE` cues exist in the scene's sprite library
- All text string IDs referenced by `EFFECT_MATRIX_TEXT` cues exist in the scene's text table
- All characters in scene text strings exist in the glyph subset
- All VU sequence IDs referenced by VU effects exist in the scene's VU data
- Total sprite data size ≤ 24 KB
- Matrix VU sequences either ≤ 32 bands or duration ≤ ~30 sec at 32 bands (size check)
- Parsed in-RAM total ≤ 64 KB
- For any cue with `no_mirror` flag set, the zone is one that has a mirror counterpart (no_mirror on `MATRIX` is a warning — matrix isn't mirrored)
- Connector override cues (`EFFECT_CONN_*`) must specify whether they replace the default or augment it (via the `additive` flag)

---

## 14. Implementation Checklist (firmware)

For an AI agent or developer implementing this engine on the Sparkle Motion:

### Core dispatch

1. Effect dispatch table mapping `effect_id` → function pointer
2. Zone resolver mapping `zone_id` → `(pin, led_range, coordinate_system)`
3. Cue scheduler: per-frame, determine active cue per zone, invoke effect

### Effect implementations

4. All ear effects (Section 5.1) — 5 implementations
5. All visor effects (Section 5.2) — 7 implementations
6. All matrix effects (Section 5.3) — 5 implementations
7. Cross-zone effects (Section 5.4) — 2 implementations
8. Connector effects + defaults (Section 5.5) — 3 implementations + 2 defaults

### Engine services

9. Global context maintenance (beat phase, segment tracking, palette binding)
10. Palette table (16 hardcoded, 10 defined)
11. Gamma + white-point correction LUT and final-stage application
12. Per-pin current projection and clamping
13. Per-zone brightness ceiling enforcement
14. Deterministic hash function for sparkle randomness

### Resource loaders

15. Sprite library parser + frame interpolator
16. VU sequence parser + frame interpolator
17. Glyph subset parser + text rendering
18. String pool with offset-based lookup

### Modes

19. Ambient mode renderer (mic FFT, attract-mode timer cycling)
20. Demo mode renderer (fixed sequence, loop on overrun)

### Built-in resources

21. 11 firmware palettes
22. Ambient mode baked-in matrix animations
23. Demo mode fixed sequence

---

## 15. Authoring Notes (for whoever writes scenes)

These are not engine concerns but worth recording for the authoring workflow:

- **Sprites and matrix VU are expensive.** Use them where they matter, default to text + procedural effects elsewhere.
- **Mirror by default.** Authoring asymmetric scenes is more work than authoring mirrored scenes that occasionally break symmetry for an effect.
- **Beat-anchored effects scale better than per-cue placement.** A 32-bar chorus can be one cue invoking `EFFECT_EAR_BEAT_GRADIENT` for its whole duration, not 128 cues at every beat.
- **Connector LED defaults are good enough for most cues.** Override them only when the scene wants a connector-specific moment.
- **The matrix cycling-within-song behavior (Flavor A) means each matrix animation is an explicit cue.** No timer-based rotation during songs — every matrix moment is intentional.

---

## 16. Locked Decisions Summary

For quick reference, the architectural decisions baked into this spec:

- **Zone model:** 5 physical + 2 virtual zones
- **Coordinate models:** diamond zones (ears), `(col, row)` grid + sub-regions (visor), `(x, y)` serpentine (matrix)
- **Mirroring:** scene-level default flag + per-cue `no_mirror` override
- **Power caps:** 4500 mA global, 2820 mA per pin
- **Brightness caps:** 45 matrix, 185 ear/visor (firmware-enforced)
- **Gamma:** engine-wide final stage, 2.6 curve, RGB multipliers (1.00, 1.10, 0.65) default
- **Cue size:** 16 bytes (unchanged from architecture doc)
- **Scene RAM ceiling:** 64 KB (revised up from 20 KB to accommodate sprites/VU)
- **Cache:** 2-scene LRU
- **Palettes:** 11 defined, 5 reserved
- **VU mode:** pre-rendered in scene during songs, mic-driven in ambient mode
- **Text mode:** pre-rendered glyph subset per scene
- **Sprite ceiling:** 24 KB per scene
- **Matrix cycling:** Flavor A (cue-driven) within songs, Flavor B (timer) in ambient
- **Connector LEDs:** addressable with defaults running when not overridden
