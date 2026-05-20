#!/usr/bin/env python3
"""
extract_earth_sprite.py
-----------------------
Extracts the earthFrames[][] sprite data from the Around the World Arduino
sketch (.ino) and converts it to the scene JSON sprite format used by the
Sparkle Motion effects engine.

Usage:
    python3 extract_earth_sprite.py <path/to/around_the_world_helmet.ino>

Output:
    earth_globe_sprite.json  —  a sprite resource object ready to paste into
                                a scene JSON's resources.sprites array.

Sprite dimensions: 8 × 8 pixels, 32 frames
Original frame rate: 12.5 fps (FRAME_MS=80ms). Encoded as 12 fps here
(83ms/frame), which is the closest safe integer. Use fps_override in the
scene cue to adjust playback speed without re-extracting.

Pixel values are stored at full 0-255 range per channel. The scene cue's
`brightness` param (max 45, the MATRIX_BRIGHTNESS_MAX hardware cap) scales
them uniformly at render time, preserving colour ratios across all 32 frames.

Authoring notes for the scene cue:
    "effect": "EFFECT_MATRIX_SPRITE",
    "params": {
        "sprite_name": "spinning_globe",
        "x_offset": 12,   -- cols 12-19 on the 32-wide matrix (centre-right)
        "y_offset": 0,
        "brightness": 40,  -- safe value under 45 cap; bump to 45 for max pop
        "loop_mode": 0,    -- 0 = loop indefinitely
        "fps_override": 0  -- 0 = use sprite's authored fps (12)
    }
"""

import sys
import re
import json
from pathlib import Path

EARTH_SIZE = 8    # sprite is 8×8
NUM_FRAMES = 32
FPS        = 12   # original is 12.5 fps (80 ms/frame); 12 is nearest safe int


def extract_frames(ino_text: str) -> list:
    """
    Parse earthFrames[NUM_FRAMES][EARTH_SIZE*EARTH_SIZE] from the .ino source.
    Returns a list of NUM_FRAMES frames, each a list of 64 ints (0xRRGGBB).
    """
    # Find the earthFrames block
    start_match = re.search(
        r'const\s+uint32_t\s+earthFrames\s*\[.*?\]\s*\[.*?\]\s*PROGMEM\s*=\s*\{',
        ino_text, re.DOTALL)
    if not start_match:
        raise ValueError("Could not find earthFrames[] in the .ino file.")

    # Grab everything after the opening brace, find the matching closing };
    body_start = start_match.end()
    depth = 1
    pos = body_start
    while pos < len(ino_text) and depth > 0:
        c = ino_text[pos]
        if   c == '{': depth += 1
        elif c == '}': depth -= 1
        pos += 1
    body = ino_text[body_start : pos - 1]  # content between outermost { }

    # Extract every hex literal in the block
    hex_values = [int(h, 16) for h in re.findall(r'0x[0-9A-Fa-f]+', body)]

    expected = NUM_FRAMES * EARTH_SIZE * EARTH_SIZE
    if len(hex_values) != expected:
        raise ValueError(
            f"Expected {expected} hex values, found {len(hex_values)}. "
            "Check that NUM_FRAMES=32 and EARTH_SIZE=8 still match the source.")

    # Split into per-frame flat pixel lists
    pixels_per_frame = EARTH_SIZE * EARTH_SIZE
    frames = []
    for f in range(NUM_FRAMES):
        flat = hex_values[f * pixels_per_frame : (f + 1) * pixels_per_frame]
        frames.append(flat)
    return frames


def flat_to_rows(flat: list) -> list:
    """
    Convert a flat list of 64 uint32_t (0xRRGGBB) into an 8×8 array of
    [r, g, b] triplets: rows[row][col] = [r, g, b].
    """
    rows = []
    for row in range(EARTH_SIZE):
        row_pixels = []
        for col in range(EARTH_SIZE):
            pixel = flat[row * EARTH_SIZE + col]
            r = (pixel >> 16) & 0xFF
            g = (pixel >>  8) & 0xFF
            b = (pixel      ) & 0xFF
            row_pixels.append([r, g, b])
        rows.append(row_pixels)
    return rows


def build_sprite(frames_flat: list) -> dict:
    """Build the sprite resource dict for the scene JSON."""
    return {
        "name":   "spinning_globe",
        "width":  EARTH_SIZE,
        "height": EARTH_SIZE,
        "fps":    FPS,
        "frames": [flat_to_rows(f) for f in frames_flat]
    }


def main():
    if len(sys.argv) < 2:
        print(f"Usage: python3 {sys.argv[0]} <around_the_world_helmet.ino>",
              file=sys.stderr)
        sys.exit(1)

    ino_path = Path(sys.argv[1])
    if not ino_path.exists():
        print(f"Error: file not found: {ino_path}", file=sys.stderr)
        sys.exit(1)

    ino_text = ino_path.read_text(encoding='utf-8', errors='replace')
    frames_flat = extract_frames(ino_text)
    sprite = build_sprite(frames_flat)

    out_path = Path("earth_globe_sprite.json")
    with open(out_path, 'w') as f:
        json.dump(sprite, f, indent=2)

    print(f"Extracted {NUM_FRAMES} frames ({EARTH_SIZE}x{EARTH_SIZE} px each).")
    print(f"Written to: {out_path}")
    print()
    print("Paste the contents of earth_globe_sprite.json into your scene's")
    print('resources.sprites array, then reference it in a cue as:')
    print()
    print('  "effect": "EFFECT_MATRIX_SPRITE",')
    print('  "params": {')
    print('    "sprite_name": "spinning_globe",')
    print('    "x_offset": 12,')
    print('    "y_offset": 0,')
    print('    "brightness": 40,')
    print('    "loop_mode": 0,')
    print('    "fps_override": 0')
    print('  }')


if __name__ == '__main__':
    main()
