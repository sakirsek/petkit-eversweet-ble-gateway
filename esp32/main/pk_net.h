/* pk_net.h - Wi-Fi, SNTP time sync, and Telegram HTTPS transport.
 *
 * Handles Wi-Fi connection, SNTP clock synchronization, and sending
 * HTML-formatted messages to Telegram Bot API with RFC 3986 URL encoding.
 */
#ifndef PK_NET_H
#define PK_NET_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t pk_net_wifi_sta_start(const char *ssid, const char *password, uint32_t timeout_ms);
void pk_net_wifi_sta_stop(void);

bool pk_net_sntp_sync(uint32_t timeout_ms);
/* Is the wall clock one we may hand to the core? False means still 1970. */
bool pk_net_time_valid(void);

/* Signal of the access point, sampled when the last session got its IP.
 * 0 means we have not associated yet. */
int pk_net_rssi(void);
/* Free internal heap sampled just after the last Telegram POST completed,
 * i.e. with the TLS session still standing - the tightest point of the
 * radio-on WiFi half of the cycle. 0 until the first send. */
uint32_t pk_net_heap_phase(void);

bool pk_net_telegram_send(const char *token, const char *chat_id, const char *html_text);

/* What the configured chat said since the last poll, collapsed to a count and
 * the first line of it.
 *
 * There is one command and every message maps to it, so five taps on the menu
 * button deserve one answer, not five identical kilobyte-long ones. The text
 * is kept only so the log line says what was typed. */
#define PK_NET_CMD_LEN 48
typedef struct {
    int  n;
    char first[PK_NET_CMD_LEN];
} pk_net_cmds_t;

/* Fetch pending Telegram messages and report what the owner sent.
 *
 * `offset` is Telegram's update_id cursor. It is read and then advanced past
 * everything the server returned, INCLUDING messages we ignore, and the caller
 * persists it: without that a reboot replays every old command, and with a
 * chat filter but no advance a stranger's message would block the queue
 * forever. Pass it through NVS, not memory.
 *
 * Only messages whose chat id matches `chat_id` are returned. Everything else
 * is dropped without a reply - anyone who learns the bot's name can message
 * it, and this gateway answers exactly one chat.
 *
 * Returns true if the call succeeded (out->n may still be 0). */
bool pk_net_telegram_get_updates(const char *token, const char *chat_id,
                                 int32_t *offset, pk_net_cmds_t *out);

#endif /* PK_NET_H */
