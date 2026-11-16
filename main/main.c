#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/i2s_std.h"
#include "driver/i2c.h"
#include "driver/gpio.h"

#include "es8311.h"

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
#define ES8311_I2C_ADDR 0x18            /* ES8311 codec 7-bit address */

/* Audio amplifier enable (NS4150B, active high) */
#define SPEAKER_AMP_EN_PIN  GPIO_NUM_53

#define I2C_MASTER_NUM      0
#define I2C_MASTER_FREQ_HZ  400000

#define I2S_PORT            I2S_NUM_0
#define I2S_DMA_BUFFER_LEN  1024
#define SAMPLE_RATE         32000
#define I2S_CHANNELS        1
#define I2S_BITS_PER_SAMPLE 16
#define I2S_MCLK_MULTIPLE   384
#define TEST_CHORD_SECONDS  2
#define TEST_SILENCE_SECONDS 1
#define TEST_BUFFER_FRAMES  256

static i2s_chan_handle_t tx_handle = NULL;
static es8311_handle_t codec_handle = NULL;

esp_err_t init_i2c(void)
{
    const i2c_config_t bus_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_NUM_0, &bus_cfg), TAG,
                        "Failed to configure I2C master bus");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0), TAG,
                        "Failed to install I2C master driver");

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
    codec_handle = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(codec_handle != NULL, ESP_FAIL, TAG,
                        "Failed to create ES8311 codec");

    const es8311_clock_config_t clock_cfg = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = SAMPLE_RATE * I2S_MCLK_MULTIPLE,
        .sample_frequency = SAMPLE_RATE,
    };

    ESP_RETURN_ON_ERROR(es8311_init(codec_handle, &clock_cfg,
                                    ES8311_RESOLUTION_16, ES8311_RESOLUTION_16), TAG,
                        "Failed to initialize ES8311 codec");
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(
                            codec_handle, SAMPLE_RATE * I2S_MCLK_MULTIPLE, SAMPLE_RATE),
                        TAG, "Failed to configure ES8311 sample frequency");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(codec_handle, 70, NULL), TAG,
                        "Failed to set ES8311 volume");
    ESP_RETURN_ON_ERROR(es8311_voice_mute(codec_handle, false), TAG,
                        "Failed to unmute ES8311 output");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(codec_handle, false), TAG,
                        "Failed to disable ES8311 microphone");

    ESP_LOGI(TAG, "ES8311 codec initialized: %d Hz, volume 70, output unmuted", SAMPLE_RATE);

    return ESP_OK;
}

esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_handle, NULL), TAG,
                        "Failed to create I2S TX channel");

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

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_handle, &std_cfg), TAG,
                        "Failed to initialize I2S standard mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_handle), TAG,
                        "Failed to enable I2S TX channel");

    ESP_LOGI(TAG, "I2S initialized: MCLK=%d, BCK=%d, WS=%d, DO=%d, DI=%d",
             I2S_MCLK_PIN, I2S_BCK_PIN, I2S_WS_PIN, I2S_DO_PIN, I2S_DI_PIN);

    return ESP_OK;
}

esp_err_t init_synth(void)
{
    ESP_LOGI(TAG, "Audio system ready at %d Hz", SAMPLE_RATE);
    return ESP_OK;
}

static void play_test_chord(void)
{
    int16_t samples[TEST_BUFFER_FRAMES * 2];
    size_t bytes_written = 0;
    const float frequencies[] = {261.63f, 329.63f, 392.00f};
    const float amplitude = 5000.0f;
    const float pi2 = 6.28318530718f;

    ESP_LOGI(TAG, "Playing C-major test chord");
    for (int frame = 0; frame < SAMPLE_RATE * TEST_CHORD_SECONDS;
         frame += TEST_BUFFER_FRAMES) {
        for (int i = 0; i < TEST_BUFFER_FRAMES; ++i) {
            const float t = (float)(frame + i) / SAMPLE_RATE;
            float sample = 0.0f;
            for (size_t note = 0; note < 3; ++note) {
                sample += sinf(pi2 * frequencies[note] * t);
            }
            const int16_t value = (int16_t)(amplitude * sample / 3.0f);
            samples[i * 2] = value;
            samples[i * 2 + 1] = value;
        }
        ESP_RETURN_VOID_ON_ERROR(i2s_channel_write(
                                    tx_handle, samples, sizeof(samples), &bytes_written,
                                    portMAX_DELAY), TAG, "I2S chord write failed");
    }

    memset(samples, 0, sizeof(samples));
    for (int frame = 0; frame < SAMPLE_RATE * TEST_SILENCE_SECONDS;
         frame += TEST_BUFFER_FRAMES) {
        ESP_RETURN_VOID_ON_ERROR(i2s_channel_write(
                                    tx_handle, samples, sizeof(samples), &bytes_written,
                                    portMAX_DELAY), TAG, "I2S silence write failed");
    }
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

    if (init_speaker_amp() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize speaker amp");
        return;
    }

    if (init_synth() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize synth");
        return;
    }

    ESP_LOGI(TAG, "=== Boot complete ===");

    /* Play the bounded test chord repeatedly for audible hardware validation. */
    while (1) {
        play_test_chord();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
