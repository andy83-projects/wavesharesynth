idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp32_p4_wifi6_touch_lcd_xc
        esp_lvgl_adapter
        lvgl
        esp_system
        esp_timer
        freertos
)
