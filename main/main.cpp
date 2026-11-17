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
#include "nvs_flash.h"
#include "esp_heap_caps.h"

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
static synth_envelope_t desired_envelope = preset_envelope(0);
/* Latest voice set from the UI; picker auditions borrow slot 0 on top of it. */
static synth_ui_voice_t desired_voices[SYNTH_UI_MAX_VOICES]{};

/* Master echo: a 1 s ring buffer; the UI sets time/feedback/mix (Q8). */
#define DELAY_MAX_SAMPLES   SAMPLE_RATE
static int16_t *delay_line = nullptr;
static uint32_t delay_pos = 0;
static volatile uint32_t desired_delay_samples = 0;
static volatile uint16_t desired_delay_feedback = 0;
static volatile uint16_t desired_delay_mix = 0;

static inline int16_t clamp16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

static inline int16_t apply_delay(int16_t dry)
{
    if(!delay_line) return dry;
    const uint32_t length = desired_delay_samples;
    /* Off still advances and writes silence so re-enabling never replays stale audio. */
    const int32_t wet = length ? delay_line[(delay_pos + DELAY_MAX_SAMPLES - length) % DELAY_MAX_SAMPLES] : 0;
    delay_line[delay_pos] = length ? clamp16(dry + ((wet * desired_delay_feedback) >> 8)) : 0;
    delay_pos = (delay_pos + 1) % DELAY_MAX_SAMPLES;
    return clamp16(dry + ((wet * desired_delay_mix) >> 8));
}
static Open303Voice *acid_voice = nullptr;
static uint32_t acid_benchmark_worst_us = 0;
static uint32_t acid_benchmark_average_us = 0;
static_assert(MAX_VOICES >= SYNTH_UI_MAX_VOICES, "Not enough synth voices for touch contacts and held layers");

typedef struct {
    synth_ui_voice_t voices[SYNTH_UI_MAX_VOICES];
    uint16_t cutoff_hz;
    uint8_t resonance;
} synth_command_t;

/* Presets rendered by the dedicated Open303 voice: 3 = Acid, 5 = darker Acid bass. */
static bool is_acid_preset(uint16_t preset) { return preset == 3 || preset == 5; }

static void apply_synth_preset(uint16_t preset, uint16_t voice)
{
    synth.noteOff(voice);
    synth.setVibrato(voice, 0, 0);
    synth.setTremolo(voice, 0, 0);
    synth.setCustomDSP(nullptr);
    synth.setMasterBitcrush(16);
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
        break;
    case 3:
    case 5:
        /* Dedicated Open303 renders these presets; ESP32Synth voices stay off. */
        break;
    case 4: synth.setWave(voice, WAVE_SINE); break;
    case 6: case 7:
        synth.setCustomWave(voice, ESP32Patches::FM_Custom);
        synth.setCustomParam(voice, 0, preset == 6 ? 1 : 3);
        synth.setCustomParam(voice, 1, preset == 6 ? 0 : 50);
        synth.setCustomParam(voice, 2, preset == 6 ? 25 : 90);
        synth.setCustomParam(voice, 3, preset == 6 ? 16000 : 6000);
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
        break;
    case 18: case 19: case 20:
        synth.setWave(voice, preset == 20 ? WAVE_SINE : WAVE_NOISE);
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

static uint16_t voice_preset[SYNTH_UI_MAX_VOICES];
static synth_envelope_t voice_envelope[SYNTH_UI_MAX_VOICES];
static bool gate[SYNTH_UI_MAX_VOICES]{};
static uint32_t triggers[SYNTH_UI_MAX_VOICES]{};
static uint16_t acid_preset = UINT16_MAX;
static synth_envelope_t acid_envelope{};
static bool acid_gate = false;
static uint32_t acid_trigger = 0;
static int acid_midi = -1;
static volatile uint16_t arp_bpm = 120;

/* Arps run here, on the audio core, so UI rendering can never stall them.
 * Each group plays one note at a time on its lowest slot; the other members
 * are cleared so they are neither heard nor counted toward voice levels. */
static void run_arps(synth_ui_voice_t *voices)
{
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t step_ms = 15000u / arp_bpm; /* sixteenth notes */
    const uint32_t gate_ms = step_ms * 3 / 4;
    uint32_t done = 0;
    for(uint16_t lead = 0; lead < SYNTH_UI_MAX_VOICES; ++lead) {
        const uint8_t group = voices[lead].arp_group;
        if(!group || (done & (1u << group)) || !voices[lead].note.pressed) continue;
        done |= 1u << group;
        uint8_t members[SYNTH_UI_MAX_VOICES];
        uint16_t n = 0;
        for(uint16_t i = lead; i < SYNTH_UI_MAX_VOICES; ++i) {
            if(voices[i].arp_group == group && voices[i].note.pressed) members[n++] = (uint8_t)i;
        }
        for(uint16_t i = 1; i < n; ++i) {
            for(uint16_t j = i; j > 0 && voices[members[j]].note.frequency_centi_hz <
                                          voices[members[j - 1]].note.frequency_centi_hz; --j) {
                const uint8_t tmp = members[j]; members[j] = members[j - 1]; members[j - 1] = tmp;
            }
        }
        const uint32_t elapsed = now_ms - voices[lead].arp_start_ms;
        const uint32_t step = elapsed / step_ms;
        synth_ui_voice_t out = voices[members[step % n]];
        if(n == 1) {
            static const float ratios[] = {1.0f, 1.1892071f, 1.4983071f, 2.0f};
            out.note.frequency_centi_hz = (uint32_t)(out.note.frequency_centi_hz * ratios[step % 4]);
        }
        out.note.trigger = voices[lead].arp_base_trigger + step;
        out.note.sustain = false;
        if(is_acid_preset(out.preset)) {
            out.note.accent = step % 4 == 0;
            out.note.slide = step % 4 == 2 || step % 4 == 3;
            const uint32_t next = (step + 1) % 4;
            out.note.pressed = next == 2 || next == 3 || elapsed % step_ms < gate_ms;
        } else {
            out.note.pressed = elapsed % step_ms < gate_ms;
        }
        for(uint16_t i = 0; i < n; ++i) voices[members[i]] = {};
        voices[lead] = out;
    }
}

static void apply_voices(const synth_ui_voice_t *voices, uint16_t cutoff_hz, uint8_t resonance)
{
    /* One Open303 exists, so the newest acid note owns it. */
    const synth_ui_voice_t *acid = nullptr;
    uint16_t active_count = 0;
    for(uint16_t i = 0; i < SYNTH_UI_MAX_VOICES; ++i) {
        const synth_ui_voice_t &voice = voices[i];
        if(is_acid_preset(voice.preset)) {
            if(voice.note.pressed && (!acid || (int32_t)(voice.note.trigger - acid->note.trigger) > 0)) acid = &voice;
        } else if(voice.note.frequency_centi_hz) {
            /* Occupied slots, gated or not, so arp gates don't pump other voices' level. */
            ++active_count;
        }
    }
    if(active_count == 0) active_count = 1;

    for(uint16_t i = 0; i < SYNTH_UI_MAX_VOICES; ++i) {
        const synth_ui_voice_t &voice = voices[i];
        if(!voice.note.pressed || is_acid_preset(voice.preset)) {
            if(gate[i]) synth.noteOff(i);
            gate[i] = false;
            continue;
        }
        bool force_envelope = false;
        if(voice_preset[i] != voice.preset) {
            apply_synth_preset(voice.preset, i);
            voice_preset[i] = voice.preset;
            gate[i] = false;
            force_envelope = true;
        }
        if(force_envelope || !synth_envelope_equal(voice.envelope, voice_envelope[i])) {
            voice_envelope[i] = voice.envelope;
            synth.setEnv(i, voice.envelope.attack, voice.envelope.decay, voice.envelope.sustain, voice.envelope.release);
        }
        const synth_ui_note_t &note = voice.note;
        if(!gate[i] || triggers[i] != note.trigger) {
            synth.noteOn(i, note.frequency_centi_hz, note.volume / active_count);
            gate[i] = true;
            triggers[i] = note.trigger;
        } else {
            synth.setFrequency(i, note.frequency_centi_hz);
            synth.setVolume(i, note.volume / active_count);
        }
    }

    if(!acid) {
        if(acid_gate) acid_voice->note_off();
        acid_gate = false;
        return;
    }
    bool force_acid_envelope = false;
    if(acid->preset != acid_preset) {
        acid_voice->reset();
        acid_voice->set_dark(acid->preset == 5);
        acid_preset = acid->preset;
        acid_gate = false;
        acid_midi = -1;
        force_acid_envelope = true;
        ESP_LOGI(TAG, "Open303 preset=%u", acid_preset);
    }
    if(force_acid_envelope || !synth_envelope_equal(acid->envelope, acid_envelope)) {
        acid_envelope = acid->envelope;
        acid_voice->set_envelope(acid_envelope.decay, acid_envelope.filter_decay, acid_envelope.release);
    }
    acid_voice->set_controls(cutoff_hz, resonance);
    const synth_ui_note_t &priority = acid->note;
    acid_voice->set_sustain(priority.sustain);
    const int midi = (int)lroundf(69.0f + 12.0f * log2f(priority.frequency_centi_hz / 44000.0f));
    if(!acid_gate || priority.trigger != acid_trigger || midi != acid_midi) {
        const bool moving_held_note = acid_gate && priority.trigger == acid_trigger;
        acid_voice->note_on(midi, priority.accent, priority.slide || moving_held_note, priority.volume);
    }
    acid_voice->set_volume(priority.volume);
    acid_trigger = priority.trigger;
    acid_midi = midi;
    acid_gate = true;
}

static void synth_output_callback(int16_t *samples, int num_samples)
{
    static synth_command_t current{};
    static bool have_command = false;
    static bool voices_initialised = false;
    if(!voices_initialised) {
        for(auto &preset : voice_preset) preset = UINT16_MAX;
        voices_initialised = true;
    }
    synth_command_t command;
    while (xQueueReceive(synth_command_queue, &command, 0) == pdTRUE) {
        current = command;
        have_command = true;
    }
    /* Re-evaluated every block (not only on UI updates) so arps keep time. */
    if(have_command) {
        synth_ui_voice_t voices[SYNTH_UI_MAX_VOICES];
        memcpy(voices, current.voices, sizeof(voices));
        run_arps(voices);
        apply_voices(voices, current.cutoff_hz, current.resonance);
    }

    if (num_samples > SYNTH_OUTPUT_BLOCK_MAX) {
        ESP_LOGE(TAG, "Synth output block too large: %d", num_samples);
        return;
    }

    /* The 303 layers on top of the synth voices rather than replacing them. */
    static int16_t acid_block[SYNTH_OUTPUT_BLOCK_MAX];
    {
        const int64_t start = esp_timer_get_time();
        acid_voice->render(acid_block, num_samples);
        for(int i = 0; i < num_samples; ++i) samples[i] = clamp16((int32_t)samples[i] + acid_block[i]);
        const uint32_t used = (uint32_t)(esp_timer_get_time() - start);
        static uint32_t worst = 0;
        static int64_t last_log = 0;
        if(used > worst) worst = used;
        if(acid_gate && esp_timer_get_time() - last_log > 2000000) {
            ESP_LOGI(TAG, "Open303 core=%d render=%lu us worst=%lu deadline=%d us faults=%u; boot bench avg=%lu worst=%lu",
                xPortGetCoreID(), (unsigned long)used, (unsigned long)worst,
                num_samples * 1000000 / SAMPLE_RATE, acid_voice->fault_count(),
                (unsigned long)acid_benchmark_average_us, (unsigned long)acid_benchmark_worst_us);
            last_log = esp_timer_get_time();
        }
    }
    for (int i = 0; i < num_samples; i++) {
        const int16_t sample = apply_delay(samples[i]);
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
    memcpy(command.voices, desired_voices, sizeof(command.voices));
    command.cutoff_hz = desired_cutoff;
    command.resonance = desired_resonance;
    command.voices[0] = {{frequency_centi_hz, volume, pressed, 0, false, false, false}, desired_preset, desired_envelope, 0, 0, 0};
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
    /* Backlight stays dark until the first UI frame is drawn (no white flash). */
    ESP_RETURN_ON_ERROR(gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0), TAG,
                        "Failed to set display backlight GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(DISPLAY_BACKLIGHT_ENABLE_PIN, 1), TAG,
                        "Failed to enable display backlight boost");

    ESP_RETURN_ON_ERROR(bsp_display_new(NULL, &display_panel, &display_io), TAG,
                        "Failed to initialize 4C MIPI-DSI display");

    esp_lv_adapter_config_t lvgl_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    /* SynthTask is pinned to P4 core 1; keep UI work on core 0. */
    lvgl_cfg.task_core_id = 0;
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&lvgl_cfg), TAG,
                        "Failed to initialize LVGL adapter");

    esp_lv_adapter_display_config_t display_cfg =
        ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
            display_panel, display_io, 720, 720, ESP_LV_ADAPTER_ROTATE_0);
    display_cfg.profile.buffer_height = 60;
    /* Partial triple-buffering left stale/blank framebuffers on screen; draw
     * straight into the single scanned-out buffer instead. */
    display_cfg.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
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

static void ui_set_voices(const synth_ui_voice_t *voices)
{
    memcpy(desired_voices, voices, sizeof(desired_voices));
    if(!synth_command_queue) return;
    synth_command_t command{};
    memcpy(command.voices, voices, sizeof(command.voices));
    command.cutoff_hz = desired_cutoff;
    command.resonance = desired_resonance;
    xQueueOverwrite(synth_command_queue, &command);
}

static void ui_set_envelope(const synth_envelope_t *envelope)
{
    desired_envelope = *envelope;
}

static void ui_set_delay(uint16_t time_ms, uint8_t feedback_pct, uint8_t mix_pct)
{
    uint32_t samples = (uint32_t)time_ms * SAMPLE_RATE / 1000;
    if(samples >= DELAY_MAX_SAMPLES) samples = DELAY_MAX_SAMPLES - 1;
    desired_delay_feedback = (uint16_t)(feedback_pct * 256 / 100);
    desired_delay_mix = (uint16_t)(mix_pct * 256 / 100);
    desired_delay_samples = samples;
}

static void ui_set_tempo(uint16_t bpm)
{
    arp_bpm = bpm;
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
        .set_voices = ui_set_voices,
        .set_acid_filter = ui_set_acid_filter,
        .set_envelope = ui_set_envelope,
        .set_delay = ui_set_delay,
        .set_tempo = ui_set_tempo,
    };
    synth_ui_init(lv_screen_active(), &audio);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== Waveshare Synth Boot ===");
    ESP_LOGI(TAG, "Target: ESP32-P4-WIFI6-Touch-LCD-XC");

    /* NVS holds the user's wedge instrument assignments. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nvs_err));

    delay_line = (int16_t *)heap_caps_calloc(DELAY_MAX_SAMPLES, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!delay_line) delay_line = (int16_t *)heap_caps_calloc(DELAY_MAX_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!delay_line) ESP_LOGE(TAG, "Delay buffer allocation failed; echo disabled");

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

    /* Start rendering only after audio bring-up: the first frame was being
     * lost (white screen until the next redraw) when LVGL started earlier. */
    if (esp_lv_adapter_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LVGL adapter");
        return;
    }
    ESP_LOGI(TAG, "LVGL UI initialized; touch events routed to widgets");
    vTaskDelay(pdMS_TO_TICKS(150));  /* let LVGL flush its first full frame */
    if (bsp_display_backlight_on() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable display backlight");
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
