#include "acid_voice.h"
#include "rosic_Open303.h"
#include <algorithm>
#include <cmath>

Open303Voice::Open303Voice(float sample_rate) : engine(std::make_unique<rosic::Open303>())
{
    engine->setSampleRate(sample_rate);
    engine->setWaveform(0.0f);
    engine->setVolume(-12.0f);
    engine->setCutoff(cutoff);
    engine->setResonance(resonance);
    engine->setEnvMod(75.0f);
    engine->setDecay(250.0f);
    engine->setAccent(85.0f);
    engine->setSlideTime(60.0f);
    engine->setOutputDrive(9.0f);
}

Open303Voice::~Open303Voice() = default;

void Open303Voice::reset()
{
    engine->allNotesOff();
    engine->ampEnv.reset();
    engine->ampDeClicker.reset();
    engine->filter.reset();
    engine->highpass1.reset();
    engine->highpass2.reset();
    engine->allpass.reset();
    engine->notch.reset();
    engine->antiAliasFilter.reset();
    gate = sounding = false;
    quiet_frames = control_count = 0;
}

void Open303Voice::set_controls(uint16_t hz, uint8_t res)
{
    target_cutoff = std::clamp(float(hz), 80.0f, 6000.0f);
    /* Musical control taper: expose the resonant region without losing zero. */
    target_resonance = 98.0f * std::sqrt(res / 255.0f);
}

void Open303Voice::set_waveform(bool square) { engine->setWaveform(square ? 1.0f : 0.0f); }

void Open303Voice::note_on(uint8_t note, bool accent, bool slide, uint16_t volume)
{
    if(!gate || !slide) engine->allNotesOff();
    engine->noteOn(std::clamp<int>(note, 24, 72), accent ? 127 : 80, 0.0f);
    gate = sounding = true;
    quiet_frames = 0;
    target_gain = std::min(volume, uint16_t(255)) / 255.0f;
}

void Open303Voice::note_off()
{
    if(gate) engine->allNotesOff();
    gate = false;
}

void Open303Voice::render(int16_t *output, unsigned frames)
{
    for(unsigned i = 0; i < frames; ++i) {
        if(!sounding) { output[i] = 0; continue; }
        if(control_count++ % 16 == 0) {
            cutoff += (target_cutoff - cutoff) * 0.09516f;
            resonance += (target_resonance - resonance) * 0.09516f;
            engine->setCutoff(cutoff);
            engine->setResonance(resonance);
        }
        gain += (target_gain - gain) * 0.00623f;
        float value = engine->getSample() * gain;
        if(!std::isfinite(value)) {
            ++faults;
            reset();
            value = 0;
        }
        if(!gate && std::fabs(value) < 0.000001f) ++quiet_frames;
        else quiet_frames = 0;
        if(quiet_frames > 2048) sounding = false;
        output[i] = static_cast<int16_t>(std::clamp(value, -0.98f, 0.98f) * 32767.0f);
    }
}
