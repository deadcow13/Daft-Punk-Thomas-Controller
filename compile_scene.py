#!/usr/bin/env python3
"""
compile_scene.py — JSON-to-binary scene compiler for the Thomas helmet light show.

Usage:
    python compile_scene.py <scene.scene.json> [options]

Options:
    --registry <path>   Path to registry.json (default: AI Instructions/registry.json)
    --sort-cues         Auto-sort cues within each segment by at_ms (warns)
    --strict            Treat all warnings as hard errors
    --output <path>     Output .scene path (default: same dir/name as input)
    --dump-binary       Also write a .scene.hex hex dump alongside the .scene file

Output:
    <name>.scene                Binary scene bundle ready for the SD card
    <name>.scene.report.txt     Human-readable compilation report

Binary format note:
    The 26-byte reserved_2 block in the header (bytes 38-63) is used for beat grid
    and extended section offsets, which are not defined in the original architecture doc:
        bytes 38-41  uint32  beat_first_downbeat_ms
        bytes 42-45  uint32  beat_period_ms * 1000 (for sub-ms precision)
        byte  46     uint8   beats_per_bar
        byte  47     uint8   reserved
        bytes 48-49  uint16  text_lib_offset  (0 = no text section)
        bytes 50-51  uint16  vu_lib_offset    (0 = no VU section)
        bytes 52-53  uint16  sprite_lib_offset (0 = no sprite section)
        bytes 54-63           reserved (zero)
"""

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

# ─── Format constants ─────────────────────────────────────────────────────────

MAGIC = b'SCNE'
FORMAT_VERSION = 1
HEADER_SIZE = 64
SEG_HDR_SIZE = 12   # segment header
CUE_SIZE = 16       # cue record
TL_ENTRY_SIZE = 8   # timeline entry
GLYPH_W = 4
GLYPH_H = 6
GLYPH_BYTES = 3     # ceil(4*6/8) = 3 bytes per glyph bitmap

# ─── 4×6 bitmap font ─────────────────────────────────────────────────────────
# Each entry: tuple of 6 nibbles (one per row top-to-bottom, MSB = leftmost pixel).
# Packed: byte0 = (row0<<4)|row1, byte1 = (row2<<4)|row3, byte2 = (row4<<4)|row5.

_FONT = {
    ' ': ( 0, 0, 0, 0, 0, 0),   '!': ( 4, 4, 4, 0, 4, 0),
    '"': (10,10, 0, 0, 0, 0),   '#': ( 5,15, 5,15, 5, 0),
    '$': ( 6,14, 4, 7, 6, 0),   '%': ( 9, 2, 4, 9, 0, 0),
    '&': ( 6, 9, 6,10, 7, 0),   "'": ( 4, 4, 0, 0, 0, 0),
    '(': ( 2, 4, 4, 4, 2, 0),   ')': ( 4, 2, 2, 2, 4, 0),
    '*': ( 0, 5, 2, 5, 0, 0),   '+': ( 0, 2, 7, 2, 0, 0),
    ',': ( 0, 0, 0, 3, 2, 0),   '-': ( 0, 0, 7, 0, 0, 0),
    '.': ( 0, 0, 0, 0, 4, 0),   '/': ( 0, 1, 2, 4, 8, 0),
    '0': ( 6, 9, 9, 9, 9, 6),   '1': ( 4,12, 4, 4,14, 0),
    '2': ( 6, 9, 3, 6, 8,15),   '3': (14, 1, 6, 1,14, 0),
    '4': ( 9, 9,15, 1, 1, 0),   '5': (15, 8,14, 1,14, 0),
    '6': ( 6, 8,14, 9, 6, 0),   '7': (15, 1, 2, 4, 4, 0),
    '8': ( 6, 9, 6, 9, 6, 0),   '9': ( 6, 9, 7, 1, 6, 0),
    ':': ( 0, 6, 0, 6, 0, 0),   ';': ( 0, 4, 0, 4, 8, 0),
    '<': ( 2, 4, 8, 4, 2, 0),   '=': ( 0,15, 0,15, 0, 0),
    '>': ( 8, 4, 2, 4, 8, 0),   '?': ( 6, 9, 2, 0, 2, 0),
    '@': ( 6, 9,11,10, 6, 0),
    'A': ( 6, 9,15, 9, 9, 0),   'B': (14, 9,14, 9,14, 0),
    'C': ( 6, 9, 8, 9, 6, 0),   'D': (14, 9, 9, 9,14, 0),
    'E': (15, 8,14, 8,15, 0),   'F': (15, 8,14, 8, 8, 0),
    'G': ( 6, 9, 8,11, 7, 0),   'H': ( 9, 9,15, 9, 9, 0),
    'I': (15, 4, 4, 4,15, 0),   'J': ( 7, 1, 1, 9, 6, 0),
    'K': ( 9,10,12,10, 9, 0),   'L': ( 8, 8, 8, 8,15, 0),
    'M': ( 9,15,15, 9, 9, 0),   'N': ( 9,13,11, 9, 9, 0),
    'O': ( 6, 9, 9, 9, 6, 0),   'P': (14, 9,14, 8, 8, 0),
    'Q': ( 6, 9, 9,10, 7, 0),   'R': (14, 9,14,10, 9, 0),
    'S': ( 7, 8, 6, 1,14, 0),   'T': (15, 6, 6, 6, 6, 0),
    'U': ( 9, 9, 9, 9, 6, 0),   'V': ( 9, 9, 9, 6, 2, 0),
    'W': ( 9, 9,15, 6, 9, 0),   'X': ( 9, 6, 6, 6, 9, 0),
    'Y': ( 9, 9, 6, 6, 6, 0),   'Z': (15, 2, 4, 8,15, 0),
    '[': ( 6, 4, 4, 4, 6, 0),   '\\':( 0, 8, 4, 2, 1, 0),
    ']': ( 6, 2, 2, 2, 6, 0),   '^': ( 4,10, 0, 0, 0, 0),
    '_': ( 0, 0, 0, 0,15, 0),   '`': ( 8, 4, 0, 0, 0, 0),
    'a': ( 0, 6, 7, 9, 7, 0),   'b': ( 8, 8,14, 9,14, 0),
    'c': ( 0, 6, 8, 9, 6, 0),   'd': ( 1, 1, 7, 9, 7, 0),
    'e': ( 0, 6,11, 8, 6, 0),   'f': ( 2, 4, 7, 4, 4, 0),
    'g': ( 0, 7, 9, 7, 1, 6),   'h': ( 8, 8,14, 9, 9, 0),
    'i': ( 4, 0, 4, 4, 6, 0),   'j': ( 2, 0, 2, 2,10, 4),
    'k': ( 8, 9,14,10, 9, 0),   'l': (12, 4, 4, 4, 6, 0),
    'm': ( 0, 0,15,11, 9, 0),   'n': ( 0, 0,14, 9, 9, 0),
    'o': ( 0, 6, 9, 9, 6, 0),   'p': ( 0,14, 9,14, 8, 8),
    'q': ( 0, 7, 9, 7, 1, 1),   'r': ( 0, 0,11,12, 8, 0),
    's': ( 0, 7, 8, 6, 7, 0),   't': ( 4,14, 4, 4, 3, 0),
    'u': ( 0, 9, 9, 9, 7, 0),   'v': ( 0, 9, 9, 6, 4, 0),
    'w': ( 0, 9, 9,15, 6, 0),   'x': ( 0, 9, 6, 6, 9, 0),
    'y': ( 0, 9, 9, 7, 1, 6),   'z': ( 0,15, 2, 4,15, 0),
    '{': ( 2, 4,12, 4, 2, 0),   '|': ( 4, 4, 4, 4, 4, 0),
    '}': ( 8, 4, 3, 4, 8, 0),   '~': ( 0,10, 5, 0, 0, 0),
}

def _pack_glyph(ch):
    """Return 3 bytes of packed 4×6 bitmap for ch (falls back to space)."""
    r = _FONT.get(ch, _FONT[' '])
    return bytes([(r[0] << 4) | r[1], (r[2] << 4) | r[3], (r[4] << 4) | r[5]])


# ─── Error ────────────────────────────────────────────────────────────────────

class CompileError(Exception):
    pass


# ─── Registry helpers ─────────────────────────────────────────────────────────

def load_registry(path):
    with open(path) as fh:
        return json.load(fh)

def _reg_palette_id(reg, name):
    entry = reg['palettes'].get(name)
    if entry is None:
        candidates = [k for k in reg['palettes'] if name.upper() in k.upper()]
        hint = f" Did you mean: {candidates[0]}?" if candidates else ""
        raise CompileError(f"Unknown palette '{name}'.{hint}")
    return entry['id']

def _reg_zone_id(reg, name):
    entry = reg['zones'].get(name)
    if entry is None:
        candidates = [k for k in reg['zones'] if name.upper() in k.upper()]
        hint = f" Did you mean: {candidates[0]}?" if candidates else ""
        raise CompileError(f"Unknown zone '{name}'.{hint}")
    return entry['id']

def _reg_effect_id(reg, name):
    entry = reg['effects'].get(name)
    if entry is None:
        candidates = [k for k in reg['effects'] if name.upper() in k.upper()]
        hint = f" Did you mean: {candidates[0]}?" if candidates else ""
        raise CompileError(f"Unknown effect '{name}'.{hint}")
    return entry['id']

def _reg_effect_zones(reg, name):
    return reg['effects'][name]['zones']

def _reg_effect_params(reg, name):
    return reg['effects'][name].get('params', [])


# ─── Parameter encoding ───────────────────────────────────────────────────────

def encode_params(effect_name, params_json, reg, resources):
    """Encode a named-params dict to 6 bytes per the registry definition."""
    param_defs = _reg_effect_params(reg, effect_name)
    result = bytearray(6)
    used = set(params_json.keys())

    for i, pdef in enumerate(param_defs):
        if i >= 6:
            break
        name  = pdef['name']
        ptype = pdef['type']
        used.discard(name)
        raw = params_json.get(name, 0)

        if ptype in ('uint8', 'palette_index'):
            if not isinstance(raw, int) or not (0 <= raw <= 255):
                raise CompileError(
                    f"  {effect_name}.{name}: expected uint8 (0–255), got {raw!r}")
            result[i] = raw

        elif ptype == 'int8':
            if not isinstance(raw, int) or not (-128 <= raw <= 127):
                raise CompileError(
                    f"  {effect_name}.{name}: expected int8 (-128–127), got {raw!r}")
            result[i] = raw & 0xFF

        elif ptype == 'bool':
            result[i] = 1 if raw else 0

        elif ptype == 'enum':
            values = pdef['values']
            if isinstance(raw, int):
                if raw not in values.values():
                    raise CompileError(
                        f"  {effect_name}.{name}: int {raw} not a valid enum value {list(values.values())}")
                result[i] = raw
            elif isinstance(raw, str):
                if raw not in values:
                    close = [k for k in values if raw.lower() in k.lower()]
                    hint = f" Did you mean '{close[0]}'?" if close else f" Valid: {list(values.keys())}"
                    raise CompileError(
                        f"  {effect_name}.{name}: unknown enum value '{raw}'.{hint}")
                result[i] = values[raw]
            else:
                raise CompileError(
                    f"  {effect_name}.{name}: enum param must be string or int, got {raw!r}")

        elif ptype == 'string':
            result[i] = _resolve_string_param(effect_name, name, raw, resources)

    # Unknown param names → hard error with suggestions
    if used:
        valid = [p['name'] for p in param_defs]
        msgs = []
        for bad in sorted(used):
            close = [v for v in valid if bad in v or v in bad
                     or bad.replace('_', '') == v.replace('_', '')]
            hint = f" (did you mean '{close[0]}'?)" if close else ''
            msgs.append(f"'{bad}'{hint}")
        raise CompileError(
            f"  {effect_name}: unrecognised param(s): {', '.join(msgs)}\n"
            f"  Valid params for this effect: {valid}")

    return bytes(result)


def _resolve_string_param(effect_name, param_name, value, resources):
    if not isinstance(value, str):
        raise CompileError(
            f"  {effect_name}.{param_name}: must be a resource name string, got {value!r}")

    if param_name == 'text':
        lst = resources.get('text_strings', [])
        if value not in lst:
            raise CompileError(
                f"  {effect_name}: text string '{value}' not in resources.text_strings")
        return lst.index(value)

    elif param_name == 'vu_name':
        names = [s['name'] for s in resources.get('vu_sequences', [])]
        if value not in names:
            raise CompileError(
                f"  {effect_name}: vu_name '{value}' not in resources.vu_sequences")
        return names.index(value)

    elif param_name == 'sprite_name':
        names = [s['name'] for s in resources.get('sprites', [])]
        if value not in names:
            raise CompileError(
                f"  {effect_name}: sprite_name '{value}' not in resources.sprites")
        return names.index(value)

    raise CompileError(f"  {effect_name}: unhandled string param '{param_name}'")


# ─── Mirror expansion ─────────────────────────────────────────────────────────

_MIRROR = {
    'LEFT_EAR':        'RIGHT_EAR',
    'LEFT_VISOR':      'RIGHT_VISOR',
    'LEFT_CONNECTORS': 'RIGHT_CONNECTORS',
}

def expand_mirrors(cues, scene_mirror_lr):
    """Return cue list with mirror duplicates appended where applicable."""
    out = []
    for cue in cues:
        out.append(cue)
        if not scene_mirror_lr:
            continue
        if cue.get('flags', {}).get('no_mirror', False):
            continue
        target = _MIRROR.get(cue.get('zone', ''))
        if target:
            mirror = dict(cue)
            mirror['zone'] = target
            mirror['flags'] = {**cue.get('flags', {}), 'no_mirror': True}
            out.append(mirror)
    return out


# ─── Resource section builders ────────────────────────────────────────────────

def build_text_section(resources):
    """Glyph subset header + glyph entries + strings table. Empty bytes if no text."""
    strings = resources.get('text_strings', [])
    if not strings:
        return b''

    unique_chars = sorted(set(''.join(strings)))

    # Glyph subset: header(4) + count×(1 char byte + 3 bitmap bytes)
    glyph_data = bytearray()
    glyph_data += struct.pack('<BBBB', len(unique_chars), GLYPH_W, GLYPH_H, 0)
    for ch in unique_chars:
        glyph_data += struct.pack('<B', ord(ch))
        glyph_data += _pack_glyph(ch)

    # Strings table: count(1) + each string as uint16-len + bytes
    str_data = bytearray()
    str_data += struct.pack('<B', len(strings))
    for s in strings:
        enc = s.encode('ascii')
        str_data += struct.pack('<H', len(enc)) + enc

    return bytes(glyph_data) + bytes(str_data)


def build_vu_section(resources, scene_dir):
    """VU library section. Empty bytes if no VU sequences."""
    seqs = resources.get('vu_sequences', [])
    if not seqs:
        return b''

    name_pool   = bytearray()
    name_offsets = {}
    headers     = bytearray()
    data_blob   = bytearray()

    for seq in seqs:
        if 'vu_from_file' in seq:
            vu_path = Path(scene_dir) / seq['vu_from_file']
            try:
                with open(vu_path) as fh:
                    ext = json.load(fh)
            except FileNotFoundError:
                raise CompileError(f"vu_from_file not found: {vu_path}")
            seq = {**seq, **ext}

        name       = seq['name']
        band_count = seq['band_count']
        fps        = seq['fps']
        frames     = seq.get('data', [])
        if not frames:
            raise CompileError(f"VU sequence '{name}': no data (vu_from_file not used)")

        duration_ms = int(len(frames) * 1000 / fps)

        sample = bytearray()
        for fi, frame in enumerate(frames):
            if len(frame) != band_count:
                raise CompileError(
                    f"VU '{name}' frame {fi}: expected {band_count} bands, got {len(frame)}")
            for v in frame:
                sample.append(max(0, min(255, int(v))))

        if name not in name_offsets:
            name_offsets[name] = len(name_pool)
            name_pool += name.encode('ascii') + b'\x00'

        # VU header: name_offset(u16), band_count(u8), fps(u8),
        #            duration_ms(u32), data_offset(u32)
        headers += struct.pack('<HBBII',
            name_offsets[name], band_count, fps, duration_ms, len(data_blob))
        data_blob += sample

    section = bytearray()
    section += struct.pack('<BH', len(seqs), len(name_pool))
    section += name_pool + headers + data_blob
    return bytes(section)


def build_sprite_section(resources):
    """Sprite library section. Empty bytes if no sprites."""
    sprites = resources.get('sprites', [])
    if not sprites:
        return b''

    name_pool    = bytearray()
    name_offsets = {}
    headers      = bytearray()
    frame_blob   = bytearray()

    for spr in sprites:
        if 'frames_from_file' in spr:
            raise CompileError(
                f"Sprite '{spr.get('name', '?')}': frames_from_file is not yet implemented. "
                f"Provide inline 'frames' data or implement GIF loading.")

        name        = spr['name']
        width       = spr['width']
        height      = spr['height']
        fps         = spr['fps']
        frames      = spr.get('frames', [])
        frame_count = len(frames)

        if not frames:
            raise CompileError(f"Sprite '{name}': no frames")
        if frame_count > 255:
            raise CompileError(f"Sprite '{name}': {frame_count} frames > max 255")

        pixels = bytearray()
        for fi, frame in enumerate(frames):
            if len(frame) != height:
                raise CompileError(
                    f"Sprite '{name}' frame {fi}: expected {height} rows, got {len(frame)}")
            for ri, row in enumerate(frame):
                if len(row) != width:
                    raise CompileError(
                        f"Sprite '{name}' frame {fi} row {ri}: expected {width} pixels")
                for px in row:
                    if len(px) != 3:
                        raise CompileError(f"Sprite '{name}': pixels must be [r,g,b]")
                    pixels += struct.pack('<BBB', px[0] & 0xFF, px[1] & 0xFF, px[2] & 0xFF)

        if name not in name_offsets:
            name_offsets[name] = len(name_pool)
            name_pool += name.encode('ascii') + b'\x00'

        frame_data_offset = len(frame_blob)
        # Sprite header: name_offset(u16), w(u8), h(u8), frames(u8), fps(u8),
        #                frame_data_offset(u32), frame_data_size(u32), reserved(u16)
        headers += struct.pack('<HBBBBIIh',
            name_offsets[name], width, height, frame_count, fps,
            frame_data_offset, len(pixels), 0)
        frame_blob += pixels

    section = bytearray()
    section += struct.pack('<BH', len(sprites), len(name_pool))
    section += name_pool + headers + frame_blob
    return bytes(section)


# ─── String pool ──────────────────────────────────────────────────────────────

class StringPool:
    def __init__(self):
        self._data   = bytearray()
        self._offset = {}

    def intern(self, s):
        if s not in self._offset:
            self._offset[s] = len(self._data)
            self._data += s.encode('ascii') + b'\x00'
        return self._offset[s]

    def data(self):
        return bytes(self._data)


# ─── Validation ───────────────────────────────────────────────────────────────

def validate(scene, reg, scene_dir, sort_cues=False, strict=False):
    """
    Full pre-compile validation. Returns list of warning strings.
    Raises CompileError on hard failures.
    """
    warnings = []

    def warn(msg):
        warnings.append(msg)
        if strict:
            raise CompileError(f"[strict] {msg}")

    # Top-level required fields
    required = ('schema_version', 'scene_name', 'total_duration_ms',
                'beat_grid', 'base_palette', 'base_effect', 'segments', 'timeline')
    for f in required:
        if f not in scene:
            raise CompileError(f"Missing required top-level field: '{f}'")

    if scene['schema_version'] != 1:
        raise CompileError(f"Unsupported schema_version: {scene['schema_version']}")

    name = scene['scene_name']
    if len(name) > 31:
        raise CompileError(f"scene_name too long ({len(name)} > 31): '{name}'")
    if name.startswith('__'):
        raise CompileError(f"scene_name must not begin with '__': '{name}'")

    if scene['total_duration_ms'] <= 0:
        raise CompileError("total_duration_ms must be > 0")

    # Beat grid
    bg = scene['beat_grid']
    for f in ('first_downbeat_ms', 'period_ms', 'beats_per_bar'):
        if f not in bg:
            raise CompileError(f"beat_grid missing field '{f}'")
    if bg['period_ms'] <= 0:
        raise CompileError("beat_grid.period_ms must be > 0")
    if not (2 <= int(bg['beats_per_bar']) <= 16):
        raise CompileError(f"beat_grid.beats_per_bar must be 2–16, got {bg['beats_per_bar']}")

    # Base palette / effect
    try:
        _reg_palette_id(reg, scene['base_palette'])
    except CompileError as e:
        raise CompileError(f"base_palette: {e}")
    try:
        _reg_effect_id(reg, scene['base_effect'])
    except CompileError as e:
        raise CompileError(f"base_effect: {e}")

    # Segments
    segments = scene['segments']
    if not segments:
        raise CompileError("'segments' must not be empty")

    resources = scene.get('resources', {})

    for seg_name, seg in segments.items():
        if len(seg_name) > 15:
            raise CompileError(f"Segment '{seg_name}': name too long ({len(seg_name)} > 15)")
        if seg_name.startswith('__'):
            raise CompileError(f"Segment '{seg_name}': name must not begin with '__'")
        if not seg_name.isascii():
            raise CompileError(f"Segment '{seg_name}': name must be ASCII")

        for f in ('nominal_duration_ms', 'base_effect', 'base_palette', 'cues'):
            if f not in seg:
                raise CompileError(f"Segment '{seg_name}': missing field '{f}'")

        if seg['nominal_duration_ms'] <= 0:
            raise CompileError(f"Segment '{seg_name}': nominal_duration_ms must be > 0")
        if seg['nominal_duration_ms'] < 5000:
            warn(f"Segment '{seg_name}': very short ({seg['nominal_duration_ms']} ms)")
        if seg['nominal_duration_ms'] > 90000:
            warn(f"Segment '{seg_name}': very long ({seg['nominal_duration_ms']} ms) — sync drift accumulates mid-segment")

        try:
            _reg_effect_id(reg, seg['base_effect'])
        except CompileError as e:
            raise CompileError(f"Segment '{seg_name}' base_effect: {e}")
        try:
            _reg_palette_id(reg, seg['base_palette'])
        except CompileError as e:
            raise CompileError(f"Segment '{seg_name}' base_palette: {e}")

        cues = seg['cues']
        if not cues:
            warn(f"Segment '{seg_name}': no cues (only base effect runs for full duration)")

        # Sort if requested
        if sort_cues and len(cues) > 1:
            original_order = [c['at_ms'] for c in cues]
            sorted_cues = sorted(cues, key=lambda c: c['at_ms'])
            if [c['at_ms'] for c in sorted_cues] != original_order:
                warn(f"Segment '{seg_name}': cues were out of order — auto-sorted (--sort-cues)")
                seg['cues'] = sorted_cues
                cues = sorted_cues

        # Chronological order check
        for i in range(1, len(cues)):
            if cues[i]['at_ms'] < cues[i - 1]['at_ms']:
                raise CompileError(
                    f"Segment '{seg_name}' cue[{i}]: at_ms {cues[i]['at_ms']} < "
                    f"previous {cues[i-1]['at_ms']}. Use --sort-cues to auto-fix.")

        # Duplicate zone at same timestamp
        seen = {}
        for i, cue in enumerate(cues):
            key = (cue['at_ms'], cue.get('zone', ''))
            if key in seen:
                raise CompileError(
                    f"Segment '{seg_name}': two cues at at_ms={cue['at_ms']} targeting "
                    f"zone '{cue.get('zone')}' (cue[{seen[key]}] and cue[{i}])")
            seen[key] = i

        for i, cue in enumerate(cues):
            _validate_cue(cue, i, seg_name, seg, reg, resources, warn)

    # Timeline
    timeline = scene['timeline']
    if not timeline:
        raise CompileError("'timeline' must not be empty")

    for i in range(1, len(timeline)):
        if timeline[i]['at_ms'] <= timeline[i - 1]['at_ms']:
            raise CompileError(
                f"timeline[{i}]: at_ms {timeline[i]['at_ms']} not strictly after "
                f"previous {timeline[i-1]['at_ms']}")

    seen_times = set()
    for entry in timeline:
        if entry['at_ms'] in seen_times:
            raise CompileError(f"timeline: duplicate at_ms {entry['at_ms']}")
        seen_times.add(entry['at_ms'])
        if entry['play'] not in segments:
            raise CompileError(f"timeline references undefined segment '{entry['play']}'")

    # Duration sanity
    last = timeline[-1]
    natural_end = last['at_ms'] + segments[last['play']]['nominal_duration_ms']
    delta = abs(natural_end - scene['total_duration_ms'])
    if delta > 5000:
        warn(f"total_duration_ms ({scene['total_duration_ms']}) differs from last segment's "
             f"natural end ({natural_end}) by {delta} ms")

    # Resources
    _validate_resources(resources, warn)

    # Segment count limits
    if len(segments) > 64:
        raise CompileError(f"Too many unique segments ({len(segments)} > 64)")
    if len(timeline) > 256:
        raise CompileError(f"Too many timeline entries ({len(timeline)} > 256)")

    return warnings


def _validate_cue(cue, ci, seg_name, seg, reg, resources, warn):
    loc = f"Segment '{seg_name}' cue[{ci}]"

    at_ms = cue.get('at_ms')
    if at_ms is None or at_ms < 0:
        raise CompileError(f"{loc}: at_ms must be >= 0")
    if at_ms >= seg['nominal_duration_ms']:
        raise CompileError(
            f"{loc}: at_ms {at_ms} >= segment nominal_duration_ms {seg['nominal_duration_ms']}")

    effect = cue.get('effect')
    zone   = cue.get('zone')
    if not effect:
        raise CompileError(f"{loc}: missing 'effect'")
    if not zone:
        raise CompileError(f"{loc}: missing 'zone'")

    try:
        _reg_effect_id(reg, effect)
    except CompileError as e:
        raise CompileError(f"{loc}: {e}")

    try:
        _reg_zone_id(reg, zone)
    except CompileError as e:
        raise CompileError(f"{loc}: {e}")

    allowed = _reg_effect_zones(reg, effect)
    if allowed != ['*'] and zone not in allowed:
        raise CompileError(
            f"{loc}: effect '{effect}' is not compatible with zone '{zone}'. "
            f"Allowed zones: {allowed}")

    if 'palette' in cue:
        try:
            _reg_palette_id(reg, cue['palette'])
        except CompileError as e:
            raise CompileError(f"{loc}: palette error: {e}")

    try:
        encode_params(effect, cue.get('params', {}), reg, resources)
    except CompileError as e:
        raise CompileError(f"{loc}:\n{e}")

    # All-zero params warning (skip EFFECT_OFF which legitimately has no params)
    if effect != 'EFFECT_OFF':
        pdefs = _reg_effect_params(reg, effect)
        if pdefs and all(cue.get('params', {}).get(p['name'], 0) == 0 for p in pdefs):
            warn(f"{loc}: all params are 0 — possible forgotten parameter init")

    # Pointless no_mirror flag
    no_mirror_zones = {'MATRIX', 'BOTH_EARS', 'BOTH_VISORS',
                       'RIGHT_EAR', 'RIGHT_VISOR', 'RIGHT_CONNECTORS'}
    if cue.get('flags', {}).get('no_mirror') and zone in no_mirror_zones:
        warn(f"{loc}: no_mirror flag set on '{zone}' which has no mirror partner")


def _validate_resources(resources, warn):
    strings = resources.get('text_strings', [])
    if len(strings) > 64:
        raise CompileError(f"text_strings: {len(strings)} strings exceeds max 64")
    total_chars = sum(len(s) for s in strings)
    if total_chars > 1024:
        raise CompileError(f"text_strings: {total_chars} total chars exceeds 1024")
    for s in strings:
        if len(s) > 63:
            raise CompileError(f"text_strings: string too long (> 63 chars): '{s[:30]}...'")
        if not s.isascii():
            raise CompileError(f"text_strings: '{s}' contains non-ASCII")
        bad = [c for c in s if c not in _FONT]
        if bad:
            raise CompileError(f"text_strings: '{s}' uses characters not in font: {bad}")

    sprites = resources.get('sprites', [])
    sprite_bytes = sum(
        s.get('width', 0) * s.get('height', 0) * len(s.get('frames', [])) * 3
        for s in sprites if 'frames_from_file' not in s)
    if sprite_bytes > 24 * 1024:
        raise CompileError(f"Total sprite data {sprite_bytes} bytes exceeds 24 KB ceiling")

    vu_seqs = resources.get('vu_sequences', [])
    vu_bytes = sum(
        seq.get('band_count', 0) * len(seq.get('data', []))
        for seq in vu_seqs if 'vu_from_file' not in seq)
    if vu_bytes > 32 * 1024:
        raise CompileError(f"Total VU data {vu_bytes} bytes exceeds 32 KB ceiling")
    elif vu_bytes > 24 * 1024:
        warn(f"VU data {vu_bytes // 1024} KB is approaching the 32 KB ceiling")


# ─── Binary emitter ───────────────────────────────────────────────────────────

def compile_binary(scene, reg, scene_dir):
    """
    Emit the binary bundle. Returns (bytes, stats_dict).
    Call only after validate() succeeds.
    """
    resources  = scene.get('resources', {})
    mirror_lr  = scene.get('mirror_lr', True)
    seg_names  = list(scene['segments'].keys())
    seg_id_map = {n: i for i, n in enumerate(seg_names)}

    bg = scene['beat_grid']
    beat_first_ms     = int(bg['first_downbeat_ms'])
    beat_period_x1000 = int(round(float(bg['period_ms']) * 1000))
    beats_per_bar     = int(bg['beats_per_bar'])

    base_pal_id = _reg_palette_id(reg, scene['base_palette'])
    base_eff_id = _reg_effect_id(reg, scene['base_effect'])

    sp = StringPool()
    scene_name_off = sp.intern(scene['scene_name'])

    # ── Segment library ────────────────────────────────────────────────────

    seg_lib = bytearray()
    total_cues_out = 0

    for seg_name in seg_names:
        seg      = scene['segments'][seg_name]
        name_off = sp.intern(seg_name)
        base_eff = _reg_effect_id(reg, seg['base_effect'])
        base_pal = _reg_palette_id(reg, seg['base_palette'])
        cues     = expand_mirrors(seg['cues'], mirror_lr)
        total_cues_out += len(cues)

        if len(cues) > 255:
            raise CompileError(
                f"Segment '{seg_name}': {len(cues)} cues after mirror expansion exceeds 255")

        # Segment header (12 bytes):
        #   name_offset(u16), name_len(u8), base_eff(u8),
        #   base_pal(u8), cue_count(u8), nominal_dur(u32), reserved(u16)
        seg_lib += struct.pack('<HBBBBIH',
            name_off, len(seg_name), base_eff, base_pal,
            len(cues), seg['nominal_duration_ms'], 0)

        for cue in cues:
            pal_name = cue.get('palette',
                       seg.get('base_palette', scene['base_palette']))
            pal_id   = _reg_palette_id(reg, pal_name)
            eff_id   = _reg_effect_id(reg, cue['effect'])
            zone_id  = _reg_zone_id(reg, cue['zone'])
            flags    = cue.get('flags', {})
            flag_byte = (0x01 if flags.get('no_mirror') else 0) | \
                        (0x02 if flags.get('additive')  else 0)
            params   = encode_params(cue['effect'], cue.get('params', {}),
                                     reg, resources)
            dur_ms   = cue.get('duration_ms', 0)

            # Cue (16 bytes):
            #   at_ms(u32), effect(u8), zone(u8), palette(u8), flags(u8),
            #   params(6s), duration_ms(u16)
            seg_lib += struct.pack('<IBBBB6sH',
                cue['at_ms'], eff_id, zone_id, pal_id, flag_byte, params, dur_ms)

    # ── Timeline ───────────────────────────────────────────────────────────

    tl = bytearray()
    for entry in scene['timeline']:
        seg_obj  = scene['segments'][entry['play']]
        is_outro = 0x01 if seg_obj.get('is_outro', False) else 0
        # Timeline entry (8 bytes): start_ms(u32), seg_id(u8), flags(u8), reserved(u16)
        tl += struct.pack('<IBBH',
            entry['at_ms'], seg_id_map[entry['play']], is_outro, 0)

    # ── Palette table ──────────────────────────────────────────────────────

    used_pal_names = {scene['base_palette']}
    for seg in scene['segments'].values():
        used_pal_names.add(seg['base_palette'])
        for cue in seg['cues']:
            if 'palette' in cue:
                used_pal_names.add(cue['palette'])
    pal_ids    = sorted({_reg_palette_id(reg, p) for p in used_pal_names})
    pal_table  = struct.pack('<B', len(pal_ids)) + bytes(pal_ids)

    # ── Resource sections ─────────────────────────────────────────────────

    text_sec   = build_text_section(resources)
    vu_sec     = build_vu_section(resources, scene_dir)
    sprite_sec = build_sprite_section(resources)

    # ── Compute section offsets ────────────────────────────────────────────

    pal_off    = HEADER_SIZE
    seg_off    = pal_off  + len(pal_table)
    tl_off     = seg_off  + len(seg_lib)
    text_off   = tl_off   + len(tl)
    vu_off     = text_off + len(text_sec)
    sprite_off = vu_off   + len(vu_sec)
    pool_off   = sprite_off + len(sprite_sec)
    pool_data  = sp.data()
    total_size = pool_off + len(pool_data) + 4  # +4 for CRC trailer

    # Absent sections signal with offset = 0
    text_lib_off   = text_off   if text_sec   else 0
    vu_lib_off     = vu_off     if vu_sec     else 0
    sprite_lib_off = sprite_off if sprite_sec else 0

    # ── Header (64 bytes) ─────────────────────────────────────────────────

    hdr = bytearray(HEADER_SIZE)
    struct.pack_into('<4sHHIIHBBHIHHHHHBB', hdr, 0,
        MAGIC,
        FORMAT_VERSION,
        0,                          # reserved_1
        total_size,
        0,                          # crc32 placeholder (real CRC at trailer)
        scene_name_off,
        len(pal_ids),
        len(seg_names),
        len(scene['timeline']),
        scene['total_duration_ms'],
        pal_off,
        seg_off,
        tl_off,
        pool_off,
        len(pool_data),
        base_pal_id,
        base_eff_id,
    )
    # reserved_2 (bytes 38–63): beat grid + extended section offsets
    struct.pack_into('<IIBBHHH', hdr, 38,
        beat_first_ms,
        beat_period_x1000,
        beats_per_bar,
        0,                          # reserved byte 47
        text_lib_off,
        vu_lib_off,
        sprite_lib_off,
    )
    # bytes 54–63 remain zero

    # ── Assemble and CRC ──────────────────────────────────────────────────

    body   = bytes(hdr) + pal_table + bytes(seg_lib) + bytes(tl) + \
             text_sec + vu_sec + sprite_sec + pool_data
    crc    = zlib.crc32(body) & 0xFFFFFFFF
    bundle = body + struct.pack('<I', crc)

    stats = {
        'scene_name':        scene['scene_name'],
        'total_duration_ms': scene['total_duration_ms'],
        'segment_count':     len(seg_names),
        'segment_names':     seg_names,
        'timeline_count':    len(scene['timeline']),
        'total_cues':        total_cues_out,
        'palette_count':     len(pal_ids),
        'text_strings':      len(resources.get('text_strings', [])),
        'vu_sequences':      len(resources.get('vu_sequences', [])),
        'sprites':           len(resources.get('sprites', [])),
        'text_bytes':        len(text_sec),
        'vu_bytes':          len(vu_sec),
        'sprite_bytes':      len(sprite_sec),
        'bundle_size':       len(bundle),
    }
    return bundle, stats


# ─── Report ───────────────────────────────────────────────────────────────────

def write_report(path, stats, warnings):
    segs = ', '.join(stats['segment_names'])
    dur  = stats['total_duration_ms'] / 1000
    ram  = stats['bundle_size'] / 1024

    lines = [
        f"Compiled {Path(path).stem} successfully.",
        f"",
        f"  Scene:            {stats['scene_name']}",
        f"  Duration:         {dur:.1f} sec",
        f"  Segments:         {stats['segment_count']}  ({segs})",
        f"  Timeline entries: {stats['timeline_count']}",
        f"  Total cues:       {stats['total_cues']}  (after mirror expansion)",
        f"  Palettes used:    {stats['palette_count']}",
    ]
    if stats['text_strings']:
        lines.append(f"  Text strings:     {stats['text_strings']}  ({stats['text_bytes']} bytes)")
    if stats['vu_sequences']:
        lines.append(f"  VU sequences:     {stats['vu_sequences']}  ({stats['vu_bytes']} bytes)")
    if stats['sprites']:
        lines.append(f"  Sprites:          {stats['sprites']}  ({stats['sprite_bytes']} bytes)")
    lines += [
        f"  Binary size:      {stats['bundle_size']} bytes  ({stats['bundle_size'] / 1024:.1f} KB)",
        f"  RAM estimate:     ≤ {ram:.1f} KB  /  64 KB ceiling",
        f"",
        f"  Warnings: {len(warnings)}",
    ]
    for w in warnings:
        lines.append(f"    [WARN] {w}")
    if not warnings:
        lines.append(f"    (none)")
    lines.append("")

    text = '\n'.join(lines)
    with open(path, 'w') as fh:
        fh.write(text)
    return text


# ─── Hex dump ─────────────────────────────────────────────────────────────────

def write_hex_dump(path, data):
    with open(path, 'w') as fh:
        for i in range(0, len(data), 16):
            chunk    = data[i:i + 16]
            hex_part = ' '.join(f'{b:02x}' for b in chunk)
            asc_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
            fh.write(f'{i:06x}  {hex_part:<47}  {asc_part}\n')


# ─── CLI ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description='Compile a Thomas helmet .scene.json to a binary .scene bundle.')
    ap.add_argument('scene_json', help='Input .scene.json file')
    ap.add_argument('--registry', metavar='PATH',
                    help='Path to registry.json  '
                         '(default: AI Instructions/registry.json next to this script)')
    ap.add_argument('--sort-cues', action='store_true',
                    help='Auto-sort cues within segments by at_ms  (warns when used)')
    ap.add_argument('--strict', action='store_true',
                    help='Treat all warnings as hard errors')
    ap.add_argument('--output', metavar='PATH',
                    help='Output .scene path  (default: same dir/stem as input)')
    ap.add_argument('--dump-binary', action='store_true',
                    help='Also write a .scene.hex hex dump for debugging')
    args = ap.parse_args()

    in_path = Path(args.scene_json).resolve()
    if not in_path.exists():
        sys.exit(f"Error: not found: {in_path}")

    # Registry location
    if args.registry:
        reg_path = Path(args.registry).resolve()
    else:
        reg_path = Path(__file__).parent / 'AI Instructions' / 'registry.json'
    if not reg_path.exists():
        sys.exit(f"Error: registry not found: {reg_path}\nUse --registry to specify the path.")

    # Output paths
    stem = in_path.name
    for sfx in ('.scene.json', '.json'):
        if stem.endswith(sfx):
            stem = stem[:-len(sfx)]
            break
    out_path    = Path(args.output).resolve() if args.output else in_path.parent / f'{stem}.scene'
    report_path = out_path.with_suffix('').with_suffix('.scene.report.txt')
    dump_path   = out_path.with_suffix('').with_suffix('.scene.hex')

    # Load
    try:
        scene = json.loads(in_path.read_text())
    except json.JSONDecodeError as e:
        sys.exit(f"Error: invalid JSON in {in_path}:\n  {e}")

    try:
        reg = load_registry(reg_path)
    except Exception as e:
        sys.exit(f"Error loading registry: {e}")

    scene_dir = str(in_path.parent)

    # Validate
    try:
        warnings = validate(scene, reg, scene_dir,
                            sort_cues=args.sort_cues, strict=args.strict)
    except CompileError as e:
        print(f"\nCompilation FAILED (validation error):\n  {e}\n", file=sys.stderr)
        sys.exit(1)

    # Compile
    try:
        bundle, stats = compile_binary(scene, reg, scene_dir)
    except CompileError as e:
        print(f"\nCompilation FAILED (emit error):\n  {e}\n", file=sys.stderr)
        sys.exit(1)

    # Write
    out_path.write_bytes(bundle)
    report_text = write_report(report_path, stats, warnings)
    print(report_text)
    print(f"Output:  {out_path}")
    print(f"Report:  {report_path}")

    if args.dump_binary:
        write_hex_dump(dump_path, bundle)
        print(f"Hex dump: {dump_path}")


if __name__ == '__main__':
    main()
