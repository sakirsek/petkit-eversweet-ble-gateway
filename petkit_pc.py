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
            -> disconnect

CMD 67 is not a cleanup command, it is the stream's flow control, and that
distinction cost this project a day of wrong alarms. The fountain hands over
one 512-byte chunk - 85 whole records - and then asks for an acknowledgement
every three seconds and sends nothing further until it gets one. A reader that
never acknowledges therefore works perfectly right up to 85 unread records and
then freezes on that same chunk forever, which is exactly what happened on
29.08.2026.

Answering it destroys nothing. Measured 30.08.2026 by reading the pending
counter on the same connection either side of three acknowledgements: 48 bytes
before, 48 after, while the request seq advanced 241 -> 242 -> 243. Consuming
is CMD 69's job, and this gateway never sends CMD 69, so the phone app keeps
every record. We answer only when the read is short, which on a normal day is
never - the whole backlog arrives in the first chunk.

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


# How long to keep collecting stream packets. The old code waited a flat 4 s
# and took whatever had turned up, which was fine while everything fitted in
# one chunk and silently wrong the moment it did not. We now wait for the byte
# count CMD 212 promised, and this is only the ceiling on that wait.
STREAM_TIMEOUT = 20.0
# The fountain re-asks for its chunk acknowledgement every 3 s, so going this
# long with no new bytes means it has nothing more to give us.
STREAM_QUIET = 7.0


# --------------------------------------------------------------- single poll
async def poll():
    """One poll round. Returns (state, [visits], hist, error_message).

    `hist` is pk.HIST_OK when the stream delivered every byte CMD 212 said was
    pending, and pk.HIST_SHORT when it delivered fewer. That distinction is the
    whole point: without it the core cannot tell "the cat did not drink" from
    "I was not shown the drinks", and on 29.08.2026 it spent a day reporting
    the first while living the second."""
    dev = await BleakScanner.find_device_by_address(MAC, timeout=15.0)
    if not dev:
        # Most common cause by far: the PetKit phone app is connected. The
        # fountain has a single connection slot and stops advertising entirely
        # while the app holds it, so this looks identical to "powered off".
        return None, [], pk.HIST_SHORT, "device not found while scanning"

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
            # CMD 69 retires the history records and makes the app lose them
            # forever. 220/221/222/225/226 change device state and this
            # gateway is read-only by design. CMD 67 is a reply rather than a
            # request and goes out through ack_chunk() instead.
            cmd = raw[3]
            if cmd not in ALLOWED_CMDS:
                raise RuntimeError(
                    "refusing to send CMD %d - not on the read-only whitelist"
                    % cmd)
            await client.write_gatt_char(TX, raw, response=False)
            await asyncio.sleep(wait)

        async def ack_chunk(seq):
            # The one frame outside ALLOWED_CMDS this driver transmits. It is a
            # reply (typ 2) echoing the fountain's own request seq rather than a
            # request of ours, and it consumes nothing: measured 30.08.2026,
            # pending 48 bytes before three acknowledgements and 48 after, with
            # the request seq advancing 241 -> 242 -> 243 each time. CMD 69,
            # which does consume, stays forbidden.
            await client.write_gatt_char(
                TX, pk.frame_build(67, 2, b"\x01", seq), response=False)

        await send(pk.frame_build(213, 1, bytes([0, 0]), 1), 1.5)   # wake
        await send(pk.frame_build(86, 1, SECRET8, 2), 2.0)          # authenticate
        if not [f for f in frames if f["cmd"] == 86 and f["payload"][:1] == b"\x01"]:
            return None, [], pk.HIST_SHORT, "no authentication reply"

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

        visits, hist, acks = [], pk.HIST_OK, 0
        if pending > 0:
            mark2 = len(frames)
            await client.write_gatt_char(TX, STREAM_START, response=False)

            def collected():
                return b"".join(f["payload"] for f in frames[mark2:]
                                if f["stream"] and f["cmd"] == 68)

            blob = b""
            acked_at, last_event = -1, time.monotonic()
            deadline = last_event + STREAM_TIMEOUT
            while time.monotonic() < deadline:
                await asyncio.sleep(0.25)
                got = collected()
                if len(got) > len(blob):
                    blob, last_event = got, time.monotonic()

                # Everything CMD 212 promised is here. Nothing left to unlock,
                # so do not answer the request at all: on a normal day the
                # whole backlog is one chunk and this exits immediately.
                if len(blob) >= pending:
                    break

                # Short, so the fountain is holding the rest behind its flow
                # control. Answer it, once per chunk and only after that
                # chunk's bytes are in hand: the request repeats every 3 s and
                # answering a repeat could advance its cursor past a chunk we
                # never received.
                if blob and len(blob) > acked_at:
                    req = [f for f in frames[mark2:]
                           if f["cmd"] == 67 and f["typ"] == 1 and not f["stream"]]
                    if req:
                        await ack_chunk(req[-1]["seq"])
                        acked_at, acks = len(blob), acks + 1
                        last_event = time.monotonic()
                        continue

                if time.monotonic() - last_event > STREAM_QUIET:
                    break

            visits = pk.history_decode(blob)
            hist = pk.HIST_OK if len(blob) >= pending else pk.HIST_SHORT
            jlog({"type": "history", "pending": pending, "received": len(blob),
                  "decoded": len(visits), "acks": acks,
                  "short": hist == pk.HIST_SHORT, "raw": blob.hex()})
            if hist == pk.HIST_SHORT:
                log("history SHORT: %d of %d bytes (%d records of %d)"
                    % (len(blob), pending, len(visits), pending // 6), "!!!")

        if state is None:
            return None, visits, hist, "could not decode state"
        return state, visits, hist, None

    except Exception as e:
        return None, [], pk.HIST_SHORT, "%s: %s" % (type(e).__name__, e)
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
        state, visits, hist, err = await poll()
        now = int(time.time())

        if err:
            core.poll_fail(now)
            log("poll failed: %s" % err, "!!!")
            jlog({"type": "poll", "ok": False, "error": err})
        else:
            core.poll_ok(state, visits, now, hist)
            log("poll ok  %s  | %d new records, %d visits today%s"
                % (state, len(visits), core.visit_count,
                   " [SHORT READ]" if hist == pk.HIST_SHORT else ""), "PLL")
            jlog({"type": "poll", "ok": True, "new": len(visits),
                  "today": core.visit_count, "battery": state.battery_pct,
                  "filter": state.filter_pct, "no_water": state.warn_no_water,
                  "fault": state.warn_fault, "short": hist == pk.HIST_SHORT})

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

    state, visits, hist, err = await poll()
    now = int(time.time())
    if err:
        core.poll_fail(now)
        log("first poll failed: %s" % err, "!!!")
    else:
        core.poll_ok(state, visits, now, hist)
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
    state, visits, hist, err = await poll()
    if err:
        print("ERROR:", err)
        return 1
    print("state  :", state)
    print("history:", len(visits), "record(s)%s"
          % ("  [SHORT READ - the fountain held records back]"
             if hist == pk.HIST_SHORT else ""))
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
