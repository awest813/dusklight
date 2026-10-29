#include "disc.hpp"

#include <SDL3/SDL_iostream.h>
#include <borealis/io.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <mutex>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#endif

namespace dusk::web::disc {
namespace {

// Large reads are split so one request never asks the worker for an unbounded allocation.
constexpr size_t kMaxRequest = 16 * 1024 * 1024;

Channel gChannel;
// One request slot per channel, shared by every thread.
std::mutex gRequestMutex;

void wait_while(std::atomic<int32_t>& state, int32_t value) {
    while (state.load(std::memory_order_acquire) == value) {
#ifdef __EMSCRIPTEN__
        // Spins on the browser main thread, which may not block; the disc worker never waits on
        // the main thread, so this always completes.
        emscripten_futex_wait(reinterpret_cast<volatile void*>(&state), static_cast<uint32_t>(value), INFINITY);
#else
        state.wait(value, std::memory_order_acquire);
#endif
    }
}

void wake(std::atomic<int32_t>& state) {
#ifdef __EMSCRIPTEN__
    emscripten_futex_wake(reinterpret_cast<volatile void*>(&state), INT_MAX);
#else
    state.notify_all();
#endif
}

int64_t request(Channel& channel, uint64_t offset, void* out, uint32_t length) {
    std::lock_guard lock{gRequestMutex};
    channel.offset = static_cast<double>(offset);
    channel.destination = static_cast<double>(reinterpret_cast<uintptr_t>(out));
    channel.length = length;
    channel.result = -1;
    channel.state.store(StateRequest, std::memory_order_release);
    wake(channel.state);
    wait_while(channel.state, StateRequest);
    const int32_t result = channel.result;
    // The worker waits for the reply to be taken before it looks for the next request.
    channel.state.store(StateIdle, std::memory_order_release);
    wake(channel.state);
    return result;
}

struct Stream {
    Channel* channel;
    uint64_t position = 0;
};

Sint64 SDLCALL stream_size(void* userdata) {
    const double size = static_cast<Stream*>(userdata)->channel->size;
    return size < 0 ? -1 : static_cast<Sint64>(size);
}

Sint64 SDLCALL stream_seek(void* userdata, Sint64 offset, SDL_IOWhence whence) {
    auto* stream = static_cast<Stream*>(userdata);
    Sint64 origin = 0;
    switch (whence) {
    case SDL_IO_SEEK_SET:
        break;
    case SDL_IO_SEEK_CUR:
        origin = static_cast<Sint64>(stream->position);
        break;
    case SDL_IO_SEEK_END:
        origin = stream_size(userdata);
        break;
    default:
        SDL_SetError("Invalid seek origin");
        return -1;
    }
    const Sint64 target = origin + offset;
    if (target < 0) {
        SDL_SetError("Seek before the start of the disc image");
        return -1;
    }
    stream->position = static_cast<uint64_t>(target);
    return target;
}

size_t SDLCALL stream_read(void* userdata, void* ptr, size_t size, SDL_IOStatus* status) {
    auto* stream = static_cast<Stream*>(userdata);
    const int64_t got = read_at(*stream->channel, stream->position, ptr, size);
    if (got < 0) {
        SDL_SetError("Failed to read the disc image");
        *status = SDL_IO_STATUS_ERROR;
        return 0;
    }
    if (static_cast<size_t>(got) < size) {
        *status = SDL_IO_STATUS_EOF;
    }
    stream->position += static_cast<uint64_t>(got);
    return static_cast<size_t>(got);
}

size_t SDLCALL stream_write(void*, const void*, size_t, SDL_IOStatus* status) {
    SDL_SetError("The disc image is read-only");
    *status = SDL_IO_STATUS_READONLY;
    return 0;
}

bool SDLCALL stream_close(void* userdata) {
    delete static_cast<Stream*>(userdata);
    return true;
}

}  // namespace

Channel& channel() { return gChannel; }

bool available() { return gChannel.size >= 0; }

std::string name() {
    return std::string(gChannel.name, strnlen(gChannel.name, sizeof(gChannel.name)));
}

std::string location() { return std::string(kScheme) + "://" + name(); }

bool is_location(std::string_view location) {
    return location.size() > kScheme.size() + 3 && location.starts_with(kScheme) &&
           location.substr(kScheme.size(), 3) == "://";
}

int64_t read_at(Channel& channel, uint64_t offset, void* out, size_t length) {
    if (channel.size < 0) {
        return -1;
    }
    const uint64_t size = static_cast<uint64_t>(channel.size);
    if (offset >= size) {
        return 0;
    }
    length = static_cast<size_t>(std::min<uint64_t>(length, size - offset));
    auto* dst = static_cast<uint8_t*>(out);
    size_t total = 0;
    while (total < length) {
        const auto chunk = static_cast<uint32_t>(std::min(length - total, kMaxRequest));
        const int64_t got = request(channel, offset + total, dst + total, chunk);
        if (got < 0) {
            return -1;
        }
        if (got == 0) {
            break;
        }
        total += static_cast<size_t>(got);
    }
    return static_cast<int64_t>(total);
}

SDL_IOStream* open_stream(Channel& channel) {
    if (channel.size < 0) {
        SDL_SetError("No disc image is attached");
        return nullptr;
    }
    SDL_IOStreamInterface iface;
    SDL_INIT_INTERFACE(&iface);
    iface.size = stream_size;
    iface.seek = stream_seek;
    iface.read = stream_read;
    iface.write = stream_write;
    iface.close = stream_close;
    auto* stream = new Stream{&channel};
    SDL_IOStream* io = SDL_OpenIO(&iface, stream);
    if (io == nullptr) {
        delete stream;
    }
    return io;
}

void initialize() {
    borealis::io::register_scheme(kScheme, [](std::string_view location) -> SDL_IOStream* {
        if (!is_location(location) || !available()) {
            return nullptr;
        }
        return open_stream(gChannel);
    });
}

}  // namespace dusk::web::disc

#ifdef __EMSCRIPTEN__
// Called by the page before main() to hand the channel to its disc worker.
extern "C" EMSCRIPTEN_KEEPALIVE dusk::web::disc::Channel* dusk_web_disc_channel() {
    return &dusk::web::disc::channel();
}
#endif
