#!/usr/bin/env python3
"""Convert a BW message dump (TSV: index, MSGxxxx, flag, text) to pokeemerald .string text.

Usage: python3 convert_text.py dump.txt ProtagHouse2F >> data/maps/ProtagHouse2F/scripts.inc

Conversions:
  \\c -> \\p (new page)        \\r\\n -> \\n
  [VAR TRNAME(n)] -> {PLAYER}   [VAR PKNAME(n)] -> {STR_VAR_1}
  [CENTER] removed
  2nd+ line break inside a page -> \\l (Emerald boxes show 2 lines; widen/heighten if you want 3)
Empty messages are skipped. Label names: <Prefix>_Text_<index>.
"""
import re
import sys


def convert(text):
    t = text.strip()
    t = t.replace("\\r\\n", "\\n")
    t = re.sub(r"\[VAR TRNAME\(\d+\)\]", "{PLAYER}", t)
    t = re.sub(r"\[VAR PKNAME\(\d+\)\]", "{STR_VAR_1}", t)
    t = t.replace("[CENTER]", "")
    t = t.replace("\\c", "\\p")
    t = t.replace('"', '\\"')
    t = t.replace("\\p\\n", "\\p")                 # "\c\n" in the dump = page break, drop the stray newline
    t = re.sub(r"(\\p|\\n)+$", "", t)          # no trailing page/line break

    pieces = re.split(r"(\\n|\\p)", t)           # text, delim, text, delim, ...
    lines, line_in_page = [], 0
    i = 0
    while i < len(pieces):
        body = pieces[i]
        delim = pieces[i + 1] if i + 1 < len(pieces) else ""
        if delim == "\\n":
            line_in_page += 1
            if line_in_page >= 2:
                delim = "\\l"
        elif delim == "\\p":
            line_in_page = 0
        lines.append(body + delim)
        i += 2
    return lines


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path, prefix = sys.argv[1], sys.argv[2]
    out = []
    for raw in open(path, encoding="utf-8"):
        parts = raw.rstrip("\n").split("\t")
        if len(parts) < 4 or not parts[3].strip():
            continue
        lines = convert(parts[3])
        out.append(f"{prefix}_Text_{int(parts[0])}:")
        for n, ln in enumerate(lines):
            end = "$" if n == len(lines) - 1 else ""
            out.append(f'\t.string "{ln}{end}"')
        out.append("")
    print("\n".join(out))


if __name__ == "__main__":
    main()
