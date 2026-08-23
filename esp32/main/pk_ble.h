/* pk_ble.h - NimBLE Central transport for PetKit water fountain.
 *
 * Implements the verified BLE central polling sequence:
 * connect -> CMD 213 (wake) -> CMD 86 (auth) -> CMD 210/230 (state) ->
 * CMD 212 (pending count) -> CMD 80 (stream start) -> collect CMD 68 stream
 * frames -> disconnect.
 *
 * PROHIBITIONS:
 * - NEVER send CMD 67 or CMD 69 (Stream Acknowledgment).
 * - NEVER send CMD 73 (Locks device secret).
 * - NEVER send CMD 220, 221, 222, 225, 226 (State modifying commands).
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
} pk_ble_result_t;

esp_err_t pk_ble_init(void);
esp_err_t pk_ble_deinit(void);

bool pk_ble_poll(const char *mac_str, const uint8_t *secret8, pk_ble_result_t *res);

#endif /* PK_BLE_H */
