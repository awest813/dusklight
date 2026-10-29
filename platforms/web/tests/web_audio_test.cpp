// Native test of the web audio ring and mixer (src/dusk/web/audio.cpp): a consumer thread plays
// the part of the AudioWorklet, pulling at a real-time pace.
//
// Built and run by platforms/web/tests/run-native-tests.sh.

#include "dusk/web/audio.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace audio = dusk::web::audio;
using namespace std::chrono_literals;

namespace {

int gFailures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++gFailures;                                                                           \
        }                                                                                          \
    } while (0)

// Renders a running sample counter on both channels.
struct Counter {
    std::atomic<uint32_t> next{0};
    std::atomic<uint32_t> calls{0};
    static void render(float* out, uint32_t frames, void* user) {
        auto& self = *static_cast<Counter*>(user);
        for (uint32_t i = 0; i < frames; ++i) {
            const float value = float(self.next++);
            out[i * 2] = value;
            out[i * 2 + 1] = -value;
        }
        ++self.calls;
    }
};

void test_ring_wraps() {
    audio::Ring ring(8, 2);
    // Start the counters near 2^32 so they wrap while in use.
    ring.header().read = 0xFFFFFFFCu;
    ring.header().write = 0xFFFFFFFCu;
    std::vector<float> in, out(16);
    for (int round = 0; round < 5; ++round) {
        in.clear();
        for (int i = 0; i < 6; ++i) {
            in.push_back(float(round * 10 + i));
            in.push_back(float(-(round * 10 + i)));
        }
        ring.push(in.data(), 6);
        CHECK(ring.buffered() == 6);
        CHECK(ring.pop(out.data(), 4) == 4);
        CHECK(ring.pop(out.data() + 8, 8) == 2);
        CHECK(ring.buffered() == 0);
        for (int i = 0; i < 12; ++i) {
            CHECK(out[i] == in[i]);
        }
    }
}

void test_plays_continuously() {
    audio::Ring ring(4096, 2);
    Counter counter;
    ring.header().playing = 1;
    audio::Mixer mixer(ring, 48000, 2048, 256, &Counter::render, &counter);

    // Pull 128 frames every ~2.7 ms, like a 48 kHz AudioWorklet, for about 0.3 s.
    std::vector<float> block(128 * 2);
    uint32_t expected = 0;
    bool continuous = true;
    uint32_t underruns = 0;
    std::this_thread::sleep_for(20ms);  // let it prefill
    for (int i = 0; i < 110; ++i) {
        const uint32_t got = ring.pop(block.data(), 128);
        underruns += 128 - got;
        for (uint32_t f = 0; f < got; ++f) {
            continuous &= block[f * 2] == float(expected) && block[f * 2 + 1] == -float(expected);
            ++expected;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(2667));
    }
    CHECK(continuous);
    CHECK(underruns == 0);
    CHECK(ring.buffered() <= 2048);
}

void test_discards_in_real_time_while_not_playing() {
    audio::Ring ring(4096, 2);
    Counter counter;
    audio::Mixer mixer(ring, 48000, 2048, 480, &Counter::render, &counter);  // 10 ms chunks
    std::this_thread::sleep_for(300ms);
    const uint32_t rendered = counter.next;
    // About 300 ms worth (14400 frames), give or take scheduling.
    CHECK(rendered >= 9600 && rendered <= 19200);
    CHECK(ring.buffered() == 0);
}

void test_pause_stops_rendering() {
    audio::Ring ring(4096, 2);
    Counter counter;
    audio::Mixer mixer(ring, 48000, 2048, 480, &Counter::render, &counter);
    mixer.set_paused(true);
    std::this_thread::sleep_for(30ms);
    const uint32_t before = counter.calls;
    std::this_thread::sleep_for(100ms);
    CHECK(counter.calls == before);
    mixer.set_paused(false);
    std::this_thread::sleep_for(100ms);
    CHECK(counter.calls > before);
}

}  // namespace

int main() {
    test_ring_wraps();
    test_plays_continuously();
    test_discards_in_real_time_while_not_playing();
    test_pause_stops_rendering();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    std::puts("web audio: all tests passed");
    return 0;
}
