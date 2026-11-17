#include "acid_voice.h"
#include "rosic_EllipticQuarterBandFilter.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>

static void verify_anti_alias_response()
{
    const double a[] = {1,-9.1891604652189471,40.177553696870497,-110.11636661771178,
        210.18506612078195,-293.84744771903240,308.16345558359234,-244.06786780384243,
        144.81877911392738,-62.770692151724198,18.867762095902137,-3.5327094230551848,0.31183189275203149};
    const double b[] = {0.00013671732099945628,-0.00055538501265606384,0.0013681887636296387,
        -0.0022158566490711852,0.0028320091007278322,-0.0029776933151090413,0.0030283628243514991,
        -0.0029776933151090413,0.0028320091007278331,-0.0022158566490711861,
        0.0013681887636296393,-0.00055538501265606384,0.00013671732099945636};
    double state[12]{};
    rosic::EllipticQuarterBandFilter filter;
    double error = 0, energy = 0;
    for(unsigned i = 0; i < 8192; ++i) {
        double v = i == 0 ? 1 : 0;
        for(unsigned k = 0; k < 12; ++k) v -= a[k + 1] * state[k];
        double reference = b[0] * v;
        for(unsigned k = 0; k < 12; ++k) reference += b[k + 1] * state[k];
        for(unsigned k = 11; k > 0; --k) state[k] = state[k - 1];
        state[0] = v;
        const double actual = filter.getSample(i == 0 ? 1.0f : 0.0f);
        error += (actual - reference) * (actual - reference);
        energy += reference * reference;
    }
    assert(error / energy < 0.0001);
}

static double note_energy(bool accent)
{
    auto voice = std::make_unique<Open303Voice>();
    voice->set_controls(500, 180);
    voice->note_on(36, accent, false, 200);
    int16_t buffer[128];
    double energy = 0;
    for(unsigned i = 0; i < 100; ++i) {
        voice->render(buffer, 128);
        for(auto sample : buffer) energy += double(sample) * sample;
    }
    assert(voice->fault_count() == 0);
    return energy;
}

int main()
{
    verify_anti_alias_response();
    assert(note_energy(true) > note_energy(false) * 1.2);
    auto voice = std::make_unique<Open303Voice>();
    int16_t samples[128];
    voice->render(samples, 128);
    for(auto s : samples) assert(s == 0);
    double energy = 0;
    for(unsigned step = 0; step < 128; ++step) {
        voice->set_controls(80 + (step % 16) * 390, step % 2 ? 255 : 0);
        voice->note_on(36 + step % 12, step % 4 == 0, step % 4 == 2, 190);
        for(unsigned block = 0; block < 31; ++block) {
            voice->render(samples, 128);
            if(voice->fault_count()) std::fprintf(stderr, "Nonfinite output: step=%u block=%u\n", step, block);
            assert(voice->fault_count() == 0);
            for(auto s : samples) energy += double(s) * s;
        }
    }
    assert(energy > 1e8);
    assert(voice->fault_count() == 0);
    voice->note_off();
    for(unsigned i = 0; i < 1000; ++i) voice->render(samples, 128);
    for(auto s : samples) assert(std::abs(s) <= 1);
    voice->reset();
    voice->render(samples, 128);
    for(auto s : samples) assert(s == 0);
    voice->set_waveform(true);
    voice->note_on(36, true, false, 190);
    for(unsigned i = 0; i < 50; ++i) voice->render(samples, 128);
    assert(voice->fault_count() == 0);
    std::puts("Open303: silence, saw/square, accent/slide, cutoff/resonance extremes, release and reset passed");
}
