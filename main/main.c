#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_codec_dev.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_check.h"

static const char *TAG = "WAVESHARESYNTH";

/* Pin configuration - from Waveshare ESP32-P4-WIFI6-Touch-LCD-XC official docs
 * https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-XC/ESP-IDF
 */

/* I2S Audio pins (connected to ES8311 codec) */
#define I2S_MCLK_PIN    GPIO_NUM_13     /* Master Clock */
#define I2S_BCK_PIN     GPIO_NUM_12     /* Bit Clock (SCLK) */
#define I2S_WS_PIN      GPIO_NUM_10     /* Word Select (LRCK) */
#define I2S_DO_PIN      GPIO_NUM_9      /* Data Out to codec (DSDIN) */
#define I2S_DI_PIN      GPIO_NUM_11     /* Data In from codec (ASDOUT) */

/* I2C codec control bus */
#define I2C_SDA_PIN     GPIO_NUM_7
#define I2C_SCL_PIN     GPIO_NUM_8
#define SAMPLE_RATE         32000
#define I2S_MCLK_MULTIPLE   384

static esp_codec_dev_handle_t speaker_codec_handle = NULL;
static esp_lcd_panel_handle_t display_panel = NULL;
static esp_lcd_panel_io_handle_t display_io = NULL;

esp_err_t init_i2c(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "Failed to initialize BSP I2C bus");

    ESP_LOGI(TAG, "BSP I2C master initialized on SDA=%d, SCL=%d (port %d)",
             I2C_SDA_PIN, I2C_SCL_PIN, BSP_I2C_NUM);
    return ESP_OK;
}

esp_err_t init_codec(void)
{
    speaker_codec_handle = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(speaker_codec_handle != NULL, ESP_FAIL, TAG,
                        "Failed to initialize ES8311 speaker codec");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(speaker_codec_handle, 70), TAG,
                        "Failed to set ES8311 output volume");

    ESP_LOGI(TAG, "ES8311 speaker codec initialized through Waveshare BSP: %d Hz, volume 70",
             SAMPLE_RATE);

    return ESP_OK;
}

esp_err_t init_i2s(void)
{
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DO_PIN,
            .din = I2S_DI_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(bsp_audio_init(&std_cfg), TAG,
                        "Failed to initialize BSP I2S audio");

    ESP_LOGI(TAG, "I2S initialized: MCLK=%d, BCK=%d, WS=%d, DO=%d, DI=%d",
             I2S_MCLK_PIN, I2S_BCK_PIN, I2S_WS_PIN, I2S_DO_PIN, I2S_DI_PIN);

    return ESP_OK;
}

esp_err_t init_synth(void)
{
    ESP_LOGI(TAG, "Audio system ready at %d Hz", SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t init_display(void)
{
    ESP_RETURN_ON_ERROR(bsp_display_new(NULL, &display_panel, &display_io), TAG,
                        "Failed to initialize 4C MIPI-DSI display");
    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG,
                        "Failed to enable display backlight");

    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_set_pattern(
                            display_panel, MIPI_DSI_PATTERN_BAR_VERTICAL), TAG,
                        "Failed to set display test pattern");

    ESP_LOGI(TAG, "4C display initialized: 720x720 color-bar test pattern");
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Waveshare Synth Boot ===");
    ESP_LOGI(TAG, "Target: ESP32-P4-WIFI6-Touch-LCD-XC");

    /* Initialize buses before the codec and audio path. */
    if (init_i2c() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C");
        return;
    }

    if (init_i2s() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S");
        return;
    }

    if (init_codec() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize codec");
        return;
    }

    if (init_synth() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize synth");
        return;
    }

    if (init_display() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display");
        return;
    }

    ESP_LOGI(TAG, "=== Boot complete ===");

    /* Keep the audio path initialized, but leave playback silent until the UI
     * provides an explicit sound event.  The repeating chord was only for the
     * Milestone 1 hardware test. */
    ESP_LOGI(TAG, "Audio test chord disabled; waiting for UI input");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
