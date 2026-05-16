#!/usr/bin/env python3
"""
Convert a Kaikki English-Wiktionary JSONL extract into the sorted TSV consumed
by the T-Deck-Pro factory firmware (see ui_dict_lookup() in
examples/factory/ui_deckpro_port.cpp).

Why Kaikki instead of FreeDict
------------------------------
FreeDict's eng-por has ~15k base headwords and no inflected forms, so common
words like "ran", "went", "mice", "children" all come up as "Word not found"
on the device. Kaikki's English-Wiktionary extract has Portuguese translations
on most senses *and* a `forms` array per entry that lists every inflection
(plurals, verb conjugations, comparatives). We emit the base entry once and a
"see <base>" pointer for each inflected form, so the firmware can resolve
inflected lookups with no code change.

Input
-----
Download the English-language extract from Kaikki:

  https://kaikki.org/dictionary/English/kaikki.org-dictionary-English.jsonl
  (also published as .jsonl.gz; this script accepts either)

Each line is one English sense with optional `translations` (filtered to
lang_code='pt') and `forms` (with `tags` indicating the inflection type).

Usage
-----
  python3 kaikki_to_tsv.py path/to/kaikki-English.jsonl[.gz] -o eng-pob.tsv

Then validate with `validate_dict_tsv.py eng-pob.tsv` and copy to the SD at
/dict/eng-pob.tsv.
"""

import argparse
import gzip
import io
import json
import re
import sys
from collections import defaultdict
from pathlib import Path


# Skip part-of-speech buckets that are noisy or rarely have PT translations.
SKIP_POS = {"character", "symbol", "punct", "infix", "circumfix"}

# Forms whose `tags` include any of these we *don't* emit as redirects — they
# aren't useful as standalone lookups.
SKIP_FORM_TAGS = {
    "romanization",
    "transliteration",
    "alternative",
    "obsolete",
    "archaic",
    "rare",
    "table-tags",
    "class",
    "inflection-template",
}


def open_input(path: Path):
    if path.suffix == ".gz":
        return io.TextIOWrapper(gzip.open(path, "rb"), encoding="utf-8")
    return open(path, "r", encoding="utf-8")


def encode_newlines(s: str) -> str:
    # Firmware expects literal "\n" (two chars) inside the definition column.
    return s.replace("\\", "\\\\").replace("\r\n", "\n").replace("\n", "\\n")


def is_ascii_word(s: str) -> bool:
    # Firmware's tolower() only folds ASCII; non-ASCII headwords can't be
    # binary-searched correctly. Also reject anything with whitespace/control
    # chars or punctuation that would confuse the user on a one-line entry.
    if not s:
        return False
    try:
        s.encode("ascii")
    except UnicodeEncodeError:
        return False
    return all(c.isprintable() and c not in "\t\n" for c in s)


def collect_translations(entry: dict) -> list[str]:
    """Pull Portuguese translations from an entry's senses + top-level translations."""
    out: list[str] = []
    seen: set[str] = set()

    def add(word: str):
        word = word.strip()
        if not word or word in seen:
            return
        seen.add(word)
        out.append(word)

    # Top-level translations array (Wiktionary often has a translations table).
    for t in entry.get("translations", []) or []:
        if t.get("lang_code") in ("pt", "pt-BR", "pt-br"):
            w = t.get("word")
            if w:
                add(w)

    # Per-sense translations, also harvest English glosses for fallback below.
    for sense in entry.get("senses", []) or []:
        for t in sense.get("translations", []) or []:
            if t.get("lang_code") in ("pt", "pt-BR", "pt-br"):
                w = t.get("word")
                if w:
                    add(w)
    return out


def collect_glosses(entry: dict, limit: int = 3) -> list[str]:
    """English glosses, used as a fallback when no PT translation exists."""
    out: list[str] = []
    for sense in entry.get("senses", []) or []:
        for g in sense.get("glosses", []) or []:
            g = re.sub(r"\s+", " ", g).strip()
            if g and g not in out:
                out.append(g)
                if len(out) >= limit:
                    return out
    return out


def collect_forms(entry: dict) -> list[str]:
    """Inflected forms worth emitting as redirects to the base headword."""
    out: list[str] = []
    seen: set[str] = set()
    for f in entry.get("forms", []) or []:
        form = f.get("form")
        tags = set(f.get("tags") or [])
        if not form or form in seen:
            continue
        if tags & SKIP_FORM_TAGS:
            continue
        if not is_ascii_word(form):
            continue
        if form.lower() == entry.get("word", "").lower():
            continue
        seen.add(form)
        out.append(form)
    return out


def format_definition(entry: dict, pt: list[str], glosses: list[str]) -> str:
    pos = entry.get("pos", "")
    header = f"[{pos}] " if pos else ""
    if pt:
        body = ", ".join(pt[:8])
    elif glosses:
        # Mark English fallback so users know there's no PT translation.
        body = "(en) " + " | ".join(glosses)
    else:
        return ""
    return header + body


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("jsonl", help="Kaikki English JSONL (.jsonl or .jsonl.gz)")
    ap.add_argument("-o", "--output", default="eng-pob.tsv", help="output TSV path")
    ap.add_argument(
        "--no-fallback-en",
        action="store_true",
        help="skip entries that have no PT translation (do not emit English glosses)",
    )
    ap.add_argument(
        "--no-inflections",
        action="store_true",
        help="do not emit redirect entries for inflected forms",
    )
    ap.add_argument(
        "--max-defs",
        type=int,
        default=4,
        help="cap stacked definitions per headword (default 4)",
    )
    args = ap.parse_args()

    src = Path(args.jsonl)
    if not src.is_file():
        print(f"error: {src}: not a file", file=sys.stderr)
        return 1

    # headword (lowercase) -> list of definition strings (preserves order seen)
    grouped: dict[str, list[str]] = defaultdict(list)
    original_case: dict[str, str] = {}
    # inflected_form (lowercase) -> set of base headwords pointing at it
    redirects: dict[str, set[str]] = defaultdict(set)
    redirect_case: dict[str, str] = {}

    n_in = n_kept = 0
    with open_input(src) as f:
        for line in f:
            n_in += 1
            line = line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
            except json.JSONDecodeError:
                continue
            word = entry.get("word", "")
            pos = entry.get("pos", "")
            if pos in SKIP_POS:
                continue
            if not is_ascii_word(word):
                continue

            pt = collect_translations(entry)
            glosses = [] if args.no_fallback_en else collect_glosses(entry)
            if not pt and (args.no_fallback_en or not glosses):
                continue

            defn = format_definition(entry, pt, glosses)
            if not defn:
                continue

            key = word.lower()
            if defn not in grouped[key]:
                grouped[key].append(defn)
            original_case.setdefault(key, word)
            n_kept += 1

            if not args.no_inflections:
                for form in collect_forms(entry):
                    fk = form.lower()
                    if fk == key:
                        continue
                    redirects[fk].add(word)
                    redirect_case.setdefault(fk, form)

    # Drop redirects that collide with a real headword — the real entry wins.
    for k in list(redirects.keys()):
        if k in grouped:
            del redirects[k]

    out_path = Path(args.output)
    written = 0
    with open(out_path, "w", encoding="utf-8", newline="\n") as out:
        # Merge real entries and redirects, then sort by lowercase headword
        # in C-locale byte order (matches firmware's binary search exactly).
        all_keys = set(grouped) | set(redirects)
        for key in sorted(all_keys, key=lambda s: s.encode("utf-8")):
            if key in grouped:
                head = original_case[key]
                defs = grouped[key][: args.max_defs]
                joined = "\n\n".join(defs) if len(defs) > 1 else defs[0]
            else:
                head = redirect_case[key]
                bases = sorted(redirects[key])
                joined = "see " + ", ".join(bases[:4])
            out.write(f"{head}\t{encode_newlines(joined)}\n")
            written += 1

    print(
        f"read {n_in} JSONL records, kept {n_kept} senses, "
        f"wrote {written} headwords ({len(grouped)} base + "
        f"{len(redirects)} inflection redirects) to {out_path}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
