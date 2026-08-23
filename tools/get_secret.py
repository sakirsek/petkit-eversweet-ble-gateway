#!/usr/bin/env python3
"""Recover your fountain's BLE auth secret from the PetKit app's own log files.

The PetKit Android app writes the per-device auth secret to its log directory in
plain text. That directory is readable over adb without root. This script finds
it, extracts the secret, and writes it into config.json.

    python tools/get_secret.py

By default the secret is NEVER printed - it goes straight into config.json.
Pass --show if you actually want to see it, and do not paste that anywhere.

Requires: adb on PATH, USB debugging enabled, and the PetKit app having
connected to the fountain at least once today.

See docs/secret.md for the full walkthrough, including what to do when this
script cannot find anything.
"""

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CONFIG = os.path.join(ROOT, "config.json")
EXAMPLE = os.path.join(ROOT, "config.example.json")

# The international app is com.petkit.oversea; the China build uses a different
# id. Both have been seen writing the same log line.
PACKAGES = ["com.petkit.oversea", "com.petkit.android", "com.petkit"]

LOG_DIR_TMPL = "/sdcard/Android/data/{pkg}/cache/logs"

# The line looks like: ... securityCheckAndSyncData secret:1a2b3c4d5e6f ...
# (that value is a dummy). Kept loose on purpose: the surrounding text has
# changed between app versions.
SECRET_RE = re.compile(r"secret[\"'\s:=]{1,4}([0-9a-fA-F]{12,16})")
MAC_RE = re.compile(r"\b((?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2})\b")
NAME_RE = re.compile(r"\b(Petkit[_A-Za-z0-9]*)\b")


def adb(args, serial=None, binary=False):
    """Run an adb command. Returns (returncode, output)."""
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    cmd += args
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=60)
    except FileNotFoundError:
        die("adb was not found on PATH.\n"
            "    Install Android platform-tools and try again:\n"
            "    https://developer.android.com/tools/releases/platform-tools")
    except subprocess.TimeoutExpired:
        return 1, b"" if binary else ""
    out = p.stdout if binary else p.stdout.decode("utf-8", "replace")
    return p.returncode, out


def die(msg):
    print("\n[!] " + msg, file=sys.stderr)
    sys.exit(1)


def pick_device(explicit):
    rc, out = adb(["devices"])
    if rc != 0:
        die("`adb devices` failed. Is the adb server able to start?")
    devices, unauthorized = [], []
    for line in out.splitlines()[1:]:
        parts = line.split()
        if len(parts) < 2:
            continue
        if parts[1] == "device":
            devices.append(parts[0])
        elif parts[1] == "unauthorized":
            unauthorized.append(parts[0])

    if explicit:
        if explicit not in devices:
            die("Device %s is not connected and authorized." % explicit)
        return explicit
    if unauthorized and not devices:
        die("The phone is connected but not authorized.\n"
            "    Unlock it and tap 'Allow USB debugging', then re-run.")
    if not devices:
        die("No phone connected.\n"
            "    Enable Developer options -> USB debugging, plug it in, and\n"
            "    accept the authorization prompt on the phone.")
    if len(devices) > 1:
        die("More than one device connected: %s\n"
            "    Re-run with --serial <id>." % ", ".join(devices))
    return devices[0]


def find_log_dir(serial):
    """Return (package, log_dir) for the installed PetKit app, or (None, None)."""
    rc, out = adb(["shell", "pm", "list", "packages"], serial)
    installed = {l.replace("package:", "").strip() for l in out.splitlines()}

    candidates = [p for p in PACKAGES if p in installed]
    # Fall back to anything that looks like PetKit, in case they rename again.
    candidates += sorted(p for p in installed
                         if "petkit" in p.lower() and p not in candidates)
    if not candidates:
        die("The PetKit app does not appear to be installed on this phone.\n"
            "    The secret can only be recovered from the phone that has\n"
            "    actually paired with your fountain.")

    for pkg in candidates:
        d = LOG_DIR_TMPL.format(pkg=pkg)
        rc, out = adb(["shell", "ls", d], serial)
        if rc == 0 and out.strip() and "No such file" not in out:
            return pkg, d
    return candidates[0], None


def list_logs(serial, log_dir):
    rc, out = adb(["shell", "ls", "-1", log_dir], serial)
    if rc != 0:
        return []
    names = [n.strip() for n in out.splitlines() if n.strip().endswith(".log")]
    # Filenames are YYYY-MM-DD.log, so a reverse sort is newest-first.
    return sorted(names, reverse=True)


def scan(serial, log_dir, names, want_all):
    """Search newest-first. Returns (secret, mac, name, source_file)."""
    secret = mac = devname = None
    src = None
    for n in names:
        path = "%s/%s" % (log_dir, n)
        rc, blob = adb(["exec-out", "cat", path], serial, binary=True)
        if rc != 0 or not blob:
            continue
        text = blob.decode("utf-8", "replace")

        if not secret:
            m = SECRET_RE.search(text)
            if m:
                secret = m.group(1).lower()
                src = n
        if not mac:
            # Ignore all-zero and broadcast addresses that show up in logs.
            for cand in MAC_RE.findall(text):
                if cand.lower() not in ("00:00:00:00:00:00",
                                        "ff:ff:ff:ff:ff:ff"):
                    mac = cand.upper()
                    break
        if not devname:
            m = NAME_RE.search(text)
            if m:
                devname = m.group(1)

        if secret and mac and not want_all:
            break
    return secret, mac, devname, src


def normalise(secret):
    """The device secret is 6 bytes. CMD 86 forward-pads it to 8."""
    s = secret.lower()
    if len(s) == 16 and s.startswith("0000"):
        return s[4:]
    return s


def write_config(secret, mac, show):
    if os.path.exists(CONFIG):
        with open(CONFIG, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    elif os.path.exists(EXAMPLE):
        with open(EXAMPLE, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    else:
        cfg = {}

    cfg["secret"] = secret
    if mac:
        cfg["mac"] = mac

    with open(CONFIG, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2, ensure_ascii=False)
        f.write("\n")

    print("[+] Wrote config.json")
    print("    secret : %s" % (secret if show else
                               "%d hex chars (hidden - pass --show to see it)"
                               % len(secret)))
    print("    mac    : %s" % (mac or "NOT FOUND - fill this in yourself"))


def main():
    ap = argparse.ArgumentParser(
        description="Recover the PetKit BLE auth secret from the app's logs.")
    ap.add_argument("--serial", help="adb device id, if more than one phone")
    ap.add_argument("--show", action="store_true",
                    help="print the secret (do not share the output)")
    ap.add_argument("--all-logs", action="store_true",
                    help="scan every log file, not just until a match")
    args = ap.parse_args()

    print("[*] Looking for a phone...")
    serial = pick_device(args.serial)
    print("[*] Using device %s" % serial)

    pkg, log_dir = find_log_dir(serial)
    if not log_dir:
        die("Found the app (%s) but its log directory is empty or unreadable.\n"
            "    Open the PetKit app, let it connect to the fountain, wait a\n"
            "    few seconds, then run this again.\n"
            "    If it still fails, see docs/secret.md for the manual route."
            % pkg)
    print("[*] App: %s" % pkg)

    names = list_logs(serial, log_dir)
    if not names:
        die("No .log files in %s\n"
            "    Open the app and connect to the fountain first." % log_dir)
    print("[*] Scanning %d log file(s), newest first..." % len(names))

    secret, mac, devname, src = scan(serial, log_dir, names, args.all_logs)

    if not secret:
        die("No secret found in the logs.\n"
            "    Most likely the app has not talked to the fountain recently.\n"
            "    Open it, connect to the fountain, then re-run.\n"
            "    If your app version stopped logging it, docs/secret.md lists\n"
            "    the fallback options.")

    secret = normalise(secret)
    if len(secret) != 12:
        die("Found something that looks wrong: %d hex chars, expected 12.\n"
            "    Please open an issue with the length (never the value)."
            % len(secret))

    print("[+] Secret found in %s" % src)
    if devname:
        print("[+] Device name seen in logs: %s" % devname)

    write_config(secret, mac, args.show)
    print("\n    This secret identifies your fountain. Treat it like a "
          "password:\n    do not paste it into issues, screenshots or chats.")


if __name__ == "__main__":
    main()
