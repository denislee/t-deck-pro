#!/usr/bin/env python3
"""Subset a 1-bpp LVGL font .c file down to one contiguous codepoint range.

The input must have the exact shape produced by
examples/factory/src/fonts_bitmap/bdf2lvgl.py:

  * glyph_bitmap[] — one line per glyph, each starting with `/* U+XXXX */`
    followed by the glyph's bytes;
  * glyph_dsc[]    — a reserved entry (id 0) followed by one
    `{.bitmap_index = N, .adv_w = ..., ...}` entry per glyph, in codepoint
    order;
  * cmaps[]        — a single `.range_start = 32, .range_length = 95` entry
    (LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY, .glyph_id_start = 1).

The output keeps only the glyphs for FIRST..LAST, recomputes every
`.bitmap_index` from the kept bitmap sizes, and rewrites
`.range_start` / `.range_length` in cmaps. Everything else in the file is
copied through unchanged.

usage: subset_lvgl_font.py IN.c OUT.c FIRST LAST

FIRST and LAST are codepoints, decimal or hex (0x2D). Example — keep only
'-' '.' '/' '0'-'9' ':' for a lock-screen clock:

    subset_lvgl_font.py lv_font_spleen_32x64.c small.c 0x2D 0x3A
"""
import re
import sys
from pathlib import Path

GLYPH_LINE_RE = re.compile(r'^\s*/\* U\+([0-9A-Fa-f]{4,6}) \*/\s*(.*?)\s*$')
BYTE_RE = re.compile(r'0x[0-9A-Fa-f]+')
BITMAP_INDEX_RE = re.compile(r'\.bitmap_index\s*=\s*\d+')
RANGE_START_RE = re.compile(r'\.range_start\s*=\s*(\d+)')
RANGE_LENGTH_RE = re.compile(r'\.range_length\s*=\s*(\d+)')


def fail(msg):
    sys.exit("subset_lvgl_font.py: error: " + msg)


def parse_codepoint(text, what):
    try:
        return int(text, 0)
    except ValueError:
        fail("%s is not a codepoint: %r (use decimal or 0x..)" % (what, text))


def array_body(text, name):
    """Return (start, end) offsets of the {...} body of `name[] = { ... }`."""
    m = re.search(r'\b' + re.escape(name) + r'\s*\[\s*\]\s*=\s*\{', text)
    if not m:
        fail("could not find `%s[] = {` in the input" % name)
    start = m.end()
    depth = 1
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return start, i
    fail("`%s[]` array is not terminated" % name)


def parse_bitmaps(body):
    """[{cp, line, size}] for every glyph line in glyph_bitmap[]."""
    glyphs = []
    for line in body.splitlines():
        if not line.strip():
            continue
        m = GLYPH_LINE_RE.match(line)
        if not m:
            fail("unrecognised line in glyph_bitmap[] (expected "
                 "`/* U+XXXX */ <bytes>`): %r" % line.strip()[:70])
        glyphs.append({
            "cp": int(m.group(1), 16),
            "line": line.rstrip(),
            "size": len(BYTE_RE.findall(m.group(2))),
        })
    if not glyphs:
        fail("glyph_bitmap[] has no glyph lines")
    return glyphs


def parse_dsc(body):
    """Entry texts of glyph_dsc[], reserved entry first."""
    entries = []
    depth = 0
    cur = []
    for ch in body:
        if ch == '{':
            depth += 1
            if depth == 1:
                cur = []
        if depth >= 1:
            cur.append(ch)
        if ch == '}':
            depth -= 1
            if depth == 0:
                entries.append("".join(cur))
    if depth != 0:
        fail("unbalanced braces in glyph_dsc[]")
    for entry in entries:
        if not BITMAP_INDEX_RE.search(entry):
            fail("glyph_dsc[] entry without .bitmap_index: %r" % entry[:70])
    return entries


def main():
    if len(sys.argv) != 5:
        sys.exit("usage: subset_lvgl_font.py IN.c OUT.c FIRST LAST"
                 "  (codepoints, e.g. 0x2D 0x3A)")
    in_path, out_path = Path(sys.argv[1]), Path(sys.argv[2])
    first = parse_codepoint(sys.argv[3], "FIRST")
    last = parse_codepoint(sys.argv[4], "LAST")
    if first > last:
        fail("FIRST (0x%X) is greater than LAST (0x%X)" % (first, last))

    text = in_path.read_text()

    # ---- glyph_bitmap -----------------------------------------------------
    bm_start, bm_end = array_body(text, "glyph_bitmap")
    glyphs = parse_bitmaps(text[bm_start:bm_end])

    # ---- glyph_dsc --------------------------------------------------------
    dsc_start, dsc_end = array_body(text, "glyph_dsc")
    dsc_entries = parse_dsc(text[dsc_start:dsc_end])
    if not dsc_entries:
        fail("glyph_dsc[] is empty (expected a reserved entry first)")
    if BITMAP_INDEX_RE.search(dsc_entries[0]).group(0) != ".bitmap_index = 0":
        fail("glyph_dsc[0] is not the reserved entry (.bitmap_index = 0)")
    if len(dsc_entries) - 1 != len(glyphs):
        fail("glyph_dsc[] has %d glyph entries but glyph_bitmap[] has %d glyphs"
             % (len(dsc_entries) - 1, len(glyphs)))

    # ---- cmaps ------------------------------------------------------------
    cm_start, cm_end = array_body(text, "cmaps")
    cmaps = text[cm_start:cm_end]
    if cmaps.count(".range_start") != 1:
        fail("input has %d cmap ranges; only a single contiguous range is "
             "supported" % cmaps.count(".range_start"))
    if "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY" not in cmaps:
        fail("cmaps[0].type is not LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY; "
             "unsupported")
    range_start = int(RANGE_START_RE.search(cmaps).group(1))
    range_length = int(RANGE_LENGTH_RE.search(cmaps).group(1))

    for i, g in enumerate(glyphs):
        if g["cp"] != range_start + i:
            fail("glyph %d is U+%04X but the cmap range starts at U+%04X; "
                 "glyph_dsc[]/glyph_bitmap[] are not in codepoint order"
                 % (i, g["cp"], range_start))
    range_end = range_start + range_length - 1
    if first < range_start or last > range_end:
        fail("requested 0x%X..0x%X is outside the font's range U+%04X..U+%04X"
             % (first, last, range_start, range_end))

    # ---- select + reindex -------------------------------------------------
    kept = [g for g in glyphs if first <= g["cp"] <= last]
    kept_cps = {g["cp"] for g in kept}
    for cp in range(first, last + 1):
        if cp not in kept_cps:
            fail("no glyph line for U+%04X (gap in glyph_bitmap[])" % cp)

    new_bitmap_lines = [g["line"] for g in kept]
    new_dsc_entries = [dsc_entries[0]]
    offset = 0
    for g in kept:
        entry = dsc_entries[1 + (g["cp"] - range_start)]
        entry = BITMAP_INDEX_RE.sub(".bitmap_index = %d" % offset, entry, count=1)
        new_dsc_entries.append(entry)
        offset += g["size"]

    new_bitmap_body = "\n" + "\n".join(new_bitmap_lines) + "\n"
    new_dsc_body = "\n" + "\n".join("    " + e + "," for e in new_dsc_entries) + "\n"
    new_cmaps_body = RANGE_START_RE.sub(".range_start = %d" % first, cmaps, count=1)
    new_cmaps_body = RANGE_LENGTH_RE.sub(
        ".range_length = %d" % (last - first + 1), new_cmaps_body, count=1)

    # ---- assemble ---------------------------------------------------------
    out = text
    for start, end, body in sorted(
            [(bm_start, bm_end, new_bitmap_body),
             (dsc_start, dsc_end, new_dsc_body),
             (cm_start, cm_end, new_cmaps_body)],
            reverse=True):
        out = out[:start] + body + out[end:]

    header = ("/* Subset by tools/subset_lvgl_font.py: kept codepoints "
              "0x%02X..0x%02X (%d glyphs). */" % (first, last, len(kept)))
    nl = out.find("\n")
    if nl < 0:
        fail("input file has no newline")
    out = out[:nl + 1] + header + "\n" + out[nl + 1:]

    out_path.write_text(out)

    before = sum(g["size"] for g in glyphs)
    after = sum(g["size"] for g in kept)
    print("subset_lvgl_font.py: U+%04X..U+%04X, %d/%d glyphs kept; "
          "glyph_bitmap %d -> %d bytes (-%d)"
          % (first, last, len(kept), len(glyphs), before, after, before - after),
          file=sys.stderr)


if __name__ == "__main__":
    main()
