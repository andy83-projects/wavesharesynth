idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        es8311
        esp32_p4_wifi6_touch_lcd_xc
        esp_system
        esp_timer
        driver
        freertos
)
