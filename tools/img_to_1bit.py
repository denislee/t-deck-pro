#!/usr/bin/env python3
"""
img_to_1bit.py — re-encode LVGL TRUE_COLOR_ALPHA image .c files to ALPHA_1BIT.

Usage:
    python3 tools/img_to_1bit.py <src_dir> [--dry-run] [--png-dir <dir>]

Reads every img_*.c file in <src_dir> that contains LV_IMG_CF_TRUE_COLOR_ALPHA
descriptors compiled for LV_COLOR_DEPTH == 1 (2 bytes/pixel: color_byte, alpha_byte).
Thresholds the alpha channel at 128 to produce a 1-bit bitmap in
LV_IMG_CF_ALPHA_1BIT layout (no palette, stride = ceil(w/8) bytes/row,
MSB = leftmost pixel, 1 = opaque, 0 = transparent) and re-writes the file.

LVGL 8.3 format references (verified in lib/lvgl/src/draw/):
  - lv_img_decoder.c:596-601  → stride = (w+7)>>3, bit 7 = pixel 0, pos counts down
  - lv_img_buf.h:37,43        → BUF_SIZE_ALPHA_1BIT = ((w/8)+1)*h, INDEXED adds 4*2 palette
  - lv_img_buf.c:106          → alpha = buf[i*LV_IMG_PX_SIZE_ALPHA_BYTE + LV_IMG_PX_SIZE_ALPHA_BYTE-1]
                                  → at depth-1: alpha = byte 1 of each 2-byte pixel

Visual verification PNGs are written to --png-dir (default: /tmp/img_1bit_verify/).
"""

import re
import sys
import os
import argparse

ALPHA_THRESHOLD = 128  # pixels with alpha >= this become opaque (bit 1)


def parse_depth1_data(c_text: str) -> list[int]:
    """Extract the raw hex bytes from the #if LV_COLOR_DEPTH == 1 || LV_COLOR_DEPTH == 8 block."""
    # Find the block boundaries
    start_pat = re.compile(r'#if\s+LV_COLOR_DEPTH\s*==\s*1\s*\|\|\s*LV_COLOR_DEPTH\s*==\s*8')
    # End is the next #endif at depth 1 after our opening #if
    m = start_pat.search(c_text)
    if not m:
        return []
    block_start = m.end()

    # Walk forward and find the matching #endif (depth-1 match)
    depth = 1
    pos = block_start
    while pos < len(c_text) and depth > 0:
        nxt_if = re.search(r'#\s*(if|ifdef|ifndef)\b', c_text[pos:])
        nxt_end = re.search(r'#\s*endif\b', c_text[pos:])
        nxt_el = re.search(r'#\s*elif\b', c_text[pos:])

        cands = []
        if nxt_if:
            cands.append((nxt_if.start(), 'if'))
        if nxt_end:
            cands.append((nxt_end.start(), 'endif'))
        if nxt_el:
            cands.append((nxt_el.start(), 'elif'))
        if not cands:
            break

        earliest_off, kind = min(cands, key=lambda t: t[0])
        pos += earliest_off

        if kind == 'if':
            depth += 1
            pos += 3  # skip past '#'
        elif kind == 'elif' and depth == 1:
            # Stop at the elif — the block ends here
            block_end = pos
            break
        elif kind == 'endif':
            depth -= 1
            if depth == 0:
                block_end = pos
                break
            pos += 7
    else:
        block_end = len(c_text)

    block_text = c_text[block_start:block_end]

    # Extract all hex literals
    return [int(h, 16) for h in re.findall(r'0[xX][0-9A-Fa-f]+', block_text)]


def find_descriptors(c_text: str):
    """
    Return a list of dicts describing each lv_img_dsc_t in the file.
    Keys: var_name, map_name, w, h, cf.
    """
    results = []
    pat = re.compile(
        r'const\s+lv_img_dsc_t\s+(\w+)\s*=\s*\{([^}]*)\}',
        re.DOTALL
    )
    for m in pat.finditer(c_text):
        var = m.group(1)
        body = m.group(2)
        w_m = re.search(r'\.header\.w\s*=\s*(\d+)', body)
        h_m = re.search(r'\.header\.h\s*=\s*(\d+)', body)
        cf_m = re.search(r'\.header\.cf\s*=\s*(\w+)', body)
        data_m = re.search(r'\.data\s*=\s*(\w+)', body)
        if w_m and h_m:
            results.append({
                'var_name': var,
                'map_name': data_m.group(1) if data_m else var + '_map',
                'w': int(w_m.group(1)),
                'h': int(h_m.group(1)),
                'cf': cf_m.group(1) if cf_m else '',
            })
    return results


def encode_alpha_1bit(pixels_alpha: list[int], w: int, h: int) -> bytes:
    """
    Encode a list of alpha values (len = w*h) into LVGL ALPHA_1BIT format.

    Stride = (w + 7) // 8  bytes per row.
    Bit 7 of each byte is the leftmost pixel (MSB first).
    1 = opaque (alpha >= ALPHA_THRESHOLD), 0 = transparent.
    data_size = ((w // 8) + 1) * h  (matching LV_IMG_BUF_SIZE_ALPHA_1BIT macro).
    """
    stride = (w + 7) // 8
    # The macro uses (w//8 + 1) — for non-multiples of 8 this equals ceil(w/8).
    # We use ceil(w/8) == (w+7)//8 which matches the decoder's stride calculation.
    assert stride == (w // 8) + 1 or w % 8 == 0, \
        f"stride mismatch: w={w}, ceil={stride}, macro={(w//8)+1}"

    out = bytearray()
    idx = 0
    for row in range(h):
        byte_val = 0
        bit_pos = 7  # MSB first
        bytes_this_row = 0
        for col in range(w):
            alpha = pixels_alpha[idx]
            idx += 1
            if alpha >= ALPHA_THRESHOLD:
                byte_val |= (1 << bit_pos)
            bit_pos -= 1
            if bit_pos < 0:
                out.append(byte_val)
                bytes_this_row += 1
                byte_val = 0
                bit_pos = 7
        # flush remaining bits (partial byte at end of row)
        if bit_pos < 7:
            out.append(byte_val)
            bytes_this_row += 1
        # pad row to stride bytes
        while bytes_this_row < stride:
            out.append(0)
            bytes_this_row += 1
    return bytes(out)


def generate_c_file(orig_path: str, desc: dict, raw_bytes: bytes) -> str:
    """
    Build the new .c file text for one image descriptor re-encoded as ALPHA_1BIT.
    The output keeps the same boilerplate (includes, attributes) but replaces
    the pixel data array and descriptor.
    """
    w, h = desc['w'], desc['h']
    var_name = desc['var_name']
    map_name = desc['map_name']
    attr_macro = f'LV_ATTRIBUTE_IMG_{var_name.upper()}'
    data_size = ((w // 8) + 1) * h  # LV_IMG_BUF_SIZE_ALPHA_1BIT(w,h)

    # Format hex bytes as C array, 16 bytes per line
    hex_lines = []
    for i in range(0, len(raw_bytes), 16):
        chunk = raw_bytes[i:i+16]
        hex_lines.append('  ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',')

    hex_block = '\n'.join(hex_lines)

    return f"""\
#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef {attr_macro}
#define {attr_macro}
#endif

/*
 * Re-encoded from LV_IMG_CF_TRUE_COLOR_ALPHA to LV_IMG_CF_ALPHA_1BIT.
 * Source: {os.path.basename(orig_path)}
 * Dimensions: {w}x{h}
 * Stride: {(w+7)//8} bytes/row  (= ceil({w}/8))
 * Alpha threshold: >= {ALPHA_THRESHOLD} → opaque
 * Tool: tools/img_to_1bit.py
 */
const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST {attr_macro} uint8_t {map_name}[] = {{
{hex_block}
}};

const lv_img_dsc_t {var_name} = {{
  .header.cf          = LV_IMG_CF_ALPHA_1BIT,
  .header.always_zero = 0,
  .header.reserved    = 0,
  .header.w           = {w},
  .header.h           = {h},
  .data_size          = {data_size}, /* LV_IMG_BUF_SIZE_ALPHA_1BIT({w}, {h}) */
  .data               = {map_name},
}};
"""


def save_png_comparison(pixels_alpha_orig: list[int], pixels_alpha_renc: list[int],
                         w: int, h: int, var_name: str, png_dir: str):
    """Save original and re-encoded PNGs for visual verification."""
    try:
        import struct, zlib

        def make_png(pixels_bool, width, height):
            """Build a minimal grayscale PNG (1 = black, 0 = white background)."""
            raw_rows = []
            for row in range(height):
                row_bytes = bytearray([0])  # filter byte = None
                for col in range(width):
                    val = 0 if pixels_bool[row * width + col] else 255  # opaque→black
                    row_bytes.append(val)
                raw_rows.append(bytes(row_bytes))
            raw_data = b''.join(raw_rows)
            compressed = zlib.compress(raw_data)

            def chunk(name, data):
                c = name + data
                return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)

            ihdr_data = struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0)
            return (b'\x89PNG\r\n\x1a\n' +
                    chunk(b'IHDR', ihdr_data) +
                    chunk(b'IDAT', compressed) +
                    chunk(b'IEND', b''))

        os.makedirs(png_dir, exist_ok=True)
        orig_bits = [1 if a >= ALPHA_THRESHOLD else 0 for a in pixels_alpha_orig]
        renc_bits = [1 if a >= ALPHA_THRESHOLD else 0 for a in pixels_alpha_renc]

        orig_png = make_png(orig_bits, w, h)
        renc_png = make_png(renc_bits, w, h)

        orig_path = os.path.join(png_dir, f'{var_name}_original.png')
        renc_path = os.path.join(png_dir, f'{var_name}_reencoded.png')
        with open(orig_path, 'wb') as f:
            f.write(orig_png)
        with open(renc_path, 'wb') as f:
            f.write(renc_png)

        return orig_path, renc_path
    except Exception as e:
        print(f'  [WARN] PNG save failed: {e}')
        return None, None


def process_file(path: str, png_dir: str, dry_run: bool):
    with open(path, 'r') as f:
        orig_text = f.read()

    descs = find_descriptors(orig_text)
    if not descs:
        print(f'  skip {path}: no lv_img_dsc_t found')
        return

    # Only process images with TRUE_COLOR_ALPHA
    descs = [d for d in descs if 'TRUE_COLOR_ALPHA' in d.get('cf', '')]
    if not descs:
        print(f'  skip {path}: no TRUE_COLOR_ALPHA descriptor')
        return

    raw_data = parse_depth1_data(orig_text)
    if not raw_data:
        print(f'  skip {path}: no depth-1 pixel data block found')
        return

    for desc in descs:
        w, h = desc['w'], desc['h']
        expected = w * h * 2  # 2 bytes/pixel at depth 1
        if len(raw_data) < expected:
            print(f'  ERROR {path}: expected {expected} bytes for {w}x{h}, got {len(raw_data)}')
            continue

        pixels_alpha = [raw_data[i * 2 + 1] for i in range(w * h)]  # byte 1 = alpha

        # Re-encode to ALPHA_1BIT
        encoded = encode_alpha_1bit(pixels_alpha, w, h)

        # Decode the re-encoded data back for verification
        stride = (w + 7) // 8
        renc_alpha = []
        for row in range(h):
            for col in range(w):
                byte_idx = row * stride + (col >> 3)
                bit_pos = 7 - (col & 7)
                bit = (encoded[byte_idx] >> bit_pos) & 1
                renc_alpha.append(255 if bit else 0)

        # Visual verification: save PNGs
        orig_p, renc_p = save_png_comparison(pixels_alpha, renc_alpha, w, h, desc['var_name'], png_dir)

        before_bytes = expected  # depth-1 raw pixel bytes (before, in C file)
        after_bytes = len(encoded)
        print(f'  {desc["var_name"]}: {w}x{h}  before={before_bytes}B  after={after_bytes}B'
              f'  saving={before_bytes - after_bytes}B ({100*(before_bytes-after_bytes)/before_bytes:.1f}%)')
        if orig_p:
            print(f'    PNG: {orig_p}')
            print(f'    PNG: {renc_p}')

        if not dry_run:
            new_text = generate_c_file(path, desc, encoded)
            with open(path, 'w') as f:
                f.write(new_text)
            print(f'    → wrote {path}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('src_dir', help='Directory containing img_*.c files')
    ap.add_argument('--dry-run', action='store_true', help='Parse and report but do not write files')
    ap.add_argument('--png-dir', default='/tmp/claude-1000/-home-dns-tmp-t-deck-pro/1c51a5e1-bc91-4dd0-8a8e-7aec6f459833/scratchpad/img_verify',
                    help='Directory to write verification PNGs')
    args = ap.parse_args()

    import glob
    files = sorted(glob.glob(os.path.join(args.src_dir, 'img_*.c')))
    if not files:
        print(f'No img_*.c files found in {args.src_dir}')
        sys.exit(1)

    for path in files:
        print(f'Processing {os.path.basename(path)}...')
        process_file(path, args.png_dir, args.dry_run)


if __name__ == '__main__':
    main()
