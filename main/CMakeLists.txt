idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        es8311
        esp_system
        esp_timer
        driver
        freertos
)
