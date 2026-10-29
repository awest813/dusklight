#pragma once

// The disc image on the web build. The page (platforms/web/shell) lets the player pick their disc
// image and hands it to a dedicated JS worker (disc-worker.js) that reads it, either from the
// browser's private file system (OPFS) or from the picked file itself. Game threads ask that
// worker for bytes through a request slot in shared wasm memory, which works from any thread and
// never needs the browser main thread to make progress.
//
// The disc is addressed as "webdisc://<file name>" through borealis::io, so the existing
// inspect/verify/open paths work unchanged.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

struct SDL_IOStream;

namespace dusk::web::disc {

inline constexpr std::string_view kScheme = "webdisc";

// Shared with disc-worker.js, which addresses the fields by byte offset; keep the layout in sync.
struct Channel {
    std::atomic<int32_t> state{0};  //  0: idle, 1: request posted, 2: reply ready
    int32_t result = 0;             //  4: bytes read, or -1
    uint32_t length = 0;            //  8: bytes requested
    uint32_t pad = 0;               // 12
    double offset = 0;              // 16: image offset
    double destination = 0;         // 24: address to read into
    double size = -1;               // 32: image size, or -1 while no disc is attached
    char name[256]{};               // 40: image file name, NUL-terminated
};
static_assert(offsetof(Channel, offset) == 16 && offsetof(Channel, destination) == 24 &&
              offsetof(Channel, size) == 32 && offsetof(Channel, name) == 40);

enum : int32_t {
    StateIdle = 0,
    StateRequest = 1,
    StateReply = 2,
};

// The channel the page's disc worker serves. Its address is exported to JS.
Channel& channel();

// Whether the page attached a disc image, and its file name.
bool available();
std::string name();

// "webdisc://<name>" for the attached disc.
std::string location();
bool is_location(std::string_view location);

// Reads up to `length` bytes at `offset` through `channel`. Thread-safe. Returns the number of
// bytes read (short only at the end of the image), or -1 on error.
int64_t read_at(Channel& channel, uint64_t offset, void* out, size_t length);

// A read-only stream over the image behind `channel`.
SDL_IOStream* open_stream(Channel& channel);

// Registers the "webdisc" scheme with borealis::io.
void initialize();

}  // namespace dusk::web::disc
