/* pk_nvs.h - NVS persistence for PetKit Gateway alarm latches and state.
 *
 * Saves ~17 bytes of alarm/day state across reboots so power cuts do not
 * trigger false alarms, re-arm latches, or lose the daily summary report.
 * Only writes to flash when state changes (preventing flash wear).
 */
#ifndef PK_NVS_H
#define PK_NVS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint8_t  thirst_level;
    uint8_t  a_no_water;
    uint8_t  a_fault;
    uint8_t  battery_latch;
    uint8_t  filter_latch;
    int32_t  day_no;
    int32_t  last_report_day;
    uint32_t last_drink_ts;
} pk_nvs_state_t;

esp_err_t pk_nvs_init(void);
esp_err_t pk_nvs_load(pk_nvs_state_t *st);
esp_err_t pk_nvs_save(const pk_nvs_state_t *st);

#endif /* PK_NVS_H */
