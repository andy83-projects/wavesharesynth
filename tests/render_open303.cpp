#include "acid_voice.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>

static void little(FILE *file, uint32_t value, unsigned bytes)
{
    for(unsigned i = 0; i < bytes; ++i) std::fputc((value >> (8*i)) & 255, file);
}

int main(int argc, char **argv)
{
    if(argc != 2) return 2;
    FILE *file = std::fopen(argv[1], "wb");
    assert(file);
    constexpr unsigned rate = 32000, frames = rate * 8;
    std::fwrite("RIFF", 1, 4, file); little(file, 36 + frames * 2, 4);
    std::fwrite("WAVEfmt ", 1, 8, file); little(file, 16, 4);
    little(file, 1, 2); little(file, 1, 2); little(file, rate, 4);
    little(file, rate * 2, 4); little(file, 2, 2); little(file, 16, 2);
    std::fwrite("data", 1, 4, file); little(file, frames * 2, 4);
    auto voice = std::make_unique<Open303Voice>(rate);
    const uint8_t notes[16] = {36,36,48,43,36,39,36,46,36,48,43,39,36,43,46,36};
    int16_t buffer[128];
    unsigned clipped = 0;
    for(unsigned i = 0; i < frames; i += 128) {
        const unsigned step = i / 4000;
        const unsigned prior_step = i == 0 ? unsigned(-1) : (i - 128) / 4000;
        voice->set_controls((uint16_t)(250 + 1700 * (0.5f - 0.5f * std::cos(6.2831853f * i / frames))), 220);
        if(step != prior_step) voice->note_on(notes[step % 16], step % 4 == 0,
            step % 4 == 2 || step % 4 == 3, 190);
        if(i % 4000 > 3040 && ((step + 1) % 4 < 2)) voice->note_off();
        const unsigned n = i + 128 > frames ? frames - i : 128;
        voice->render(buffer, n);
        for(unsigned j = 0; j < n; ++j) {
            if(std::abs(buffer[j]) >= 32000) ++clipped;
            little(file, (uint16_t)buffer[j], 2);
        }
    }
    std::fclose(file);
    assert(voice->fault_count() == 0);
    std::printf("8-second Open303 reference rendered; clipped samples=%u/%u\n", clipped, frames);
}
