/* pk_ble.h - NimBLE Central transport for PetKit water fountain.
 *
 * Implements the verified BLE central polling sequence:
 * connect -> CMD 213 (wake) -> CMD 86 (auth) -> CMD 210/230 (state) ->
 * CMD 212 (pending count) -> CMD 80 (stream start) -> collect CMD 68 stream
 * frames -> disconnect.
 *
 * PROHIBITIONS:
 * - NEVER send CMD 69 (Stream End Acknowledgment). This is the one that
 *   consumes: it retires the records and the phone app can then never display
 *   them again. Nothing here needs it.
 * - NEVER send CMD 73 (Locks device secret).
 * - NEVER send CMD 220, 221, 222, 225, 226 (State modifying commands).
 *
 * CMD 67 IS sent, on every poll, and it destroys nothing.
 *
 * It is not a cleanup command, it is the stream's flow control: the fountain
 * sends one 512-byte chunk - 85 whole records - then asks for a CMD 67 every
 * three seconds and sends nothing more until it gets one. A reader that never
 * answers works perfectly up to 85 unread records and then freezes on that
 * same chunk forever. Measured on 29.08.2026, where it cost a full day of
 * drinking history and two false thirst alarms.
 *
 * That answering is harmless was measured on 30.08.2026, not assumed: the
 * pending counter was read on the same connection before and after three
 * acknowledgements and did not move, 48 bytes both times. The fountain
 * answered each one - the request seq advanced 241, 242, 243 - so the frame
 * shape is right and the records stayed put. Consuming is CMD 69's job.
 */
#ifndef PK_BLE_H
#define PK_BLE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "petkit_core.h"

#define PK_BLE_MAX_VISITS 256

typedef struct {
    pk_state_t state;
    pk_visit_t visits[PK_BLE_MAX_VISITS];
    int visit_count;
    char error[128];
    int8_t   rssi;        /* fountain signal at the moment we found it   */
    uint16_t mtu;         /* ATT MTU actually in effect for this poll    */
    uint32_t heap_phase;  /* free internal heap with the BLE stack fully
                           * up and the history collected - the tightest
                           * point of the radio-on half of the cycle    */
    /* CMD 212 promises a byte count and the stream either delivers it or does
     * not. Keeping both numbers is what turns a frozen feed from an invisible
     * "no drinks today" into something the core can report. */
    uint32_t pending_bytes;   /* what CMD 212 said was waiting  */
    uint32_t stream_bytes;    /* how much of it actually arrived */
    uint8_t  hist_short;      /* got less than was promised     */
    uint8_t  acks;            /* flow-control replies sent      */
} pk_ble_result_t;

esp_err_t pk_ble_init(void);
esp_err_t pk_ble_deinit(void);

bool pk_ble_poll(const char *mac_str, const uint8_t *secret8, pk_ble_result_t *res);

#endif /* PK_BLE_H */
