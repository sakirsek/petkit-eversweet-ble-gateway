#!/usr/bin/env python3
"""Enforce the two prose rules this project actually cares about.

    python tools/check_style.py

1. No em dashes or en dashes anywhere. Rewrite the sentence with a full stop,
   a colon or a comma instead of swapping in a hyphen, which keeps the same
   style. Use a plain hyphen for ranges: 26-29, not 26 en-dash 29.

2. No non-English text. This project is used by people who do not read Turkish,
   which is what the capture in data/ was originally recorded in.

Both rules already existed for the generated Telegram messages, pinned by test
17 in test_core.py. This extends them to everything the repository publishes.

Exit code 0 = clean, 1 = something to fix.
"""

import gzip
import io
import subprocess
import sys

if hasattr(sys.stdout, "reconfigure"):        # Windows consoles default to cp1252
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# test_core.py contains one em dash on purpose: it is the literal that test 17
# searches the generated messages for.
EXEMPT = {"test_core.py"}

# Built from code points rather than written out, so that this file does not
# trip its own check and need an exemption of its own.
DASHES = {
    chr(0x2014): "em dash",
    chr(0x2013): "en dash",
    chr(0x2015): "horizontal bar",
}

# Letters that give away the languages this repository must not contain. Built
# from code points for the same reason: g-breve, S-cedilla, dotless i and so on.
NON_ENGLISH = {chr(c) for c in (0x011F, 0x011E, 0x015F, 0x015E, 0x0131, 0x0130)}


def tracked_files():
    r = subprocess.run(["git", "ls-files"], capture_output=True, text=True)
    if r.returncode != 0:
        print("[!] not a git repository")
        sys.exit(0)
    return [f for f in r.stdout.split() if f]


def read(path):
    if path.endswith(".gz"):
        try:
            return gzip.open(path, "rt", errors="replace").read()
        except Exception:
            return ""
    try:
        return io.open(path, encoding="utf-8", errors="replace").read()
    except Exception:
        return ""


def main():
    problems = 0

    for path in tracked_files():
        text = read(path)
        if not text:
            continue

        for lineno, line in enumerate(text.splitlines(), 1):
            if path not in EXEMPT:
                for ch, name in DASHES.items():
                    if ch in line:
                        problems += 1
                        print("%s:%d: %s" % (path, lineno, name))
                        print("    %s" % line.strip()[:100])

            bad = NON_ENGLISH.intersection(line)
            if bad:
                problems += 1
                print("%s:%d: non-English letters %s"
                      % (path, lineno, " ".join(sorted(bad))))
                print("    %s" % line.strip()[:100])

    if problems:
        print("\n%d problem(s). See tools/check_style.py for the rules."
              % problems)
        return 1

    print("CLEAN - no em dashes, no non-English text.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
