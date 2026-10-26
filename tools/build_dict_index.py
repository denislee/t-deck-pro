#!/usr/bin/env python3
"""Build a sparse sidecar index for the firmware dictionary (/dict/eng-pob.tsv).

Why
---
/dict/eng-pob.tsv is ~104 MB (~1.5 M lines). Without an index every lookup runs
a binary search that seeks ~18 times, and each seek lands in the middle of a
line, so the firmware has to over-read and discard the partial line. On SD over
SPI those seeks dominate the latency of a lookup.

This script writes a small sidecar file next to the TSV that holds the byte
offset of every stride-th line together with that line's headword. With the
default stride (64) the index is a few hundred KB, the firmware binary-searches
it in memory (a handful of block reads) and then seeks once to the line just
before the target, so a lookup costs a few block reads instead of ~18 seeks.

Format (all integers little-endian)
-----------------------------------
    header:  b'DIX1'          magic, 4 bytes
             u32 stride       lines between two records (>= 1)
             u32 count        number of 32-byte records that follow
             u32 tsv_size     size in bytes of the TSV the index was built from
    record:  u32 offset       byte offset of a line start in the TSV
             28 bytes key     the line's headword (bytes before the first
                              b'\\t', b'\\r' stripped), ASCII-lowercased with
                              bytes.lower(), truncated to 27 bytes and
                              NUL-padded to 28

Record 0 is the first line of the file (offset 0); after that, every stride-th
line. Records are therefore in non-decreasing headword order, exactly like the
file, and the trailing NUL padding makes the key a NUL-terminated C string.

Firmware use
------------
ui_dict_lookup() in examples/factory/ui_deckpro_port.cpp uses the index when it
is present: it binary-searches the records for the LAST record whose key is
strictly less than the (lowercased, 27-byte-truncated) query, seeks to that
record's offset and linearly scans forward with the normal headword compare.
When the index is missing, unreadable, or built for a different TSV size it
falls back to the plain bisection, so the .idx file is purely an optimisation.

Install: copy the index next to the dictionary on the SD card, i.e.
/dict/eng-pob.idx alongside /dict/eng-pob.tsv.

Usage: python3 -I build_dict_index.py TSV [IDX] [--stride N]
       (IDX defaults to TSV with a .idx suffix, stride defaults to 64)
"""
import argparse
import os
import struct
import sys

MAGIC = b"DIX1"
KEY_LEN = 28                    # 28-byte key field, incl. NUL padding
KEY_MAX = KEY_LEN - 1           # at most 27 real bytes, so a NUL always ends it
RECORD_LEN = 4 + KEY_LEN        # u32 offset + key
HEADER = struct.Struct("<4sIII")
DEFAULT_STRIDE = 64


def index_key(headword):
    """Index key for one headword: ASCII-lowercased, <=27 bytes, NUL-padded."""
    return headword.lower()[:KEY_MAX].ljust(KEY_LEN, b"\x00")


def iter_records(path, stride):
    """Yield (offset, key) for line 0 and every stride-th line after it.

    Streams the file one line at a time, so it never holds the TSV in memory.
    """
    stride = max(1, int(stride))
    offset = 0
    with open(path, "rb") as f:
        for n, raw in enumerate(f):
            if n % stride == 0:
                # Headword: bytes before the first tab, line ending stripped.
                head = raw.split(b"\t", 1)[0].rstrip(b"\r\n")
                yield offset, index_key(head)
            offset += len(raw)


def build_records(path, stride=DEFAULT_STRIDE):
    """In-memory [(offset, key28), ...] for `path` — used by the host test."""
    return list(iter_records(path, stride))


def write_index(tsv, idx_path, stride=DEFAULT_STRIDE):
    """Write the sidecar index for `tsv`; returns (count, bytes written)."""
    stride = max(1, int(stride))
    tsv_size = os.path.getsize(tsv)
    if tsv_size >= 1 << 32:
        raise ValueError("TSV is too large for the 32-bit size field: %d" % tsv_size)

    count = 0
    with open(idx_path, "wb") as out:
        out.write(HEADER.pack(MAGIC, stride, 0, tsv_size))  # count patched below
        for offset, key in iter_records(tsv, stride):
            out.write(struct.pack("<I", offset))
            out.write(key)
            count += 1
        out.seek(0)
        out.write(HEADER.pack(MAGIC, stride, count, tsv_size))
    return count, os.path.getsize(idx_path)


def default_idx_path(tsv):
    """TSV path with the extension replaced by .idx (eng-pob.tsv -> eng-pob.idx)."""
    return os.path.splitext(tsv)[0] + ".idx"


def main(argv=None):
    ap = argparse.ArgumentParser(description="Build the /dict/eng-pob.idx sidecar index.")
    ap.add_argument("tsv", help="dictionary TSV (e.g. eng-pob.tsv)")
    ap.add_argument("idx", nargs="?", default=None,
                    help="output index (default: TSV with a .idx suffix)")
    ap.add_argument("--stride", type=int, default=DEFAULT_STRIDE,
                    help="lines between two index records (default: %d)" % DEFAULT_STRIDE)
    args = ap.parse_args(argv)
    if args.stride < 1:
        ap.error("--stride must be >= 1")

    idx_path = args.idx or default_idx_path(args.tsv)
    count, idx_size = write_index(args.tsv, idx_path, args.stride)
    print("tsv=%s (%d bytes)" % (args.tsv, os.path.getsize(args.tsv)))
    print("idx=%s records=%d stride=%d size=%d bytes"
          % (idx_path, count, args.stride, idx_size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
