#include <stdio.h>
#include <stdint.h>
#include <string.h>
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
#include "ESP32Synth_Patches.hpp"
#include <math.h>
#include "ui/synth_ui.h"
#include "acid_voice.h"
#include <new>

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
#define I2S_MCLK_MULTIPLE   I2S_MCLK_MULTIPLE_384
#define SYNTH_OUTPUT_BLOCK_MAX 512

static esp_codec_dev_handle_t speaker_codec_handle = NULL;
static esp_lcd_panel_handle_t display_panel = NULL;
static esp_lcd_panel_io_handle_t display_io = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;
static lv_display_t *lvgl_display = NULL;
static ESP32Synth synth;
static QueueHandle_t synth_command_queue = NULL;
static int16_t synth_stereo_buffer[SYNTH_OUTPUT_BLOCK_MAX * 2];
static int16_t preset_wavetables[3][256];
/* UI task owns desired_preset; each queue snapshot carries it to core 1. */
static uint16_t desired_preset = 0;
static uint16_t desired_cutoff = 350;
static uint8_t desired_resonance = 140;
static Open303Voice *acid_voice = nullptr;
static uint32_t acid_benchmark_worst_us = 0;
static uint32_t acid_benchmark_average_us = 0;
static_assert(MAX_VOICES >= SYNTH_UI_MAX_CONTACTS, "Not enough synth voices for touch contacts");

typedef struct {
    synth_ui_note_t notes[SYNTH_UI_MAX_CONTACTS];
    uint16_t preset;
    uint16_t cutoff_hz;
    uint8_t resonance;
} synth_command_t;

static void apply_synth_preset(uint16_t preset, uint16_t voice)
{
    synth.noteOff(voice);
    synth.setVibrato(voice, 0, 0);
    synth.setTremolo(voice, 0, 0);
    synth.setCustomDSP(nullptr);
    synth.setMasterBitcrush(16);
    synth.setEnv(voice, 5, 60, 200, 120);
    for(uint8_t i = 0; i < 4; ++i) synth.setCustomParam(voice, i, 0);
    switch(preset) {
    case 0: synth.setCustomWave(voice, ESP32Patches::AA_Saw); break;
    case 1:
        synth.setPulseWidthBitDepth(8);
        synth.setPulseWidth(voice, 96);
        synth.setCustomWave(voice, ESP32Patches::AA_Pulse);
        break;
    case 2:
        synth.setWave(voice, WAVE_TRIANGLE);
        synth.setEnv(voice, 15, 250, 140, 200);
        break;
    case 3:
        /* Dedicated Open303 renders this preset; ESP32Synth voices stay off. */
        break;
    case 4: synth.setWave(voice, WAVE_SINE); break;
    case 6: case 7:
        synth.setCustomWave(voice, ESP32Patches::FM_Custom);
        synth.setCustomParam(voice, 0, preset == 6 ? 1 : 3);
        synth.setCustomParam(voice, 1, preset == 6 ? 0 : 50);
        synth.setCustomParam(voice, 2, preset == 6 ? 25 : 90);
        synth.setCustomParam(voice, 3, preset == 6 ? 16000 : 6000);
        synth.setEnv(voice, 3, preset == 6 ? 500 : 1000, 0, 200);
        break;
    case 8: case 9:
        synth.setCustomWave(voice, ESP32Patches::HammondB3);
        synth.setCustomParam(voice, 0, preset == 8 ? 0 : 2);
        if(preset == 9) {
            synth.setVibrato(voice, 550, 180);
            synth.setTremolo(voice, 550, 65);
        }
        break;
    case 12: case 13: case 14:
        synth.setWave(voice, preset == 13 ? WAVE_SINE : (preset == 12 ? WAVE_TRIANGLE : WAVE_PULSE));
        synth.setEnv(voice, 2, preset == 14 ? 130 : (preset == 12 ? 450 : 750), 0, 120);
        break;
    case 18: case 19: case 20:
        synth.setWave(voice, preset == 20 ? WAVE_SINE : WAVE_NOISE);
        synth.setEnv(voice, 1, preset == 18 ? 35 : (preset == 19 ? 140 : 220), 0, 30);
        break;
    case 24: case 25: case 26:
        synth.setWavetable(voice, preset_wavetables[preset - 24], 256, BITS_16);
        synth.setWave(voice, WAVE_WAVETABLE);
        break;
    case 36:
        synth.setWave(voice, WAVE_TRIANGLE);
        synth.setVibrato(voice, 550, 700);
        break;
    case 37:
        synth.setCustomWave(voice, ESP32Patches::AA_Saw);
        synth.setTremolo(voice, 800, 200);
        break;
    default: synth.setWave(voice, WAVE_SINE); break;
    }
}

static void synth_output_callback(int16_t *samples, int num_samples)
{
    static uint16_t applied_preset = UINT16_MAX;
    static bool gate[SYNTH_UI_MAX_CONTACTS]{};
    static uint32_t triggers[SYNTH_UI_MAX_CONTACTS]{};
    static bool acid_gate = false;
    static uint32_t acid_trigger = 0;
    static int acid_midi = -1;
    synth_command_t command;
    while (xQueueReceive(synth_command_queue, &command, 0) == pdTRUE) {
        if(command.preset != applied_preset) {
            for(uint16_t i = 0; i < SYNTH_UI_MAX_CONTACTS; ++i) {
                apply_synth_preset(command.preset, i);
                gate[i] = false;
            }
            applied_preset = command.preset;
            acid_voice->reset();
            acid_gate = false;
            acid_midi = -1;
            ESP_LOGI(TAG, "Preset=%u on %u voices, core %d", applied_preset, SYNTH_UI_MAX_CONTACTS, xPortGetCoreID());
        }
        if(applied_preset == 3) {
            acid_voice->set_controls(command.cutoff_hz, command.resonance);
            const synth_ui_note_t *priority = nullptr;
            for(const auto &note : command.notes) {
                if(note.pressed && (!priority || (int32_t)(note.trigger - priority->trigger) > 0)) priority = &note;
            }
            if(priority && priority->frequency_centi_hz > 0) {
                const int midi = (int)lroundf(69.0f + 12.0f * log2f(priority->frequency_centi_hz / 44000.0f));
                if(!acid_gate || priority->trigger != acid_trigger || midi != acid_midi) {
                    const bool moving_held_note = acid_gate && priority->trigger == acid_trigger;
                    acid_voice->note_on(midi, priority->accent, priority->slide || moving_held_note, priority->volume);
                }
                acid_voice->set_volume(priority->volume);
                acid_trigger = priority->trigger;
                acid_midi = midi;
                acid_gate = true;
            } else {
                acid_voice->note_off();
                acid_gate = false;
            }
            continue;
        }
        uint16_t active_count = 0;
        for(const auto &note : command.notes) if(note.pressed) ++active_count;
        if(active_count == 0) active_count = 1;
        for(uint16_t i = 0; i < SYNTH_UI_MAX_CONTACTS; ++i) {
            const synth_ui_note_t &note = command.notes[i];
            if (note.pressed) {
                if(!gate[i] || triggers[i] != note.trigger) {
                    synth.noteOn(i, note.frequency_centi_hz, note.volume / active_count);
                    gate[i] = true;
                    triggers[i] = note.trigger;
                } else {
                    synth.setFrequency(i, note.frequency_centi_hz);
                    synth.setVolume(i, note.volume / active_count);
                }
            } else {
                if(gate[i]) synth.noteOff(i);
                gate[i] = false;
            }
        }
    }

    if (num_samples > SYNTH_OUTPUT_BLOCK_MAX) {
        ESP_LOGE(TAG, "Synth output block too large: %d", num_samples);
        return;
    }

    if(applied_preset == 3) {
        const int64_t start = esp_timer_get_time();
        acid_voice->render(samples, num_samples);
        const uint32_t used = (uint32_t)(esp_timer_get_time() - start);
        static uint32_t worst = 0;
        static int64_t last_log = 0;
        if(used > worst) worst = used;
        if(esp_timer_get_time() - last_log > 2000000) {
            ESP_LOGI(TAG, "Open303 core=%d render=%lu us worst=%lu deadline=%d us faults=%u; boot bench avg=%lu worst=%lu",
                xPortGetCoreID(), (unsigned long)used, (unsigned long)worst,
                num_samples * 1000000 / SAMPLE_RATE, acid_voice->fault_count(),
                (unsigned long)acid_benchmark_average_us, (unsigned long)acid_benchmark_worst_us);
            last_log = esp_timer_get_time();
        }
    }
    for (int i = 0; i < num_samples; i++) {
        const int16_t sample = samples[i];
        synth_stereo_buffer[i * 2] = sample;
        synth_stereo_buffer[i * 2 + 1] = sample;
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
    synth_command_t command{};
    command.preset = desired_preset;
    command.cutoff_hz = desired_cutoff;
    command.resonance = desired_resonance;
    command.notes[0] = {frequency_centi_hz, volume, pressed, 0, false, false};
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

    esp_codec_dev_sample_info_t sample_info = {
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

struct AcidBenchmark {
    TaskHandle_t caller;
    bool passed;
};

static void benchmark_acid(void *arg)
{
    auto *result = static_cast<AcidBenchmark *>(arg);
    acid_voice = new(std::nothrow) Open303Voice(SAMPLE_RATE);
    result->passed = acid_voice != nullptr;
    if(acid_voice) {
        int16_t buffer[128];
        uint64_t total = 0;
        for(unsigned block = 0; block < 128; ++block) {
            acid_voice->set_controls(80 + (block % 16) * 390, block % 2 ? 255 : 0);
            acid_voice->note_on(36 + block % 12, block % 4 == 0, block % 4 == 2, 190);
            const int64_t start = esp_timer_get_time();
            acid_voice->render(buffer, 128);
            const uint32_t used = (uint32_t)(esp_timer_get_time() - start);
            total += used;
            if(used > acid_benchmark_worst_us) acid_benchmark_worst_us = used;
            if(block % 8 == 0) vTaskDelay(1);
        }
        acid_benchmark_average_us = total / 128;
        result->passed = acid_voice->fault_count() == 0 && acid_benchmark_worst_us < 3200;
        ESP_LOGI(TAG, "Open303 benchmark core=%d avg=%lu worst=%lu us / 4000 us deadline: %s",
            xPortGetCoreID(), (unsigned long)acid_benchmark_average_us,
            (unsigned long)acid_benchmark_worst_us, result->passed ? "PASS" : "FAIL");
        acid_voice->reset();
    }
    xTaskNotifyGive(result->caller);
    vTaskDelete(nullptr);
}

esp_err_t init_synth(void)
{
    AcidBenchmark benchmark{xTaskGetCurrentTaskHandle(), false};
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(benchmark_acid, "303Bench", 16384, &benchmark,
        6, nullptr, 1) == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to start Open303 benchmark");
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_RETURN_ON_FALSE(benchmark.passed, ESP_FAIL, TAG, "Open303 missed performance/stability check");
    for(unsigned i = 0; i < 256; ++i) {
        const float phase = 2.0f * (float)M_PI * i / 256;
        preset_wavetables[0][i] = (int16_t)(15000 * sinf(phase) + 6500 * sinf(5 * phase) + 3500 * sinf(9 * phase));
        preset_wavetables[1][i] = (int16_t)(14000 * sinf(phase) + 8000 * sinf(3 * phase) + 4000 * sinf(4 * phase));
        preset_wavetables[2][i] = (int16_t)((i / 32) * 8000 - 28000);
    }
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
    /* SynthTask is pinned to P4 core 1; keep UI work on core 0. */
    lvgl_cfg.task_core_id = 0;
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

static esp_err_t read_touch_contacts(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t *points,
                                     uint8_t *count, uint8_t max_count, void *)
{
    ESP_RETURN_ON_ERROR(esp_lcd_touch_read_data(tp), TAG, "Touch read failed");
    const esp_err_t result = esp_lcd_touch_get_data(tp, points, count, max_count);
    static uint8_t previous = 0, highest = 0;
    if(result == ESP_OK && *count != previous) {
        previous = *count;
        if(*count > highest) highest = *count;
        ESP_LOGI(TAG, "Touch contacts=%u highest observed=%u (driver limit=%u)", *count, highest, SYNTH_UI_MAX_CONTACTS);
    }
    return result;
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
    esp_lv_adapter_touch_config_t lvgl_touch_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(lvgl_display, touch_handle);
    lvgl_touch_cfg.multi_touch.mode = ESP_LV_ADAPTER_TOUCH_MODE_MULTI_CONTROL;
    lvgl_touch_cfg.multi_touch.pointers = SYNTH_UI_MAX_CONTACTS;
    lvgl_touch_cfg.callbacks.custom_touch_read = read_touch_contacts;
    ESP_RETURN_ON_FALSE(esp_lv_adapter_register_touch(&lvgl_touch_cfg) != NULL,
                        ESP_FAIL, TAG, "Failed to register LVGL touch input");
    ESP_LOGI(TAG, "Touch initialized; polling GT911-compatible controller");
    return ESP_OK;
}

static void ui_note_on(uint32_t frequency_centi_hz, uint16_t volume)
{
    queue_synth_command(frequency_centi_hz, volume, true);
}

static void ui_note_off(void)
{
    queue_synth_command(0, 0, false);
}

static void ui_select_preset(uint16_t preset)
{
    desired_preset = preset;
    queue_synth_command(0, 0, false);
}

static void ui_set_notes(const synth_ui_note_t *notes)
{
    if(!synth_command_queue) return;
    synth_command_t command{};
    command.preset = desired_preset;
    command.cutoff_hz = desired_cutoff;
    command.resonance = desired_resonance;
    memcpy(command.notes, notes, sizeof(command.notes));
    xQueueOverwrite(synth_command_queue, &command);
}

static void ui_set_acid_filter(uint16_t cutoff_hz, uint8_t resonance)
{
    desired_cutoff = cutoff_hz;
    desired_resonance = resonance;
}

static void init_ui(void)
{
    const synth_ui_audio_callbacks_t audio = {
        .note_on = ui_note_on,
        .note_off = ui_note_off,
        .select_preset = ui_select_preset,
        .set_notes = ui_set_notes,
        .set_acid_filter = ui_set_acid_filter,
    };
    synth_ui_init(lv_screen_active(), &audio);
}

extern "C" void app_main(void)
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
