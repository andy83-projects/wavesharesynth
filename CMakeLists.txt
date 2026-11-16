cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/version.cmake)

project(wavesharesynth)

set(COMPONENTS
    esp_common
    esp_rom
    esp_hw_support
    esp_system
    hal
    freertos
    soc
    esp_idf_version
    newlib
    driver
    esp_adc_cal
    esp_timer
    esp_partition
    app_update
    bootloader_support
    spi_flash
    mbedtls
    mqtt
    fatfs
    wear_levelling
    spiffs
    http_parser
    json
    log
    lwip
    vfs
    wpa_supplicant
    pthread
    cxx
    esp_eth
    ethernet
    esp_netif
    tcp_transport
    esp_http_client
    esp_http_server
    esp_https_ota
    sdmmc
    sd_card
    esp_lcd
    esp_lvgl_port
    lvgl
    display_drivers
    button
    knob
    touch_element
    bsp_esp32_p4
    es8311
    esp32_synth
)

idf_build_process(esp32p4 "${COMPONENTS}")
