/* pk_nvs.c - see pk_nvs.h */
#include "pk_nvs.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "PK_NVS";
#define NVS_NAMESPACE "pk_state"

static pk_nvs_state_t s_cached_state;
static bool s_has_cache = false;

esp_err_t pk_nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash due to init error: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t pk_nvs_load(pk_nvs_state_t *st) {
    if (!st) return ESP_ERR_INVALID_ARG;
    memset(st, 0, sizeof(*st));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No saved state in NVS (fresh start)");
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Failed to open NVS for reading: %s", esp_err_to_name(err));
        return err;
    }

    nvs_get_u8(h, "thirst_lvl", &st->thirst_level);
    nvs_get_u8(h, "a_no_water", &st->a_no_water);
    nvs_get_u8(h, "a_fault", &st->a_fault);
    nvs_get_u8(h, "bat_latch", &st->battery_latch);
    nvs_get_u8(h, "flt_latch", &st->filter_latch);
    nvs_get_i32(h, "day_no", &st->day_no);
    nvs_get_i32(h, "rep_day", &st->last_report_day);
    nvs_get_u32(h, "drink_ts", &st->last_drink_ts);

    nvs_close(h);

    s_cached_state = *st;
    s_has_cache = true;

    ESP_LOGI(TAG, "Loaded state: thirst=%d day=%ld rep_day=%ld last_drink=%lu",
             st->thirst_level, (long)st->day_no, (long)st->last_report_day, (unsigned long)st->last_drink_ts);
    return ESP_OK;
}

esp_err_t pk_nvs_save(const pk_nvs_state_t *st) {
    if (!st) return ESP_ERR_INVALID_ARG;

    // Check if anything actually changed to prevent flash wear
    if (s_has_cache && memcmp(&s_cached_state, st, sizeof(*st)) == 0) {
        return ESP_OK;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u8(h, "thirst_lvl", st->thirst_level);
    nvs_set_u8(h, "a_no_water", st->a_no_water);
    nvs_set_u8(h, "a_fault", st->a_fault);
    nvs_set_u8(h, "bat_latch", st->battery_latch);
    nvs_set_u8(h, "flt_latch", st->filter_latch);
    nvs_set_i32(h, "day_no", st->day_no);
    nvs_set_i32(h, "rep_day", st->last_report_day);
    nvs_set_u32(h, "drink_ts", st->last_drink_ts);

    err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        s_cached_state = *st;
        s_has_cache = true;
        ESP_LOGI(TAG, "Saved state to NVS: thirst=%d day=%ld rep_day=%ld last_drink=%lu",
                 st->thirst_level, (long)st->day_no, (long)st->last_report_day, (unsigned long)st->last_drink_ts);
    } else {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }
    return err;
}
