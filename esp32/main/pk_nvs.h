/* pk_nvs.h - NVS persistence for PetKit Gateway alarm latches and state.
 *
 * Saves ~17 bytes of alarm/day state across reboots so power cuts do not
 * trigger false alarms, re-arm latches, or lose the daily summary report.
 * Only writes to flash when state changes (preventing flash wear).
 *
 * It also saves the visit list, and that part is not a convenience. Once the
 * gateway acknowledges the fountain's history, those records are gone from the
 * fountain for good, so this flash copy is the only one that exists. The 5 to
 * 10 minute power cut on the night of 29.08.2026 is the case it is for: a
 * reboot must not be able to lose a day.
 */
#ifndef PK_NVS_H
#define PK_NVS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "petkit_core.h"

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

/* The visit list. save is a no-op when nothing changed, so it is safe to call
 * every cycle. load returns the number of visits restored, 0 if there are
 * none. Both are plain struct bytes: same compiler, same target, one machine. */
esp_err_t pk_nvs_save_visits(const pk_visit_t *v, int n);
int       pk_nvs_load_visits(pk_visit_t *v, int max);

#endif /* PK_NVS_H */
