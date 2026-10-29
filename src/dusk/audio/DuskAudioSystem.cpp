#include "DuskAudioSystem.h"

#include "DuskDsp.hpp"

#include "JSystem/JAudio2/JASAiCtrl.h"
#include "JSystem/JAudio2/JASAudioThread.h"
#include "JSystem/JAudio2/JASChannel.h"
#include "JSystem/JAudio2/JASCriticalSection.h"
#include "JSystem/JAudio2/JASDriverIF.h"
#include "JSystem/JAudio2/JASDSPChannel.h"
#include "JSystem/JAudio2/JASHeapCtrl.h"

#include <SDL3/SDL_init.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <memory>
#include <span>

#ifdef __EMSCRIPTEN__
#include "dusk/logging.h"
#include "dusk/web/audio.hpp"
#endif

using namespace dusk::audio;

static OutputSubframe OutBuffer;
static std::array<f32, DSP_SUBFRAME_SIZE * OutputSubframe::NUM_CHANNELS> OutInterleaveBufferFull;

static SDL_AudioStream* PlaybackStream;

#ifdef __EMSCRIPTEN__
// On the web, PlaybackStream is a plain FIFO (no device): a mixer thread renders into it and
// hands the samples to the browser (see dusk/web/audio.hpp).
static std::unique_ptr<dusk::web::audio::Ring> WebRing;
static std::unique_ptr<dusk::web::audio::Mixer> WebMixer;
// The rate the browser plays at (0 without audio output).
static u32 WebOutputRate;

static void RenderForWeb(float* out, uint32_t frames, void*);
static void StartWebOutput();
#endif

/**
 * SDL audiostream callback to trigger rendering of new audio data.
 */
static void SDLCALL GetNewAudio(
    void*,
    SDL_AudioStream*,
    int needed,
    int);

/**
 * Render an entire new frame of audio and output it to SDL3.
 * Note: "audio frames" are unrelated to video frames.
 * @return Amount of audio samples rendered in bytes.
 */
static int RenderNewAudioFrame();

/**
 * Render an audio subframe and output it to SDL3.
 */
static int RenderAudioSubframe();

static size_t GetChannelCountForOutputMode(dusk::AudioOutputMode config) {
    switch (config) {
        default:
        case dusk::AudioOutputMode::StereoSpeakers:
        case dusk::AudioOutputMode::StereoHeadphones:
            return 2;
        case dusk::AudioOutputMode::Surround6ch:
            return 6;
        case dusk::AudioOutputMode::Surround8ch:
            return 8;
    }
}

static bool InitSDL3Output() {
    const auto speakerConfig = dusk::getSettings().audio.outputMode.getValue();
#ifdef __EMSCRIPTEN__
    // The browser output is stereo.
    const size_t desiredChannelCount = 2;
#else
    const auto desiredChannelCount = GetChannelCountForOutputMode(speakerConfig);
#endif
    const bool hrtf = speakerConfig == dusk::AudioOutputMode::StereoHeadphones;

    if (PlaybackStream && desiredChannelCount == OutChannelCount) {
        JASCriticalSection section;
        EnableHrtf = hrtf;
        return false;
    }

    if (PlaybackStream) {
        SDL_PauseAudioStreamDevice(PlaybackStream);
        SDL_DestroyAudioStream(PlaybackStream);
    } else {
        SDL_Init(SDL_INIT_AUDIO);
    }

    const SDL_AudioSpec spec = {
        SDL_AUDIO_F32,
        static_cast<int>(desiredChannelCount),
        SampleRate,
    };
#ifdef __EMSCRIPTEN__
    SDL_AudioStream* newStream = SDL_CreateAudioStream(&spec, &spec);
#else
    SDL_AudioStream* newStream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &GetNewAudio, nullptr);
#endif

    {
        JASCriticalSection section;
        EnableHrtf = hrtf;
        OutChannelCount = desiredChannelCount;
        PlaybackStream = newStream;
    }

    return true;
}

void dusk::audio::Initialize() {
    // enable 48 kHz mode
    // this will scale voice pitch and track tempo accordingly
    JASDriver::setOutputRate(static_cast<JASOutputRate>(-1));

    InitSDL3Output();
    DspInit();

    JASDsp::initBuffer();
    JASDSPChannel::initAll();

    JASPoolAllocObject_MultiThreaded<JASChannel>::newMemPool(0x48);

#ifdef __EMSCRIPTEN__
    StartWebOutput();
#else
    SDL_ResumeAudioStreamDevice(PlaybackStream);
#endif
}

void dusk::audio::Reinitialize() {
    // don't re-init unless we've initialized first (using PlaybackStream being set as proxy)
    if (PlaybackStream && InitSDL3Output()) {
        SDL_ResumeAudioStreamDevice(PlaybackStream);
    }
}

void dusk::audio::Shutdown() {
#ifdef __EMSCRIPTEN__
    // The worklet reads the ring until the browser's audio thread has stopped, which
    // stop_output() waits for.
    WebMixer.reset();
    dusk::web::audio::stop_output();
    WebRing.reset();
#endif
    if (PlaybackStream) {
        SDL_DestroyAudioStream(PlaybackStream);
        PlaybackStream = nullptr;
    }

    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void dusk::audio::SetMasterVolume(const f32 value) {
    JASCriticalSection section;

    MasterVolume = value;
}

void dusk::audio::SetPaused(const bool paused) {
#ifdef __EMSCRIPTEN__
    if (WebMixer) {
        WebMixer->set_paused(paused);
        dusk::web::audio::set_output_paused(paused);
    }
#else
    if (paused) {
        SDL_PauseAudioStreamDevice(PlaybackStream);
    } else {
        SDL_ResumeAudioStreamDevice(PlaybackStream);
    }
#endif
}

void dusk::audio::SetEnableReverb(const bool value) {
    JASCriticalSection section;

    EnableReverb = value;
}

#ifdef TRACY_ENABLE
static auto FrameName = "GetNewAudio";
#endif

void SDLCALL GetNewAudio(
    void*,
    SDL_AudioStream*,
    int needed,
    int) {
    FrameMarkStart(FrameName);
    while (needed > 0) {
        const int rendered = RenderNewAudioFrame();
        needed -= rendered;
    }
    FrameMarkEnd(FrameName);
}

#ifdef __EMSCRIPTEN__
void RenderForWeb(float* out, uint32_t frames, void*) {
    const int bytes = static_cast<int>(frames * OutChannelCount * sizeof(f32));
    while (SDL_GetAudioStreamAvailable(PlaybackStream) < bytes) {
        if (RenderNewAudioFrame() <= 0) {
            break;
        }
    }
    const int got = SDL_GetAudioStreamData(PlaybackStream, out, bytes);
    if (got < bytes) {
        std::memset(reinterpret_cast<u8*>(out) + std::max(got, 0), 0, bytes - std::max(got, 0));
    }
}

void StartWebOutput() {
    using namespace dusk::web::audio;
    WebRing = std::make_unique<Ring>(4096, static_cast<uint32_t>(OutChannelCount));
    WebOutputRate = start_output(*WebRing, SampleRate);
    u32 mixRate = WebOutputRate;
    if (WebOutputRate == 0) {
        // The mixer keeps the game's audio moving silently, at the game's own rate.
        DuskLog.warn("Web audio output is unavailable; continuing without sound");
        mixRate = SampleRate;
    } else if (WebOutputRate != static_cast<u32>(SampleRate)) {
        // The browser only plays at the device's rate; the stream resamples the game's output.
        const SDL_AudioSpec deviceSpec = {
            SDL_AUDIO_F32,
            static_cast<int>(OutChannelCount),
            static_cast<int>(WebOutputRate),
        };
        SDL_SetAudioStreamFormat(PlaybackStream, nullptr, &deviceSpec);
        DuskLog.info("Web audio plays at {} Hz; resampling from {} Hz", WebOutputRate, SampleRate);
    }
    // About 43 ms buffered (2048 frames at 48 kHz), rendered in steps of about 5 ms.
    WebMixer = std::make_unique<Mixer>(*WebRing, mixRate, mixRate * 43 / 1000, 256, &RenderForWeb, nullptr);
}

dusk::audio::WebOutputStats dusk::audio::GetWebOutputStats() {
    WebOutputStats stats{};
    if (!WebRing) {
        return stats;
    }
    const auto& header = WebRing->header();
    stats.sampleRate = WebOutputRate;
    stats.playing = header.playing.load(std::memory_order_relaxed) != 0;
    stats.hidden = header.hidden.load(std::memory_order_relaxed) != 0;
    stats.bufferedFrames = WebRing->buffered();
    stats.underrunFrames = header.underruns.load(std::memory_order_relaxed);
    return stats;
}
#endif

int RenderNewAudioFrame() {
    ZoneScoped;
    JASCriticalSection section;
    const u32 countSubframes = JASDriver::getSubFrames();
    int bytesWritten = 0;

    JASAudioThread::setDSPSyncCount(countSubframes);

    for (u32 i = 0; i < countSubframes; i++) {
        bytesWritten += RenderAudioSubframe();

        JASAudioThread::snIntCount -= 1;
    }

    return bytesWritten;
}

static void InterleaveOutputData(const OutputSubframe& data, std::span<f32> target) {
    assert(target.size() >= data.channels[0].size() * OutChannelCount);

    size_t outPos = 0;
    for (size_t inPos = 0; inPos < data.channels[0].size(); inPos++) {
        for (size_t channelIdx = 0; channelIdx < OutChannelCount; channelIdx++) {
            target[outPos++] = data.channels[channelIdx][inPos];
        }
    }
}

constexpr auto kExtChannels = 2;
constexpr auto kExtSrcInSampleCount = DSP_SUBFRAME_SIZE * 2;
constexpr auto kExtSrcOutSampleCount = DSP_SUBFRAME_SIZE * 3;

// simple 3:2 resampler
struct LinearResampler {
    f32 prev = 0.0f;
    int phase = 0;

    void resample(std::span<const s16> in, std::span<f32> out, int skip = 1, int offset = 0) {
        const auto inSize = in.size() / skip;
        const auto outSize = out.size() / skip;
        assert(inSize * 3 == outSize * 2);

        int idx = -1;
        int phi = phase;
        int n = 0;

        while (n < outSize) {
            f32 s0 = (idx < 0) ? prev : in[idx * skip + offset] / 32768.0f;
            f32 s1 = in[idx * skip + skip + offset] / 32768.0f;
            f32 frac = phi * (1.0f / 3.0f);

            out[n++ * skip + offset] = s0 + frac * (s1 - s0);

            phi += 2;
            if (phi >= 3) {
                phi -= 3;
                idx++;
            }
        }

        prev = in[(inSize - 1) * skip + offset] / 32768.0f;
        phase = phi;
    }
};

static struct {
    std::array<f32, kExtSrcOutSampleCount * kExtChannels> buf;
    int available = 0;
    LinearResampler leftSrc;
    LinearResampler rightSrc;

    bool hungry() const { return available <= 0; }

    void feed(std::span<const s16> in) {
        const auto out = std::span{buf};
        leftSrc.resample(in, out, kExtChannels, 0);
        rightSrc.resample(in, out, kExtChannels, 1);
        available = buf.size();
    }

    std::span<const f32> drain_subframe() {
        constexpr auto kDrainSize = DSP_SUBFRAME_SIZE * kExtChannels;
        assert(available >= kDrainSize);
        const auto rv = std::span{&buf[buf.size() - available], kDrainSize};
        available -= kDrainSize;
        return rv;
    }
} extResampler;

int RenderAudioSubframe() {
    ZoneScoped;
    OutBuffer = {};

    JASDriver::updateDSP();
    DspRender(OutBuffer);

    std::span<f32> OutInterleaveBuffer{OutInterleaveBufferFull.data(), static_cast<size_t>(DSP_SUBFRAME_SIZE * OutChannelCount)};
    InterleaveOutputData(OutBuffer, OutInterleaveBuffer);

    if (JASDriver::extMixCallback != nullptr && JASDriver::sMixMode == MIX_MODE_INTERLEAVE) {
        if (extResampler.hungry()) {
            const auto mixData = JASDriver::extMixCallback(kExtSrcInSampleCount);
            if (mixData) {
                extResampler.feed(std::span{mixData, kExtSrcInSampleCount * kExtChannels});
            }
        }

        if (!extResampler.hungry()) {
            const auto inBuf = extResampler.drain_subframe();
            for (int i = 0; i < DSP_SUBFRAME_SIZE; i++) {
                const auto oi = i * OutChannelCount;
                OutInterleaveBuffer[oi]     += inBuf[i * kExtChannels];
                OutInterleaveBuffer[oi + 1] += inBuf[i * kExtChannels + 1];
            }
        }
    }

    auto bytesToWrite = OutInterleaveBuffer.size_bytes();
    SDL_PutAudioStreamData(PlaybackStream, OutInterleaveBuffer.data(), bytesToWrite);
    return bytesToWrite;
}

u32 dusk::audio::GetResetCount(int channelIdx) {
    return ChannelAux[channelIdx].resetCount;
}

f32 dusk::audio::VolumeFromU16(u16 value) {
    return static_cast<f32>(value) / static_cast<f32>(JASDriver::getChannelLevel_dsp());
}
