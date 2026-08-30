/* pk_net.c - see pk_net.h */
#include "pk_net.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "petkit_core.h"   /* PK_MSG_LEN, so the buffers cannot be outgrown */

static const char *TAG = "PK_NET";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_wifi_inited = false;
static int s_retry_num = 0;
/* Set while we are taking the radio down on purpose. esp_wifi_stop() raises
 * WIFI_EVENT_STA_DISCONNECTED exactly like a real dropout, and without this
 * flag the handler answered our own shutdown by reconnecting - the boot log
 * showed "Retrying Wi-Fi connection (1/5)" one line after a clean stop. */
static bool s_stopping = false;
static int s_ap_rssi = 0;
static uint32_t s_heap_phase = 0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Set TX power to 8.5 dBm (34 * 0.25 dBm = 8.5 dBm) to prevent brownout
        esp_wifi_set_max_tx_power(34);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_stopping) {
            return;                 /* our own teardown, not a dropout */
        }
        if (s_retry_num < 5) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Retrying Wi-Fi connection (%d/5)...", s_retry_num);
        } else {
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Wi-Fi connected. IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (esp_wifi_sta_get_rssi(&s_ap_rssi) != ESP_OK) s_ap_rssi = 0;
        s_retry_num = 0;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

esp_err_t pk_net_wifi_sta_start(const char *ssid, const char *password, uint32_t timeout_ms) {
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;
    s_stopping = false;

    if (!s_wifi_inited) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_sta_netif = esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            NULL));
        s_wifi_inited = true;
    }

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi SSID: %s", ssid);
        esp_wifi_stop();
        return ESP_FAIL;
    }
}

void pk_net_wifi_sta_stop(void) {
    if (s_wifi_inited) {
        s_stopping = true;
        esp_wifi_disconnect();
        esp_wifi_stop();
        ESP_LOGI(TAG, "Wi-Fi stopped, RF returned to idle");
    }
}

/* 2025-01-01. Anything below this is the power-on epoch, not a real clock. */
#define PK_MIN_VALID_EPOCH 1735689600

bool pk_net_time_valid(void) {
    time_t now = 0;
    time(&now);
    return (long)now > PK_MIN_VALID_EPOCH;
}

static bool s_sntp_inited = false;

bool pk_net_sntp_sync(uint32_t timeout_ms) {
    if (!s_sntp_inited) {
        ESP_LOGI(TAG, "Initializing SNTP...");
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_setservername(2, "time.cloudflare.com");
        esp_sntp_init();
        s_sntp_inited = true;
    } else {
        /* esp_sntp_init() must not run twice. Restart re-polls the client we
         * already have, which is what every call after the first one wants. */
        esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
        esp_sntp_restart();
    }

    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        time_t now = 0;
        time(&now);
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED &&
            (long)now > PK_MIN_VALID_EPOCH) {
            struct tm timeinfo;
            gmtime_r(&now, &timeinfo);
            ESP_LOGI(TAG, "SNTP synced: %04d-%02d-%02d %02d:%02d:%02d UTC (epoch %ld)",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, (long)now);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        elapsed += 500;
    }

    ESP_LOGW(TAG, "SNTP sync timed out");
    return false;
}

/* RFC 3986 URL encoder for UTF-8 HTML payload */
static int url_encode(const char *src, char *dst, size_t dst_cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t k = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '~') {
            if (k + 1 >= dst_cap) return -1;
            dst[k++] = c;
        } else if (c == ' ') {
            if (k + 1 >= dst_cap) return -1;
            dst[k++] = '+';
        } else {
            if (k + 3 >= dst_cap) return -1;
            dst[k++] = '%';
            dst[k++] = hex[(c >> 4) & 0x0F];
            dst[k++] = hex[c & 0x0F];
        }
    }
    if (k >= dst_cap) return -1;
    dst[k] = '\0';
    return (int)k;
}

/* Big enough for the longest message the core can produce with every single
 * byte percent-encoded, plus the form fields around it. Sized from PK_MSG_LEN
 * rather than guessed, because the overflow path here throws the message away
 * and a gateway that silently drops what it decided to say is worse than one
 * that never spoke. Real messages encode at about 1.7x, not 3x, so this is
 * mostly headroom - and it is still smaller than the two buffers it replaced,
 * because the encoder now writes straight into the form body.
 *
 * getUpdates borrows the same buffer for its response. Nothing else is in
 * flight while either call runs. */
static char s_post_buf[PK_MSG_LEN * 3 + 96];
static char s_url_buf[256];

int pk_net_rssi(void) { return s_ap_rssi; }
uint32_t pk_net_heap_phase(void) { return s_heap_phase; }

bool pk_net_telegram_send(const char *token, const char *chat_id, const char *html_text) {
    if (!token || !token[0] || !chat_id || !chat_id[0] || !html_text || !html_text[0]) {
        ESP_LOGE(TAG, "Invalid Telegram parameters");
        return false;
    }

    int post_len = snprintf(s_post_buf, sizeof(s_post_buf),
                            "chat_id=%s&text=", chat_id);
    if (post_len < 0 || post_len >= (int)sizeof(s_post_buf)) {
        ESP_LOGE(TAG, "Post buffer overflow");
        return false;
    }
    int enc = url_encode(html_text, s_post_buf + post_len,
                         sizeof(s_post_buf) - (size_t)post_len);
    if (enc < 0) {
        ESP_LOGE(TAG, "URL encode buffer overflow for Telegram message");
        return false;
    }
    post_len += enc;
    int tail = snprintf(s_post_buf + post_len,
                        sizeof(s_post_buf) - (size_t)post_len,
                        "&parse_mode=HTML&disable_web_page_preview=true");
    if (tail < 0 || post_len + tail >= (int)sizeof(s_post_buf)) {
        ESP_LOGE(TAG, "Post buffer overflow");
        return false;
    }
    post_len += tail;

    snprintf(s_url_buf, sizeof(s_url_buf), "https://api.telegram.org/bot%s/sendMessage", token);

    esp_http_client_config_t config = {
        .url = s_url_buf,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
    };

    bool success = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client");
            break;
        }

        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, s_post_buf, post_len);

        esp_err_t err = esp_http_client_perform(client);
        /* Before cleanup, while the TLS session is still allocated. */
        s_heap_phase = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int status_code = esp_http_client_get_status_code(client);

        if (err == ESP_OK && status_code == 200) {
            ESP_LOGI(TAG, "Telegram message sent successfully (HTTP 200)");
            success = true;
            esp_http_client_cleanup(client);
            break;
        } else {
            ESP_LOGW(TAG, "Telegram send attempt %d/3 failed: err=%s, http_status=%d",
                     attempt, esp_err_to_name(err), status_code);
            esp_http_client_cleanup(client);

            // Don't retry on 4xx permanent client errors (e.g. 400 Bad Request, 401 Unauthorized)
            if (status_code >= 400 && status_code < 500) {
                ESP_LOGE(TAG, "Permanent client error HTTP %d, not retrying", status_code);
                break;
            }

            if (attempt < 3) {
                vTaskDelay(pdMS_TO_TICKS(3000 * attempt));
            }
        }
    }

    return success;
}

/* ------------------------------------------------------- receiving commands */

/* Reuses s_post_buf as the response body. Nothing else is in flight when this
 * runs (the sends are finished) and a second 5 KB static buffer is 5 KB the
 * BLE stack would rather have. */
static int http_collect(esp_http_client_handle_t c, char *buf, int cap) {
    int total = 0;
    while (total < cap - 1) {
        int r = esp_http_client_read(c, buf + total, cap - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    buf[total] = 0;
    return total;
}

bool pk_net_telegram_get_updates(const char *token, const char *chat_id,
                                 int32_t *offset, pk_net_cmds_t *out) {
    if (!token || !token[0] || !chat_id || !chat_id[0] || !offset || !out)
        return false;
    out->n = 0;
    out->first[0] = 0;

    /* timeout=0 is a plain poll, not a long poll. Holding the socket open
     * would mean holding the radio open, and the whole cycle is built around
     * the radio being off. limit=8 caps the response so it cannot outgrow the
     * buffer no matter how long the gateway was away. */
    snprintf(s_url_buf, sizeof(s_url_buf),
             "https://api.telegram.org/bot%s/getUpdates"
             "?offset=%ld&limit=8&timeout=0", token, (long)*offset);

    esp_http_client_config_t config = {
        .url = s_url_buf,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;

    bool ok = false;
    int len = 0;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) == 200)
            len = http_collect(client, s_post_buf, sizeof(s_post_buf));
        else
            ESP_LOGW(TAG, "getUpdates HTTP %d",
                     esp_http_client_get_status_code(client));
        esp_http_client_close(client);
    } else {
        ESP_LOGW(TAG, "getUpdates could not open the connection");
    }
    esp_http_client_cleanup(client);
    if (len <= 0) return false;

    cJSON *root = cJSON_Parse(s_post_buf);
    if (!root) {
        ESP_LOGW(TAG, "getUpdates returned unparseable JSON (%d bytes)", len);
        return false;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (cJSON_IsArray(result)) {
        ok = true;
        cJSON *u;
        cJSON_ArrayForEach(u, result) {
            cJSON *uid = cJSON_GetObjectItem(u, "update_id");
            if (cJSON_IsNumber(uid) && (int32_t)uid->valuedouble >= *offset)
                *offset = (int32_t)uid->valuedouble + 1;   /* advance ALWAYS */

            cJSON *m = cJSON_GetObjectItem(u, "message");
            if (!m) m = cJSON_GetObjectItem(u, "edited_message");
            if (!m) continue;

            cJSON *chat = cJSON_GetObjectItem(m, "chat");
            cJSON *cid = chat ? cJSON_GetObjectItem(chat, "id") : NULL;
            if (!cJSON_IsNumber(cid)) continue;
            /* Compared as numbers, never by formatting the id into text.
             * CONFIG_NEWLIB_NANO_FORMAT is on, so snprintf has no 64-bit
             * conversions: "%lld" produced the literal "ld", which matched
             * nothing, so every message the owner sent was thrown away as a
             * stranger's while the cursor advanced past it. The bot consumed
             * its commands and answered none of them, silently, because the
             * drop path is meant to be silent. Same trap as the cycle timing
             * log in main.c, which carries the same warning. */
            if ((long long)cid->valuedouble != strtoll(chat_id, NULL, 10)) {
                /* Someone else found the bot. Say nothing to them at all: a
                 * reply, even a refusal, confirms the bot is live. */
                ESP_LOGW(TAG, "ignoring a message from another chat");
                continue;
            }

            /* Any text at all, not just a leading slash. The owner talking
             * to their own bot deserves an answer whatever they typed, and
             * silence is the one reply that cannot be told apart from a dead
             * board. */
            cJSON *txt = cJSON_GetObjectItem(m, "text");
            if (!cJSON_IsString(txt) || !txt->valuestring[0]) continue;
            if (out->n++ == 0) {
                strncpy(out->first, txt->valuestring, PK_NET_CMD_LEN - 1);
                out->first[PK_NET_CMD_LEN - 1] = 0;
            }
        }
    }
    cJSON_Delete(root);
    if (out->n) ESP_LOGI(TAG, "%d message(s) from the owner, first: %s",
                         out->n, out->first);
    return ok;
}
