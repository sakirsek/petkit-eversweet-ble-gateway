/* petkit_core.h - PetKit CTW3UV fountain monitoring core
 *
 * Pure C99. No platform dependencies: no malloc, no files, no sockets, no
 * time() (the current time is always passed in). The exact same file is
 * compiled for the PC (as a DLL driven over ctypes) and for the ESP32-C3
 * (ESP-IDF + NimBLE).
 *
 * Platform layer owns:  BLE transport, WiFi/HTTPS, wall clock, persistence.
 * This core owns:       protocol, decoding, alarm rules, message text.
 *
 * Behaviour: poll every 5 minutes, STAY SILENT, and emit a message only for
 *   1) water empty / device fault      2) unreachable (3 failed polls)
 *   3) battery / filter thresholds     4) thirst alarm (6h -> 12h -> cleared)
 *   5) daily summary at 23:59 + system health receipt
 * There is deliberately NO routine "the cat drank" message. The valuable
 * signal is the ABSENCE of drinking; reporting every visit causes alert
 * fatigue and buries the one message that matters.
 */
#ifndef PETKIT_CORE_H
#define PETKIT_CORE_H

#include <stdint.h>

#ifdef _WIN32
#  define PK_API __declspec(dllexport)
#else
#  define PK_API
#endif

#define PK_MAX_VISITS 128   /* rolling window, ~2 days of records     */
#define PK_MAX_MSG      8   /* messages producible in a single round  */
#define PK_MSG_LEN   1600   /* Telegram allows 4096; reports fit here */
#define PK_FRAME_MAX  288   /* negotiated MTU is 158, so this is ample */

/* How long after a visit ENDS before the fountain has written the record and
 * we can read it back. Measured at about 40 s (docs/protocol.md); the test
 * harness has modelled it at 120 s since it was written, and 120 is the figure
 * to design against here. Being early costs a false alarm; being late costs a
 * few minutes on an eight-hour dry spell, which nobody will ever notice.
 * See check_thirst - this is why the thirst threshold carries a grace. */
#define PK_RECORD_LAG_SEC 120

/* ---------------------------------------------------------------- commands */
enum {
    PK_CMD_AUTH      = 86,   /* verify secret        -> reply 01 = accepted  */
    PK_CMD_STREAM    = 68,   /* history stream data  (FA FC FE header!)      */
    PK_CMD_STREAMACK = 67,   /* stream chunk ack     - NEVER SEND (consumes) */
    PK_CMD_STREAMEND = 69,   /* stream end ack       - NEVER SEND (consumes) */
    PK_CMD_STREAMSET = 80,   /* stream settings      -> THIS starts the flow */
    PK_CMD_STATE     = 210,  /* running state                                */
    PK_CMD_SETTINGS  = 211,  /* settings                                     */
    PK_CMD_SYNC      = 212,  /* request history sync -> 01|BE32 pending      */
    PK_CMD_IDSN      = 213,  /* deviceId + serial number                     */
    PK_CMD_PUSH      = 230   /* unsolicited state push                       */
};

/* Frame headers: FD for normal command/reply, FE for streamed payloads. */
#define PK_HDR0 0xFA
#define PK_HDR1 0xFC
#define PK_HDR2_CMD    0xFD
#define PK_HDR2_STREAM 0xFE
#define PK_END  0xFB

/* ----------------------------------------------------------------- decoding */
typedef struct {
    uint8_t  stream;      /* 1 = FA FC FE streamed frame */
    uint8_t  cmd, typ, seq;
    uint16_t len;
    const uint8_t *payload;
} pk_frame_t;

/* State payload: 42 bytes from CMD 230, 30 bytes from CMD 210. */
typedef struct {
    uint8_t  valid;
    uint8_t  power, suspended, mode, psu, dnd;
    uint8_t  warn_fault, warn_no_water, warn_low_battery, warn_filter;
    uint32_t pump_total_sec;
    uint8_t  filter_pct, pump_running;
    uint32_t pump_today_sec;
    uint8_t  detect;              /* 0 = clear, 2 = cat at the sensor */
    uint16_t supply_mv, battery_mv;
    uint8_t  battery_pct, module;
    uint16_t prox_raw;            /* 12-bit ADC reading   (bytes 28-29 LE) */
    uint16_t prox_baseline;       /* auto-calibrated base (bytes 26-27 LE) */
    uint8_t  smart_on_min, smart_off_min, led, led_brightness, lock;
} pk_state_t;

typedef struct {
    uint32_t ts;     /* unix seconds - the device's own record */
    uint16_t sec;    /* duration in seconds */
} pk_visit_t;

/* NOTE on the visit list: because we never acknowledge the stream, the device
 * replays its entire buffer on every poll - including records from previous
 * days. The list is therefore a rolling multi-day window, deduplicated on
 * timestamp, and the daily report filters it by civil day. Do not "clear it
 * at midnight": the records are simply re-added on the next poll. */

/* ------------------------------------------------------------------ config */
typedef struct {
    int32_t  tz_offset_sec;       /* local UTC offset, e.g. 3*3600 for Turkey */
    uint32_t poll_sec;            /* 300   = 5 minutes                        */
    uint32_t thirst_sec;          /* 28800 = 8 hours (measured, see README)    */
    uint32_t thirst_escalate_sec; /* 43200 = 12 hours                         */
    uint32_t unreachable_sec;     /* 900   = 15 minutes (3 polls)             */
    uint8_t  report_hour, report_min;  /* NO LONGER CONSULTED. Kept so the
                                   * config plumbing and the ctypes layout in
                                   * pk.py stay valid. The report time is not
                                   * a knob any more: a day closes exactly
                                   * report_settle_min after midnight. Setting
                                   * these does nothing - see pk_tick.      */
    uint8_t  report_settle_min;   /* minutes past midnight to wait before
                                   * closing the day. The fountain writes a
                                   * record ~40 s after the visit ends, so
                                   * closing at 00:00 would drop a 23:59
                                   * drink from every report. Default 3.   */
    uint8_t  battery_thresh[3];        /* 30, 20, 10                          */
    uint8_t  filter_thresh[3];         /* 20, 10, 5                           */
    uint32_t normal_gap_sec;      /* longest gap measured as normal; quoted in
                                   * the escalation message for context       */
} pk_cfg_t;

/* ------------------------------------------------------------------- state */
typedef struct {
    pk_cfg_t   cfg;
    pk_state_t last;

    pk_visit_t visit[PK_MAX_VISITS];
    int        visit_n;
    uint32_t   last_drink_ts;      /* 0 = not known yet */
    uint8_t    baseline_ready;     /* no thirst alarm until history read once */

    /* alarm latches - each fires once, rearms when the condition clears */
    uint8_t  a_no_water, a_fault;
    uint8_t  thirst_level;         /* 0 none, 1 = 6h sent, 2 = 12h sent */
    uint8_t  battery_latch;        /* bit0..2 = threshold crossed       */
    uint8_t  filter_latch;
    uint8_t  unreachable;
    uint32_t last_ok_ts;
    int      fail_streak;

    /* statistics for the daily health receipt. The day_* counters cover the
     * day being reported; the plain ones are lifetime. */
    uint32_t polls_total, polls_ok;
    uint32_t day_polls_total, day_polls_ok, day_messages;
    uint32_t day_pump_peak;        /* see pk_poll_ok - the device resets
                                    * its own daily pump counter at
                                    * midnight, before we report it */
    uint32_t started_ts;
    uint32_t messages_sent;
    int32_t  day_no;               /* local day number, for rollover */
    int32_t  last_report_day;

    /* outbound queue - drained by the platform layer */
    char msg[PK_MAX_MSG][PK_MSG_LEN];
    int  msg_n;
} pk_t;

/* --------------------------------------------------------------------- API */
PK_API void pk_cfg_defaults(pk_cfg_t *c);
PK_API void pk_init(pk_t *p, const pk_cfg_t *c, uint32_t now);

/* Build a frame. out must hold PK_FRAME_MAX. Returns bytes written, -1 if too big. */
PK_API int  pk_frame_build(uint8_t cmd, uint8_t typ, const uint8_t *pl,
                           uint16_t n, uint8_t seq, uint8_t *out);
/* Parse a frame. Returns 1 on success. payload points into the input buffer. */
PK_API int  pk_frame_parse(const uint8_t *f, int n, pk_frame_t *out);

PK_API int  pk_state_decode(const uint8_t *pl, int n, pk_state_t *out);
/* 6 bytes per record: BE32 timestamp + BE16 duration. Returns record count. */
PK_API int  pk_history_decode(const uint8_t *buf, int n, pk_visit_t *out, int max);
/* Pending byte count from a CMD 212 reply. -1 if undecodable. */
PK_API int32_t pk_sync_pending(const uint8_t *pl, int n);

/* Successful poll: current state plus any newly fetched history records. */
PK_API void pk_poll_ok(pk_t *p, const pk_state_t *s,
                       const pk_visit_t *fresh, int n, uint32_t now);
PK_API void pk_poll_fail(pk_t *p, uint32_t now);
/* Time-driven checks: thirst alarm, daily report, unreachable detection.
 * pk_tick is the SOLE owner of the day boundary - it must be called after
 * every pk_poll_ok / pk_poll_fail, and it is what emits the daily report. */
PK_API void pk_tick(pk_t *p, uint32_t now);

PK_API int         pk_msg_count(const pk_t *p);
PK_API const char *pk_msg(const pk_t *p, int i);
PK_API void        pk_msg_clear(pk_t *p);
/* Discard the queue without counting it as sent - startup swallow only. */
PK_API void        pk_msg_drop(pk_t *p);

/* Accessors so the platform layer can treat pk_t as opaque. */
PK_API int      pk_sizeof(void);
PK_API int      pk_visit_count(const pk_t *p);
PK_API uint32_t pk_last_drink_ts(const pk_t *p);
PK_API int      pk_thirst_level(const pk_t *p);
PK_API void     pk_report(pk_t *p, uint32_t now);   /* force a daily report */

#endif /* PETKIT_CORE_H */
