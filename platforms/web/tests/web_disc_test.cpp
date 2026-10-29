// Native test of the web disc channel (src/dusk/web/disc.cpp): a thread plays the part of the
// page's disc worker (platforms/web/shell/disc-worker.js), serving requests from an in-memory
// "image", while several threads read through the channel, the SDL stream and borealis::io.
//
// Built and run by platforms/web/tests/run-native-tests.sh.

#include "dusk/web/disc.hpp"

#include <SDL3/SDL_iostream.h>
#include <borealis/io.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace disc = dusk::web::disc;

namespace {

int gFailures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++gFailures;                                                                           \
        }                                                                                          \
    } while (0)

uint8_t pattern(uint64_t offset) { return static_cast<uint8_t>((offset * 2654435761u) >> 13); }

// What disc-worker.js does: wait for a request, copy the bytes, post the reply.
class FakeWorker {
public:
    FakeWorker(disc::Channel& channel, uint64_t size, const char* name) : mChannel(channel) {
        std::strncpy(mChannel.name, name, sizeof(mChannel.name) - 1);
        mChannel.size = static_cast<double>(size);
        mThread = std::thread([this, size] {
            while (true) {
                mChannel.state.wait(disc::StateIdle, std::memory_order_acquire);
                const int32_t state = mChannel.state.load(std::memory_order_acquire);
                if (mStop.load()) {
                    return;
                }
                if (state != disc::StateRequest) {
                    continue;
                }
                const auto offset = static_cast<uint64_t>(mChannel.offset);
                auto* dst = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(mChannel.destination));
                const uint64_t n = offset >= size ? 0 : std::min<uint64_t>(mChannel.length, size - offset);
                for (uint64_t i = 0; i < n; ++i) {
                    dst[i] = pattern(offset + i);
                }
                ++mRequests;
                mChannel.result = mFailNext.exchange(false) ? -1 : static_cast<int32_t>(n);
                mChannel.state.store(disc::StateReply, std::memory_order_release);
                mChannel.state.notify_all();
                // Wait for the requester to take the reply before looking for the next request.
                mChannel.state.wait(disc::StateReply, std::memory_order_acquire);
            }
        });
    }

    ~FakeWorker() {
        mStop = true;
        mChannel.state.store(disc::StateRequest);
        mChannel.state.notify_all();
        mThread.join();
        mChannel.state.store(disc::StateIdle);
        mChannel.size = -1;
    }

    std::atomic<int> mRequests{0};
    std::atomic<bool> mFailNext{false};

private:
    disc::Channel& mChannel;
    std::atomic<bool> mStop{false};
    std::thread mThread;
};

bool matches(const std::vector<uint8_t>& data, uint64_t offset) {
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] != pattern(offset + i)) {
            return false;
        }
    }
    return true;
}

void test_reads_through_the_channel() {
    disc::Channel channel;
    const uint64_t size = 40 * 1024 * 1024 + 123;  // larger than one request
    FakeWorker worker(channel, size, "Synthetic.iso");

    std::vector<uint8_t> buf(4096);
    CHECK(disc::read_at(channel, 1000, buf.data(), buf.size()) == 4096);
    CHECK(matches(buf, 1000));

    // Split into several requests.
    std::vector<uint8_t> big(36 * 1024 * 1024);
    const int before = worker.mRequests;
    CHECK(disc::read_at(channel, 7, big.data(), big.size()) == int64_t(big.size()));
    CHECK(matches(big, 7));
    CHECK(worker.mRequests - before == 3);

    // Short at the end, empty past it.
    CHECK(disc::read_at(channel, size - 10, buf.data(), buf.size()) == 10);
    CHECK(disc::read_at(channel, size, buf.data(), buf.size()) == 0);

    worker.mFailNext = true;
    CHECK(disc::read_at(channel, 0, buf.data(), 16) == -1);
    CHECK(disc::read_at(channel, 0, buf.data(), 16) == 16);
}

void test_concurrent_readers() {
    disc::Channel channel;
    const uint64_t size = 8 * 1024 * 1024;
    FakeWorker worker(channel, size, "Synthetic.iso");
    std::atomic<int> bad{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 6; ++t) {
        readers.emplace_back([&, t] {
            std::vector<uint8_t> buf(3000 + t * 17);
            for (int i = 0; i < 200; ++i) {
                const uint64_t offset = (uint64_t(i) * 7919 + t * 104729) % (size - buf.size());
                if (disc::read_at(channel, offset, buf.data(), buf.size()) != int64_t(buf.size()) ||
                    !matches(buf, offset))
                {
                    ++bad;
                }
            }
        });
    }
    for (auto& r : readers) {
        r.join();
    }
    CHECK(bad == 0);
}

void test_sdl_stream_and_borealis_scheme() {
    CHECK(!disc::available());
    CHECK(disc::open_stream(disc::channel()) == nullptr);

    const uint64_t size = 1 << 20;
    FakeWorker worker(disc::channel(), size, "My Disc.iso");
    CHECK(disc::available());
    CHECK(disc::name() == "My Disc.iso");
    CHECK(disc::location() == "webdisc://My Disc.iso");
    CHECK(disc::is_location(disc::location()));
    CHECK(!disc::is_location("webdisc://"));
    CHECK(!disc::is_location("/home/user/game.iso"));

    SDL_IOStream* io = disc::open_stream(disc::channel());
    CHECK(io != nullptr);
    CHECK(SDL_GetIOSize(io) == int64_t(size));
    CHECK(SDL_SeekIO(io, 100, SDL_IO_SEEK_SET) == 100);
    std::vector<uint8_t> buf(64);
    CHECK(SDL_ReadIO(io, buf.data(), buf.size()) == buf.size());
    CHECK(matches(buf, 100));
    CHECK(SDL_SeekIO(io, -8, SDL_IO_SEEK_END) == int64_t(size - 8));
    CHECK(SDL_ReadIO(io, buf.data(), buf.size()) == 8);
    CHECK(SDL_GetIOStatus(io) == SDL_IO_STATUS_EOF);
    uint8_t byte = 0;
    CHECK(SDL_WriteIO(io, &byte, 1) == 0);
    CHECK(SDL_CloseIO(io));

    disc::initialize();
    CHECK(borealis::io::check(disc::location()) == borealis::io::Status::Ok);
    auto opened = borealis::io::open(disc::location());
    CHECK(opened.status == borealis::io::Status::Ok);
    CHECK(opened.file.handle() != nullptr);
    if (opened.file.handle() != nullptr) {
        CHECK(SDL_SeekIO(opened.file.handle(), 4096, SDL_IO_SEEK_SET) == 4096);
        CHECK(SDL_ReadIO(opened.file.handle(), buf.data(), buf.size()) == buf.size());
        CHECK(matches(buf, 4096));
    }
    CHECK(borealis::io::open(disc::location(), borealis::io::File::Mode::Truncate).status ==
          borealis::io::Status::Unsupported);
    // Other schemes are still unsupported.
    CHECK(borealis::io::check("other://x") == borealis::io::Status::Unsupported);
}

}  // namespace

int main() {
    test_reads_through_the_channel();
    test_concurrent_readers();
    test_sdl_stream_and_borealis_scheme();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    std::puts("web disc: all tests passed");
    return 0;
}
