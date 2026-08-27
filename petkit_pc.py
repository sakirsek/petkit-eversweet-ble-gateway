#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PetKit fountain gateway - PC driver.

Every decision is made by the C core (core/petkit_core.c). This file is only
transport: BLE (bleak), Telegram (urllib), the wall clock and logging. When we
move to the ESP32-C3 this file is replaced by NimBLE + esp_http_client and the
core is carried over unchanged.

Poll sequence (verified against the vendor app's own traffic):
    connect -> 213 ping -> 86 auth -> 210 state -> 212 pending counter
            -> if pending: 80 to start the stream, collect 68 packets
            -> DO NOT ACKNOWLEDGE (no 67/69) -> disconnect

Not acknowledging is essential. If we ack, the device marks those records
synced and the PetKit phone app can never show them again. Reading without
acking was verified on the device: the records are returned, the pending
counter stays put, and the app still displays them. The cost is that the same
records keep arriving, so the core deduplicates on timestamp.

  python petkit_pc.py           # run the gateway
  python petkit_pc.py --once    # single poll, print the result, exit
"""
import asyncio, json, sys, time, urllib.request, urllib.parse, urllib.error
import datetime
from pathlib import Path
from bleak import BleakClient, BleakScanner

import pk

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

BASE = Path(__file__).resolve().parent
LOGDIR = BASE / "logs"; LOGDIR.mkdir(exist_ok=True)
CFG = json.loads((BASE / "config.json").read_text(encoding="utf-8"))

TX = "0000aaa2-0000-1000-8000-00805f9b34fb"
RX = "0000aaa1-0000-1000-8000-00805f9b34fb"

MAC = CFG["mac"]
_raw_secret = bytes.fromhex(CFG["secret"])
SECRET8 = bytes(8 - len(_raw_secret)) + _raw_secret   # left-pad with zeros to 8
POLL_SEC = int(CFG.get("poll_minutes", 5) * 60)
# CMD 80 setStreamSetting(window=32, mtu=158) - byte-identical to the app's frame
STREAM_START = pk.frame_build(80, 1, bytes.fromhex("000000200000009e"), 11)

# Every command this gateway may transmit. See send() for why this is a
# whitelist and what the excluded commands do. Documented in docs/protocol.md.
ALLOWED_CMDS = frozenset({
    213,   # ping / deviceId + serial
    86,    # verify secret
    210,   # state snapshot
    212,   # pending-record counter
    80,    # start history stream
})


def log(line, tag="   "):
    s = "%s [%s] %s" % (datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), tag, line)
    print(s, flush=True)
    with open(LOGDIR / ("gateway-%s.log" % datetime.date.today().strftime("%Y%m%d")),
              "a", encoding="utf-8") as f:
        f.write(s + "\n")


def jlog(obj):
    obj["ts"] = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    with open(LOGDIR / ("gateway-%s.jsonl" % datetime.date.today().strftime("%Y%m%d")),
              "a", encoding="utf-8") as f:
        f.write(json.dumps(obj, ensure_ascii=False) + "\n")


# ------------------------------------------------------------------ telegram
class Telegram:
    def __init__(self):
        self.token = CFG.get("telegram_token", "")
        self.chat = str(CFG.get("telegram_chat_id", ""))
        self.enabled = bool(CFG.get("telegram_enabled", True) and self.token and self.chat)
        self.q = asyncio.Queue()
        self.errors = 0
        if not self.enabled:
            log("Telegram disabled - messages will only be logged", "TG!")

    def send(self, text):
        log("TELEGRAM >> " + text.replace("\n", " | ")[:300], "TG ")
        if self.enabled:
            self.q.put_nowait(text)

    def _post(self, text):
        data = urllib.parse.urlencode({
            "chat_id": self.chat, "text": text,
            "parse_mode": "HTML", "disable_web_page_preview": "true",
        }).encode("utf-8")
        req = urllib.request.Request(
            "https://api.telegram.org/bot%s/sendMessage" % self.token, data=data)
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status

    async def worker(self):
        while True:
            text = await self.q.get()
            for attempt in range(3):
                try:
                    await asyncio.to_thread(self._post, text)
                    break
                except Exception as e:
                    self.errors += 1
                    log("Telegram error (%d/3): %s: %s"
                        % (attempt + 1, type(e).__name__, e), "TG!")
                    await asyncio.sleep(3 * (attempt + 1))
            self.q.task_done()


# --------------------------------------------------------------- single poll
async def poll():
    """One poll round. Returns (state, [visits], error_message)."""
    dev = await BleakScanner.find_device_by_address(MAC, timeout=15.0)
    if not dev:
        # Most common cause by far: the PetKit phone app is connected. The
        # fountain has a single connection slot and stops advertising entirely
        # while the app holds it, so this looks identical to "powered off".
        return None, [], "device not found while scanning"

    frames = []

    def on_notify(_h, data):
        f = pk.frame_parse(bytes(data))
        if f:
            frames.append(f)

    client = BleakClient(dev, timeout=25.0)
    try:
        await client.connect()
        await client.start_notify(RX, on_notify)

        async def send(raw, wait):
            # Whitelist, not blacklist: nothing outside this set can leave,
            # even by accident. CMD 73 permanently rewrites the deviceId and
            # secret and can lock the PetKit app out of the fountain for good;
            # CMD 67/69 acknowledge the history stream and make the app lose
            # those records forever. 220/221/222/225/226 change device state
            # and this gateway is read-only by design.
            cmd = raw[3]
            if cmd not in ALLOWED_CMDS:
                raise RuntimeError(
                    "refusing to send CMD %d - not on the read-only whitelist"
                    % cmd)
            await client.write_gatt_char(TX, raw, response=False)
            await asyncio.sleep(wait)

        await send(pk.frame_build(213, 1, bytes([0, 0]), 1), 1.5)   # wake
        await send(pk.frame_build(86, 1, SECRET8, 2), 2.0)          # authenticate
        if not [f for f in frames if f["cmd"] == 86 and f["payload"][:1] == b"\x01"]:
            return None, [], "no authentication reply"

        await send(pk.frame_build(210, 1, bytes([0, 0]), 3), 1.5)   # state
        mark = len(frames)
        await send(pk.frame_build(212, 1, b"", 4), 2.0)             # pending counter

        # Prefer a 42-byte CMD 230 push (it carries the proximity sensor);
        # fall back to the 30-byte CMD 210 reply.
        state = None
        for f in frames:
            if f["cmd"] == 230 and len(f["payload"]) >= 42:
                state = pk.state_decode(f["payload"])
        if state is None:
            for f in frames:
                if f["cmd"] == 210 and len(f["payload"]) >= 26:
                    state = pk.state_decode(f["payload"])

        pending = 0
        for f in frames[mark:]:
            if f["cmd"] == 212 and not f["stream"]:
                pending = max(0, pk.sync_pending(f["payload"]))

        visits = []
        if pending > 0:
            mark2 = len(frames)
            await send(STREAM_START, 4.0)
            blob = b"".join(f["payload"] for f in frames[mark2:]
                            if f["stream"] and f["cmd"] == 68)
            visits = pk.history_decode(blob)
            # No ack (CMD 67/69) on purpose - see the module docstring.
            jlog({"type": "history", "pending": pending, "decoded": len(visits),
                  "raw": blob.hex()})

        if state is None:
            return None, visits, "could not decode state"
        return state, visits, None

    except Exception as e:
        return None, [], "%s: %s" % (type(e).__name__, e)
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass


# --------------------------------------------------------------- main loop
async def loop(core, tg):
    # main() already ran one poll. The fountain stops advertising for a few
    # seconds right after a disconnect, so polling again immediately just
    # records a spurious failure. Wait out a full interval first.
    await asyncio.sleep(POLL_SEC)
    while True:
        t0 = time.time()
        state, visits, err = await poll()
        now = int(time.time())

        if err:
            core.poll_fail(now)
            log("poll failed: %s" % err, "!!!")
            jlog({"type": "poll", "ok": False, "error": err})
        else:
            core.poll_ok(state, visits, now)
            log("poll ok  %s  | %d new records, %d visits today"
                % (state, len(visits), core.visit_count), "PLL")
            jlog({"type": "poll", "ok": True, "new": len(visits),
                  "today": core.visit_count, "battery": state.battery_pct,
                  "filter": state.filter_pct, "no_water": state.warn_no_water,
                  "fault": state.warn_fault})

        core.tick(now)
        for m in core.take():
            tg.send(m)

        await asyncio.sleep(max(5.0, POLL_SEC - (time.time() - t0)))


async def main():
    tg = Telegram()
    cfg = pk.default_cfg()
    cfg.poll_sec = POLL_SEC
    cfg.thirst_sec = int(CFG.get("thirst_hours", 6) * 3600)
    cfg.thirst_escalate_sec = int(CFG.get("thirst_escalate_hours", 12) * 3600)
    cfg.normal_gap_sec = int(CFG.get("normal_gap_minutes", 220) * 60)
    hh, mm = str(CFG.get("report_time", "23:59")).split(":")
    cfg.report_hour, cfg.report_min = int(hh), int(mm)
    # A FIXED offset, deliberately - the core never calls localtime()/tzset(),
    # so the PC build and the ESP32-C3 build agree to the second. This is
    # correct for Turkey, which has been permanently UTC+3 since 2016; a
    # country that still observes DST would need real timezone handling here.
    cfg.tz_offset_sec = int(float(CFG.get("timezone_offset_hours", 3)) * 3600)
    cfg.report_settle_min = int(CFG.get("report_settle_minutes", 3))

    core = pk.Core(cfg, int(time.time()))
    log("=" * 70)
    log("gateway started | poll %d min | thirst %d h | report %s UTC%+g"
        % (POLL_SEC // 60, cfg.thirst_sec // 3600,
           CFG.get("report_time", "23:59"), cfg.tz_offset_sec / 3600.0), "RUN")

    state, visits, err = await poll()
    now = int(time.time())
    if err:
        core.poll_fail(now)
        log("first poll failed: %s" % err, "!!!")
    else:
        core.poll_ok(state, visits, now)
        core.drop()   # swallow startup alarms; only send the banner below
        tg.send("🚰 <b>Fountain monitoring active</b>\n"
                "<blockquote><b>Status:</b> %s · %s mode\n"
                "<b>Pump:</b> %s · %s\n"
                "<b>Battery:</b> %d%% (%d mV)\n"
                "<b>Filter:</b> %d%%</blockquote>\n"
                "<i>Checking every %d minutes. From now on I will only write "
                "if something is wrong.</i>"
                % ("on" if state.power else "off",
                   "Intermittent" if state.mode == 2 else "Continuous",
                   "running" if state.pump_running else "stopped",
                   "adapter" if state.psu else "battery",
                   state.battery_pct, state.battery_mv, state.filter_pct,
                   POLL_SEC // 60))
        log("initial state: %s | %d visits" % (state, core.visit_count), "PLL")

    await asyncio.gather(tg.worker(), loop(core, tg))


async def once():
    state, visits, err = await poll()
    if err:
        print("ERROR:", err)
        return 1
    print("state  :", state)
    print("history:", len(visits), "record(s)")
    for ts, sec in visits:
        print("   %s  %d sec"
              % (datetime.datetime.fromtimestamp(ts).strftime("%H:%M:%S"), sec))
    return 0


if __name__ == "__main__":
    if "--once" in sys.argv:
        sys.exit(asyncio.run(once()))
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log("stopped by user", "RUN")
