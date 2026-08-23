#!/usr/bin/env python3
"""Refuse to let your own secrets reach GitHub.

Reads the real values out of config.json (and secrets.h if present), then
searches everything git is about to publish for them. Prints only pass/fail -
never the values themselves.

    python tools/check_leaks.py          # check staged + tracked files
    python tools/check_leaks.py --all    # also check the whole git history

Run it before every push. This repo has caught a real leak this way twice: a
template file carrying a real device secret, and an example comment inside a
script whose entire purpose was handling secrets safely.

Exit code 0 = clean, 1 = something leaked.
"""

import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG = os.path.join(ROOT, "config.json")
SECRETS_H = os.path.join(ROOT, "esp32", "main", "secrets.h")

# Config keys whose values must never appear in published files.
SENSITIVE_KEYS = ("secret", "mac", "telegram_token", "telegram_chat_id",
                  "wifi_ssid", "wifi_password")

# Anything shorter than this is too generic to search for safely.
MIN_LEN = 5


def git(args):
    return subprocess.run(["git"] + args, cwd=ROOT,
                          capture_output=True, text=True)


def collect_needles():
    """Return {label: value} of things that must not be published."""
    needles = {}

    if os.path.exists(CONFIG):
        try:
            cfg = json.load(open(CONFIG, encoding="utf-8"))
        except Exception as e:
            print("[!] could not read config.json: %s" % e)
            cfg = {}
        for k in SENSITIVE_KEYS:
            v = cfg.get(k)
            if isinstance(v, (str, int)) and not isinstance(v, bool):
                s = str(v)
                if len(s) >= MIN_LEN:
                    needles[k] = s
        if "secret" in needles:
            # CMD 86 sends the secret forward-padded to 8 bytes; a leak could
            # be in either form.
            needles["secret (padded)"] = "0000" + needles["secret"]

    if os.path.exists(SECRETS_H):
        text = open(SECRETS_H, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r'#define\s+(\w+)\s+"([^"]{%d,})"' % MIN_LEN, text):
            name, val = m.group(1), m.group(2)
            if any(t in name for t in ("SECRET", "PASS", "TOKEN", "MAC", "SSID",
                                       "CHAT")):
                needles["secrets.h:" + name] = val

    return needles


def search(value, history):
    """Return the list of files where value appears."""
    args = ["grep", "-i", "-l", "--fixed-strings", value]
    if history:
        r = git(["grep", "-i", "-l", "--fixed-strings", value,
                 "--", "."] )
        # Search every commit as well.
        r2 = git(["rev-list", "--all"])
        commits = [c for c in r2.stdout.split() if c]
        hits = {l for l in r.stdout.splitlines() if l.strip()}
        for c in commits:
            rc = git(["grep", "-i", "-l", "--fixed-strings", value, c])
            for line in rc.stdout.splitlines():
                if line.strip():
                    hits.add(line.strip())
        return sorted(hits)

    hits = set()
    for scope in (["--cached"], []):
        r = git(["grep", "-i", "-l", "--fixed-strings", value] + scope)
        for line in r.stdout.splitlines():
            if line.strip():
                hits.add(line.strip())
    return sorted(hits)


def main():
    ap = argparse.ArgumentParser(description="Check for leaked secrets.")
    ap.add_argument("--all", action="store_true",
                    help="also scan the entire git history")
    args = ap.parse_args()

    if git(["rev-parse", "--git-dir"]).returncode != 0:
        print("[!] not a git repository - nothing to check")
        return 0

    needles = collect_needles()
    if not needles:
        print("[*] No local secrets found to check against.")
        print("    Nothing to compare - this is not the same as 'safe'.")
        return 0

    print("[*] Checking %d value(s) against %s\n"
          % (len(needles), "the full history" if args.all else "tracked files"))

    bad = 0
    for label, value in sorted(needles.items()):
        hits = search(value, args.all)
        if hits:
            bad += 1
            print("  LEAK   %-22s -> %s" % (label, ", ".join(hits)))
        else:
            print("  clean  %-22s" % label)

    print()
    if bad:
        print("STOP. %d value(s) would be published." % bad)
        print("If any of these has already been pushed, rotating it is safer")
        print("than deleting the commit - forks and caches keep old objects.")
        return 1

    print("CLEAN - nothing sensitive in what git would publish.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
