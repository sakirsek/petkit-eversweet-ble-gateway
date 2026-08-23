/* petkit_core.c - see petkit_core.h
 *
 * Pure C99. Uses nothing from libc except string.h and snprintf; no malloc,
 * no files, no sockets, no time(). Every timestamp arrives as a parameter.
 */
#include "petkit_core.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ========================================================= small helpers */

static uint32_t be32(const uint8_t *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}
static uint16_t be16(const uint8_t *b) { return (uint16_t)((b[0] << 8) | b[1]); }
static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }

/* Unix seconds -> local civil time. We avoid localtime()/tzset() so the PC
 * build and the ESP build agree exactly; this is the standard
 * days-from-civil algorithm. */
typedef struct { int year, mon, day, hour, min, sec; int32_t day_no; } pk_civil_t;

static void pk_civil(uint32_t unix_ts, int32_t tz, pk_civil_t *o) {
    int64_t t = (int64_t)unix_ts + tz;
    int32_t days = (int32_t)(t / 86400);
    int32_t rem  = (int32_t)(t % 86400);
    if (rem < 0) { rem += 86400; days -= 1; }
    o->day_no = days;
    o->hour = rem / 3600;
    o->min  = (rem % 3600) / 60;
    o->sec  = rem % 60;

    int32_t z = days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t y = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp + (mp < 10 ? 3 : -9);
    o->year = y + (m <= 2);
    o->mon = (int)m;
    o->day = (int)d;
}

/* "43 sec" / "1 min 7 sec" / "3 hr 40 min" */
static void fmt_duration(uint32_t s, char *out, int cap) {
    if (s < 60)        snprintf(out, cap, "%u sec", s);
    else if (s < 3600) snprintf(out, cap, "%u min %u sec", s / 60, s % 60);
    else               snprintf(out, cap, "%u hr %u min", s / 3600, (s % 3600) / 60);
}

static void fmt_clock(uint32_t ts, int32_t tz, char *out, int cap) {
    pk_civil_t c; pk_civil(ts, tz, &c);
    snprintf(out, cap, "%02d:%02d", c.hour, c.min);
}

/* Queue a message. Silently drops if full - never overflows. */
static void msg(pk_t *p, const char *fmt, ...) {
    if (p->msg_n >= PK_MAX_MSG) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->msg[p->msg_n], PK_MSG_LEN, fmt, ap);
    va_end(ap);
    p->msg_n++;
    p->messages_sent++;
}

/* ================================================================ frames */

int pk_frame_build(uint8_t cmd, uint8_t typ, const uint8_t *pl,
                   uint16_t n, uint8_t seq, uint8_t *out) {
    if (n + 9 > PK_FRAME_MAX) return -1;
    out[0] = PK_HDR0; out[1] = PK_HDR1; out[2] = PK_HDR2_CMD;
    out[3] = cmd; out[4] = typ; out[5] = seq;
    out[6] = (uint8_t)(n & 0xFF);
    out[7] = (uint8_t)((n >> 8) & 0xFF);
    if (n && pl) memcpy(out + 8, pl, n);
    out[8 + n] = PK_END;
    return 9 + n;
}

int pk_frame_parse(const uint8_t *f, int n, pk_frame_t *o) {
    if (n < 9 || f[n - 1] != PK_END) return 0;
    if (f[0] != PK_HDR0 || f[1] != PK_HDR1) return 0;
    o->cmd = f[3]; o->typ = f[4]; o->seq = f[5];
    if (f[2] == PK_HDR2_CMD) {
        /* fa fc fd | cmd typ seq | len(LE16) | payload | fb */
        o->stream = 0;
        o->len = (uint16_t)(f[6] | (f[7] << 8));
        o->payload = f + 8;
        return 1;
    }
    if (f[2] == PK_HDR2_STREAM) {
        /* fa fc fe | cmd typ seq | flag(1) len(LE16) | payload | fb
         * 9-byte header - verified against live captures. */
        o->stream = 1;
        o->len = (uint16_t)(f[7] | (f[8] << 8));
        o->payload = f + 9;
        return 1;
    }
    return 0;
}

/* ============================================================== decoding */

int pk_state_decode(const uint8_t *p, int n, pk_state_t *o) {
    if (n < 26) { o->valid = 0; return 0; }
    memset(o, 0, sizeof(*o));
    o->power = p[0]; o->suspended = p[1]; o->mode = p[2]; o->psu = p[3]; o->dnd = p[4];
    o->warn_fault = p[5]; o->warn_no_water = p[6];
    o->warn_low_battery = p[7]; o->warn_filter = p[8];
    o->pump_total_sec = be32(p + 9);
    o->filter_pct = p[13]; o->pump_running = p[14];
    o->pump_today_sec = be32(p + 15);
    o->detect = p[19];
    o->supply_mv = be16(p + 20); o->battery_mv = be16(p + 22);
    o->battery_pct = p[24]; o->module = p[25];
    if (n >= 42) {
        /* Bytes 26-29 are a capacitive proximity sensor: 12-bit ADC values,
         * little-endian. With no cat present raw sits ~5 above the baseline;
         * a cat pushes it to 2000-4095. Measured separation across a full
         * night: max 1981 with no cat, min 1996 with a cat - no overlap. */
        o->prox_baseline = le16(p + 26);
        o->prox_raw      = le16(p + 28);
        o->smart_on_min = p[30]; o->smart_off_min = p[31];
        o->led = p[36]; o->led_brightness = p[37]; o->lock = p[39];
    }
    o->valid = 1;
    return 1;
}

int pk_history_decode(const uint8_t *b, int n, pk_visit_t *out, int max) {
    int k = 0;
    for (int i = 0; i + 6 <= n && k < max; i += 6, k++) {
        out[k].ts  = be32(b + i);
        out[k].sec = be16(b + i + 4);
    }
    return k;
}

int32_t pk_sync_pending(const uint8_t *pl, int n) {
    /* payload: 01 | BE32 pending_bytes.
     * NOTE the offset: the counter is pl[1..4], not pl[0..3]. Reading it one
     * byte early yields nonsense like 16777216. */
    if (n < 5) return -1;
    return (int32_t)be32(pl + 1);
}

/* ================================================================= setup */

void pk_cfg_defaults(pk_cfg_t *c) {
    memset(c, 0, sizeof(*c));
    c->tz_offset_sec       = 3 * 3600;   /* Turkey */
    c->poll_sec            = 300;        /* 5 minutes */
    c->thirst_sec          = 8u * 3600;
    c->thirst_escalate_sec = 12u * 3600;
    c->unreachable_sec     = 900;        /* 15 minutes = 3 polls */
    c->report_hour = 23; c->report_min = 59;
    c->report_settle_min = 3;
    c->battery_thresh[0] = 30; c->battery_thresh[1] = 20; c->battery_thresh[2] = 10;
    c->filter_thresh[0]  = 20; c->filter_thresh[1]  = 10; c->filter_thresh[2]  = 5;
    c->normal_gap_sec = 0;
}

void pk_init(pk_t *p, const pk_cfg_t *c, uint32_t now) {
    memset(p, 0, sizeof(*p));
    p->cfg = *c;
    p->started_ts = now;
    p->last_ok_ts = now;
    pk_civil_t cv; pk_civil(now, c->tz_offset_sec, &cv);
    p->day_no = cv.day_no;
    /* Today's report is always still owed, whatever time we started. A day is
     * only ever closed once midnight has passed AND report_settle_min with it,
     * so no report for today can have gone out yet. */
    p->last_report_day = cv.day_no - 1;
}

int pk_msg_count(const pk_t *p) { return p->msg_n; }
const char *pk_msg(const pk_t *p, int i) {
    return (i >= 0 && i < p->msg_n) ? p->msg[i] : "";
}
void pk_msg_clear(pk_t *p) { p->msg_n = 0; }
int pk_visit_count(const pk_t *p) { return p->visit_n; }
int pk_sizeof(void) { return (int)sizeof(pk_t); }
uint32_t pk_last_drink_ts(const pk_t *p) { return p->last_drink_ts; }
int pk_thirst_level(const pk_t *p) { return p->thirst_level; }

/* ============================================================ internals */

static const char *mode_name(uint8_t m) { return m == 2 ? "Intermittent" : "Continuous"; }
static const char *psu_name(uint8_t e)  { return e ? "adapter" : "battery"; }

/* Is something wrong with the device itself? The thirst alarm is SUPPRESSED
 * while this holds: if the reservoir is empty the cat physically cannot
 * drink, and reporting one root cause as two separate alarms is noise. */
static int device_unhealthy(const pk_t *p) {
    const pk_state_t *s = &p->last;
    if (!s->valid) return 1;
    return s->warn_no_water || s->warn_fault || !s->power || p->unreachable;
}

static void day_rollover(pk_t *p, int32_t new_day, uint32_t now) {
    p->day_no = new_day;
    p->day_polls_total = p->day_polls_ok = p->day_messages = 0;
    p->day_pump_peak = 0;

    /* The visit list is NOT emptied here. Because we never acknowledge the
     * stream, the device replays its whole buffer on every poll, so anything
     * cleared at midnight is re-added seconds later and then counted as
     * today's drinking. The list stays a rolling window instead and the
     * report filters it by civil day; we only drop what is older than two
     * days, to keep it bounded. */
    uint32_t cutoff = (now > 2u * 86400u) ? now - 2u * 86400u : 0;
    int w = 0;
    for (int i = 0; i < p->visit_n; i++)
        if (p->visit[i].ts >= cutoff) p->visit[w++] = p->visit[i];
    p->visit_n = w;

    /* thirst_level and last_drink_ts are deliberately PRESERVED: a dry spell
     * that spans midnight is still a real dry spell. */
}

static void add_visit(pk_t *p, const pk_visit_t *v) {
    /* We never acknowledge the stream, so the device keeps handing us the
     * same records every poll - deduplicate on timestamp. */
    for (int i = 0; i < p->visit_n; i++)
        if (p->visit[i].ts == v->ts) return;

    if (p->visit_n >= PK_MAX_VISITS) {
        /* Full: drop the OLDEST, not the newest, so the window always holds
         * the most recent history. */
        memmove(p->visit, p->visit + 1, (PK_MAX_VISITS - 1) * sizeof p->visit[0]);
        p->visit_n = PK_MAX_VISITS - 1;
    }
    /* Insert in chronological order. The report subtracts adjacent timestamps
     * to find the longest gap, and an out-of-order insert would underflow
     * that unsigned subtraction into a nonsense multi-thousand-hour gap. */
    int i = p->visit_n;
    while (i > 0 && p->visit[i - 1].ts > v->ts) { p->visit[i] = p->visit[i - 1]; i--; }
    p->visit[i] = *v;
    p->visit_n++;

    if (v->ts > p->last_drink_ts) p->last_drink_ts = v->ts;
}

/* ========================================================== alarm engine */

static void check_thresholds(pk_t *p) {
    const pk_state_t *s = &p->last;
    for (int i = 0; i < 3; i++) {
        uint8_t bit = (uint8_t)(1 << i);
        if (s->battery_pct <= p->cfg.battery_thresh[i] && !(p->battery_latch & bit)) {
            p->battery_latch |= bit;
            msg(p, "🔋 <b>Battery dropped below %u%%</b>\n"
                   "<i>now %u%% (%u mV) · %s</i>",
                p->cfg.battery_thresh[i], s->battery_pct, s->battery_mv,
                s->psu ? "adapter connected" : "adapter not connected");
        }
        if (s->filter_pct <= p->cfg.filter_thresh[i] && !(p->filter_latch & bit)) {
            p->filter_latch |= bit;
            msg(p, "🧹 <b>Filter dropped below %u%%</b>\n"
                   "<i>now %u%% · you will need to replace it soon</i>",
                p->cfg.filter_thresh[i], s->filter_pct);
        }
    }
    /* Rearm once the value climbs back (filter reset, battery recharged). */
    if (s->filter_pct  > p->cfg.filter_thresh[0])  p->filter_latch = 0;
    if (s->battery_pct > p->cfg.battery_thresh[0]) p->battery_latch = 0;
}

static void check_critical(pk_t *p, uint32_t now) {
    const pk_state_t *s = &p->last;
    char t[16]; fmt_clock(now, p->cfg.tz_offset_sec, t, sizeof t);

    if (s->warn_no_water && !p->a_no_water) {
        p->a_no_water = 1;
        msg(p, "🚨 <b>WATER EMPTY</b>\nThe reservoir needs refilling.\n"
               "<i>%s · pump %s</i>", t, s->pump_running ? "running" : "stopped");
    } else if (!s->warn_no_water && p->a_no_water) {
        p->a_no_water = 0;
        msg(p, "✅ <b>Water refilled</b> · <i>%s · pump %s</i>",
            t, s->pump_running ? "running" : "stopped");
    }

    if (s->warn_fault && !p->a_fault) {
        p->a_fault = 1;
        msg(p, "⛔ <b>Fountain reported a fault</b>\n<i>%s · needs checking</i>", t);
    } else if (!s->warn_fault && p->a_fault) {
        p->a_fault = 0;
        msg(p, "✅ <b>Fault cleared</b> · <i>%s</i>", t);
    }
}

static void check_thirst(pk_t *p, uint32_t now) {
    /* Without a baseline we must stay quiet, otherwise every power cut would
     * produce a false alarm the moment the gateway restarts. */
    if (!p->baseline_ready || !p->last_drink_ts) return;
    if (device_unhealthy(p)) return;

    uint32_t gap = (now > p->last_drink_ts) ? now - p->last_drink_ts : 0;
    char at[16], dur[32];
    fmt_clock(p->last_drink_ts, p->cfg.tz_offset_sec, at, sizeof at);

    uint16_t last_sec = 0;
    for (int i = 0; i < p->visit_n; i++)
        if (p->visit[i].ts == p->last_drink_ts) last_sec = p->visit[i].sec;
    fmt_duration(last_sec, dur, sizeof dur);

    const pk_state_t *s = &p->last;

    if (p->thirst_level == 0 && gap >= p->cfg.thirst_sec) {
        p->thirst_level = 1;
        msg(p, "⚠️ <b>Your cat has not drunk for %u hours</b>\n"
               "Last drink <b>%s</b> · %s\n"
               "<blockquote><b>Fountain:</b> on · pump %s\n"
               "<b>Water:</b> present\n"
               "<b>Filter:</b> %u%% · <b>Battery:</b> %u%%\n\n"
               "Nothing is wrong with the device. That is why I am telling you."
               "</blockquote>",
            p->cfg.thirst_sec / 3600, at, dur,
            s->pump_running ? "running" : "stopped",
            s->filter_pct, s->battery_pct);
    } else if (p->thirst_level == 1 && gap >= p->cfg.thirst_escalate_sec) {
        p->thirst_level = 2;
        if (p->cfg.normal_gap_sec) {
            char ng[32]; fmt_duration(p->cfg.normal_gap_sec, ng, sizeof ng);
            msg(p, "🔴 <b>Your cat has not drunk for %u hours</b>\n"
                   "Last drink <b>%s</b> · %s\n"
                   "<blockquote>The device is still healthy. This gap is unusual "
                   "for this cat. The longest normal gap measured is <b>%s</b>."
                   "</blockquote>\n<i>I will not write about this again.</i>",
                p->cfg.thirst_escalate_sec / 3600, at, dur, ng);
        } else {
            msg(p, "🔴 <b>Your cat has not drunk for %u hours</b>\n"
                   "Last drink <b>%s</b> · %s\n"
                   "<blockquote>The device is still healthy.</blockquote>\n"
                   "<i>I will not write about this again.</i>",
                p->cfg.thirst_escalate_sec / 3600, at, dur);
        }
    }
}

/* ========================================================== daily report */

/* Emit the summary for one civil day. The day is passed in rather than taken
 * from `now`, because the report is normally produced by the first tick AFTER
 * midnight - using `now` would date yesterday's drinking as today. */
static void emit_report(pk_t *p, int32_t day_no, uint32_t now) {
    pk_civil_t c;
    pk_civil((uint32_t)day_no * 86400u, 0, &c);   /* midnight of the reported day */
    const pk_state_t *s = &p->last;

    /* Find this day's slice of the rolling multi-day window. add_visit keeps
     * the list chronological, so one day is always a contiguous range - no
     * need to copy the records out, which on the ESP32-C3 would put another
     * kilobyte on a task stack that also holds the 1.6 KB message buffer. */
    int lo = 0, hi = 0;
    for (int i = 0; i < p->visit_n; i++) {
        pk_civil_t vc; pk_civil(p->visit[i].ts, p->cfg.tz_offset_sec, &vc);
        if (vc.day_no != day_no) continue;
        if (hi == lo) lo = i;
        hi = i + 1;
    }
    const pk_visit_t *day = p->visit + lo;
    int day_n = hi - lo;

    uint32_t total = 0, longest = 0, longest_ts = 0;
    for (int i = 0; i < day_n; i++) {
        total += day[i].sec;
        if (day[i].sec > longest) { longest = day[i].sec; longest_ts = day[i].ts; }
    }
    /* Longest gap - the figure we accumulate to tune the thirst threshold. */
    uint32_t longest_gap = 0;
    for (int i = 1; i < day_n; i++) {
        uint32_t g = day[i].ts - day[i - 1].ts;
        if (g > longest_gap) longest_gap = g;
    }

    char buf[PK_MSG_LEN];
    int k = 0;
    k += snprintf(buf + k, sizeof buf - k,
                  "📊 <b>Daily summary · %02d.%02d.%04d</b>\n\n", c.day, c.mon, c.year);

    if (day_n == 0) {
        k += snprintf(buf + k, sizeof buf - k,
                      "🐱 <b>No drinks recorded today.</b>\n");
    } else {
        char tot[32], gap[32], lng[32], at[16];
        fmt_duration(total, tot, sizeof tot);
        fmt_duration(longest, lng, sizeof lng);
        fmt_duration(longest_gap, gap, sizeof gap);
        fmt_clock(longest_ts, p->cfg.tz_offset_sec, at, sizeof at);
        k += snprintf(buf + k, sizeof buf - k,
                      "🐱 <b>%d %s</b> · %s total\n"
                      "Longest drink <b>%s</b> (%s) · longest gap <b>%s</b>\n",
                      day_n, day_n == 1 ? "drink" : "drinks",
                      tot, lng, at, gap);
        k += snprintf(buf + k, sizeof buf - k, "<blockquote expandable>");
        int shown = 0;
        for (int i = 0; i < day_n && k < (int)sizeof buf - 300; i++, shown++) {
            char h[16], d[32];
            fmt_clock(day[i].ts, p->cfg.tz_offset_sec, h, sizeof h);
            fmt_duration(day[i].sec, d, sizeof d);
            k += snprintf(buf + k, sizeof buf - k, "%s%s   %s", i ? "\n" : "", h, d);
        }
        /* Say so when the list is cut short. A quietly shortened list reads as
         * a complete one, and this whole design depends on the messages being
         * trustworthy rather than merely present. */
        if (shown < day_n)
            k += snprintf(buf + k, sizeof buf - k,
                          "\n… %d more not shown", day_n - shown);
        k += snprintf(buf + k, sizeof buf - k, "</blockquote>\n");
    }

    if (s->valid) {
        /* No litre estimate on purpose: the pump recirculates the same water
         * rather than consuming it, so a "70 L" figure reads as the amount
         * the cat drank and is actively misleading. Runtime is the honest
         * number, and it is what tells you the pump is alive. */
        k += snprintf(buf + k, sizeof buf - k,
                      "\n💧 <b>Pump</b> ran %u hr %u min\n"
                      "🔋 <b>Battery</b> %u%% (%u mV)\n"
                      "🧹 <b>Filter</b> %u%%\n"
                      "⚙️ %s mode · %s\n",
                      p->day_pump_peak / 3600, (p->day_pump_peak % 3600) / 60,
                      s->battery_pct, s->battery_mv, s->filter_pct,
                      mode_name(s->mode), psu_name(s->psu));
    }

    uint32_t uptime = now - p->started_ts;
    snprintf(buf + k, sizeof buf - k,
             "<blockquote expandable><b>System health</b>\n"
             "%u / %u polls succeeded today\n"
             "%u hr %u min uptime\n"
             "%u messages sent today</blockquote>",
             p->day_polls_ok, p->day_polls_total,
             uptime / 3600, (uptime % 3600) / 60, p->day_messages);

    msg(p, "%s", buf);
}

/* Public entry point: report the civil day that `now` falls in. */
void pk_report(pk_t *p, uint32_t now) {
    pk_civil_t c; pk_civil(now, p->cfg.tz_offset_sec, &c);
    emit_report(p, c.day_no, now);
}

/* ============================================================ public API */

void pk_poll_ok(pk_t *p, const pk_state_t *s,
                const pk_visit_t *fresh, int n, uint32_t now) {
    p->polls_total++;
    p->polls_ok++;
    p->day_polls_total++;
    p->day_polls_ok++;
    p->fail_streak = 0;

    /* The day boundary is deliberately NOT handled here. pk_tick owns it,
     * because rolling the day over is exactly what triggers the missed-report
     * recovery - doing it here first silently swallowed the daily report for
     * an entire run. */

    if (p->unreachable) {
        p->unreachable = 0;
        uint32_t outage = now - p->last_ok_ts;
        char t[16], o[32];
        fmt_clock(now, p->cfg.tz_offset_sec, t, sizeof t);
        fmt_duration(outage, o, sizeof o);
        msg(p, "✅ <b>Fountain is back</b> · <i>%s · %s outage</i>", t, o);
    }
    p->last_ok_ts = now;

    if (s && s->valid) {
        p->last = *s;
        /* Keep the day's PEAK, not the latest reading. The daily report is
         * normally emitted just after midnight, by which time the device has
         * already reset pump_today_sec - reporting it directly printed
         * "Pump ran 0 hr 3 min" for a day it actually ran 23 hours. */
        if (s->pump_today_sec > p->day_pump_peak)
            p->day_pump_peak = s->pump_today_sec;
    }

    uint32_t prev_drink = p->last_drink_ts;
    for (int i = 0; i < n; i++) add_visit(p, &fresh[i]);

    /* The first successful poll establishes the baseline; until then the
     * thirst alarm stays silent. */
    if (!p->baseline_ready) p->baseline_ready = 1;

    /* The cat drank again -> clear any open thirst alarm. This is the one
     * drink-related message we do send, because it closes an alarm the user
     * is already worried about. */
    if (p->thirst_level && p->last_drink_ts > prev_drink) {
        uint32_t gap = (prev_drink && p->last_drink_ts > prev_drink)
                       ? p->last_drink_ts - prev_drink : 0;
        char h[16], d[32], g[32];
        fmt_clock(p->last_drink_ts, p->cfg.tz_offset_sec, h, sizeof h);
        uint16_t sec = 0;
        for (int i = 0; i < p->visit_n; i++)
            if (p->visit[i].ts == p->last_drink_ts) sec = p->visit[i].sec;
        fmt_duration(sec, d, sizeof d);
        fmt_duration(gap, g, sizeof g);
        msg(p, "✅ <b>Your cat drank, alarm cleared</b>\n<b>%s</b> · %s\n"
               "<i>Gap: %s</i>", h, d, g);
        p->thirst_level = 0;
    }

    if (p->last.valid) { check_critical(p, now); check_thresholds(p); }
    check_thirst(p, now);
}

void pk_poll_fail(pk_t *p, uint32_t now) {
    p->polls_total++;
    p->day_polls_total++;
    p->fail_streak++;
    (void)now;
}

void pk_tick(pk_t *p, uint32_t now) {
    pk_civil_t c; pk_civil(now, p->cfg.tz_offset_sec, &c);

    /* The day changed: emit the finished day's report, dated to THAT day,
     * before the daily counters reset. This is the ONLY place a day is ever
     * closed - see the warning below about the branch that used to be second.
     *
     * We wait report_settle_min past midnight before closing. The fountain
     * writes each record about 40 seconds after the visit ends (measured), so
     * a drink at 23:59 is not readable until just after midnight. Closing the
     * day at 00:00 would leave that drink in no report at all - it belongs to
     * the finished day, whose report would already have gone out. */
    if (c.day_no != p->day_no &&
        c.hour * 60 + c.min >= (int)p->cfg.report_settle_min) {
        if (p->last_report_day < p->day_no) {
            p->last_report_day = p->day_no;
            emit_report(p, p->day_no, now);
        }
        day_rollover(p, c.day_no, now);
    }

    if (!p->unreachable && p->last_ok_ts &&
        now - p->last_ok_ts >= p->cfg.unreachable_sec) {
        p->unreachable = 1;
        char t[16]; fmt_clock(p->last_ok_ts, p->cfg.tz_offset_sec, t, sizeof t);
        const pk_state_t *s = &p->last;
        msg(p, "📴 <b>Fountain unreachable</b>\n"
               "No response for %u minutes (%d consecutive polls failed).\n"
               "<i>Last seen %s · battery %u%% · %s</i>",
            p->cfg.unreachable_sec / 60, p->fail_streak, t,
            s->valid ? s->battery_pct : 0,
            (s->valid && !s->warn_no_water) ? "water present" : "water level unknown");
    }

    check_thirst(p, now);

    /* DO NOT add a second way to close the day here.
     *
     * There used to be one: a same-day branch that fired when a tick landed at
     * or after cfg.report_hour:report_min (23:59) and stamped last_report_day
     * itself. It looked harmless - it only moved the report four minutes
     * earlier - but stamping last_report_day made the rollover guard above
     * (last_report_day < day_no) false, so the settled report never ran and
     * report_settle_min became inert on exactly the nights it was written for.
     * Every drink still unreadable at that tick was then in NO report at all,
     * silently: not the finished day's, which had already gone out, and not
     * the next day's, because emit_report filters strictly by civil day.
     *
     * And it did not miss at random. 86400 is an exact multiple of poll_sec
     * and both drivers sleep the remainder, so a run holds its poll phase for
     * its whole life. One gateway start in five lands in that minute and then
     * loses the end of every single day until something restarts it. The
     * ESP32's first self-sent report, on 22.08.2026, arrived at 23:59 from
     * this branch - that is how it was found. Test 19 pins the property that
     * matters: the same day seen through two poll phases must produce the same
     * report. */
}
