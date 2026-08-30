/* main.c - ESP32-C3 PetKit BLE -> Telegram Gateway Main Application.
 *
 * Runs 24/7 on an ESP32-C3 SuperMini. Polls the fountain over BLE every 5 minutes,
 * evaluates logic in pure C99 core (core/petkit_core.c), stays silent unless an
 * alarm condition is met, and sends notifications to Telegram over Wi-Fi/HTTPS.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "petkit_core.h"
#include "pk_ble.h"
#include "pk_net.h"
#include "pk_nvs.h"
#include "secrets.h"

static const char *TAG = "MAIN";

/* Allocate the 13.9 KB core struct statically in .bss (never on task stack) */
static pk_t g_core;

/* Outbox for failed messages to retry on next cycle (up to 4 messages) */
#define OUTBOX_MAX 4
static char s_outbox[OUTBOX_MAX][PK_MSG_LEN];
static int s_outbox_count = 0;

static void outbox_push(const char *msg) {
    if (s_outbox_count < OUTBOX_MAX) {
        strncpy(s_outbox[s_outbox_count++], msg, PK_MSG_LEN - 1);
        s_outbox[s_outbox_count - 1][PK_MSG_LEN - 1] = '\0';
    } else {
        ESP_LOGW(TAG, "Outbox full, dropping oldest message");
        memmove(s_outbox, s_outbox + 1, (OUTBOX_MAX - 1) * PK_MSG_LEN);
        strncpy(s_outbox[OUTBOX_MAX - 1], msg, PK_MSG_LEN - 1);
    }
}

/* The three that mean trouble are PANIC, TASK_WDT and BROWNOUT: the first two
 * say the previous run wedged, the third says the SuperMini regulator sagged
 * (which is also why BLE TX power is pinned to 8.5 dBm). USB is benign - it
 * just means someone opened the serial port or flashed the board. */
static const char *reset_reason_name(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
    case ESP_RST_WDT:      return "OTHER WATCHDOG";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB:      return "usb (serial open or flash)";
    case ESP_RST_JTAG:     return "jtag";
    default:               return "unknown";
    }
}

/* Sleep in slices, feeding the task watchdog as we go.
 *
 * The watchdog has to be short enough to catch a wedged BLE poll but the
 * idle period is five minutes, so one plain vTaskDelay cannot satisfy both.
 * This matters more here than it looks: on the PC prototype the gateway once
 * sat alive but deaf for 3 h 18 min after a BLE call never returned, and in a
 * silence-by-default design a dead gateway and a healthy one look identical. */
static void wdt_sleep_ms(int64_t ms) {
    while (ms > 0) {
        uint32_t slice = (ms > 10000) ? 10000 : (uint32_t)ms;
        vTaskDelay(pdMS_TO_TICKS(slice));
        esp_task_wdt_reset();
        ms -= (int64_t)slice;
    }
}

static void parse_secret_hex(const char *hex_str, uint8_t *out8) {
    memset(out8, 0, 8);
    int len = strlen(hex_str);
    int byte_count = len / 2;
    if (byte_count > 8) byte_count = 8;
    
    // Left-pad with zeros to exactly 8 bytes
    int offset = 8 - byte_count;
    for (int i = 0; i < byte_count; i++) {
        unsigned int val = 0;
        sscanf(hex_str + (i * 2), "%02x", &val);
        out8[offset + i] = (uint8_t)val;
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "PetKit BLE -> Telegram Gateway (ESP32-C3 SuperMini)");
    ESP_LOGI(TAG, "Target MAC: %s | Poll interval: %d min", PETKIT_MAC, CONFIG_POLL_MINUTES);
    /* Why we came up matters: a watchdog, panic or brownout reset means the
     * previous run wedged or the supply sagged, and in a silence-by-default
     * gateway that is the one failure nobody would otherwise notice. Print
     * the name, not the number - this log gets read months from now. */
    ESP_LOGI(TAG, "Reset reason: %s | CPU: %d MHz | Flash: 4 MB",
             reset_reason_name(esp_reset_reason()), CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    ESP_LOGI(TAG, "==================================================");

    uint8_t secret8[8];
    parse_secret_hex(PETKIT_SECRET_HEX, secret8);

    // 1. Initialize NVS
    ESP_ERROR_CHECK(pk_nvs_init());
    pk_nvs_state_t nvs_st;
    pk_nvs_load(&nvs_st);

    /* Arm the watchdog on THIS task before anything that can block. 180 s
     * clears the worst legitimate case (a 75 s poll, or one Telegram send
     * burning three 20 s timeouts plus backoff) while still rebooting a
     * genuinely stuck cycle. trigger_panic makes it an actual reset rather
     * than a log line nobody reads. */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = 180000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&wdt_cfg));
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    esp_task_wdt_reset();

    /* 2. Get a clock we can trust, and do not start without one.
     *
     * Every decision the core makes is a function of this number: which civil
     * day a drink belongs to, when the daily report is due, how long the cat
     * has gone without water. Booting with no network used to fall through to
     * a 1970 clock and carry on, which would produce a confident wrong report
     * and a thirst alarm measured from the epoch. Waiting here is the honest
     * failure: the gateway is simply not up yet, and the console says so.
     *
     * Always attempt a sync here, even when the RTC already looks sane: the
     * C3 keeps its RTC across a soft reset, so a reboot carries whatever
     * drift it had straight back into the new run. Two seconds of radio is
     * cheap. Only a clock we cannot use at all blocks the gateway. */
    for (int attempt = 1; ; attempt++) {
        ESP_LOGI(TAG, "Bringing up Wi-Fi for SNTP clock sync (attempt %d)...", attempt);
        bool synced = false;
        if (pk_net_wifi_sta_start(WIFI_SSID, WIFI_PASS, 20000) == ESP_OK) {
            synced = pk_net_sntp_sync(15000);
            pk_net_wifi_sta_stop();
        } else {
            ESP_LOGE(TAG, "Wi-Fi connection failed");
        }
        esp_task_wdt_reset();
        if (synced) break;
        if (pk_net_time_valid()) {
            /* No network, but the RTC survived a reboot and is still in a
             * plausible range. Run on it rather than sulk - the 12-hourly
             * re-sync below will correct it as soon as the network is back. */
            ESP_LOGW(TAG, "SNTP failed; running on the retained RTC for now");
            break;
        }
        ESP_LOGW(TAG, "clock unusable - retrying in 60 s, gateway is NOT running");
        wdt_sleep_ms(60000);
    }

    uint32_t now = (uint32_t)time(NULL);
    uint32_t last_sync = now;

    // 3. Configure Core
    pk_cfg_t cfg;
    pk_cfg_defaults(&cfg);
    cfg.poll_sec            = CONFIG_POLL_MINUTES * 60;
    cfg.thirst_sec          = (uint32_t)CONFIG_THIRST_HOURS * 3600;
    cfg.thirst_escalate_sec = (uint32_t)CONFIG_THIRST_ESCALATE_HOURS * 3600;
    cfg.normal_gap_sec      = (uint32_t)CONFIG_NORMAL_GAP_MINUTES * 60;
    cfg.report_hour         = CONFIG_REPORT_HOUR;
    cfg.report_min          = CONFIG_REPORT_MIN;
    cfg.report_settle_min   = CONFIG_REPORT_SETTLE_MIN;
    cfg.tz_offset_sec       = CONFIG_TIMEZONE_OFFSET_HOURS * 3600;

    pk_init(&g_core, &cfg, now);

    // Restore persistent alarm and day states from NVS
    g_core.thirst_level   = nvs_st.thirst_level;
    g_core.a_no_water     = nvs_st.a_no_water;
    g_core.a_fault        = nvs_st.a_fault;
    g_core.battery_latch  = nvs_st.battery_latch;
    g_core.filter_latch   = nvs_st.filter_latch;
    if (nvs_st.day_no > 0) g_core.day_no = nvs_st.day_no;
    if (nvs_st.last_report_day > 0) g_core.last_report_day = nvs_st.last_report_day;
    if (nvs_st.last_drink_ts > 0) g_core.last_drink_ts = nvs_st.last_drink_ts;

    /* Restore the visit list. This used to live only in RAM, on the theory
     * that a restarted gateway could always rebuild the day from the
     * fountain's buffer. That theory has two holes: the phone app can drain
     * that buffer, and once WE acknowledge it the records are gone from the
     * fountain by our own hand. So the flash copy is now the authoritative
     * one, and it has to be restored before anything reads g_core.visit. */
    g_core.visit_n = pk_nvs_load_visits(g_core.visit, PK_MAX_VISITS);

    // 4. Perform Initial Poll
    ESP_LOGI(TAG, "Performing initial BLE poll...");
    static pk_ble_result_t s_ble_res;
    bool ok = pk_ble_poll(PETKIT_MAC, secret8, &s_ble_res);
    esp_task_wdt_reset();
    now = (uint32_t)time(NULL);

    if (ok) {
        pk_poll_ok(&g_core, &s_ble_res.state, s_ble_res.visits, s_ble_res.visit_count,
                   s_ble_res.hist_short ? PK_HIST_SHORT : PK_HIST_OK, now);
        pk_msg_drop(&g_core); // Swallow startup alarms so we don't spam (and do not count them as sent)

        // Send startup banner
        if (TG_ENABLED) {
            char banner[512];
            snprintf(banner, sizeof(banner),
                     "🚰 <b>Fountain monitoring active</b>\n"
                     "<blockquote><b>Status:</b> %s · %s mode\n"
                     "<b>Pump:</b> %s · %s\n"
                     "<b>Battery:</b> %d%% (%d mV)\n"
                     "<b>Filter:</b> %d%%</blockquote>\n"
                     "<i>Checking every %d minutes. From now on I will only write if something is wrong.</i>",
                     s_ble_res.state.power ? "on" : "off",
                     s_ble_res.state.mode == 2 ? "Intermittent" : "Continuous",
                     s_ble_res.state.pump_running ? "running" : "stopped",
                     s_ble_res.state.psu ? "adapter" : "battery",
                     s_ble_res.state.battery_pct, s_ble_res.state.battery_mv, s_ble_res.state.filter_pct,
                     CONFIG_POLL_MINUTES);

            ESP_LOGI(TAG, "Sending startup banner to Telegram...");
            if (pk_net_wifi_sta_start(WIFI_SSID, WIFI_PASS, 15000) == ESP_OK) {
                pk_net_telegram_send(TG_TOKEN, TG_CHAT_ID, banner);
                esp_task_wdt_reset();
                pk_net_wifi_sta_stop();
            }
        }
    } else {
        pk_poll_fail(&g_core, now);
        ESP_LOGW(TAG, "First poll failed: %s (will retry next cycle)", s_ble_res.error);
    }

    /* Persist what the first poll recovered before waiting five minutes for
     * the loop to do it. A board that reboots more often than that - a flaky
     * supply, a power cut that comes back in stages - would otherwise never
     * write a visit to flash at all, which matters now that acknowledging
     * makes flash the only copy. */
    pk_nvs_save_visits(g_core.visit, g_core.visit_n);

    ESP_LOGI(TAG, "Gateway initialized. Entering 5-minute main loop...");

    // Wait full poll interval before second poll (fountain stops advertising right after disconnect)
    wdt_sleep_ms((int64_t)cfg.poll_sec * 1000);

    // 5. Main 5-minute Superloop
    while (1) {
        int64_t t0 = esp_timer_get_time();

        ESP_LOGI(TAG, "--- Starting scheduled poll ---");
        memset(&s_ble_res, 0, sizeof(s_ble_res));
        ok = pk_ble_poll(PETKIT_MAC, secret8, &s_ble_res);
        esp_task_wdt_reset();
        now = (uint32_t)time(NULL);

        if (ok) {
            ESP_LOGI(TAG, "Poll OK: power=%d pump=%d batt=%d%% filter=%d%% new_visits=%d%s",
                     s_ble_res.state.power, s_ble_res.state.pump_running, s_ble_res.state.battery_pct,
                     s_ble_res.state.filter_pct, s_ble_res.visit_count,
                     s_ble_res.hist_short ? " SHORT" : "");
            pk_poll_ok(&g_core, &s_ble_res.state, s_ble_res.visits, s_ble_res.visit_count,
                       s_ble_res.hist_short ? PK_HIST_SHORT : PK_HIST_OK, now);
        } else {
            ESP_LOGW(TAG, "Poll failed: %s", s_ble_res.error);
            pk_poll_fail(&g_core, now);
        }

        // pk_tick is the sole owner of the day boundary and daily report
        pk_tick(&g_core, now);

        // Collect new messages
        int msg_count = pk_msg_count(&g_core);
        if (msg_count > 0 || s_outbox_count > 0) {
            ESP_LOGI(TAG, "%d new message(s), %d in outbox. Connecting Wi-Fi to send...",
                     msg_count, s_outbox_count);

            if (TG_ENABLED && pk_net_wifi_sta_start(WIFI_SSID, WIFI_PASS, 20000) == ESP_OK) {
                // Send pending outbox messages first
                int sent_outbox = 0;
                for (int i = 0; i < s_outbox_count; i++) {
                    esp_task_wdt_reset();
                    if (pk_net_telegram_send(TG_TOKEN, TG_CHAT_ID, s_outbox[i])) {
                        sent_outbox++;
                    } else {
                        break;
                    }
                }
                if (sent_outbox > 0) {
                    memmove(s_outbox, s_outbox + sent_outbox,
                            (s_outbox_count - sent_outbox) * PK_MSG_LEN);
                    s_outbox_count -= sent_outbox;
                }

                // Send fresh messages
                for (int i = 0; i < msg_count; i++) {
                    const char *m = pk_msg(&g_core, i);
                    esp_task_wdt_reset();
                    if (!pk_net_telegram_send(TG_TOKEN, TG_CHAT_ID, m)) {
                        ESP_LOGW(TAG, "Failed to send message %d, queueing to outbox", i);
                        outbox_push(m);
                    }
                }
                pk_net_wifi_sta_stop();
            } else if (TG_ENABLED) {
                ESP_LOGE(TAG, "Wi-Fi connection failed, saving messages to outbox");
                for (int i = 0; i < msg_count; i++) {
                    outbox_push(pk_msg(&g_core, i));
                }
            }
        }
        pk_msg_clear(&g_core);

        /* Persist the visit list. The fountain's buffer is a recovery aid,
         * not storage: the phone app can drain it at any moment, and a power
         * cut like the one on 29.08.2026 must not be able to lose a day.
         * This is a no-op on the cycles where nothing changed. */
        pk_nvs_save_visits(g_core.visit, g_core.visit_n);

        /* Re-sync the clock twice a day. The RTC drifts, and this gateway is
         * meant to run for months without anyone touching it; left alone the
         * daily report would slide off midnight. Bringing the radio up for two
         * seconds also proves the network path still works on days when there
         * is nothing to report - which, by design, is most days. */
        if ((uint32_t)(now - last_sync) >= 12u * 3600u) {
            if (pk_net_wifi_sta_start(WIFI_SSID, WIFI_PASS, 20000) == ESP_OK) {
                if (pk_net_sntp_sync(15000)) {
                    last_sync = (uint32_t)time(NULL);
                    ESP_LOGI(TAG, "clock re-synced");
                } else {
                    ESP_LOGW(TAG, "clock re-sync failed, retrying next cycle");
                }
                pk_net_wifi_sta_stop();
            }
            esp_task_wdt_reset();
        }

        // Save alarm & day state to NVS on change
        pk_nvs_state_t cur_st = {
            .thirst_level    = g_core.thirst_level,
            .a_no_water      = g_core.a_no_water,
            .a_fault         = g_core.a_fault,
            .battery_latch   = g_core.battery_latch,
            .filter_latch    = g_core.filter_latch,
            .day_no          = g_core.day_no,
            .last_report_day = g_core.last_report_day,
            .last_drink_ts   = g_core.last_drink_ts,
        };
        pk_nvs_save(&cur_st);

        // Diagnostics
        // Monotonic cadence pacing
        int64_t elapsed_us = esp_timer_get_time() - t0;

        /* ---- health telemetry ----
         * Integer math only. CONFIG_NEWLIB_NANO_FORMAT drops float
         * conversions as well as 64-bit ones, so a %f here would print
         * garbage exactly the way %lld did. */
        unsigned long heap_now = (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        unsigned long heap_min = (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        unsigned long heap_big = (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        unsigned long el_ms = (unsigned long)(elapsed_us / 1000);
        /* tenths of a percent: ms busy / seconds per cycle */
        unsigned long duty10 = cfg.poll_sec ? el_ms / cfg.poll_sec : 0;
        /* No fragmentation percentage here on purpose. The C3 splits its heap
         * across four physically separate regions, so free-minus-largest is
         * dominated by that split and reads as alarming fragmentation when
         * nothing is wrong. The largest contiguous block is the number that
         * actually decides whether the next allocation succeeds. */
        ESP_LOGI(TAG, "mem  free %lu KB | ever-low %lu KB | largest block %lu KB",
                 heap_now / 1024, heap_min / 1024, heap_big / 1024);
        ESP_LOGI(TAG, "mem  radio-on low: BLE %lu KB | WiFi+TLS %lu KB | main stack spare %lu B",
                 (unsigned long)s_ble_res.heap_phase / 1024,
                 (unsigned long)pk_net_heap_phase() / 1024,
                 (unsigned long)hwm);
        ESP_LOGI(TAG, "rf   fountain %d dBm (mtu %u) | wifi %d dBm",
                 (int)s_ble_res.rssi, (unsigned)s_ble_res.mtu, pk_net_rssi());
        ESP_LOGI(TAG, "cpu  %d MHz | busy %lu ms of %lu s (%lu.%lu%%)",
                 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, el_ms,
                 (unsigned long)cfg.poll_sec, duty10 / 10, duty10 % 10);

        if (hwm < 2048) {
            /* emit_report() puts a 1600-byte buffer on THIS stack and only
             * runs at the day boundary, so a stack that looks fine on every
             * ordinary cycle can still overflow on the one cycle that
             * matters. Say so while it is still a log line, not a panic. */
            ESP_LOGE(TAG, "STACK LOW: %lu B free, the daily report needs 1600 B",
                     (unsigned long)hwm);
        }
        int64_t target_us = (int64_t)cfg.poll_sec * 1000000LL;
        int64_t remaining_us = target_us - elapsed_us;
        int64_t sleep_ms = remaining_us > 5000000LL ? remaining_us / 1000LL : 5000LL;

        /* Not %lld: CONFIG_NEWLIB_NANO_FORMAT is on and nano's vsnprintf has
         * no 64-bit conversions, so the old line printed the literal "ld".
         * A five-minute cycle cannot overflow 32 bits of milliseconds. */
        ESP_LOGI(TAG, "Cycle elapsed: %lu ms. Sleeping for %lu ms...",
                 (unsigned long)(elapsed_us / 1000LL), (unsigned long)sleep_ms);
        esp_task_wdt_reset();
        wdt_sleep_ms(sleep_ms);
    }
}
