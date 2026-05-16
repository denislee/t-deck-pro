#!/usr/bin/env python3
"""
Download FreeDict's English-Portuguese dictionary (StarDict format) and
convert it to the sorted TSV file consumed by the T-Deck-Pro factory firmware.

Output: eng-pob.tsv (or whatever -o specifies). Drop it on the SD card at
/dict/eng-pob.tsv and the dictionary screen will find it.

Format produced (matches ui_dict_lookup() in ui_deckpro_port.cpp):
    headword<TAB>definition\n
where `definition` uses the literal two-character "\\n" sequence for line
breaks. Sorted ASCII case-insensitively by headword (the firmware's binary
search uses byte-wise tolower comparison, so we match that exactly).

StarDict references:
  https://github.com/huzheng001/stardict-3/blob/master/dict/doc/StarDictFileFormat
  Source: https://freedict.org/downloads/  (eng-por 0.3, 15766 headwords)
"""

import argparse
import gzip
import hashlib
import os
import re
import struct
import sys
import tarfile
import tempfile
import urllib.request
from collections import defaultdict
from html.parser import HTMLParser
from pathlib import Path

DEFAULT_VERSION = "0.3"
BASE_URL = "https://download.freedict.org/dictionaries/eng-por"


def url_for(version: str) -> tuple[str, str]:
    archive = f"freedict-eng-por-{version}.stardict.tar.xz"
    return f"{BASE_URL}/{version}/{archive}", archive


def download(url: str, dest: Path) -> None:
    print(f"[1/5] downloading {url}", file=sys.stderr)
    with urllib.request.urlopen(url) as r, open(dest, "wb") as f:
        total = 0
        while True:
            chunk = r.read(64 * 1024)
            if not chunk:
                break
            f.write(chunk)
            total += len(chunk)
        print(f"      {total / 1024:.1f} KiB", file=sys.stderr)


def verify_sha512(file: Path, expected_url: str) -> None:
    try:
        with urllib.request.urlopen(expected_url) as r:
            line = r.read().decode().strip().split()
            expected = line[0].lower()
    except Exception as e:
        print(f"[2/5] sha512 check skipped ({e})", file=sys.stderr)
        return
    h = hashlib.sha512()
    with open(file, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    if h.hexdigest().lower() != expected:
        sys.exit(f"sha512 mismatch: got {h.hexdigest()} expected {expected}")
    print("[2/5] sha512 ok", file=sys.stderr)


def extract(archive: Path, dest_dir: Path) -> Path:
    print(f"[3/5] extracting {archive.name}", file=sys.stderr)
    with tarfile.open(archive, "r:xz") as tar:
        tar.extractall(dest_dir)
    # FreeDict tarballs unpack into freedict-eng-por-<ver>/. Find the .ifo.
    ifo_files = list(dest_dir.rglob("*.ifo"))
    if not ifo_files:
        sys.exit("no .ifo file found in archive")
    return ifo_files[0]


def parse_ifo(ifo: Path) -> dict:
    info = {}
    with open(ifo, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if "=" not in line:
                continue
            k, _, v = line.partition("=")
            info[k.strip()] = v.strip()
    return info


def read_idx(idx_path: Path, offset_bits: int) -> list[tuple[str, int, int]]:
    # .idx records: null-terminated word + offset (32 or 64-bit BE) + size (32-bit BE).
    data = idx_path.read_bytes()
    entries = []
    pos = 0
    off_size = 8 if offset_bits == 64 else 4
    off_fmt = ">Q" if offset_bits == 64 else ">I"
    while pos < len(data):
        end = data.find(b"\x00", pos)
        if end < 0:
            break
        word = data[pos:end].decode("utf-8", errors="replace")
        pos = end + 1
        if pos + off_size + 4 > len(data):
            break
        offset = struct.unpack(off_fmt, data[pos : pos + off_size])[0]
        pos += off_size
        size = struct.unpack(">I", data[pos : pos + 4])[0]
        pos += 4
        entries.append((word, offset, size))
    return entries


def load_dict_blob(stardict_dir: Path, base: str) -> bytes:
    # .dict.dz is dictzip (gzip-compatible); .dict is plain.
    dz = stardict_dir / f"{base}.dict.dz"
    plain = stardict_dir / f"{base}.dict"
    if dz.exists():
        with gzip.open(dz, "rb") as f:
            return f.read()
    if plain.exists():
        return plain.read_bytes()
    sys.exit(f"missing {base}.dict[.dz]")


class FreeDictHTML(HTMLParser):
    """Convert FreeDict's HTML entries to compact plain text suitable for the
    e-paper screen.

    The format uses <ol><li>...</li></ol> (nestable) for numbered translations,
    <font color="gray"> for IPA, <font color="green"> for POS labels (s/vt/vi…),
    and <br>/<div> for line breaks. We render top-level lists as "1. 2. 3." and
    nested lists as indented "a. b. c." so that two levels stay readable.
    """

    def __init__(self):
        super().__init__()
        self.out: list[str] = []
        self.list_counters: list[int] = []
        self._font_stack: list[str | None] = []

    def _depth(self) -> int:
        return len(self.list_counters)

    def handle_starttag(self, tag, attrs):
        d = dict(attrs)
        if tag == "br":
            self.out.append("\n")
        elif tag == "ol" or tag == "ul":
            self.list_counters.append(0)
            self.out.append("\n")
        elif tag == "li":
            if self.list_counters:
                self.list_counters[-1] += 1
                n = self.list_counters[-1]
                depth = self._depth()
                indent = "  " * (depth - 1)
                if depth == 1:
                    marker = f"{n}. "
                else:
                    # a., b., c. … wraps after z.
                    marker = f"{chr(ord('a') + (n - 1) % 26)}. "
                self.out.append(f"\n{indent}{marker}")
        elif tag == "font":
            color = d.get("color")
            self._font_stack.append(color)
            if color == "green":
                # POS label
                self.out.append("[")
        elif tag == "p":
            self.out.append("\n")

    def handle_endtag(self, tag):
        if tag in ("ol", "ul"):
            if self.list_counters:
                self.list_counters.pop()
        elif tag == "font":
            color = self._font_stack.pop() if self._font_stack else None
            if color == "green":
                self.out.append("]")
        elif tag in ("li", "div", "p"):
            pass

    def handle_data(self, data):
        self.out.append(data)

    def text(self) -> str:
        s = "".join(self.out)
        # Merge an empty top-level marker into its nested sub-list's first
        # item: "1.\n\n  a. amor" -> "1. a. amor". Keeps two-level entries
        # readable on a small screen.
        s = re.sub(r"(\n\d+\.) *\n+  ", r"\1 ", s)
        # Collapse 3+ newlines, trim trailing spaces on each line, and strip.
        s = re.sub(r"[ \t]+\n", "\n", s)
        s = re.sub(r"\n{3,}", "\n\n", s)
        return s.strip()


def html_to_text(html: str) -> str:
    p = FreeDictHTML()
    p.feed(html)
    p.close()
    return p.text()


def slice_definition(blob: bytes, offset: int, size: int, sametype: str) -> str:
    raw = blob[offset : offset + size]
    # sametypesequence=m / t / l / g => single typed entry, no per-entry header.
    # If unset or multi-char we'd need to walk type tags; FreeDict eng-por uses 'h'.
    text = raw.decode("utf-8", errors="replace")
    if sametype.lower() == "h":
        text = html_to_text(text)
    return text.strip()


def encode_newlines(s: str) -> str:
    # Firmware expects literal "\n" (two chars) inside the definition column;
    # actual newlines would break the line-oriented binary search.
    return s.replace("\\", "\\\\").replace("\r\n", "\n").replace("\n", "\\n")


def convert(ifo: Path, out_path: Path) -> int:
    info = parse_ifo(ifo)
    base = ifo.stem  # e.g. "eng-por"
    offset_bits = int(info.get("idxoffsetbits", "32"))
    sametype = info.get("sametypesequence", "m")
    wordcount = int(info.get("wordcount", "0"))
    print(
        f"[4/5] parsing {base} ({wordcount} headwords, "
        f"sametype={sametype!r}, offsetbits={offset_bits})",
        file=sys.stderr,
    )

    idx_path = ifo.with_suffix(".idx")
    if not idx_path.exists():
        gz = ifo.with_suffix(".idx.gz")
        if gz.exists():
            idx_path.write_bytes(gzip.decompress(gz.read_bytes()))
        else:
            sys.exit(f"missing {idx_path}")

    entries = read_idx(idx_path, offset_bits)
    blob = load_dict_blob(ifo.parent, base)

    # Group by lowercase headword so duplicates collapse into one TSV row.
    grouped: dict[str, list[str]] = defaultdict(list)
    original_case: dict[str, str] = {}
    for word, off, size in entries:
        if not word or "\t" in word or "\n" in word:
            continue
        defn = slice_definition(blob, off, size, sametype)
        if not defn:
            continue
        key = word.lower()
        grouped[key].append(defn)
        original_case.setdefault(key, word)

    print(f"[5/5] writing {out_path}", file=sys.stderr)
    written = 0
    with open(out_path, "w", encoding="utf-8", newline="\n") as out:
        for key in sorted(grouped.keys()):
            head = original_case[key]
            defs = grouped[key]
            joined = "\n\n".join(defs) if len(defs) > 1 else defs[0]
            out.write(f"{head}\t{encode_newlines(joined)}\n")
            written += 1
    return written


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("-o", "--output", default="eng-pob.tsv", help="output TSV path")
    p.add_argument("--version", default=DEFAULT_VERSION, help="FreeDict release version")
    p.add_argument("--keep", action="store_true", help="keep the temp download dir")
    p.add_argument("--no-verify", action="store_true", help="skip sha512 check")
    args = p.parse_args()

    url, name = url_for(args.version)
    out_path = Path(args.output).resolve()

    workdir = Path(tempfile.mkdtemp(prefix="freedict-eng-por-"))
    try:
        archive = workdir / name
        download(url, archive)
        if not args.no_verify:
            verify_sha512(archive, url + ".sha512")
        ifo = extract(archive, workdir)
        n = convert(ifo, out_path)
        print(f"done: {n} entries -> {out_path}", file=sys.stderr)
    finally:
        if not args.keep:
            import shutil

            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"workdir kept at {workdir}", file=sys.stderr)


if __name__ == "__main__":
    main()
