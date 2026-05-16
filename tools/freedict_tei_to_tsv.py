#!/usr/bin/env python3
"""
Convert a FreeDict TEI P5 dictionary (e.g. eng-por) into the sorted TSV format
expected by the T-Deck-Pro factory firmware's dictionary screen.

Output format (one entry per line):
    headword<TAB>definition

`definition` uses the literal two-character sequence "\\n" (backslash + 'n') for
line breaks; the firmware decodes these back into real newlines when rendering.
The file is sorted ASCII case-insensitively by headword, which is what
ui_dict_lookup() in examples/factory/ui_deckpro_port.cpp binary-searches over.

Usage:
    python3 freedict_tei_to_tsv.py path/to/eng-por.tei > eng-pob.tsv

Source: download eng-por from https://freedict.org/ (or the FreeDict GitHub
repo freedict/fd-dictionaries). Look for the .tei file inside the release
tarball.
"""

import re
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict

TEI_NS = "{http://www.tei-c.org/ns/1.0}"


def text_of(elem):
    """All text inside an element, joined and whitespace-collapsed."""
    if elem is None:
        return ""
    return re.sub(r"\s+", " ", "".join(elem.itertext())).strip()


def encode_newlines(s):
    # Firmware expects literal "\n" (two chars) inside the definition column;
    # actual newlines would break the line-oriented binary search.
    return s.replace("\\", "\\\\").replace("\n", "\\n")


def parse_entry(entry):
    """Return (headword, definition_text) or None if entry is unusable."""
    # Headword: <form><orth>...</orth></form>. FreeDict sometimes has multiple
    # <orth> (variants); take the first.
    orth = entry.find(f"{TEI_NS}form/{TEI_NS}orth")
    headword = text_of(orth)
    if not headword:
        return None

    # Part of speech (optional): <gramGrp><pos>n</pos></gramGrp>
    pos = text_of(entry.find(f"{TEI_NS}gramGrp/{TEI_NS}pos"))

    senses = entry.findall(f"{TEI_NS}sense")
    if not senses:
        return None

    lines = []
    for i, sense in enumerate(senses, start=1):
        # Translations: <cit type="trans"><quote>...</quote></cit>
        trans = []
        for cit in sense.findall(f"{TEI_NS}cit"):
            if cit.get("type") != "trans":
                continue
            q = text_of(cit.find(f"{TEI_NS}quote"))
            if q:
                trans.append(q)

        # Optional gloss/definition text inside the sense.
        defn = text_of(sense.find(f"{TEI_NS}def"))

        if not trans and not defn:
            continue

        prefix = f"{i}. " if len(senses) > 1 else ""
        body = ", ".join(trans)
        if defn:
            body = f"{body} — {defn}" if body else defn
        lines.append(prefix + body)

    if not lines:
        return None

    header = f"[{pos}] " if pos else ""
    definition = header + "\n".join(lines)
    return headword, definition


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)

    tree = ET.parse(sys.argv[1])
    root = tree.getroot()

    # Group by lowercase headword so duplicate entries (different POS, etc.)
    # collapse into a single TSV row with stacked definitions.
    grouped = defaultdict(list)
    original_case = {}

    for entry in root.iter(f"{TEI_NS}entry"):
        parsed = parse_entry(entry)
        if not parsed:
            continue
        headword, definition = parsed
        key = headword.lower()
        grouped[key].append(definition)
        # Preserve the first-seen original casing for display.
        original_case.setdefault(key, headword)

    # Sort case-insensitively; keys are already lowercase.
    out = sys.stdout
    for key in sorted(grouped.keys()):
        head = original_case[key]
        # Skip headwords containing tab/newline — would corrupt the format.
        if "\t" in head or "\n" in head:
            continue
        defs = grouped[key]
        joined = "\n\n".join(defs) if len(defs) > 1 else defs[0]
        out.write(f"{head}\t{encode_newlines(joined)}\n")


if __name__ == "__main__":
    main()
