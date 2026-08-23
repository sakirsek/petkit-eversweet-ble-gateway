idf_component_register(
    SRCS
        "main.c"
        "pk_ble.c"
        "pk_net.c"
        "pk_nvs.c"
        "../../core/petkit_core.c"
    INCLUDE_DIRS
        "."
        "../../core"
    REQUIRES
        nvs_flash
        bt
        esp_wifi
        esp_netif
        esp_http_client
        mbedtls
        lwip
        esp_timer
        driver
)

# The frozen C99 core prints uint32_t with %u, which on RISC-V (where uint32_t
# is unsigned long) trips -Wformat. Silence it for THAT FILE ONLY. Disabling
# format checking across the whole component is how a broken %lld reached the
# board once: the console printed the literal text "ld" for the cycle time.
set_source_files_properties(../../core/petkit_core.c
    PROPERTIES COMPILE_OPTIONS "-Wno-format;-Wno-format-truncation")
