#pragma once

// Audio output for the web build.
//
// SDL3 plays audio in the browser from a ScriptProcessorNode on the main thread, which would run
// the game's whole mixer (JAudio's sequencer and DSP) there. Dusklight's audio code expects to run
// concurrently with the game: Z2SoundMgr::stopSync() spins until the audio side advances. So on
// the web a dedicated mixer thread renders into a ring buffer in shared wasm memory, and an
// AudioWorklet plays from it.

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace dusk::web::audio {

// Shared with the AudioWorklet processor in audio.cpp, which addresses the fields by index.
struct RingHeader {
    std::atomic<uint32_t> read{0};       // 0: frames consumed, ever (wraps)
    std::atomic<uint32_t> write{0};      // 1: frames produced, ever (wraps)
    std::atomic<int32_t> playing{0};     // 2: whether the browser is pulling audio
    uint32_t underruns = 0;              // 3: frames the worklet had to fill with silence
};

// Fills `frames` interleaved frames of `channels` samples. Called on the mixer thread.
using RenderCallback = void (*)(float* out, uint32_t frames, void* user);

class Ring {
public:
    Ring(uint32_t capacityFrames, uint32_t channels);

    RingHeader& header() { return mHeader; }
    float* data() { return mData.data(); }
    uint32_t capacity() const { return mCapacity; }
    uint32_t channels() const { return mChannels; }

    uint32_t buffered() const;
    // Producer side: copies `frames` frames (wrapping) and publishes them.
    void push(const float* interleaved, uint32_t frames);
    // Consumer side (the worklet's job; used by tests): reads up to `frames` frames, returns how many.
    uint32_t pop(float* interleaved, uint32_t frames);

private:
    RingHeader mHeader;
    uint32_t mCapacity;
    uint32_t mChannels;
    std::vector<float> mData;
};

// Renders into a ring on its own thread, keeping about `targetFrames` buffered. While nothing
// plays the ring (autoplay not yet allowed, or paused), it keeps rendering at real-time pace and
// discards the output, so the game never waits on audio that isn't running.
class Mixer {
public:
    Mixer(Ring& ring, uint32_t sampleRate, uint32_t targetFrames, uint32_t chunkFrames, RenderCallback render,
          void* user);
    ~Mixer();

    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    // While paused (the game is paused), nothing is rendered, as when native output is paused.
    void set_paused(bool paused) { mPaused.store(paused, std::memory_order_release); }

private:
    void run();

    Ring& mRing;
    uint32_t mSampleRate;
    uint32_t mTargetFrames;
    uint32_t mChunkFrames;
    RenderCallback mRender;
    void* mUser;
    std::atomic<bool> mStop{false};
    std::atomic<bool> mPaused{false};
    std::thread mThread;
};

#ifdef __EMSCRIPTEN__
// Starts browser playback of `ring` at `sampleRate` (stereo). Main thread only.
bool start_output(Ring& ring, uint32_t sampleRate);
// Suspends or resumes browser playback. Main thread only.
void set_output_paused(bool paused);
void stop_output();
#endif

}  // namespace dusk::web::audio
