#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ctypes bindings for the compiled core (.dll, .so or .dylib).

All protocol, alarm and message logic lives in C. Python is only the
transport here, which is exactly the split that lets the same core run on the
ESP32-C3: this file gets replaced by NimBLE + esp_http_client, the C core
does not change at all.
"""
import ctypes as C
import sys
from pathlib import Path

_CORE = Path(__file__).resolve().parent / "core"

if sys.platform == "win32":
    _NAMES, _BUILD = ("petkit_core.dll",), "core\\build.bat"
elif sys.platform == "darwin":
    _NAMES, _BUILD = ("libpetkit_core.dylib", "petkit_core.dylib"), "make -C core"
else:
    _NAMES, _BUILD = ("libpetkit_core.so", "petkit_core.so"), "make -C core"

_DLL = next((p for p in (_CORE / n for n in _NAMES) if p.exists()), None)
if _DLL is None:
    raise SystemExit(
        "The core library is missing. Build it with:\n    %s\n"
        "Looked for: %s" % (_BUILD, ", ".join("core/" + n for n in _NAMES)))


class Cfg(C.Structure):
    _fields_ = [
        ("tz_offset_sec", C.c_int32),
        ("poll_sec", C.c_uint32),
        ("thirst_sec", C.c_uint32),
        ("thirst_escalate_sec", C.c_uint32),
        ("unreachable_sec", C.c_uint32),
        ("report_hour", C.c_uint8),
        ("report_min", C.c_uint8),
        ("report_settle_min", C.c_uint8),
        ("battery_thresh", C.c_uint8 * 3),
        ("filter_thresh", C.c_uint8 * 3),
        ("normal_gap_sec", C.c_uint32),
    ]


class State(C.Structure):
    _fields_ = [
        ("valid", C.c_uint8),
        ("power", C.c_uint8), ("suspended", C.c_uint8), ("mode", C.c_uint8),
        ("psu", C.c_uint8), ("dnd", C.c_uint8),
        ("warn_fault", C.c_uint8), ("warn_no_water", C.c_uint8),
        ("warn_low_battery", C.c_uint8), ("warn_filter", C.c_uint8),
        ("pump_total_sec", C.c_uint32),
        ("filter_pct", C.c_uint8), ("pump_running", C.c_uint8),
        ("pump_today_sec", C.c_uint32),
        ("detect", C.c_uint8),
        ("supply_mv", C.c_uint16), ("battery_mv", C.c_uint16),
        ("battery_pct", C.c_uint8), ("module", C.c_uint8),
        ("prox_raw", C.c_uint16), ("prox_baseline", C.c_uint16),
        ("smart_on_min", C.c_uint8), ("smart_off_min", C.c_uint8),
        ("led", C.c_uint8), ("led_brightness", C.c_uint8), ("lock", C.c_uint8),
    ]

    def __repr__(self):
        return ("State(power=%d mode=%d pump=%d psu=%d no_water=%d fault=%d "
                "battery=%d%%/%dmV filter=%d%% detect=%d prox=%d/%d today=%ds)" % (
                    self.power, self.mode, self.pump_running, self.psu,
                    self.warn_no_water, self.warn_fault, self.battery_pct,
                    self.battery_mv, self.filter_pct, self.detect,
                    self.prox_raw, self.prox_baseline, self.pump_today_sec))


class Visit(C.Structure):
    _fields_ = [("ts", C.c_uint32), ("sec", C.c_uint16)]


class Host(C.Structure):
    """Facts only the platform layer knows, for the health block of a reply.
    Mirrors pk_host_t. Every field may be left at zero."""
    _fields_ = [
        ("heap_free", C.c_uint32), ("heap_min", C.c_uint32),
        ("rssi_fountain", C.c_int8), ("rssi_wifi", C.c_int8),
        ("mtu", C.c_uint16),
        ("pending_bytes", C.c_uint32), ("stream_bytes", C.c_uint32),
        ("reset_reason", C.c_char_p),
    ]


class Frame(C.Structure):
    _fields_ = [
        ("stream", C.c_uint8), ("cmd", C.c_uint8), ("typ", C.c_uint8),
        ("seq", C.c_uint8), ("len", C.c_uint16), ("payload", C.POINTER(C.c_uint8)),
    ]


_lib = C.CDLL(str(_DLL))

_lib.pk_cfg_defaults.argtypes = [C.POINTER(Cfg)]
_lib.pk_init.argtypes = [C.c_void_p, C.POINTER(Cfg), C.c_uint32]
_lib.pk_sizeof.restype = C.c_int
_lib.pk_frame_build.argtypes = [C.c_uint8, C.c_uint8, C.c_char_p, C.c_uint16,
                                C.c_uint8, C.c_char_p]
_lib.pk_frame_build.restype = C.c_int
_lib.pk_frame_parse.argtypes = [C.c_char_p, C.c_int, C.POINTER(Frame)]
_lib.pk_frame_parse.restype = C.c_int
_lib.pk_state_decode.argtypes = [C.c_char_p, C.c_int, C.POINTER(State)]
_lib.pk_state_decode.restype = C.c_int
_lib.pk_history_decode.argtypes = [C.c_char_p, C.c_int, C.POINTER(Visit), C.c_int]
_lib.pk_history_decode.restype = C.c_int
_lib.pk_sync_pending.argtypes = [C.c_char_p, C.c_int]
_lib.pk_sync_pending.restype = C.c_int32
_lib.pk_poll_ok.argtypes = [C.c_void_p, C.POINTER(State), C.POINTER(Visit),
                            C.c_int, C.c_int, C.c_uint32]
_lib.pk_poll_fail.argtypes = [C.c_void_p, C.c_uint32]
_lib.pk_tick.argtypes = [C.c_void_p, C.c_uint32]
_lib.pk_msg_count.argtypes = [C.c_void_p]; _lib.pk_msg_count.restype = C.c_int
_lib.pk_msg.argtypes = [C.c_void_p, C.c_int]; _lib.pk_msg.restype = C.c_char_p
_lib.pk_msg_clear.argtypes = [C.c_void_p]
_lib.pk_msg_drop.argtypes = [C.c_void_p]
_lib.pk_visit_count.argtypes = [C.c_void_p]; _lib.pk_visit_count.restype = C.c_int
_lib.pk_last_drink_ts.argtypes = [C.c_void_p]
_lib.pk_last_drink_ts.restype = C.c_uint32
_lib.pk_thirst_level.argtypes = [C.c_void_p]; _lib.pk_thirst_level.restype = C.c_int
_lib.pk_report.argtypes = [C.c_void_p, C.c_uint32]
_lib.pk_command.argtypes = [C.c_void_p, C.c_char_p, C.POINTER(Host), C.c_uint32]
_lib.pk_command.restype = C.c_int
# Outcome of a history read, mirroring the enum in petkit_core.h. The core
# refuses to distinguish "no drinks" from "I could not read the drinks" unless
# the transport tells it which one this was.
HIST_OK, HIST_SHORT = 0, 1


# ------------------------------------------------------------ free functions
def default_cfg() -> Cfg:
    c = Cfg(); _lib.pk_cfg_defaults(C.byref(c)); return c


def frame_build(cmd, typ=1, payload=b"", seq=1) -> bytes:
    buf = C.create_string_buffer(288)
    n = _lib.pk_frame_build(cmd, typ, payload, len(payload), seq, buf)
    if n < 0:
        raise ValueError("frame too large")
    return buf.raw[:n]


def frame_parse(data: bytes):
    f = Frame()
    if not _lib.pk_frame_parse(data, len(data), C.byref(f)):
        return None
    # f.payload points into the caller's buffer; copy it out
    off = 9 if f.stream else 8
    pl = data[off:off + f.len] if f.len else b""
    return dict(stream=bool(f.stream), cmd=f.cmd, typ=f.typ, seq=f.seq,
                len=f.len, payload=pl)


def state_decode(payload: bytes):
    s = State()
    if not _lib.pk_state_decode(payload, len(payload), C.byref(s)):
        return None
    return s


def history_decode(buf: bytes, max_n=256):
    arr = (Visit * max_n)()
    n = _lib.pk_history_decode(buf, len(buf), arr, max_n)
    return [(arr[i].ts, arr[i].sec) for i in range(n)]


def sync_pending(payload: bytes) -> int:
    return _lib.pk_sync_pending(payload, len(payload))


# -------------------------------------------------------------------- object
class Core:
    """Holds pk_t as an opaque buffer sized by the C side, so changes to the
    struct layout cannot silently desynchronise the Python bindings."""

    def __init__(self, cfg: Cfg, now: int):
        self._buf = C.create_string_buffer(_lib.pk_sizeof())
        self._p = C.cast(self._buf, C.c_void_p)
        self.cfg = cfg
        _lib.pk_init(self._p, C.byref(cfg), now)

    def poll_ok(self, state, visits, now: int, hist: int = HIST_OK):
        arr = (Visit * max(1, len(visits)))()
        for i, (ts, sec) in enumerate(visits):
            arr[i].ts, arr[i].sec = ts, sec
        sp = C.byref(state) if state is not None else None
        _lib.pk_poll_ok(self._p, sp, arr, len(visits), hist, now)

    def poll_fail(self, now: int):
        _lib.pk_poll_fail(self._p, now)

    def tick(self, now: int):
        _lib.pk_tick(self._p, now)

    def report(self, now: int):
        _lib.pk_report(self._p, now)

    def command(self, text: str, now: int, host: Host = None) -> bool:
        """Handle one Telegram command and queue the reply. Replies join the
        normal outbound queue but are not counted as alerts."""
        hp = C.byref(host) if host is not None else None
        return bool(_lib.pk_command(self._p, text.encode("utf-8"), hp, now))

    def messages(self):
        n = _lib.pk_msg_count(self._p)
        return [_lib.pk_msg(self._p, i).decode("utf-8", "replace") for i in range(n)]

    def clear(self):
        _lib.pk_msg_clear(self._p)

    def drop(self):
        """Throw the queue away without counting it as sent (startup only)."""
        _lib.pk_msg_drop(self._p)

    def take(self):
        """Return queued messages and empty the queue."""
        m = self.messages()
        self.clear()
        return m

    @property
    def visit_count(self): return _lib.pk_visit_count(self._p)
    @property
    def last_drink_ts(self): return _lib.pk_last_drink_ts(self._p)
    @property
    def thirst_level(self): return _lib.pk_thirst_level(self._p)
