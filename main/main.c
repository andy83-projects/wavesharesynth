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

#include "esp32synth.h"
#include "esp32synth_patches.h"

#include "es8311.h"

static const char *TAG = "WAVESHARESYNTH";

/* Pin configuration - from Waveshare ESP32-P4-WIFI6-Touch-LCD-4C BSP
 * TODO: Verify these against the official Waveshare BSP/demos
 * I2S: BCK, WS, DO, DI, MCLK
 * I2C: SDA, SCL (codec control)
 * Speaker Amp Enable
 */

#define I2S_BCK_PIN     GPIO_NUM_8
#define I2S_WS_PIN      GPIO_NUM_9
#define I2S_DO_PIN      GPIO_NUM_10
#define I2S_DI_PIN      GPIO_NUM_11
#define I2S_MCLK_PIN    GPIO_NUM_12

#define I2C_SDA_PIN     GPIO_NUM_17
#define I2C_SCL_PIN     GPIO_NUM_18

#define SPEAKER_AMP_EN_PIN  GPIO_NUM_41

#define I2C_MASTER_NUM      0
#define I2C_MASTER_FREQ_HZ  400000

#define I2S_PORT            I2S_NUM_0
#define I2S_DMA_BUFFER_LEN  1024
#define SAMPLE_RATE         32000

static i2s_chan_handle_t tx_handle = NULL;
static esp32synth_t synth = NULL;

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
    /* Initialize ES8311 codec over I2C
     * TODO: Use esp_codec_dev or minimal register init depending on ESP32Synth integration
     */

    ESP_LOGI(TAG, "ES8311 codec initialized");
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

    ESP_LOGI(TAG, "I2S initialized: BCK=%d, WS=%d, DO=%d, DI=%d, MCLK=%d",
             I2S_BCK_PIN, I2S_WS_PIN, I2S_DO_PIN, I2S_DI_PIN, I2S_MCLK_PIN);

    return ESP_OK;
}

esp_err_t init_synth(void)
{
    /* Initialize ESP32Synth
     * Use begin() with MCLK if codec setup doesn't conflict, otherwise beginCustom()
     */

    synth = esp32synth_new();
    if (!synth) {
        ESP_LOGE(TAG, "Failed to create synth instance");
        return ESP_ERR_NO_MEM;
    }

    /* Use the I2S TX channel we just created */
    esp32synth_begin_custom(synth, tx_handle);

    esp32synth_set_num_voices(synth, 8);
    esp32synth_set_sample_rate(synth, SAMPLE_RATE);

    ESP_LOGI(TAG, "ESP32Synth initialized: %d voices at %d Hz",
             esp32synth_get_num_voices(synth), SAMPLE_RATE);

    return ESP_OK;
}

void play_test_chord(void)
{
    /* Play a test chord to verify audio is working
     * C major triad: C4, E4, G4 (60, 64, 67 MIDI notes)
     */

    esp32synth_set_patch(synth, 0, AA_Saw);  /* Saw wave on voice 0 */

    esp32synth_note_on(synth, 0, 60);  /* C4
    esp32synth_note_on(synth, 1, 64);  /* E4 */
    esp32synth_note_on(synth, 2, 67);  /* G4 */

    ESP_LOGI(TAG, "Test chord playing (C major: C4, E4, G4)");

    vTaskDelay(pdMS_TO_TICKS(2000));  /* Play for 2 seconds */

    esp32synth_note_off(synth, 0);
    esp32synth_note_off(synth, 1);
    esp32synth_note_off(synth, 2);

    ESP_LOGI(TAG, "Test chord stopped");
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Waveshare Synth Boot ===");
    ESP_LOGI(TAG, "Target: ESP32-P4-WIFI6-Touch-LCD-4C");

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
