#!/usr/bin/env python3
"""Enforce the two prose rules this project actually cares about.

    python tools/check_style.py

1. No em dashes or en dashes anywhere. Rewrite the sentence with a full stop,
   a colon or a comma instead of swapping in a hyphen, which keeps the same
   style. Use a plain hyphen for ranges: 26-29, not 26 en-dash 29.

2. No non-English text. This project is used by people who do not read Turkish,
   which is what the capture in data/ was originally recorded in.

The first two already existed for the generated Telegram messages, pinned by
test 17 in test_core.py. This extends them to everything the repository
publishes. The third rule is not about prose at all:

3. No printf conversion in the firmware that the firmware cannot perform.
   sdkconfig sets CONFIG_NEWLIB_NANO_FORMAT, and nano vsnprintf has no 64-bit
   and no floating-point conversions. They do not fail loudly. The format
   escapes into the output as literal text, so "%lld" prints "ld". That cost
   this project twice: once in the cycle timing log, and once in the Telegram
   chat filter, where the id it compared was the string "ld" and so the bot
   threw away every command its owner sent while advancing the cursor past
   them. Cast to long / unsigned long and use %ld / %lu instead.

Exit code 0 = clean, 1 = something to fix.
"""

import gzip
import io
import re
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

# Conversions nano vsnprintf silently passes through. Comments are allowed to
# name them, which is the only reason the checked line has to contain a quote.
NANO_UNSUPPORTED = re.compile(
    r'%[-+ #0-9.*]*(?:ll[diouxX]|[jt][diouxX]|[fFeEgGaA])')


def tracked_files():
    r = subprocess.run(["git", "ls-files"], capture_output=True, text=True)
    if r.returncode != 0:
        print("[!] not a git repository")
        sys.exit(0)
    return [f for f in r.stdout.split() if f]


# Binary formats. Decoding these as text produces byte sequences that look like
# whatever you are searching for, so skip them outright. The .gz capture is the
# exception: it is text once decompressed, and its field names matter.
BINARY_EXT = (".jpg", ".jpeg", ".png", ".gif", ".webp", ".ico", ".pdf",
              ".bin", ".elf", ".dll", ".so", ".dylib", ".zip", ".ttf", ".woff")


def read(path):
    if path.lower().endswith(BINARY_EXT):
        return ""
    if path.endswith(".gz"):
        try:
            return gzip.open(path, "rt", errors="replace").read()
        except Exception:
            return ""
    try:
        return io.open(path, encoding="utf-8", errors="replace").read()
    except Exception:
        return ""


CHR_QUOTE = chr(34)
PCT = chr(37)


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

            if (path.startswith("esp32/") and path.endswith((".c", ".h"))
                    and CHR_QUOTE in line
                    and not line.lstrip().startswith(("*", "//", "/*"))):
                # A doubled percent is a literal one, not a conversion.
                m = NANO_UNSUPPORTED.search(line.replace(PCT + PCT, ""))
                if m:
                    problems += 1
                    print("%s:%d: %s is not in nano vsnprintf, it will print "
                          "as literal text" % (path, lineno, m.group(0)))
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

    print("CLEAN - no em dashes, no non-English text, no unprintable formats.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
