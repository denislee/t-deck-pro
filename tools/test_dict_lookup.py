#!/usr/bin/env python3
"""Host-side regression test for the firmware dictionary lookup.

Ports ui_dict_lookup() from examples/factory/ui_deckpro_port.cpp byte-for-byte:
same 512-byte dict_buf_t chunk accounting, same dict_strcasecmp_word(), same
convergence and final-scan logic. lookup_fixed = bisection path (line_start
>= hi guard; final scan from lo with no skip, inclusive of the line at hi);
lookup_old = previous buggy one (no guard; final scan skips the partial line at
lo and stops before hi).

lookup_indexed ports the sidecar-index path (see tools/build_dict_index.py):
binary-search the in-memory records for the last key strictly less than the
query, seek to its offset, then run the same linear scan with no hi bound.

Usage: python3 tools/test_dict_lookup.py [tsv] [--samples N] [--seed S]
                                        [--stride N]
       python3 tools/test_dict_lookup.py --expect-old-failures

Exit 0 iff lookup_fixed and lookup_indexed find every expected definition; with
--expect-old-failures it also exits 1 if lookup_old has zero misses, a negative
control proving the test detects the bug.
"""
import argparse, importlib.util, os, random, sys

CHUNK = 512       # DICT_CHUNK_SIZE
LINE_BUF = 512    # char line[512]
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

class Reader:
    """dict_buf_t: chunked reader whose tell() mirrors f.position()."""
    def __init__(self, f):
        self.f, self.chunk = f, b""
        self.chunk_len = self.chunk_pos = self.chunk_start = 0
    def tell(self): return self.chunk_start + self.chunk_pos
    def seek(self, pos):
        self.f.seek(pos); self.chunk_start, self.chunk_pos, self.chunk_len = pos, 0, 0
    def _refill(self):  # advance the chunk window by CHUNK bytes
        self.chunk_start += self.chunk_len; self.chunk_pos = 0
        self.chunk = self.f.read(CHUNK); self.chunk_len = len(self.chunk)
        return self.chunk_len > 0
    def _getc(self):  # next byte, or -1 at EOF
        if self.chunk_pos >= self.chunk_len and not self._refill(): return -1
        c = self.chunk[self.chunk_pos]
        self.chunk_pos += 1
        return c
    def skip_line(self):  # consume through next b'\n' or EOF
        while True:
            c = self._getc()
            if c < 0 or c == 0x0A: return
    def read_line(self):  # no '\n', CRs stripped, <= 511 bytes, None at EOF
        out, any_byte = bytearray(), False
        while True:
            c = self._getc()
            if c < 0: break
            any_byte = True
            if c == 0x0A: break
            if c != 0x0D and len(out) + 1 < LINE_BUF: out.append(c)
        return bytes(out) if any_byte else None

def headword_cmp(line, query):
    """dict_strcasecmp_word: headword of `line` vs `query`, ASCII case-insensitive."""
    i = j = 0
    while i < len(line) and line[i] != 0x09 and j < len(query):
        ca = line[i] + 32 if 65 <= line[i] <= 90 else line[i]   # only A-Z fold
        cb = query[j] + 32 if 65 <= query[j] <= 90 else query[j]
        if ca != cb: return ca - cb
        i, j = i + 1, j + 1
    a_end = i >= len(line) or line[i] == 0x09
    if a_end and j >= len(query): return 0
    return -1 if a_end else 1

def _search(f, size, word, guard, inclusive, skip_first):
    r, lo, hi = Reader(f), 0, size
    while hi - lo > CHUNK:
        prev, mid = (lo, hi), lo + (hi - lo) // 2
        r.seek(mid)
        if mid != 0: r.skip_line()
        line_start = r.tell()
        if guard and line_start >= hi:   # long line straddling mid: never grow hi
            break
        line = r.read_line()
        if line is None:
            hi = line_start
        elif headword_cmp(line, word) < 0:
            lo = r.tell()
        else:
            hi = line_start
        if (lo, hi) == prev:             # stalled (old variant): cannot converge
            break
    r.seek(lo)
    if skip_first and lo != 0: r.skip_line()
    while (r.tell() <= hi) if inclusive else (r.tell() < hi):
        line = r.read_line()
        if line is None: break
        cmp = headword_cmp(line, word)
        if cmp == 0:
            return line.partition(b"\t")[2]   # text after first tab, or b""
        if cmp > 0: break                     # sorted file: target is behind
    return None

def lookup_fixed(f, size, word):
    return _search(f, size, word, guard=True, inclusive=True, skip_first=False)

def lookup_old(f, size, word):
    return _search(f, size, word, guard=False, inclusive=False, skip_first=True)

# ---------------------------------------------------------------------------
# Indexed path (ui_dict_lookup with /dict/eng-pob.idx present)
#
# Records are 32 bytes: u32 LE line offset at +0, 28-byte key at +4 (ASCII-
# lowercased headword, <=27 bytes, NUL-padded). The search takes the LAST
# record whose key is strictly less than the query (unsigned byte-wise, shorter
# prefix is less), seeks to that record's line start and scans forward with the
# normal headword compare -- the same compare the bisection uses, on the full
# untruncated query.
# ---------------------------------------------------------------------------
IDX_KEY_LEN = 28

def load_builder():
    """Import build_dict_index.py from this directory (python3 -I drops cwd)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build_dict_index.py")
    spec = importlib.util.spec_from_file_location("build_dict_index", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

def index_query(word):
    """q = query ASCII-lowercased, truncated to 27 bytes (mirrors firmware)."""
    return word.lower()[:IDX_KEY_LEN - 1]

def lookup_indexed(f, size, idx_records, word):
    """Sidecar-index lookup: binary search the records, then linear-scan.

    `size` is accepted for parity with lookup_fixed(); the records already
    bound the file, so the indexed path never needs the TSV size.
    """
    q = index_query(word)
    lo, hi = 0, len(idx_records)              # first record whose key >= q
    while lo < hi:
        mid = lo + (hi - lo) // 2
        key = idx_records[mid][1].split(b"\x00", 1)[0]
        if key < q:                           # unsigned byte-wise, prefix < longer
            lo = mid + 1
        else:
            hi = mid
    start = idx_records[lo - 1][0] if lo > 0 else 0   # last key < q
    r = Reader(f)
    r.seek(start)
    while True:                               # no hi bound on this path
        line = r.read_line()
        if line is None: return None
        cmp = headword_cmp(line, word)
        if cmp == 0: return line.partition(b"\t")[2]
        if cmp > 0: return None               # sorted file: target is behind

def line_parts(raw):
    """Normalise a streamed raw line the way Reader.read_line() would."""
    line = raw[:-1] if raw.endswith(b"\n") else raw
    line = line.replace(b"\r", b"")[:LINE_BUF - 1]
    return line, line.find(b"\t")

def collect_words(path, samples, seed):
    """First line, last line and a reservoir sample of tabbed-line headwords."""
    rng, reservoir, seen, first, last = random.Random(seed), [], 0, None, None
    with open(path, "rb") as f:
        for raw in f:
            line, tab = line_parts(raw)
            if tab < 0: continue
            word = line[:tab]
            first = word if first is None else first   # keep first and last line
            last, seen = word, seen + 1
            if len(reservoir) < samples: reservoir.append(word)
            elif (k := rng.randrange(seen)) < samples: reservoir[k] = word
    return list(dict.fromkeys(w for w in [first, last] + reservoir if w is not None))

def find_expected(path, words):
    """Definition of the FIRST line per headword (keys are lowercased bytes)."""
    keys, expected = {w.lower() for w in words}, {}
    with open(path, "rb") as f:
        for raw in f:
            line, tab = line_parts(raw)
            if tab >= 0 and line[:tab].lower() in keys:
                expected.setdefault(line[:tab].lower(), line[tab + 1:])
    return expected

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("tsv", nargs="?", default=os.path.join(ROOT, "tools", "eng-pob.tsv"))
    ap.add_argument("--samples", type=int, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--stride", type=int, default=64,
                    help="index stride used for lookup_indexed (default: 64)")
    ap.add_argument("--expect-old-failures", action="store_true")
    args = ap.parse_args()
    size = os.path.getsize(args.tsv)
    words = collect_words(args.tsv, args.samples, args.seed)
    expected = find_expected(args.tsv, words)
    idx_records = load_builder().build_records(args.tsv, args.stride)
    fixed_misses, old_misses, indexed_misses = [], [], []
    with open(args.tsv, "rb") as f:
        for word in words:
            want = expected.get(word.lower())
            if lookup_fixed(f, size, word) != want: fixed_misses.append(word)
            if lookup_old(f, size, word) != want: old_misses.append(word)
            if lookup_indexed(f, size, idx_records, word) != want:
                indexed_misses.append(word)

    show = lambda ws: ", ".join(w.decode("utf-8", "replace") for w in ws[:10])
    print(f"file={args.tsv} size={size} words={len(words)} records={len(idx_records)}")
    print(f"fixed_misses={len(fixed_misses)} old_misses={len(old_misses)} "
          f"indexed_misses={len(indexed_misses)}")
    if fixed_misses: print("fixed misses:", show(fixed_misses))
    if old_misses: print("old misses (up to 10):", show(old_misses))
    if indexed_misses: print("indexed misses (up to 10):", show(indexed_misses))
    rc = 0 if not fixed_misses else 1
    if indexed_misses: rc = 1
    if args.expect_old_failures and not old_misses:
        print("FAIL: lookup_old had zero misses -- test cannot detect the bug"); rc = 1
    return rc

if __name__ == "__main__":
    sys.exit(main())
