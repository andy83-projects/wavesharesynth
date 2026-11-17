#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lcd_touch.h"
#include "esp_codec_dev.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "ESP32Synth.h"

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
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_26
#define DISPLAY_BACKLIGHT_ENABLE_PIN GPIO_NUM_33

/* I2C codec control bus */
#define I2C_SDA_PIN     GPIO_NUM_7
#define I2C_SCL_PIN     GPIO_NUM_8
#define SAMPLE_RATE         32000
#define I2S_MCLK_MULTIPLE   384
#define SYNTH_OUTPUT_BLOCK_MAX 512

static esp_codec_dev_handle_t speaker_codec_handle = NULL;
static esp_lcd_panel_handle_t display_panel = NULL;
static esp_lcd_panel_io_handle_t display_io = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;
static lv_display_t *lvgl_display = NULL;
static lv_obj_t *touch_status_label = NULL;
static ESP32Synth synth;
static QueueHandle_t synth_command_queue = NULL;
static int16_t synth_stereo_buffer[SYNTH_OUTPUT_BLOCK_MAX * 2];

typedef struct {
    uint32_t frequency_centi_hz;
    uint16_t volume;
    bool pressed;
} synth_command_t;

static void synth_output_callback(int16_t *samples, int num_samples)
{
    synth_command_t command;
    while (xQueueReceive(synth_command_queue, &command, 0) == pdTRUE) {
        if (command.pressed) {
            synth.noteOn(0, command.frequency_centi_hz, command.volume);
        } else {
            synth.noteOff(0);
        }
    }

    if (num_samples > SYNTH_OUTPUT_BLOCK_MAX) {
        ESP_LOGE(TAG, "Synth output block too large: %d", num_samples);
        return;
    }

    for (int i = 0; i < num_samples; i++) {
        synth_stereo_buffer[i * 2] = samples[i];
        synth_stereo_buffer[i * 2 + 1] = samples[i];
    }
    esp_codec_dev_write(speaker_codec_handle, synth_stereo_buffer,
                        num_samples * 2 * (int)sizeof(int16_t));
}

static void queue_synth_command(uint32_t frequency_centi_hz, uint16_t volume,
                                bool pressed)
{
    if (synth_command_queue == NULL) {
        return;
    }
    const synth_command_t command = {
        .frequency_centi_hz = frequency_centi_hz,
        .volume = volume,
        .pressed = pressed,
    };
    xQueueOverwrite(synth_command_queue, &command);
}

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

    const esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0,
        .sample_rate = SAMPLE_RATE,
        .mclk_multiple = I2S_MCLK_MULTIPLE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(speaker_codec_handle, &sample_info) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "Failed to open ES8311 speaker stream");

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
    synth_command_queue = xQueueCreate(1, sizeof(synth_command_t));
    ESP_RETURN_ON_FALSE(synth_command_queue != NULL, ESP_ERR_NO_MEM, TAG,
                        "Failed to allocate synth command queue");
    ESP_RETURN_ON_FALSE(synth.beginCustom(SAMPLE_RATE, synth_output_callback), ESP_FAIL,
                        TAG, "Failed to initialize ESP32Synth custom output");
    synth.setMasterVolume(255);
    synth.setWave(0, WAVE_SINE);
    synth.setEnv(0, 5, 40, 220, 120);
    ESP_LOGI(TAG, "ESP32Synth initialized at %d Hz with codec output callback", SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t init_display(void)
{
    const gpio_config_t backlight_cfg = {
        .pin_bit_mask = (1ULL << DISPLAY_BACKLIGHT_PIN) |
                        (1ULL << DISPLAY_BACKLIGHT_ENABLE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&backlight_cfg), TAG,
                        "Failed to configure display backlight");
    ESP_RETURN_ON_ERROR(gpio_set_level(DISPLAY_BACKLIGHT_PIN, 1), TAG,
                        "Failed to enable display backlight GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(DISPLAY_BACKLIGHT_ENABLE_PIN, 1), TAG,
                        "Failed to enable display backlight boost");

    ESP_RETURN_ON_ERROR(bsp_display_new(NULL, &display_panel, &display_io), TAG,
                        "Failed to initialize 4C MIPI-DSI display");
    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG,
                        "Failed to enable display backlight");

    esp_lv_adapter_config_t lvgl_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&lvgl_cfg), TAG,
                        "Failed to initialize LVGL adapter");

    esp_lv_adapter_display_config_t display_cfg =
        ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
            display_panel, display_io, 720, 720, ESP_LV_ADAPTER_ROTATE_0);
    display_cfg.profile.buffer_height = 60;
    lvgl_display = esp_lv_adapter_register_display(&display_cfg);
    ESP_RETURN_ON_FALSE(lvgl_display != NULL, ESP_FAIL, TAG,
                        "Failed to register LVGL display");

    ESP_LOGI(TAG, "4C display initialized: 720x720 LVGL display");
    return ESP_OK;
}

static void touch_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) {
        return;
    }

    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point = {0};
    if (indev != NULL) {
        lv_indev_get_point(indev, &point);
    }
    if (code == LV_EVENT_RELEASED) {
        queue_synth_command(0, 0, false);
        lv_label_set_text(touch_status_label, "Touch: released");
        return;
    }

    const int x = (point.x < 0) ? 0 : (point.x > 719 ? 719 : point.x);
    const int y = (point.y < 0) ? 0 : (point.y > 719 ? 719 : point.y);
    const uint32_t frequency_centi_hz = 13000u + ((uint32_t)x * 87000u) / 719u;
    const uint16_t volume = (uint16_t)(((uint32_t)(719 - y) * 255u) / 719u);
    queue_synth_command(frequency_centi_hz, volume, true);
    lv_label_set_text_fmt(touch_status_label, "Touch: %d, %d", x, y);
    ESP_LOGI(TAG, "Synth touch: x=%d y=%d freq=%lu cHz volume=%u",
             x, y, (unsigned long)frequency_centi_hz, volume);
}

esp_err_t init_touch(void)
{
    const bsp_display_cfg_t touch_cfg = {
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(bsp_touch_new(&touch_cfg, &touch_handle), TAG,
                        "Failed to initialize GT911-compatible touch");
    const esp_lv_adapter_touch_config_t lvgl_touch_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(lvgl_display, touch_handle);
    ESP_RETURN_ON_FALSE(esp_lv_adapter_register_touch(&lvgl_touch_cfg) != NULL,
                        ESP_FAIL, TAG, "Failed to register LVGL touch input");
    ESP_LOGI(TAG, "Touch initialized; polling GT911-compatible controller");
    return ESP_OK;
}

static void init_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101318), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "WAVESHARE SYNTH");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *subtitle = lv_label_create(screen);
    lv_label_set_text(subtitle, "LVGL UI foundation");
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 76);

    lv_obj_t *touch_surface = lv_btn_create(screen);
    lv_obj_set_size(touch_surface, 560, 430);
    lv_obj_align(touch_surface, LV_ALIGN_CENTER, 0, 30);
    lv_obj_add_event_cb(touch_surface, touch_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *surface_label = lv_label_create(touch_surface);
    lv_label_set_text(surface_label, "TOUCH SURFACE");
    lv_obj_center(surface_label);

    touch_status_label = lv_label_create(screen);
    lv_label_set_text(touch_status_label, "Touch: --, --");
    lv_obj_align(touch_status_label, LV_ALIGN_BOTTOM_MID, 0, -36);
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

    /* Bring up the panel before audio so a codec failure cannot mask display
     * validation during Milestone 2. */
    if (init_display() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display");
        return;
    }

    if (init_touch() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize touch");
        return;
    }

    init_ui();
    if (esp_lv_adapter_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LVGL adapter");
        return;
    }
    ESP_LOGI(TAG, "LVGL UI initialized; touch events routed to widgets");

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

    ESP_LOGI(TAG, "=== Boot complete ===");

    /* Keep the audio path initialized, but leave playback silent until the UI
     * provides an explicit sound event.  The repeating chord was only for the
     * Milestone 1 hardware test. */
    ESP_LOGI(TAG, "Audio test chord disabled; waiting for UI input");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
