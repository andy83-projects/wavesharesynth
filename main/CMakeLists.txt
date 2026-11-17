idf_component_register(
    SRCS "main.cpp" "ui/synth_ui.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp32_p4_wifi6_touch_lcd_xc
        esp32synth
        open303
        esp_lvgl_adapter
        lvgl
        esp_system
        esp_timer
        freertos
)
