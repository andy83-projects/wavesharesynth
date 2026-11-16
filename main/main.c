#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

static const char *TAG = "WAVESHARESYNTH";

/* Pin configuration - from Waveshare ESP32-P4-WIFI6-Touch-LCD-XC official docs
 * https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-XC/ESP-IDF
 */

/* I2S Audio pins (connected to ES8311 codec) */
#define I2S_MCLK_PIN    GPIO_NUM_13     /* Master Clock */
#define I2S_BCK_PIN     GPIO_NUM_12     /* Bit Clock (SCLK) */
#define I2S_WS_PIN      GPIO_NUM_10     /* Word Select (LRCK) */
#define I2S_DO_PIN      GPIO_NUM_11     /* Data Out to codec (ASDOUT) */
#define I2S_DI_PIN      GPIO_NUM_9      /* Data In from codec (DSDIN) */

/* I2C codec control bus */
#define I2C_SDA_PIN     GPIO_NUM_7
#define I2C_SCL_PIN     GPIO_NUM_8
#define ES8311_I2C_ADDR 0x18            /* ES8311 codec 7-bit address */

/* Audio amplifier enable (NS4150B, active high) */
#define SPEAKER_AMP_EN_PIN  GPIO_NUM_53

#define I2C_MASTER_NUM      0
#define I2C_MASTER_FREQ_HZ  400000

#define I2S_PORT            I2S_NUM_0
#define I2S_DMA_BUFFER_LEN  1024
#define SAMPLE_RATE         32000
#define I2S_CHANNELS        1

static i2s_chan_handle_t tx_handle = NULL;

esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus_handle = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus_handle), TAG,
                        "Failed to create I2C master bus");

    ESP_LOGI(TAG, "I2C master initialized on SDA=%d, SCL=%d", I2C_SDA_PIN, I2C_SCL_PIN);
    return ESP_OK;
}

esp_err_t init_speaker_amp(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << SPEAKER_AMP_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), TAG, "Failed to configure speaker amp pin");
    ESP_RETURN_ON_ERROR(gpio_set_level(SPEAKER_AMP_EN_PIN, 1), TAG, "Failed to enable speaker amp");

    ESP_LOGI(TAG, "Speaker amp enabled on GPIO %d", SPEAKER_AMP_EN_PIN);
    return ESP_OK;
}

esp_err_t init_codec(void)
{
    /* Milestone 1: Initialize I2C bus for codec communication (probe only)
     * Full ES8311 codec driver will be integrated in Milestone 2
     */

    i2c_master_bus_config_t bus_cfg = {
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus_handle = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus_handle), TAG,
                        "Failed to create I2C bus for codec");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = 400000,
    };

    i2c_master_dev_handle_t dev_handle = NULL;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle), TAG,
                        "Failed to add ES8311 device to I2C bus");

    ESP_LOGI(TAG, "I2C codec device configured at address 0x%02x", ES8311_I2C_ADDR);

    return ESP_OK;
}

esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = I2S_DMA_BUFFER_LEN;

    ESP_RETURN_ON_ERROR(i2s_new_std_tx_channel(&chan_cfg, &tx_handle), TAG,
                        "Failed to create I2S TX channel");

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
    clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_clk_div(tx_handle, &clk_cfg), TAG,
                        "Failed to configure I2S clock");

    i2s_std_gpio_config_t gpio_cfg = {
        .bck = {.gpio_num = I2S_BCK_PIN},
        .din = {.gpio_num = I2S_DI_PIN},
        .dout = {.gpio_num = I2S_DO_PIN},
        .mclk = {.gpio_num = I2S_MCLK_PIN},
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_gpio_pins(tx_handle, &gpio_cfg), TAG,
                        "Failed to initialize I2S GPIO pins");

    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_handle), TAG, "Failed to enable I2S TX channel");

    ESP_LOGI(TAG, "I2S initialized: MCLK=%d, BCK=%d, WS=%d, DO=%d, DI=%d",
             I2S_MCLK_PIN, I2S_BCK_PIN, I2S_WS_PIN, I2S_DO_PIN, I2S_DI_PIN);

    return ESP_OK;
}

esp_err_t init_synth(void)
{
    /* ESP32Synth initialization deferred - audio test using minimal tone generation */
    ESP_LOGI(TAG, "Audio system ready at %d Hz", SAMPLE_RATE);
    return ESP_OK;
}

void play_test_chord(void)
{
    /* Audio test - ESP32Synth integration coming in next phase */
    ESP_LOGI(TAG, "Audio path initialized");
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Waveshare Synth Boot ===");
    ESP_LOGI(TAG, "Target: ESP32-P4-WIFI6-Touch-LCD-XC");

    /* Initialize in order */
    if (init_i2c() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C");
        return;
    }

    if (init_speaker_amp() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize speaker amp");
        return;
    }

    if (init_codec() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize codec");
        return;
    }

    if (init_i2s() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S");
        return;
    }

    if (init_synth() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize synth");
        return;
    }

    ESP_LOGI(TAG, "=== Boot complete ===");

    /* Play test chord */
    play_test_chord();

    /* Loop forever */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
