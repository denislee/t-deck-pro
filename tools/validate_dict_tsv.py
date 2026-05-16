#!/usr/bin/env python3
"""
Validate a dictionary TSV against the format consumed by the T-Deck-Pro
factory firmware (ui_dict_lookup() in examples/factory/ui_deckpro_port.cpp).

The firmware does a byte-offset binary search and expects:
  * One entry per line: headword<TAB>definition\n
  * Sorted ASCII case-insensitively by headword (matches the C tolower()
    comparison the binary search uses — anything else and lookups silently
    miss valid entries)
  * Headwords are ASCII (the firmware's tolower() only folds ASCII; non-ASCII
    bytes won't compare correctly during binary search)
  * No real newlines inside a row — multi-line definitions encode "\\n"
    (literal backslash + 'n') which the firmware decodes when rendering
  * Lines fit in the firmware's 512-byte read buffer (511 usable). Longer
    lines are silently truncated by dict_line_read(), corrupting the
    headword used for comparison.

Usage:
  python3 validate_dict_tsv.py path/to/eng-pob.tsv

Exit code: 0 on clean validation, 1 if any error is found. Warnings (long
lines, duplicate headwords) do not fail the run on their own.
"""

import argparse
import sys
from pathlib import Path

LINE_BUF_LIMIT = 511  # firmware's dict_line_read() buf_size is 512 incl. NUL


def ascii_lower(s: str) -> str:
    # Mirror the firmware's per-byte tolower(): only ASCII A-Z folds; other
    # bytes pass through unchanged. We compare the encoded bytes for sort
    # checking so non-ASCII headwords (which we reject) still produce a
    # deterministic ordering relative to ASCII ones.
    return "".join(chr(c + 32) if 0x41 <= c <= 0x5A else chr(c) for c in s.encode("utf-8"))


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("tsv", help="path to the TSV file to validate")
    p.add_argument(
        "--max-errors",
        type=int,
        default=20,
        help="stop printing after this many errors (default 20)",
    )
    args = p.parse_args()

    path = Path(args.tsv)
    if not path.is_file():
        print(f"error: {path}: not a file", file=sys.stderr)
        return 1

    raw = path.read_bytes()
    errors: list[str] = []
    warnings: list[str] = []

    if raw.startswith(b"\xef\xbb\xbf"):
        errors.append("file starts with a UTF-8 BOM (firmware reads bytes raw)")
        raw = raw[3:]

    if b"\r" in raw:
        errors.append("file contains carriage returns (\\r); use Unix line endings")

    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as e:
        errors.append(f"file is not valid UTF-8: {e}")
        for line in errors:
            print(f"ERROR: {line}", file=sys.stderr)
        return 1

    lines = text.split("\n")
    # Tolerate exactly one trailing empty (file ends with newline).
    if lines and lines[-1] == "":
        lines.pop()

    headwords: list[str] = []
    seen_keys: dict[str, int] = {}

    for i, line in enumerate(lines, start=1):
        if line == "":
            errors.append(f"line {i}: empty line")
            continue
        if line.count("\t") < 1:
            errors.append(f"line {i}: missing TAB separator")
            continue
        head, _, defn = line.partition("\t")
        if not head:
            errors.append(f"line {i}: empty headword")
            continue
        if "\t" in head:
            errors.append(f"line {i}: headword contains TAB")
        # Headword should be ASCII so the firmware's tolower() works.
        try:
            head.encode("ascii")
        except UnicodeEncodeError:
            non_ascii = [c for c in head if ord(c) > 0x7F][:3]
            errors.append(
                f"line {i}: non-ASCII headword {head!r} "
                f"(first non-ASCII chars: {''.join(non_ascii)!r}) — "
                f"firmware tolower() won't case-fold this and binary search will skip it"
            )
        if not defn:
            errors.append(f"line {i}: empty definition for {head!r}")

        # Real newlines can't appear here (we already split on \n). But check
        # that "\\n" was used where multi-line was intended. We can't fully
        # detect bad encodings, but a literal backslash followed by anything
        # other than '\\' or 'n' is suspicious.
        # (Skip: too noisy, since legitimate "\\path" content is allowed.)

        line_bytes = len(line.encode("utf-8"))
        if line_bytes > LINE_BUF_LIMIT:
            warnings.append(
                f"line {i}: {line_bytes} bytes exceeds firmware buffer ({LINE_BUF_LIMIT}); "
                f"definition will be truncated for {head!r}"
            )

        key = ascii_lower(head)
        if key in seen_keys:
            warnings.append(
                f"line {i}: duplicate headword {head!r} "
                f"(first seen on line {seen_keys[key]})"
            )
        else:
            seen_keys[key] = i
        headwords.append(head)

    # Verify sort order matches the firmware's byte-wise case-insensitive
    # comparison. We sort by ascii_lower(head) and compare to file order.
    sorted_keys = sorted((ascii_lower(h), idx) for idx, h in enumerate(headwords))
    out_of_order = 0
    first_bad: tuple[int, str, str] | None = None
    for new_pos, (_, orig_idx) in enumerate(sorted_keys):
        if new_pos != orig_idx:
            out_of_order += 1
            if first_bad is None:
                # Find an adjacent pair that's actually inverted in file order.
                for j in range(len(headwords) - 1):
                    if ascii_lower(headwords[j]) > ascii_lower(headwords[j + 1]):
                        first_bad = (j + 1, headwords[j], headwords[j + 1])
                        break
    if out_of_order:
        a = first_bad if first_bad else (0, "?", "?")
        errors.append(
            f"sort order: {out_of_order} entries out of order; "
            f"first inversion near line {a[0]}: {a[1]!r} > {a[2]!r} "
            f"(must be ASCII case-insensitive byte-sorted, e.g. `LC_ALL=C sort -f`)"
        )

    # Report.
    shown = 0
    for e in errors:
        if shown >= args.max_errors:
            print(f"... {len(errors) - shown} more errors suppressed", file=sys.stderr)
            break
        print(f"ERROR: {e}", file=sys.stderr)
        shown += 1
    shown = 0
    for w in warnings:
        if shown >= args.max_errors:
            print(f"... {len(warnings) - shown} more warnings suppressed", file=sys.stderr)
            break
        print(f"warn:  {w}", file=sys.stderr)
        shown += 1

    print(
        f"\n{path}: {len(headwords)} entries, "
        f"{len(errors)} error(s), {len(warnings)} warning(s)",
        file=sys.stderr,
    )
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
