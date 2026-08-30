#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Register the bot's command list with Telegram.

This is the same thing BotFather's /setcommands does, done through the API so
it lives in version control instead of in a chat log. It is purely cosmetic on
the client side: it gives the chat a menu button and autocomplete when you type
a slash. It does NOT change how commands are delivered, and the gateway never
needs to know it was run.

There is one command and it answers everything, so the menu is one line long.
The gateway does not read the text anyway: any message from the configured
chat gets the same reply. The menu exists so the answer is one tap away and so
nobody has to remember what to type.

This description is the only user-facing documentation most people will ever
read, so keep it short and honest.

  python tools/set_commands.py          # register
  python tools/set_commands.py --show   # print what Telegram currently has
  python tools/set_commands.py --clear  # remove the list
"""
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

CFG = json.loads((Path(__file__).resolve().parent.parent / "config.json")
                 .read_text(encoding="utf-8"))
TOKEN = CFG["telegram_token"]

COMMANDS = [
    ("status", "Everything at once: what is wrong, today, yesterday, health"),
]


def api(method, payload=None):
    url = "https://api.telegram.org/bot%s/%s" % (TOKEN, method)
    data = json.dumps(payload).encode("utf-8") if payload is not None else None
    req = urllib.request.Request(url, data=data)
    if data:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return {"ok": False, "error": e.read().decode("utf-8", "replace")[:300]}


def main():
    if "--show" in sys.argv:
        got = api("getMyCommands")
        for c in got.get("result", []):
            print("  /%-8s %s" % (c["command"], c["description"]))
        if not got.get("result"):
            print("  (none registered)")
        return 0

    if "--clear" in sys.argv:
        print("clear:", api("setMyCommands", {"commands": []}))
        return 0

    payload = {"commands": [{"command": c, "description": d}
                            for c, d in COMMANDS]}
    res = api("setMyCommands", payload)
    if not res.get("ok"):
        print("FAILED:", res)
        return 1
    for c, d in COMMANDS:
        print("  /%-8s %s" % (c, d))
    print("\nRegistered. The menu button appears in the chat immediately; the "
          "gateway answers on its next poll.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
