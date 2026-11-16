idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_system
        esp_timer
        driver
        es8311
        esp32_synth
        freertos
)
