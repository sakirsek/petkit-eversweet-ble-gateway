/* pk_ble.c - see pk_ble.h */
#include "pk_ble.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "PK_BLE";

static bool s_ble_inited = false;
static bool s_ble_synced = false;

/* Frame storage during a poll */
#define MAX_STORED_FRAMES 64
typedef struct {
    uint8_t  stream;
    uint8_t  cmd;
    uint8_t  typ;
    uint8_t  seq;
    uint16_t len;
    uint8_t  payload[256];
} stored_frame_t;

static stored_frame_t s_frames[MAX_STORED_FRAMES];
static int s_frame_count = 0;

#define MAX_STREAM_BLOB_LEN (PK_BLE_MAX_VISITS * 6)
static uint8_t s_stream_blob[MAX_STREAM_BLOB_LEN];
static int s_stream_blob_len = 0;
static bool s_collecting_stream = false;
/* Every CMD 68 byte this poll received, even ones the window below has since
 * dropped. This is the number that gets compared with the CMD 212 promise. */
static volatile int  s_stream_total = 0;
static volatile bool s_window_slid = false;
static volatile int  s_stream82_frames = 0;

/* The fountain's own request for a chunk acknowledgement, captured in the
 * NimBLE host callback and answered from the polling task. Writing to GATT
 * from inside the host callback is how one bad poll becomes a wedged stack. */
static volatile bool    s_ack_req = false;
static volatile uint8_t s_ack_seq = 0;

/* How long to keep collecting. The old code waited a flat 4 s and took
 * whatever had arrived, which was correct only while everything fitted in one
 * chunk. We now wait for the byte count CMD 212 promised; this is the ceiling
 * on that wait, and QUIET_MS is how long with no new bytes means "that is all
 * you are getting" (the fountain re-asks every 3 s, so 7 s is two misses). */
#define STREAM_SLICE_MS    250
#define STREAM_TIMEOUT_MS 20000
#define STREAM_QUIET_MS    7000

/* Connection & discovery state */
static uint8_t s_target_mac[6];
static ble_addr_t s_peer_addr;
static bool s_device_found = false;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start_handle = 0;
static uint16_t s_svc_end_handle = 0;
static uint16_t s_rx_val_handle = 0;
static uint16_t s_rx_end_handle = 0;
static uint16_t s_tx_val_handle = 0;
static uint16_t s_cccd_handle = 0;

static SemaphoreHandle_t s_sem_scan = NULL;
static SemaphoreHandle_t s_sem_conn = NULL;
static SemaphoreHandle_t s_sem_disc = NULL;
static SemaphoreHandle_t s_sem_cccd = NULL;
static SemaphoreHandle_t s_sem_disc_done = NULL;
static SemaphoreHandle_t s_sem_mtu = NULL;
static uint16_t s_mtu = 0;
static int8_t s_rssi = 0;

static bool match_uuid(const ble_uuid_t *u, uint16_t val16) {
    if (!u) return false;
    if (u->type == BLE_UUID_TYPE_16) {
        return ((const ble_uuid16_t *)u)->value == val16;
    }
    if (u->type == BLE_UUID_TYPE_128) {
        const uint8_t *v = ((const ble_uuid128_t *)u)->value;
        // Standard Bluetooth SIG 16-bit expansion in 128-bit LE:
        // FB 34 9B 5F 80 00 00 80 00 10 00 00 [16-bit LE] 00 00
        return (v[12] == (val16 & 0xFF) && v[13] == ((val16 >> 8) & 0xFF) &&
                v[0] == 0xFB && v[1] == 0x34 && v[2] == 0x9B && v[3] == 0x5F &&
                v[4] == 0x80 && v[5] == 0x00 && v[6] == 0x00 && v[7] == 0x80 &&
                v[8] == 0x00 && v[9] == 0x10 && v[10] == 0x00 && v[11] == 0x00 &&
                v[14] == 0x00 && v[15] == 0x00);
    }
    return false;
}

/* ---------------------------------------------------------------- helpers */
static bool parse_mac(const char *str, uint8_t *out) {
    int v[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)v[i];
    }
    return true;
}

/* ------------------------------------------------------- GATT Callbacks */
static int on_write_cccd(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg) {
    if (error->status == 0) {
        ESP_LOGI(TAG, "CCCD subscribe succeeded");
        if (s_sem_cccd) xSemaphoreGive(s_sem_cccd);
    } else {
        ESP_LOGE(TAG, "CCCD subscribe failed: status=%d", error->status);
    }
    return 0;
}

static int on_disc_dsc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                       void *arg) {
    if (error->status == 0) {
        if (match_uuid(&dsc->uuid.u, 0x2902)) {
            s_cccd_handle = dsc->handle;
            ESP_LOGI(TAG, "Found CCCD handle: %d", s_cccd_handle);
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_sem_disc) xSemaphoreGive(s_sem_disc);
    }
    return 0;
}

static int on_disc_chr(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == 0) {
        if (match_uuid(&chr->uuid.u, 0xAAA1)) {
            s_rx_val_handle = chr->val_handle;
            s_rx_end_handle = chr->def_handle + 5;
            ESP_LOGI(TAG, "Found RX char handle: %d", s_rx_val_handle);
        } else if (match_uuid(&chr->uuid.u, 0xAAA2)) {
            s_tx_val_handle = chr->val_handle;
            ESP_LOGI(TAG, "Found TX char handle: %d", s_tx_val_handle);
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_rx_val_handle != 0) {
            uint16_t end_h = (s_rx_end_handle < s_svc_end_handle) ? s_rx_end_handle : s_svc_end_handle;
            ble_gattc_disc_all_dscs(conn_handle, s_rx_val_handle,
                                    end_h, on_disc_dsc, NULL);
        } else {
            ESP_LOGW(TAG, "RX char 0xAAA1 not found");
            if (s_sem_disc) xSemaphoreGive(s_sem_disc);
        }
    }
    return 0;
}

static int on_disc_svc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg) {
    if (error->status == 0) {
        if (match_uuid(&svc->uuid.u, 0xAAA0)) {
            s_svc_start_handle = svc->start_handle;
            s_svc_end_handle = svc->end_handle;
            ESP_LOGI(TAG, "Found 0xAAA0 Service: handles %d..%d",
                     s_svc_start_handle, s_svc_end_handle);
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_svc_start_handle != 0) {
            ble_gattc_disc_all_chrs(conn_handle, s_svc_start_handle,
                                    s_svc_end_handle, on_disc_chr, NULL);
        } else {
            ESP_LOGW(TAG, "0xAAA0 Service not found in service list");
            if (s_sem_disc) xSemaphoreGive(s_sem_disc);
        }
    }
    return 0;
}

static int on_exchange_mtu(uint16_t conn_handle, const struct ble_gatt_error *error,
                           uint16_t mtu, void *arg) {
    if (error->status == 0) {
        ESP_LOGI(TAG, "ATT MTU negotiated: %u", mtu);
    } else {
        ESP_LOGW(TAG, "MTU exchange failed: status=%d", error->status);
    }
    if (s_sem_mtu) xSemaphoreGive(s_sem_mtu);
    return 0;
}

/* -------------------------------------------------------- GAP Callbacks */
static int gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        const uint8_t *addr = event->disc.addr.val;
        // Compare byte by byte (NimBLE reports address in little-endian)
        if (addr[5] == s_target_mac[0] && addr[4] == s_target_mac[1] &&
            addr[3] == s_target_mac[2] && addr[2] == s_target_mac[3] &&
            addr[1] == s_target_mac[4] && addr[0] == s_target_mac[5]) {
            ESP_LOGI(TAG, "Target device found with RSSI %d", event->disc.rssi);
            s_rssi = (int8_t)event->disc.rssi;
            s_peer_addr = event->disc.addr;
            s_device_found = true;
            ble_gap_disc_cancel();
            if (s_sem_scan) xSemaphoreGive(s_sem_scan);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_sem_scan) xSemaphoreGive(s_sem_scan);
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE Connected (handle=%d)", event->connect.conn_handle);
            s_conn_handle = event->connect.conn_handle;
            if (s_sem_conn) xSemaphoreGive(s_sem_conn);
        } else {
            ESP_LOGW(TAG, "BLE Connection failed: status=%d", event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (s_sem_conn) xSemaphoreGive(s_sem_conn);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE Disconnected (reason=%d)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (s_sem_disc_done) xSemaphoreGive(s_sem_disc_done);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t raw[288];
        uint16_t len = 0;
        int rc = ble_hs_mbuf_to_flat(event->notify_rx.om, raw, sizeof(raw), &len);
        if (rc == 0 && len >= 9) {
            pk_frame_t parsed;
            if (pk_frame_parse(raw, len, &parsed)) {
                // Safety bound check for payload
                int hdr = parsed.stream ? 9 : 8;
                int avail = len - hdr - 1;
                if (avail >= 0 && parsed.len <= (uint16_t)avail) {
                    if (s_frame_count < MAX_STORED_FRAMES) {
                        stored_frame_t *sf = &s_frames[s_frame_count++];
                        sf->stream = parsed.stream;
                        sf->cmd = parsed.cmd;
                        sf->typ = parsed.typ;
                        sf->seq = parsed.seq;
                        sf->len = parsed.len;
                        if (parsed.len > 0 && parsed.len <= sizeof(sf->payload)) {
                            memcpy(sf->payload, parsed.payload, parsed.len);
                        }
                    }

                    if (s_collecting_stream && parsed.stream && parsed.cmd == 68) {
                        s_stream_total += parsed.len;
                        /* Keep the NEWEST bytes. A long backlog arrives oldest
                         * first and the core only needs the last two days, so
                         * when the buffer fills we drop from the front rather
                         * than refusing the rest - refusing would freeze
                         * last_drink_ts, which is the exact failure this whole
                         * change is about. Dropping stays 6-byte aligned so a
                         * record is never cut in half. */
                        if (parsed.len <= (int)sizeof(s_stream_blob)) {
                            int over = s_stream_blob_len + parsed.len
                                       - (int)sizeof(s_stream_blob);
                            if (over > 0) {
                                int drop = ((over + 5) / 6) * 6;
                                if (drop > s_stream_blob_len) drop = s_stream_blob_len;
                                memmove(s_stream_blob, s_stream_blob + drop,
                                        (size_t)(s_stream_blob_len - drop));
                                s_stream_blob_len -= drop;
                                s_window_slid = true;
                            }
                            memcpy(s_stream_blob + s_stream_blob_len,
                                   parsed.payload, parsed.len);
                            s_stream_blob_len += parsed.len;
                        }
                    }
                    /* Seen once the stream is exhausted, carrying a copy of the
                     * last chunk. Counted, not collected: adding it would
                     * double-count bytes against the CMD 212 promise. */
                    if (s_collecting_stream && parsed.stream && parsed.cmd == 82)
                        s_stream82_frames++;

                    /* "Chunk delivered, acknowledge me." The fountain repeats
                     * this every 3 s and sends nothing further until answered.
                     * Record it; the polling task decides whether to reply. */
                    if (!parsed.stream && parsed.cmd == 67 && parsed.typ == 1) {
                        s_ack_seq = parsed.seq;
                        s_ack_req = true;
                    }
                }
            }
        }
        return 0;
    }
    default:
        return 0;
    }
}

static void on_sync(void) {
    s_ble_synced = true;
    ESP_LOGI(TAG, "NimBLE host synced");
}

static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t pk_ble_init(void) {
    if (s_ble_inited) return ESP_OK;

    if (!s_sem_scan) s_sem_scan = xSemaphoreCreateBinary();
    if (!s_sem_conn) s_sem_conn = xSemaphoreCreateBinary();
    if (!s_sem_disc) s_sem_disc = xSemaphoreCreateBinary();
    if (!s_sem_cccd) s_sem_cccd = xSemaphoreCreateBinary();
    if (!s_sem_disc_done) s_sem_disc_done = xSemaphoreCreateBinary();
    if (!s_sem_mtu) s_sem_mtu = xSemaphoreCreateBinary();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(ble_host_task);

    // Wait up to 3s for NimBLE host sync
    for (int i = 0; i < 30 && !s_ble_synced; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!s_ble_synced) {
        ESP_LOGE(TAG, "NimBLE host sync timeout");
        return ESP_FAIL;
    }

    // Set BLE TX power level to P9 (~8.5 dBm)
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);

    s_ble_inited = true;
    return ESP_OK;
}

esp_err_t pk_ble_deinit(void) {
    if (!s_ble_inited) return ESP_OK;

    int rc = nimble_port_stop();
    if (rc != 0) {
        /* Do NOT tear the controller down underneath a host that is still
         * running - that is how one bad poll becomes a board that never
         * polls again. Leave the stack up and reuse it next cycle. */
        ESP_LOGE(TAG, "nimble_port_stop failed: rc=%d, keeping the stack up", rc);
        return ESP_FAIL;
    }
    /* nimble_port_deinit() already calls esp_bt_controller_disable() and
     * esp_bt_controller_deinit() itself (see nimble_port.c). Calling them
     * again here was a double teardown. */
    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_deinit failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ble_inited = false;
    s_ble_synced = false;
    ESP_LOGI(TAG, "NimBLE stopped and memory reclaimed");
    return ESP_OK;
}

/* The only REQUESTS this gateway is ever allowed to transmit. This is a
 * whitelist, not a blacklist, on purpose: anything not listed here cannot go
 * out even by accident.
 *
 * Two must never be sent, and both do permanent damage:
 *   CMD 73        rewrites deviceId + secret on the fountain and can lock the
 *                 official PetKit app out of it for good. Only a physical
 *                 factory reset recovers.
 *   CMD 69        the stream END acknowledgement. This is the one that
 *                 retires records so the phone app can never show them again,
 *                 and nothing here needs it.
 * Commands 220/221/222/225/226 change device state; this project is read-only
 * by design, so they are excluded too.
 *
 * CMD 67 does not travel this path at all, because it is a REPLY to the
 * fountain's own request rather than a request of ours. It goes out from
 * send_chunk_ack() below, and it destroys nothing - see the note there.
 */
static bool cmd_is_allowed(uint8_t cmd) {
    switch (cmd) {
    case 213: /* ping / deviceId + serial   */
    case 86:  /* verify secret              */
    case 210: /* state snapshot             */
    case 212: /* pending-record counter     */
    case 80:  /* start history stream       */
        return true;
    default:
        return false;
    }
}

/* Helper to write a GATT frame without response and dwell */
static bool send_cmd(uint16_t conn, uint16_t tx_h, uint8_t cmd, uint8_t typ,
                     const uint8_t *pl, uint16_t plen, uint8_t seq, uint32_t dwell_ms) {
    uint8_t frame_buf[PK_FRAME_MAX];

    if (!cmd_is_allowed(cmd)) {
        ESP_LOGE(TAG, "REFUSING to send CMD %d - not on the read-only "
                      "whitelist. See cmd_is_allowed().", cmd);
        return false;
    }

    int n = pk_frame_build(cmd, typ, pl, plen, seq, frame_buf);
    if (n <= 0) return false;

    int rc = ble_gattc_write_no_rsp_flat(conn, tx_h, frame_buf, (uint16_t)n);
    if (rc != 0) {
        ESP_LOGW(TAG, "write_no_rsp CMD %d failed: rc=%d", cmd, rc);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(dwell_ms));
    return true;
}

/* Answer the fountain's chunk request so it will send the next chunk.
 *
 * Shape taken from the wire: the fountain sends `fa fc fd 43 01 d8 00 00 fb`,
 * a request (typ 1) with an empty payload, and the convention throughout this
 * protocol is that a reply echoes the seq with typ 2.
 *
 * This does NOT consume. Measured 30.08.2026 by reading the pending counter on
 * the same connection either side of three acknowledgements: 48 bytes before,
 * 48 bytes after, while the request seq advanced 241 -> 242 -> 243, so the
 * fountain was answering rather than ignoring us. Retiring records is CMD 69's
 * job and this project never sends it. It is still kept out of send_cmd's
 * request whitelist, because it is a reply to the fountain rather than a
 * request of ours. */
static bool send_chunk_ack(uint16_t conn, uint16_t tx_h, uint8_t seq) {
    uint8_t frame_buf[PK_FRAME_MAX];
    uint8_t pl = 0x01;
    int n = pk_frame_build(PK_CMD_STREAMACK, 2, &pl, 1, seq, frame_buf);
    if (n <= 0) return false;
    int rc = ble_gattc_write_no_rsp_flat(conn, tx_h, frame_buf, (uint16_t)n);
    if (rc != 0) {
        ESP_LOGW(TAG, "chunk ack (seq %u) failed: rc=%d", seq, rc);
        return false;
    }
    return true;
}

bool pk_ble_poll(const char *mac_str, const uint8_t *secret8, pk_ble_result_t *res) {
    if (!res) return false;
    memset(res, 0, sizeof(*res));

    if (!parse_mac(mac_str, s_target_mac)) {
        snprintf(res->error, sizeof(res->error), "invalid MAC address format");
        return false;
    }

    if (pk_ble_init() != ESP_OK) {
        snprintf(res->error, sizeof(res->error), "BLE stack init failed");
        return false;
    }

    s_frame_count = 0;
    s_stream_blob_len = 0;
    s_collecting_stream = false;
    s_device_found = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_svc_start_handle = s_svc_end_handle = 0;
    s_rx_val_handle = s_tx_val_handle = s_cccd_handle = 0;

    xSemaphoreTake(s_sem_scan, 0);
    xSemaphoreTake(s_sem_conn, 0);
    xSemaphoreTake(s_sem_disc, 0);
    xSemaphoreTake(s_sem_cccd, 0);
    xSemaphoreTake(s_sem_disc_done, 0);
    xSemaphoreTake(s_sem_mtu, 0);
    s_mtu = 0;
    s_rssi = 0;

    /* 1. Scan for the fountain (15 s timeout) */
    struct ble_gap_disc_params disc_params = {
        .filter_duplicates = 1,
        .passive = 0,
        .itvl = BLE_GAP_SCAN_ITVL_MS(50),
        .window = BLE_GAP_SCAN_WIN_MS(30),
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
    };

    ESP_LOGI(TAG, "Scanning for %s (15s timeout)...", mac_str);
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 15000, &disc_params, gap_event, NULL);
    if (rc != 0) {
        snprintf(res->error, sizeof(res->error), "scan start failed: rc=%d", rc);
        pk_ble_deinit();
        return false;
    }

    xSemaphoreTake(s_sem_scan, pdMS_TO_TICKS(15500));
    ble_gap_disc_cancel();

    if (!s_device_found) {
        snprintf(res->error, sizeof(res->error), "device not found while scanning");
        pk_ble_deinit();
        return false;
    }

    /* 2. Connect (25 s timeout) */
    struct ble_gap_conn_params conn_params = {
        .scan_itvl = 0x0010,
        .scan_window = 0x0010,
        .itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN,
        .itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX,
        .latency = 0,
        .supervision_timeout = 0x0100,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };

    ESP_LOGI(TAG, "Connecting to device...");
    rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &s_peer_addr, 25000,
                          &conn_params, gap_event, NULL);
    if (rc != 0) {
        snprintf(res->error, sizeof(res->error), "connect call failed: rc=%d", rc);
        pk_ble_deinit();
        return false;
    }

    if (!xSemaphoreTake(s_sem_conn, pdMS_TO_TICKS(26000)) ||
        s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        snprintf(res->error, sizeof(res->error), "connection timed out");
        pk_ble_deinit();
        return false;
    }

    /* 2b. Raise the ATT MTU before anything else.
     *
     * The fountain starts this exchange itself, but NimBLE only registers a
     * handler for BLE_ATT_OP_MTU_REQ under MYNEWT_VAL(BLE_GATTS)
     * (ble_att.c), and BLE_GATTS follows CONFIG_BT_NIMBLE_ROLE_PERIPHERAL,
     * which this central build has off. So the fountain's request was
     * dropped - "ATT handler not found; op=0x02" - and the link stayed at
     * the 23-byte default. A notification then carries 20 bytes while the
     * CMD 230 state frame needs 42 of payload alone, so pk_frame_parse never
     * saw its terminating 0xFB and every single poll died as "could not
     * decode state". Measured on the board on 22 Aug.
     *
     * As the central we may open the exchange ourselves, and the RESPONSE
     * opcode IS handled, because that one sits under BLE_GATTC. */
    rc = ble_gattc_exchange_mtu(s_conn_handle, on_exchange_mtu, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "could not start MTU exchange: rc=%d", rc);
    } else {
        xSemaphoreTake(s_sem_mtu, pdMS_TO_TICKS(4000));
    }
    s_mtu = ble_att_mtu(s_conn_handle);
    ESP_LOGI(TAG, "ATT MTU in effect: %u", s_mtu);

    /* 3. Discover Service & Characteristics */
    ESP_LOGI(TAG, "Discovering all services...");
    ble_gattc_disc_all_svcs(s_conn_handle, on_disc_svc, NULL);
    if (!xSemaphoreTake(s_sem_disc, pdMS_TO_TICKS(8000)) ||
        s_rx_val_handle == 0 || s_tx_val_handle == 0 || s_cccd_handle == 0) {
        snprintf(res->error, sizeof(res->error), "GATT discovery failed");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        xSemaphoreTake(s_sem_disc_done, pdMS_TO_TICKS(2000));
        pk_ble_deinit();
        return false;
    }

    /* 4. Subscribe to RX notifications (write 0x0001 to CCCD) */
    uint8_t cccd_val[2] = { 0x01, 0x00 };
    ble_gattc_write_flat(s_conn_handle, s_cccd_handle, cccd_val, 2, on_write_cccd, NULL);
    if (!xSemaphoreTake(s_sem_cccd, pdMS_TO_TICKS(4000))) {
        snprintf(res->error, sizeof(res->error), "CCCD write timed out");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        xSemaphoreTake(s_sem_disc_done, pdMS_TO_TICKS(2000));
        pk_ble_deinit();
        return false;
    }

    /* 5. Poll Sequence */
    uint8_t zero2[2] = { 0, 0 };
    // Step A: CMD 213 (wake / idsn)
    send_cmd(s_conn_handle, s_tx_val_handle, 213, 1, zero2, 2, 1, 1500);

    // Step B: CMD 86 (authenticate)
    send_cmd(s_conn_handle, s_tx_val_handle, 86, 1, secret8, 8, 2, 2000);

    // Verify auth reply
    bool auth_ok = false;
    for (int i = 0; i < s_frame_count; i++) {
        if (s_frames[i].cmd == 86 && s_frames[i].len >= 1 &&
            s_frames[i].payload[0] == 0x01) {
            auth_ok = true;
            break;
        }
    }
    if (!auth_ok) {
        snprintf(res->error, sizeof(res->error), "no authentication reply");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        xSemaphoreTake(s_sem_disc_done, pdMS_TO_TICKS(2000));
        pk_ble_deinit();
        return false;
    }

    // Step C: CMD 210 (request state)
    send_cmd(s_conn_handle, s_tx_val_handle, 210, 1, zero2, 2, 3, 1500);

    int mark = s_frame_count;

    // Step D: CMD 212 (request sync / pending count)
    send_cmd(s_conn_handle, s_tx_val_handle, 212, 1, NULL, 0, 4, 2000);

    // Step E: Select state (prefer CMD 230 >= 42 bytes; fallback to CMD 210 >= 26 bytes)
    bool state_decoded = false;
    for (int i = 0; i < s_frame_count; i++) {
        if (s_frames[i].cmd == 230 && s_frames[i].len >= 42) {
            if (pk_state_decode(s_frames[i].payload, s_frames[i].len, &res->state)) {
                state_decoded = true;
            }
        }
    }
    if (!state_decoded) {
        for (int i = 0; i < s_frame_count; i++) {
            if (s_frames[i].cmd == 210 && s_frames[i].len >= 26) {
                if (pk_state_decode(s_frames[i].payload, s_frames[i].len, &res->state)) {
                    state_decoded = true;
                }
            }
        }
    }

    // Step F: Decode pending counter
    int32_t pending = 0;
    for (int i = mark; i < s_frame_count; i++) {
        if (s_frames[i].cmd == 212 && !s_frames[i].stream) {
            int32_t p = pk_sync_pending(s_frames[i].payload, s_frames[i].len);
            if (p > 0) pending = p;
        }
    }

    // Step G: Fetch history stream if pending > 0
    res->pending_bytes = (uint32_t)(pending > 0 ? pending : 0);
    if (pending > 0) {
        s_stream_blob_len = 0;
        s_stream_total = 0;
        s_stream82_frames = 0;
        s_window_slid = false;
        s_ack_req = false;
        s_collecting_stream = true;

        // CMD 80: setStreamSetting(window=32, mtu=158)
        static const uint8_t stream_start_pl[8] = {
            0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x9E
        };
        send_cmd(s_conn_handle, s_tx_val_handle, 80, 1, stream_start_pl, 8, 11,
                 STREAM_SLICE_MS);

        /* Collect until we have every byte CMD 212 promised, rather than for a
         * fixed 4 s. The old wait was not a transfer-speed problem - the whole
         * first chunk lands in about a millisecond - it was that there was no
         * second chunk to wait for unless we acknowledged the first. */
        int last_len = -1, acked_at = -1, quiet_ms = 0;
        for (int waited = 0; waited < STREAM_TIMEOUT_MS; waited += STREAM_SLICE_MS) {
            vTaskDelay(pdMS_TO_TICKS(STREAM_SLICE_MS));
            esp_task_wdt_reset();

            int have = s_stream_total;
            if (have != last_len) { last_len = have; quiet_ms = 0; }
            else                  { quiet_ms += STREAM_SLICE_MS; }

            /* We have everything CMD 212 promised. Nothing more to unlock, so
             * do not answer the request at all - on a normal day the whole
             * backlog is one chunk and this exits immediately, costing the
             * poll nothing. */
            if (have >= pending) break;

            /* Short. The fountain is holding the rest behind its flow control,
             * so answer it. One reply per chunk, and only once that chunk's
             * bytes are in hand: the request repeats every 3 s and answering a
             * repeat could advance its cursor past a chunk we never received. */
            if (s_ack_req && have > 0 && have > acked_at) {
                uint8_t seq = s_ack_seq;
                s_ack_req = false;
                if (send_chunk_ack(s_conn_handle, s_tx_val_handle, seq)) {
                    acked_at = have;
                    quiet_ms = 0;
                    if (res->acks < 255) res->acks++;
                }
                continue;
            }

            if (quiet_ms >= STREAM_QUIET_MS) break;
        }
        s_collecting_stream = false;

        /* Decode the whole records we have. A trailing partial record used to
         * throw the entire read away; keeping the good part and flagging the
         * read is strictly better, because the flag is what stops the core
         * calling an unread day a dry one. */
        res->stream_bytes = (uint32_t)s_stream_total;
        if (s_stream_blob_len > 0) {
            res->visit_count = pk_history_decode(s_stream_blob, s_stream_blob_len,
                                                 res->visits, PK_BLE_MAX_VISITS);
        }
        res->hist_short = (s_stream_total < pending ||
                           s_stream_total % 6 != 0) ? 1 : 0;

        ESP_LOGI(TAG, "history: %d of %ld bytes, %d visits, %u ack(s)%s%s",
                 s_stream_total, (long)pending, res->visit_count,
                 (unsigned)res->acks, s_window_slid ? ", window slid" : "",
                 res->hist_short ? "  SHORT" : "");
        if (res->hist_short) {
            /* The failure this whole change exists for. On 29.08.2026 it was
             * this exact condition and it printed nothing at all. Reaching it
             * now means the flow-control replies did not unlock the rest, so
             * the assumption that they would is the thing to go and check. */
            ESP_LOGW(TAG, "SHORT history read - the fountain is holding records "
                          "back after %u acknowledgement(s). One 512-byte chunk "
                          "is 85 records; syncing the phone app clears it.",
                     (unsigned)res->acks);
        }
        if (s_stream82_frames)
            ESP_LOGI(TAG, "%d CMD 82 tail frame(s) seen (counted, not collected)",
                     s_stream82_frames);
    }

    /* Sample the radio-on half of the cycle here: NimBLE is fully up, the
     * history stream has been collected, and nothing has been released
     * yet. This is the number the whole BLE-then-WiFi architecture was
     * built around, and until now it was only ever estimated. */
    res->rssi = s_rssi;
    res->mtu = s_mtu;
    res->heap_phase = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    // 6. Clean Disconnect
    ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    xSemaphoreTake(s_sem_disc_done, pdMS_TO_TICKS(3000));

    pk_ble_deinit();

    if (!state_decoded) {
        /* Say what actually arrived. The bare "could not decode state" cost a
         * day of guessing; a frame list plus the MTU points straight at the
         * cause, because a truncating MTU shows up as very few short frames. */
        char seen[160];
        int k = 0;
        for (int i = 0; i < s_frame_count; i++) {
            if (k >= (int)sizeof(seen) - 20) break;
            k += snprintf(seen + k, sizeof(seen) - k, "%s%u%s/%u",
                          i ? " " : "", s_frames[i].cmd,
                          s_frames[i].stream ? "s" : "", s_frames[i].len);
        }
        if (s_frame_count == 0) snprintf(seen, sizeof(seen), "(none)");
        ESP_LOGW(TAG, "no usable state frame. MTU=%u, frames: %s", s_mtu, seen);
        snprintf(res->error, sizeof(res->error),
                 "could not decode state (mtu %u, %d frames)", s_mtu, s_frame_count);
        return false;
    }

    return true;
}
