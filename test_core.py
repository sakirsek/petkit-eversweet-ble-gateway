#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Regression tests for the C core.

Every expected value below was measured live against the real device, not
invented:
  - frames come from the PetKit app's own debug log
  - state payloads are real CMD 230 pushes captured overnight
  - history records are real streams pulled from the fountain
  - the visit timeline was cross-checked against the app's history screen
"""
import sys, datetime, calendar

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

import pk

PASS = FAIL = 0


def eq(name, got, want):
    global PASS, FAIL
    if got == want:
        PASS += 1
        print("  [ok]   %s" % name)
    else:
        FAIL += 1
        print("  [FAIL] %s\n         want: %r\n         got : %r" % (name, want, got))


def ok(name, cond):
    eq(name, bool(cond), True)


# =====================================================================
print("\n=== 1. FRAME BUILDING (byte-identical to the app's traffic) ===")
# app sent: fafcfdd501000000fb   -> cmd 213, empty payload, seq 0
eq("CMD 213 empty payload", pk.frame_build(213, 1, b"", 0).hex(), "fafcfdd501000000fb")
# cmd 86, 8-byte secret, seq 1. The secret below is a DUMMY - the real one is
# device-specific and lives in config.json, which is gitignored. What this test
# pins is the framing, not the value: a 6-byte secret is forward-padded to 8.
eq("CMD 86 secret", pk.frame_build(86, 1, bytes.fromhex("00001a2b3c4d5e6f"), 1).hex(),
   "fafcfd560101080000001a2b3c4d5e6ffb")
# app sent: fafcfdd4010a0000fb   -> cmd 212, empty, seq 10
eq("CMD 212 sync request", pk.frame_build(212, 1, b"", 10).hex(), "fafcfdd4010a0000fb")
# app sent: fafcfd50010b0800000000200000009efb
# payload is BE32(32) + BE32(158): stream window and MTU
eq("CMD 80 stream start",
   pk.frame_build(80, 1, bytes.fromhex("000000200000009e"), 11).hex(),
   "fafcfd50010b0800000000200000009efb")

print("\n=== 2. FRAME PARSING ===")
f = pk.frame_parse(bytes.fromhex("fafcfd560201010001fb"))
eq("CMD 86 reply cmd", f["cmd"], 86)
eq("CMD 86 reply payload", f["payload"].hex(), "01")
ok("CMD 86 is not a stream frame", not f["stream"])

# history stream frame - FA FC FE header, 9-byte header
f = pk.frame_parse(bytes.fromhex("fafcfe440300010c006a85720300136a8572800027fb"))
ok("stream flag set", f["stream"])
eq("stream cmd", f["cmd"], 68)
eq("stream length", f["len"], 12)
eq("stream payload", f["payload"].hex(), "6a85720300136a8572800027")

print("\n=== 3. HISTORY RECORD DECODING ===")
# app log: [{"stayTime":19,"workTime":1787130371},{"stayTime":39,"workTime":1787130496}]
recs = pk.history_decode(bytes.fromhex("6a85720300136a8572800027"))
eq("two records decoded", len(recs), 2)
eq("record 1", recs[0], (1787130371, 19))
eq("record 2", recs[1], (1787130496, 39))
# record pulled live from the device: 2026-08-19 13:05:31, 61 sec
eq("live record", pk.history_decode(bytes.fromhex("6a857feb003d"))[0], (1787133931, 61))

print("\n=== 4. PENDING COUNTER (byte-offset trap) ===")
eq("nothing pending", pk.sync_pending(bytes.fromhex("0100000000")), 0)
eq("12 bytes = 2 records", pk.sync_pending(bytes.fromhex("010000000c")), 12)
eq("18 bytes = 3 records", pk.sync_pending(bytes.fromhex("0100000012")), 18)
eq("6 bytes = 1 record", pk.sync_pending(bytes.fromhex("0100000006")), 6)

print("\n=== 5. STATE DECODING (real device pushes) ===")
# 02:21:03 push - no cat at the fountain
CLEAR = "01010102000000000000005b7464010000210c0014ef106f64009b06a306030300190e10010200000001"
s = pk.state_decode(bytes.fromhex(CLEAR))
eq("power", s.power, 1)
eq("mode (1=continuous)", s.mode, 1)
eq("psu (2=adapter)", s.psu, 2)
eq("pump running", s.pump_running, 1)
eq("no water warning clear", s.warn_no_water, 0)
eq("filter pct", s.filter_pct, 100)
eq("battery pct", s.battery_pct, 100)
eq("battery mV", s.battery_mv, 4207)
eq("supply mV", s.supply_mv, 5359)
eq("detect clear", s.detect, 0)
eq("proximity baseline", s.prox_baseline, 1691)
eq("proximity raw", s.prox_raw, 1699)

# 02:43:14 push - cat present, ADC saturated
CAT = "010101020000000000000060a764010000263f0214f8106f64009b06ff0f030300190e10010200000001"
s2 = pk.state_decode(bytes.fromhex(CAT))
eq("cat detected", s2.detect, 2)
eq("proximity saturated", s2.prox_raw, 4095)
ok("above the measured 1996 threshold", s2.prox_raw > 1996)

print("\n=== 6. THIRST ALARM LIFECYCLE ===")


def ts(h, m, day=19):
    """Local wall clock on 2026-08-19 (UTC+3) -> unix seconds."""
    return calendar.timegm(datetime.datetime(2026, 8, day, h, m).timetuple()) - 3 * 3600


cfg = pk.default_cfg()
# Pinned here, not inherited from pk_cfg_defaults. The production threshold is a
# tunable that moves as we measure more of this particular cat: it went 6 h -> 8 h
# on 21 Aug, after a 6 h 56 min gap that turned out to be perfectly normal. These
# tests check the alarm MECHANISM, so they fix the threshold themselves. Otherwise
# a tuning change silently rewrites what every assertion below is asserting.
cfg.thirst_sec = 6 * 3600
cfg.normal_gap_sec = 3 * 3600 + 40 * 60      # longest normal gap known at the time


def advance(core, state, start, end, visits=(), step=300):
    """Poll every 5 minutes like the real gateway does, collecting messages.
    Skipping the polls would (correctly) trip the unreachable alarm."""
    out = []
    t = start
    while t <= end:
        core.poll_ok(state, list(visits), t)
        core.tick(t)
        out += core.take()
        t += step
    return out


core = pk.Core(cfg, ts(10, 0))
healthy = pk.state_decode(bytes.fromhex(CLEAR))
VISITS = [(ts(10, 23), 67)]                  # last drink at 10:23

core.poll_ok(healthy, VISITS, ts(10, 25))
core.take()                                   # swallow startup output

eq("silent before the threshold",
   advance(core, healthy, ts(10, 30), ts(16, 0), VISITS), [])
m = advance(core, healthy, ts(16, 5), ts(19, 0), VISITS)
eq("exactly one message at 6h", len(m), 1)
ok("6h wording", "not drunk for 6 hours" in m[0])
ok("last drink time shown", "10:23" in m[0])
eq("no repeat in between", advance(core, healthy, ts(19, 5), ts(22, 0), VISITS), [])
m = advance(core, healthy, ts(22, 5), ts(23, 30), VISITS)
eq("exactly one message at 12h", len(m), 1)
ok("12h wording", "not drunk for 12 hours" in m[0])
ok("normal gap quoted", "3 hr 40 min" in m[0])

core.poll_ok(healthy, VISITS + [(ts(23, 40), 94)], ts(23, 45))
m = core.take()
eq("alarm cleared message", len(m), 1)
ok("clear wording", "alarm cleared" in m[0])
eq("thirst level reset", core.thirst_level, 0)

print("\n=== 7. THIRST ALARM SUPPRESSED WHILE THE DEVICE IS UNHEALTHY ===")
core2 = pk.Core(cfg, ts(10, 0))
dry = pk.state_decode(bytes.fromhex(CLEAR))
dry.warn_no_water = 1
core2.poll_ok(pk.state_decode(bytes.fromhex(CLEAR)), VISITS, ts(10, 25))
core2.take()
core2.poll_ok(dry, [], ts(10, 30))
ok("water empty reported", any("WATER EMPTY" in x for x in core2.take()))
m = advance(core2, dry, ts(10, 35), ts(17, 30), VISITS)
eq("no thirst alarm while water is empty", [x for x in m if "not drunk" in x], [])

print("\n=== 8. DEDUPLICATION (we never ack, so records repeat) ===")
core3 = pk.Core(cfg, ts(10, 0))
core3.poll_ok(healthy, [(ts(10, 23), 67), (ts(11, 5), 40)], ts(11, 10)); core3.take()
core3.poll_ok(healthy, [(ts(10, 23), 67), (ts(11, 5), 40)], ts(11, 15)); core3.take()
eq("repeats not double counted", core3.visit_count, 2)
core3.poll_ok(healthy, [(ts(10, 23), 67), (ts(11, 5), 40), (ts(12, 0), 55)], ts(12, 5))
core3.take()
eq("genuinely new record added", core3.visit_count, 3)

print("\n=== 9. UNREACHABLE DETECTION ===")
core4 = pk.Core(cfg, ts(10, 0))
core4.poll_ok(healthy, [(ts(9, 0), 50)], ts(10, 0)); core4.take()
for i in range(3):
    core4.poll_fail(ts(10, 5 + i * 5))
core4.tick(ts(10, 16))
m = core4.take()
eq("unreachable message", len(m), 1)
ok("unreachable wording", "unreachable" in m[0])
core4.poll_ok(healthy, [], ts(10, 30))
ok("recovery message", any("is back" in x for x in core4.take()))

print("\n=== 10. DAILY REPORT (real 19 Aug timeline) ===")
REAL = [(1, 0, 43), (1, 29, 220), (1, 59, 68), (2, 8, 33), (2, 23, 52),
        (2, 43, 44), (3, 48, 131), (7, 28, 162), (7, 31, 124), (10, 23, 67)]
core5 = pk.Core(cfg, ts(0, 30))
core5.poll_ok(healthy, [(ts(h, m), s) for h, m, s in REAL], ts(10, 30))
core5.take()
core5.report(ts(23, 59))
r = core5.take()
eq("report produced", len(r), 1)
report = r[0]
ok("10 drinks", "10 drinks" in report)
ok("longest drink", "3 min 40 sec" in report)
ok("longest gap", "3 hr 40 min" in report)
ok("expandable visit list", "<blockquote expandable>" in report)
ok("health receipt", "System health" in report)
ok("no <pre> (it adds a COPY CODE button)", "<pre>" not in report)
ok("under the Telegram limit", len(report.encode("utf-8")) < 4096)
print("\n--- rendered report ---")
print(report)

print("\n=== 11. MIDNIGHT BOUNDARY (the bug that ate the 19 Aug report) ===")
# On 19-20 Aug 2026 the gateway ran for 18 h 37 min, polled 223/223 times
# successfully, and never sent the daily report. The report window is one
# minute wide (23:59) and the polls landed on :58 and :03 - so no tick ever
# observed 23:59, and pk_poll_ok rolled the day over before pk_tick could
# notice one was owed. These tests drive the core exactly the way the real
# driver does, with the real timestamps from that night.


def T(day, h, m, sec=0):
    """Local wall clock in Aug 2026 (UTC+3) -> unix seconds."""
    return calendar.timegm(
        datetime.datetime(2026, 8, day, h, m, sec).timetuple()) - 3 * 3600


# The 16 records the fountain actually handed us that night.
AUG19 = [(T(19, 17, 28, 38), 64), (T(19, 17, 33, 35), 20),
         (T(19, 18, 18, 31), 112), (T(19, 18, 33, 4), 55),
         (T(19, 21, 35, 37), 87), (T(19, 22, 1, 38), 70)]
AUG20 = [(T(20, 2, 1, 54), 133), (T(20, 2, 14, 8), 49),
         (T(20, 2, 18, 18), 38), (T(20, 2, 25, 6), 303),
         (T(20, 4, 4, 32), 71), (T(20, 4, 42, 45), 11),
         (T(20, 7, 18, 56), 241), (T(20, 10, 10, 20), 102),
         (T(20, 11, 19, 8), 58), (T(20, 11, 35, 23), 36)]


class Fountain:
    """The device as it really behaves: because we never acknowledge the
    stream it replays its ENTIRE buffer on every poll, including previous
    days, and it writes each record ~2 minutes after the visit ends."""

    def __init__(self, records, write_delay=120):
        self.records = sorted(records)
        self.write_delay = write_delay

    def replay(self, now):
        return [(ts, sec) for ts, sec in self.records
                if now >= ts + sec + self.write_delay]


def run(core, fountain, start, end, step=300, state=None):
    """Drive the core the way petkit_pc.loop() does: poll_ok, then tick."""
    state = state if state is not None else healthy
    out, t = [], start
    while t <= end:
        core.poll_ok(state, fountain.replay(t), t)
        core.tick(t)
        out += core.take()
        t += step
    return out


fountain = Fountain(AUG19 + AUG20)
core6 = pk.Core(cfg, T(19, 17, 23))
core6.poll_ok(healthy, [], T(19, 17, 23))
core6.take()                                   # swallow the startup output

# 17:28 -> 00:03 on the real cadence: polls at :03/:08/.../:58, never 23:59.
msgs = run(core6, fountain, T(19, 17, 28, 31), T(20, 0, 3, 32))
reports = [m for m in msgs if "Daily summary" in m]
eq("report survives a poll cadence that never sees 23:59", len(reports), 1)
if not reports:
    print("\nRESULT: %d passed, %d failed (aborted - no report to inspect)"
          % (PASS, FAIL))
    sys.exit(1)
aug19 = reports[0]
ok("dated to the day it covers, not the day it was sent",
   "19.08.2026" in aug19)
ok("counts only that day's drinks", "<b>6 drinks</b>" in aug19)
ok("first drink listed", "17:28" in aug19)
ok("last drink listed", "22:01" in aug19)
ok("next day's drinks excluded", "02:01" not in aug19)
eq("nothing else was sent that evening",
   [m for m in msgs if "Daily summary" not in m], [])

# Carry on through 20 Aug. The device is still replaying 19 Aug's records on
# every poll - they must not be counted again.
msgs = run(core6, fountain, T(20, 0, 8, 32), T(21, 0, 3, 32))
reports = [m for m in msgs if "Daily summary" in m]
eq("exactly one report per day", len(reports), 1)
aug20 = reports[0]
ok("second report dated correctly", "20.08.2026" in aug20)
ok("yesterday's replayed records not re-counted", "<b>10 drinks</b>" in aug20)
ok("yesterday's drinks not listed", "17:28" not in aug20)
ok("longest drink is that day's", "5 min 3 sec" in aug20)
ok("health receipt is per-day, not lifetime", "polls succeeded today" in aug20)

print("\n--- the report that should have arrived at midnight ---")
print(aug19)

print("\n=== 12. THE DAY CLOSES AFTER MIDNIGHT, NEVER BEFORE ===")
# This run's polls land on :59 - the minute that used to fire a second, earlier
# report path. It was deleted on 23 Aug; see test 19 for what it cost. Nothing
# may go out while the day is still running, because the fountain has not
# written the last few minutes of it yet.
core7 = pk.Core(cfg, T(19, 17, 23))
core7.poll_ok(healthy, [], T(19, 17, 23)); core7.take()
f2 = Fountain(AUG19)
m = run(core7, f2, T(19, 17, 29), T(19, 23, 59), step=300)
eq("nothing is sent while 19 Aug is still running",
   [x for x in m if "Daily summary" in x], [])
m = run(core7, f2, T(20, 0, 4), T(20, 2, 0), step=300)
rep12 = [x for x in m if "Daily summary" in x]
eq("the day closes exactly once, after the settle wait", len(rep12), 1)
ok("dated to the day it covers, not the day it was sent",
   rep12 and "19.08.2026" in rep12[0])

print("\n=== 13. THIRST ALARM SURVIVES MIDNIGHT ===")
# Last drink 22:01 on 19 Aug, nothing after -> the 6 h alarm is due at 04:01
# on 20 Aug. The old rollover emptied the visit list at midnight, so the
# alarm could still fire but printed the duration as "0 sec".
core8 = pk.Core(cfg, T(19, 20, 0))
core8.poll_ok(healthy, [], T(19, 20, 0)); core8.take()
f3 = Fountain([(T(19, 22, 1, 38), 70)])
m = run(core8, f3, T(19, 22, 5), T(20, 5, 0))
thirst = [x for x in m if "not drunk" in x]
eq("alarm fires across the day boundary", len(thirst), 1)
ok("last drink time survives the rollover", "22:01" in thirst[0])
ok("duration survives the rollover", "1 min 10 sec" in thirst[0])
eq("last_drink_ts preserved", core8.last_drink_ts, T(19, 22, 1, 38))

print("\n=== 14. OUT-OF-ORDER RECORDS DO NOT CORRUPT THE GAP ===")
# An unsigned subtraction on an out-of-order pair used to underflow into a
# multi-thousand-hour "longest gap".
core9 = pk.Core(cfg, T(19, 0, 30))
core9.poll_ok(healthy, [(T(19, 12, 0), 60), (T(19, 9, 0), 60),
                        (T(19, 15, 0), 60)], T(19, 16, 0))
core9.take()
core9.report(T(19, 23, 59))
rep = core9.take()[0]
ok("gap computed from sorted records", "longest gap <b>3 hr 0 min</b>" in rep)
ok("visit list rendered in order", rep.index("09:00") < rep.index("12:00"))

print("\n=== 15. A SHORTENED VISIT LIST SAYS SO ===")
# The message buffer is 1600 bytes. A silently truncated list reads exactly
# like a complete one, which is the failure mode this whole project is
# designed to avoid.
core10 = pk.Core(cfg, T(19, 0, 10))
many = [(T(19, 0, 20) + i * 600, 30) for i in range(120)]
core10.poll_ok(healthy, many, T(19, 20, 30))
core10.take()
core10.report(T(19, 23, 59))
rep = core10.take()[0]
ok("truncation is announced", "more not shown" in rep)
ok("headline count is still the true total", "<b>120 drinks</b>" in rep)
ok("still within the Telegram limit", len(rep.encode("utf-8")) < 4096)

print("\n=== 16. THE CONFIGURED OFFSET REALLY DRIVES THE DAY BOUNDARY ===")
# The core never calls localtime(); every local time it prints or compares
# comes from cfg.tz_offset_sec, so that one number decides which day a drink
# belongs to. 01:00 in Turkey is 22:00 the previous day in UTC.
drink = T(20, 1, 0)

tr = pk.default_cfg(); tr.tz_offset_sec = 3 * 3600
core11 = pk.Core(tr, T(19, 20, 0))
core11.poll_ok(healthy, [(drink, 90)], T(20, 2, 0)); core11.take()
core11.report(T(20, 2, 0))
r_tr = core11.take()[0]
ok("UTC+3: that drink lands on 20 Aug", "20.08.2026" in r_tr)
ok("UTC+3: printed as 01:00", "01:00" in r_tr)

utc = pk.default_cfg(); utc.tz_offset_sec = 0
core12 = pk.Core(utc, T(19, 20, 0))
core12.poll_ok(healthy, [(drink, 90)], T(20, 2, 0)); core12.take()
core12.report(T(19, 23, 0))
r_utc = core12.take()[0]
ok("UTC: the same drink lands on 19 Aug", "19.08.2026" in r_utc)
ok("UTC: printed as 22:00", "22:00" in r_utc)

print("\n=== 17. MESSAGE FORMATTING RULES HOLD FOR EVERY MESSAGE ===")
# Both rules come from reading the messages on a real phone: an em dash reads
# badly in this context, and Battery + Filter on one line wrapped mid-value.
core13 = pk.Core(cfg, T(19, 8, 0))
dry2 = pk.state_decode(bytes.fromhex(CLEAR)); dry2.warn_no_water = 1
broken = pk.state_decode(bytes.fromhex(CLEAR)); broken.warn_fault = 1
FIRST = [(T(19, 8, 30), 70)]

produced = []
core13.poll_ok(healthy, FIRST, T(19, 9, 0)); core13.take()
core13.poll_ok(dry2, FIRST, T(19, 9, 5));    produced += core13.take()   # empty
core13.poll_ok(healthy, FIRST, T(19, 9, 10)); produced += core13.take()  # refilled
core13.poll_ok(broken, FIRST, T(19, 9, 15)); produced += core13.take()   # fault
core13.poll_ok(healthy, FIRST, T(19, 9, 20)); produced += core13.take()  # cleared
for i in range(3):
    core13.poll_fail(T(19, 9, 25 + i * 5))
core13.tick(T(19, 9, 41));                   produced += core13.take()   # unreachable
core13.poll_ok(healthy, FIRST, T(19, 9, 50)); produced += core13.take()  # is back
produced += advance(core13, healthy, T(19, 10, 0), T(19, 15, 0), FIRST)  # thirst 6h
core13.poll_ok(healthy, FIRST + [(T(19, 15, 5), 80)], T(19, 15, 10))
produced += core13.take()                                               # cleared
core13.report(T(19, 23, 59));                produced += core13.take()   # summary

eq("every message type produced", len(produced) >= 9, True)
bad = [m for m in produced if "—" in m]
eq("no em dash in any message", bad, [])

report2 = [m for m in produced if "Daily summary" in m][0]
batt = [l for l in report2.split("\n") if "<b>Battery</b>" in l][0]
ok("Filter is not squeezed onto the Battery line", "Filter" not in batt)
ok("Filter has its own line",
   any(l.startswith("\U0001F9F9") for l in report2.split("\n")))

print("\n=== 18. A 23:59 DRINK STILL MAKES IT INTO ITS OWN REPORT ===")
# Measured on the device: a record is written ~40 s after the visit ends. So a
# drink at 23:59 is not readable until just after midnight. If the day is
# closed at the first tick after midnight, that drink lands in NO report - it
# belongs to the finished day, whose summary has already gone out.
LATE = [(T(19, 23, 59, 0), 40)]
late_fountain = Fountain(LATE, write_delay=60)   # readable from 00:00:40


def run_phase(core, fountain, start, end, step=300):
    """Same as run(), for a poll schedule that lands just after midnight."""
    return run(core, fountain, start, end, step)


settled = pk.default_cfg()
settled.normal_gap_sec = cfg.normal_gap_sec
eq("settle wait defaults to 3 minutes", settled.report_settle_min, 3)

core14 = pk.Core(settled, T(19, 20, 0, 30))
core14.poll_ok(healthy, [], T(19, 20, 0, 30)); core14.take()
# Polls land at :00:30 - the first one after midnight is 00:00:30, before the
# record exists. The day must not be closed yet.
m = run_phase(core14, late_fountain, T(19, 23, 55, 30), T(20, 0, 0, 30))
eq("day not closed at 00:00:30", [x for x in m if "Daily summary" in x], [])
m = run_phase(core14, late_fountain, T(20, 0, 5, 30), T(20, 0, 20, 30))
rep = [x for x in m if "Daily summary" in x]
eq("report goes out once the settle wait passes", len(rep), 1)
ok("dated 19 Aug", "19.08.2026" in rep[0])
ok("the 23:59 drink is in it", "23:59" in rep[0])
ok("counted, and singular", "<b>1 drink</b>" in rep[0])

# Contrast: settle 0 reproduces the loss.
eager = pk.default_cfg()
eager.report_settle_min = 0
core15 = pk.Core(eager, T(19, 20, 0, 30))
core15.poll_ok(healthy, [], T(19, 20, 0, 30)); core15.take()
m = run_phase(core15, Fountain(LATE, write_delay=60),
              T(19, 23, 55, 30), T(20, 0, 20, 30))
rep0 = [x for x in m if "Daily summary" in x]
eq("settle 0 still reports", len(rep0), 1)
ok("but the 23:59 drink is missing from it", "23:59" not in rep0[0])
ok("it reports an empty day", "No drinks recorded" in rep0[0])

print("\n=== 19. THE POLL PHASE MUST NOT DECIDE WHAT THE REPORT CONTAINS ===")
# Found on 23 Aug, from the first daily report the ESP32 ever sent by itself:
# it arrived at 23:59 instead of just after midnight. That is a second report
# path, and it quietly cancelled the first.
#
# The day used to be closable from two places: the rollover past
# report_settle_min, and a same-day branch that fired when a tick happened to
# land inside the 23:59 minute. The second one stamped last_report_day, which
# made the rollover's `last_report_day < day_no` guard false, so the settled
# report never ran - report_settle_min became inert on exactly the nights it
# was written for. Test 18 above never caught it because its polls land on
# :00:30 and so never observe 23:59.
#
# The nastiest part is who it hits. 86400 is an exact multiple of poll_sec and
# both drivers sleep the remainder, so a run holds its phase. A gateway is not
# unlucky one night in five - one gateway start in five is unlucky FOREVER,
# losing the end of every single day until something restarts it. The board
# that sent the 22.08 report was in exactly that band.
#
# So this test does not assert a report time. It asserts the property that
# actually matters: the same day, seen through two poll phases, must produce
# the same report.
LATE_ONLY = [(T(19, 20, 30, 0), 90), (T(19, 23, 59, 0), 40)]


def report_for(phase_sec):
    """Run one full day at a given poll phase, return the 19 Aug summary."""
    c = pk.Core(settled, T(19, 20, 0, 0) + phase_sec)
    c.poll_ok(healthy, [], T(19, 20, 0, 0) + phase_sec); c.take()
    m = run(c, Fountain(LATE_ONLY, write_delay=60),
            T(19, 20, 5, 0) + phase_sec, T(20, 0, 30, 0) + phase_sec)
    r = [x for x in m if "Daily summary" in x]
    return r


# 240 s past the five-minute mark puts a tick at 23:59:00 - the bad band.
cursed = report_for(240)
# 30 s past puts every tick on :00:30 - the band test 18 already covers.
lucky = report_for(30)

eq("cursed phase still sends exactly one report", len(cursed), 1)
eq("lucky phase still sends exactly one report", len(lucky), 1)
if cursed and lucky:
    ok("lucky phase keeps the 23:59 drink", "23:59" in lucky[0])
    ok("cursed phase keeps it too", "23:59" in cursed[0])
    ok("both phases count the same drinks", "<b>2 drinks</b>" in cursed[0]
       and "<b>2 drinks</b>" in lucky[0])
    eq("the poll phase changes nothing about the report",
       cursed[0].split("System health")[0], lucky[0].split("System health")[0])

# And the day must still close exactly once, with no second copy after
# midnight and none the next night either.
c16 = pk.Core(settled, T(19, 20, 0, 0) + 240)
c16.poll_ok(healthy, [], T(19, 20, 0, 0) + 240); c16.take()
m = run(c16, Fountain(LATE_ONLY, write_delay=60),
        T(19, 23, 40, 0) + 240, T(21, 0, 30, 0) + 240)
allrep = [x for x in m if "Daily summary" in x]
eq("two nights of ticks produce exactly two reports", len(allrep), 2)
ok("first is dated 19 Aug", "19.08.2026" in allrep[0])
ok("second is dated 20 Aug", "20.08.2026" in allrep[1])
ok("the 23:59 drink is not double-counted onto 20 Aug",
   "23:59" not in allrep[1])

print("\n=== 20. THE DAILY RECEIPT COUNTS THE MESSAGES IT ACTUALLY SENT ===")
# Every report the gateway has ever sent said "0 messages sent today" - the
# 26.08.2026 one said it after sending four (water empty, refilled, thirst
# alarm, alarm cleared). day_messages was reset at midnight and printed in the
# receipt, but no code path ever incremented it.
#
# The second half of this test is the trap that comes with fixing the first:
# the startup poll raises alarms about a state we have only just learned
# about, and the platform layer throws them away. Counting at queue time would
# make the receipt claim a message nobody ever received.
count_cfg = pk.default_cfg()
count_cfg.normal_gap_sec = cfg.normal_gap_sec
dry3 = pk.state_decode(bytes.fromhex(CLEAR)); dry3.warn_no_water = 1
FIRST_DRINK = [(T(19, 7, 30), 60)]
# a drink every two hours, so nothing here is about the thirst alarm
STEADY = FIRST_DRINK + [(T(19, h, 15), 45) for h in range(9, 24, 2)]

core17 = pk.Core(count_cfg, T(19, 8, 0))
core17.poll_ok(dry3, FIRST_DRINK, T(19, 8, 0))
eq("the startup poll really did raise an alarm", len(core17.messages()), 1)
core17.drop()                                        # swallowed, never sent

core17.poll_ok(healthy, FIRST_DRINK, T(19, 8, 5))    # refilled
core17.poll_ok(dry3,    FIRST_DRINK, T(19, 8, 10))   # empty again
core17.poll_ok(healthy, FIRST_DRINK, T(19, 8, 15))   # refilled again
eq("three alerts really went out", len(core17.take()), 3)

m = run(core17, Fountain(STEADY), T(19, 8, 20), T(20, 0, 10))
rep17 = [x for x in m if "Daily summary" in x]
eq("exactly one report", len(rep17), 1)
ok("the receipt counts what was sent", "3 messages sent today" in rep17[0])
ok("the swallowed startup alarm is not counted",
   "4 messages sent today" not in rep17[0])
ok("and it is no longer the old constant zero",
   "0 messages sent today" not in rep17[0])

print("\n=== 21. NO ALARM WE WILL HAVE TO TAKE BACK FIVE MINUTES LATER ===")
# 26.08.2026, live: last drink 07:41, so the 8 h threshold fell at 15:41. The
# cat drank at 15:43. The poll ran at 15:43:40 and the fountain had not yet
# written the record, so "Your cat has not drunk for 8 hours" went out at
# 15:44 - and "alarm cleared" at 15:49.
#
# The alarm was not wrong about the past. It was asked before it could know,
# and an alarm retracted five minutes later is worse than no alarm: it is what
# teaches you to stop reading them.
race_cfg = pk.default_cfg()
race_cfg.thirst_sec = 6 * 3600
race_cfg.normal_gap_sec = cfg.normal_gap_sec

#   last drink 10:00:00 (60 s) -> readable 10:03:00, threshold at 16:00:00
#   next drink 16:02:00 (17 s) -> readable 16:04:17
#   polls land on :03:40, so 16:03:40 sees a 6 h 03 m gap and no record yet
RACE     = [(T(19, 10, 0, 0), 60), (T(19, 16, 2, 0), 17)]
DRY_ONLY = [(T(19, 10, 0, 0), 60)]

core18 = pk.Core(race_cfg, T(19, 9, 58, 40))
core18.poll_ok(healthy, [], T(19, 9, 58, 40)); core18.drop()
m = run(core18, Fountain(RACE), T(19, 10, 3, 40), T(19, 17, 0, 40))
eq("a drink just past the threshold produces no alarm at all", m, [])
eq("so there is no alarm left open either", core18.thirst_level, 0)
eq("and the drink itself was seen", core18.last_drink_ts, T(19, 16, 2, 0))

# The grace must not have quietly disabled the alarm. Same timeline, minus the
# drink: that dry spell is real and must still be reported.
core19 = pk.Core(race_cfg, T(19, 9, 58, 40))
core19.poll_ok(healthy, [], T(19, 9, 58, 40)); core19.drop()
early = run(core19, Fountain(DRY_ONLY), T(19, 10, 3, 40), T(19, 16, 3, 40))
eq("silent at the first poll past the raw threshold", early, [])
late = run(core19, Fountain(DRY_ONLY), T(19, 16, 8, 40), T(19, 16, 8, 40))
eq("and it speaks at the very next one", len(late), 1)
ok("still worded as the configured threshold",
   "not drunk for 6 hours" in late[0])
ok("with the real last drink", "10:00" in late[0])
eq("the alarm is open", core19.thirst_level, 1)

# The cost is bounded and self-tuning: one poll interval plus the write lag,
# whatever poll_sec happens to be. On an eight-hour dry spell that is noise.
slow = pk.default_cfg()
slow.thirst_sec = 6 * 3600
slow.poll_sec = 900
core20 = pk.Core(slow, T(19, 9, 58, 40))
core20.poll_ok(healthy, [], T(19, 9, 58, 40)); core20.drop()
m = run(core20, Fountain(DRY_ONLY), T(19, 10, 3, 40), T(19, 16, 13, 40), step=900)
eq("a slower poll waits proportionally longer, not a fixed amount", m, [])
m = run(core20, Fountain(DRY_ONLY), T(19, 16, 18, 40), T(19, 16, 18, 40), step=900)
eq("and then it reports", len(m), 1)

print("\n" + "=" * 60)
print("RESULT: %d passed, %d failed" % (PASS, FAIL))
sys.exit(1 if FAIL else 0)
