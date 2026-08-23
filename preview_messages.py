#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Send every message the gateway can produce to Telegram, as a preview.

Useful for checking wording and rendering on a real phone without waiting for
the conditions to occur. The sample values are real measurements from
2026-08-19, so what you see is what a real day looks like.

FORMATTING RULES (measured on a phone, do not regress):
  - Never use <pre>. Telegram attaches a "COPY CODE" button to it and the
    space-based alignment drifts on mobile. We are not sharing code.
  - Use <blockquote> for grouped detail, <blockquote expandable> for long
    lists (the visit list, the health receipt) so the message stays short.
  - Align with <b>bold labels</b>, not padding spaces. The one exception is a
    pure numeric column (time + duration), where digits are equal width.
  - Never put emoji inside an aligned block: they are double width.
  - One fact per line in the report body. Battery and Filter shared a
    line once and wrapped mid-value on a phone.
  - No em dashes. Use · as a separator, or a full stop.

  python preview_messages.py        # send all
  python preview_messages.py 4 5    # send only these
"""
import json, sys, time, urllib.request, urllib.parse, urllib.error
from pathlib import Path

CFG = json.loads((Path(__file__).resolve().parent / "config.json").read_text(encoding="utf-8"))
TOKEN, CHAT = CFG["telegram_token"], str(CFG["telegram_chat_id"])

SAMPLES = [
    ("startup banner", """🚰 <b>Fountain monitoring active</b>
<blockquote><b>Status:</b> on · Continuous mode
<b>Pump:</b> running · adapter
<b>Battery:</b> 100% (4207 mV)
<b>Filter:</b> 98%</blockquote>
<i>Checking every 5 minutes. From now on I will only write if something is wrong.</i>"""),

    ("thirst alarm 8h", """⚠️ <b>Your cat has not drunk for 8 hours</b>
Last drink <b>10:23</b> · 1 min 7 sec
<blockquote><b>Fountain:</b> on · pump running
<b>Water:</b> present
<b>Filter:</b> 98% · <b>Battery:</b> 100%

Nothing is wrong with the device. That is why I am telling you.</blockquote>"""),

    ("thirst alarm 12h", """🔴 <b>Your cat has not drunk for 12 hours</b>
Last drink <b>10:23</b> · 1 min 7 sec
<blockquote>The device is still healthy. This gap is unusual for this cat. The longest normal gap measured is <b>6 hr 56 min</b>.</blockquote>
<i>I will not write about this again.</i>"""),

    ("thirst alarm cleared", """✅ <b>Your cat drank, alarm cleared</b>
<b>18:41</b> · 1 min 34 sec
<i>Gap: 8 hr 18 min</i>"""),

    ("water empty", """🚨 <b>WATER EMPTY</b>
The reservoir needs refilling.
<i>14:22 · pump stopped</i>"""),

    ("water refilled", """✅ <b>Water refilled</b> · <i>14:47 · pump running</i>"""),

    ("device fault", """⛔ <b>Fountain reported a fault</b>
<i>14:22 · needs checking</i>"""),

    ("unreachable", """📴 <b>Fountain unreachable</b>
No response for 15 minutes (3 consecutive polls failed).
<i>Last seen 14:52 · battery 100% · water present</i>"""),

    ("back online", """✅ <b>Fountain is back</b> · <i>15:20 · 28 min outage</i>"""),

    ("filter threshold", """🧹 <b>Filter dropped below 20%</b>
<i>now 19% · you will need to replace it soon</i>"""),

    ("battery threshold", """🔋 <b>Battery dropped below 30%</b>
<i>now 28% (3612 mV) · adapter not connected</i>"""),

    ("daily summary", """📊 <b>Daily summary · 19.08.2026</b>

🐱 <b>10 drinks</b> · 15 min 44 sec total
Longest drink <b>3 min 40 sec</b> (01:29) · longest gap <b>3 hr 40 min</b>
<blockquote expandable>01:00   43 sec
01:29   3 min 40 sec
01:59   1 min 8 sec
02:08   33 sec
02:23   52 sec
02:43   44 sec
03:48   2 min 11 sec
07:28   2 min 42 sec
07:31   2 min 4 sec
10:23   1 min 7 sec</blockquote>

💧 <b>Pump</b> ran 14 hr 3 min
🔋 <b>Battery</b> 100% (4214 mV)
🧹 <b>Filter</b> 98%
⚙️ Continuous mode · adapter
<blockquote expandable><b>System health</b>
288 / 288 polls succeeded today
23 hr 59 min uptime
4 messages sent today</blockquote>"""),
]


def send(text):
    data = urllib.parse.urlencode({
        "chat_id": CHAT, "text": text,
        "parse_mode": "HTML", "disable_web_page_preview": "true",
    }).encode("utf-8")
    req = urllib.request.Request(
        "https://api.telegram.org/bot%s/sendMessage" % TOKEN, data=data)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.loads(r.read().decode("utf-8")).get("ok")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")[:300]


def main():
    wanted = [int(a) for a in sys.argv[1:] if a.isdigit()]
    for i, (name, text) in enumerate(SAMPLES, 1):
        if wanted and i not in wanted:
            continue
        code, result = send(text)
        print("[%s] %2d. %s  (HTTP %s)" % ("ok " if result is True else "ERR", i, name, code))
        if result is not True:
            print("        %s" % result)
        time.sleep(1.3)


if __name__ == "__main__":
    main()
