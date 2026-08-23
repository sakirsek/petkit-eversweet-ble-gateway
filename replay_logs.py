#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Replay recorded gateway logs through the current core.

The strongest check available short of another overnight run: it drives the
real core with the real polls, in the real order, at the real times of a run
that already happened, and prints every message the core would send.

Use it after changing anything in core/ — a behaviour regression shows up as a
message that appears, disappears, or changes wording against a day you already
know the truth about. This is how the missing 19 Aug daily report was
confirmed fixed.

  python replay_logs.py                    # every log in logs/
  python replay_logs.py 20260819           # just that day (and what follows)
"""
import sys, re, json, glob, datetime, calendar
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

import pk

BASE = Path(__file__).resolve().parent
CFG = json.loads((BASE / "config.json").read_text(encoding="utf-8"))
TZ = int(float(CFG.get("timezone_offset_hours", 3)) * 3600)

POLL = re.compile(
    r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) \[PLL\] poll ok  State\("
    r"power=(\d+) mode=(\d+) pump=(\d+) psu=(\d+) no_water=(\d+) fault=(\d+) "
    r"battery=(\d+)%/(\d+)mV filter=(\d+)% detect=(\d+) prox=(\d+)/(\d+) "
    r"today=(\d+)s\)")
FAIL = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) \[!!!\] poll failed")


def unix(s):
    return calendar.timegm(
        datetime.datetime.strptime(s, "%Y-%m-%d %H:%M:%S").timetuple()) - TZ


def load(pattern):
    """History records keyed by the poll timestamp that fetched them."""
    history = {}
    for fn in sorted(glob.glob(str(BASE / "logs" / ("gateway-%s.jsonl" % pattern)))):
        for line in open(fn, encoding="utf-8"):
            o = json.loads(line)
            if o.get("type") == "history":
                history[o["ts"]] = pk.history_decode(bytes.fromhex(o["raw"]))

    events = []
    for fn in sorted(glob.glob(str(BASE / "logs" / ("gateway-%s.log" % pattern)))):
        for line in open(fn, encoding="utf-8"):
            m = POLL.match(line)
            if m:
                g = m.groups()
                st = pk.State()
                st.valid = 1
                (st.power, st.mode, st.pump_running, st.psu,
                 st.warn_no_water, st.warn_fault) = [int(x) for x in g[1:7]]
                st.battery_pct, st.battery_mv = int(g[7]), int(g[8])
                st.filter_pct, st.detect = int(g[9]), int(g[10])
                st.prox_raw, st.prox_baseline = int(g[11]), int(g[12])
                st.pump_today_sec = int(g[13])
                events.append((unix(g[0]), g[0], st, history.get(g[0], [])))
                continue
            m = FAIL.match(line)
            if m:
                events.append((unix(m.group(1)), m.group(1), None, []))
    events.sort()
    return events


def main():
    pattern = sys.argv[1] + "*" if len(sys.argv) > 1 else "*"
    events = load(pattern)
    if not events:
        print("no polls found in logs/gateway-%s.log" % pattern)
        return 1

    fails = sum(1 for e in events if e[2] is None)
    span = (events[-1][0] - events[0][0]) / 3600.0
    print("replaying %d polls over %.1f h  (%s -> %s)"
          % (len(events), span, events[0][1], events[-1][1]))
    print("recorded outcome: %d ok, %d failed\n" % (len(events) - fails, fails))

    cfg = pk.default_cfg()
    cfg.thirst_sec = int(CFG.get("thirst_hours", 6) * 3600)
    cfg.thirst_escalate_sec = int(CFG.get("thirst_escalate_hours", 12) * 3600)
    cfg.normal_gap_sec = int(CFG.get("normal_gap_minutes", 220) * 60)
    hh, mm = str(CFG.get("report_time", "23:59")).split(":")
    cfg.report_hour, cfg.report_min = int(hh), int(mm)
    cfg.tz_offset_sec = int(float(CFG.get("timezone_offset_hours", 3)) * 3600)
    cfg.report_settle_min = int(CFG.get("report_settle_minutes", 3))

    core = pk.Core(cfg, events[0][0])
    sent = []
    for now, human, state, visits in events:
        if state is None:
            core.poll_fail(now)
        else:
            core.poll_ok(state, visits, now)
        core.tick(now)
        sent += [(human, m) for m in core.take()]

    print("=" * 66)
    print("MESSAGES THE CORE WOULD SEND: %d" % len(sent))
    print("=" * 66)
    for human, m in sent:
        print("\n[%s]\n%s" % (human, m))
    if not sent:
        print("\n(none — silence is the correct output for a healthy run "
              "that does not cross midnight)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
