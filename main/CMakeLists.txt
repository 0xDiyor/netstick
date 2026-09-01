idf_component_register(
    SRCS
        "app_main.c"
        "display.c"
        "gfx.c"
        "led.c"
        "button.c"
        "storage.c"
        "netmgr.c"
        "netprobe.c"
        "speed.c"
        "doctor.c"
        "survey.c"
        "ui.c"
        "provision.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        driver
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_spi
        esp_driver_sdspi
        esp_driver_usb_serial_jtag
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_http_client
        esp_http_server
        esp-tls
        mbedtls
        nvs_flash
        fatfs
        sdmmc
        vfs
        lwip
)

# Fixed-width screen strings are truncated on purpose all over the UI.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
