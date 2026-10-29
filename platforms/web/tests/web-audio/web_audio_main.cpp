// Runs the web audio output (src/dusk/web/audio.cpp) for real in a browser: a sine wave through
// the mixer thread, the ring and the AudioWorklet, with pausing, a hidden page and shutdown.
// Built and driven by run.mjs, which reads the PASS/FAIL lines it prints.

#include "dusk/web/audio.hpp"

#include <emscripten/emscripten.h>

#include <cmath>
#include <cstdio>

using namespace dusk::web::audio;

namespace {

int gFailures = 0;

void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    gFailures += ok ? 0 : 1;
}

struct Sine {
    uint32_t rate;
    double phase = 0;
};

void render(float* out, uint32_t frames, void* user) {
    auto& sine = *static_cast<Sine*>(user);
    for (uint32_t i = 0; i < frames; ++i) {
        const float v = 0.2f * float(std::sin(sine.phase));
        out[i * 2] = v;
        out[i * 2 + 1] = v;
        sine.phase += 2 * M_PI * 440.0 / sine.rate;
    }
}

EM_JS(void, set_page_hidden, (int hidden), {
  Object.defineProperty(document, 'hidden', { configurable: true, get: () => !!hidden });
  document.dispatchEvent(new Event('visibilitychange'));
});

}  // namespace

int main() {
    Ring ring(4096, 2);
    const uint32_t rate = start_output(ring, 48000);
    std::printf("rate=%u\n", rate);
    check(rate != 0, "the browser plays audio");
    if (rate == 0) {
        return 1;
    }
    Sine sine{rate};
    auto& header = ring.header();
    {
        Mixer mixer(ring, rate, rate * 43 / 1000, 256, &render, &sine);

        emscripten_sleep(1500);
        const uint32_t played = header.read;
        std::printf("played=%u underruns=%u\n", played, header.underruns.load());
        check(header.playing == 1, "the context is running");
        check(played > rate, "more than a second was played in 1.5 s");
        check(header.underruns < rate / 4, "under 250 ms of underruns");

        mixer.set_paused(true);
        set_output_paused(true);
        emscripten_sleep(300);
        const uint32_t pausedAt = header.read;
        emscripten_sleep(500);
        check(header.read == pausedAt, "nothing plays while paused");
        check(header.playing == 0, "the context is suspended while paused");

        mixer.set_paused(false);
        set_output_paused(false);
        emscripten_sleep(700);
        check(header.read != pausedAt, "playback resumes");

        set_page_hidden(1);
        emscripten_sleep(300);
        const uint32_t hiddenRead = header.read;
        const uint32_t hiddenWrite = header.write;
        emscripten_sleep(500);
        check(header.hidden == 1, "the mixer sees the page hidden");
        check(header.read == hiddenRead && header.write == hiddenWrite, "nothing plays or renders while hidden");

        set_page_hidden(0);
        emscripten_sleep(700);
        check(header.read != hiddenRead, "playback resumes when the page is shown");
    }
    stop_output();
    check(true, "output stopped");
    std::puts(gFailures == 0 ? "PASS" : "FAILED");
    return gFailures == 0 ? 0 : 1;
}
