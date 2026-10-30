#include "audio.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <climits>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#endif

namespace dusk::web::audio {

Ring::Ring(uint32_t capacityFrames, uint32_t channels)
    : mCapacity(capacityFrames), mChannels(channels), mData(size_t(capacityFrames) * channels) {
    // Positions are free-running 32-bit counters, so the capacity must divide 2^32.
    assert(capacityFrames != 0 && (capacityFrames & (capacityFrames - 1)) == 0);
}

uint32_t Ring::buffered() const {
    return mHeader.write.load(std::memory_order_acquire) - mHeader.read.load(std::memory_order_acquire);
}

void Ring::push(const float* interleaved, uint32_t frames) {
    const uint32_t write = mHeader.write.load(std::memory_order_relaxed);
    assert(frames <= mCapacity - buffered());
    const uint32_t start = write % mCapacity;
    const uint32_t first = std::min(frames, mCapacity - start);
    std::memcpy(&mData[size_t(start) * mChannels], interleaved, size_t(first) * mChannels * sizeof(float));
    std::memcpy(mData.data(), interleaved + size_t(first) * mChannels,
                size_t(frames - first) * mChannels * sizeof(float));
    mHeader.write.store(write + frames, std::memory_order_release);
}

uint32_t Ring::pop(float* interleaved, uint32_t frames) {
    const uint32_t read = mHeader.read.load(std::memory_order_relaxed);
    const uint32_t count = std::min(frames, mHeader.write.load(std::memory_order_acquire) - read);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t base = size_t((read + i) % mCapacity) * mChannels;
        std::memcpy(interleaved + size_t(i) * mChannels, &mData[base], mChannels * sizeof(float));
    }
    mHeader.read.store(read + count, std::memory_order_release);
    mHeader.read.notify_all();
    return count;
}

Mixer::Mixer(Ring& ring, uint32_t sampleRate, uint32_t targetFrames, uint32_t chunkFrames, RenderCallback render,
             void* user)
    : mRing(ring), mSampleRate(sampleRate), mTargetFrames(std::min(targetFrames, ring.capacity())),
      mChunkFrames(chunkFrames), mRender(render), mUser(user), mThread([this] { run(); }) {}

Mixer::~Mixer() {
    mStop = true;
    mThread.join();
}

void Mixer::run() {
    using clock = std::chrono::steady_clock;
    std::vector<float> chunk(size_t(mChunkFrames) * mRing.channels());
    const auto chunkDuration = std::chrono::nanoseconds(1'000'000'000ull * mChunkFrames / mSampleRate);
    auto next = clock::now();

    while (!mStop.load(std::memory_order_relaxed)) {
        if (mPaused.load(std::memory_order_acquire) || mRing.header().hidden.load(std::memory_order_acquire) != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            next = clock::now();
            continue;
        }
        if (mRing.header().playing.load(std::memory_order_acquire) == 0) {
            // Nothing is playing: keep the game's audio moving in real time, but drop it.
            mRender(chunk.data(), mChunkFrames, mUser);
            next += chunkDuration;
            const auto now = clock::now();
            if (next < now - std::chrono::milliseconds(100)) {
                next = now;  // fell far behind (e.g. the tab was in the background); don't catch up
            }
            std::this_thread::sleep_until(next);
            continue;
        }
        next = clock::now();

        const uint32_t buffered = mRing.buffered();
        if (buffered < mTargetFrames) {
            const uint32_t frames = std::min(mChunkFrames, mTargetFrames - buffered);
            mRender(chunk.data(), frames, mUser);
            mRing.push(chunk.data(), frames);
            continue;
        }

        // Full enough: wait for the player to consume some.
        const uint32_t read = mRing.header().read.load(std::memory_order_acquire);
#ifdef __EMSCRIPTEN__
        emscripten_futex_wait(reinterpret_cast<volatile void*>(&mRing.header().read), read, 5.0);
#else
        (void)read;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    }
}

#ifdef __EMSCRIPTEN__

// Creates the AudioContext and the worklet (platforms/web/shell/audio-worklet.js) that plays the
// ring. Returns the context's sample rate, or 0.
EM_ASYNC_JS(uint32_t, dusk_web_audio_start, (void* header, float* data, uint32_t frames, uint32_t channels,
                                              uint32_t preferredRate), {
  try {
    // Pointers arrive as signed numbers; this keeps those above 2 GiB valid offsets.
    header >>>= 0;
    data >>>= 0;
    const memory = typeof wasmMemory !== 'undefined' ? wasmMemory : Module['wasmMemory'];
    let ctx;
    try {
      ctx = new AudioContext({ sampleRate: preferredRate, latencyHint: 'interactive' });
    } catch (e) {
      // Some browsers only run at the device's rate; the game's stream resamples to it.
      ctx = new AudioContext({ latencyHint: 'interactive' });
    }
    // Served next to the page (platforms/web/shell).
    await ctx.audioWorklet.addModule('audio-worklet.js');
    const node = new AudioWorkletNode(ctx, 'dusk-audio', {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [2],
      processorOptions: { memory, header, data, frames, channels },
    });
    node.connect(ctx.destination);
    const flags = () => new Int32Array(memory.buffer, header, 5);
    const playing = () => Atomics.store(flags(), 2, ctx.state === 'running' ? 1 : 0);
    ctx.onstatechange = playing;
    playing();
    // Browsers start audio only after a user gesture; resume on the next one if it is still waiting.
    // A refused resume() just leaves audio waiting, so its rejection is ignored.
    const wanted = () => !Module['duskAudioPaused'] && !document.hidden;
    const resume = () => {
      if (ctx.state === 'suspended' && wanted()) ctx.resume().catch(() => {});
    };
    const gestures = ['pointerdown', 'keydown', 'touchend'];
    for (const type of gestures) {
      document.addEventListener(type, resume, { capture: true });
    }
    // A hidden page gets no animation frames, so the game stops; stop its audio with it.
    const visibility = () => {
      Atomics.store(flags(), 4, document.hidden ? 1 : 0);
      if (document.hidden) ctx.suspend().catch(() => {});
      else resume();
    };
    document.addEventListener('visibilitychange', visibility);
    visibility();
    Module['duskAudio'] = { ctx, node, resume, gestures, visibility };
    resume();
    return ctx.sampleRate;
  } catch (e) {
    console.error('web audio: ' + e);
    return 0;
  }
});

EM_JS(void, dusk_web_audio_set_paused, (int paused), {
  Module['duskAudioPaused'] = !!paused;
  const audio = Module['duskAudio'];
  if (audio) {
    if (paused) audio.ctx.suspend().catch(() => {});
    else audio.resume();
  }
});

// Resolves once the audio thread has stopped, so the ring it reads can be freed afterwards.
EM_ASYNC_JS(void, dusk_web_audio_stop, (), {
  const audio = Module['duskAudio'];
  if (!audio) return;
  delete Module['duskAudio'];
  for (const type of audio.gestures) {
    document.removeEventListener(type, audio.resume, { capture: true });
  }
  document.removeEventListener('visibilitychange', audio.visibility);
  audio.ctx.onstatechange = null;
  audio.node.disconnect();
  await audio.ctx.close().catch(() => {});
});

uint32_t start_output(Ring& ring, uint32_t preferredRate) {
    return dusk_web_audio_start(&ring.header(), ring.data(), ring.capacity(), ring.channels(), preferredRate);
}

void set_output_paused(bool paused) { dusk_web_audio_set_paused(paused ? 1 : 0); }

void stop_output() { dusk_web_audio_stop(); }

#endif

}  // namespace dusk::web::audio
