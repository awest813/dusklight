// A plain GameCube disc image reader behind the part of nod's C API (include/nod.h) that aurora's
// DVD layer and borealis::disc use. The web build links this instead of nod, which is written in
// Rust and has no threaded wasm build. It reads uncompressed ISO/GCM images only: compressed
// formats (RVZ, WIA, GCZ, CISO, WBFS, ...) and Wii discs, whose partitions are encrypted, are
// rejected with an error message the UI shows.

#include "nod.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr uint64_t kBootSize = 0x440;
constexpr uint64_t kBi2Offset = 0x440;
constexpr uint64_t kBi2Size = 0x2000;
constexpr uint64_t kApploaderOffset = 0x2440;
constexpr uint64_t kApploaderHeaderSize = 0x20;
constexpr uint64_t kDolHeaderSize = 0x100;
constexpr uint32_t kFstEntrySize = 12;

thread_local std::string tLastError;

void set_error(std::string message) { tLastError = std::move(message); }

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// The caller's stream, shared by the disc handle and every handle opened from it. Reads may come
// from several threads (the DVD worker and the game), so they are serialized.
struct Stream {
    NodDiscStream callbacks{};
    uint64_t length = 0;
    std::mutex mutex;

    ~Stream() {
        if (callbacks.close != nullptr) {
            callbacks.close(callbacks.user_data);
        }
    }

    // Reads exactly `len` bytes at `offset`, or fails.
    bool read_exact(uint64_t offset, void* out, size_t len) {
        if (offset > length || len > length - offset) {
            set_error("read past the end of the disc image");
            return false;
        }
        std::lock_guard lock(mutex);
        auto* dst = static_cast<uint8_t*>(out);
        size_t total = 0;
        while (total < len) {
            const int64_t got = callbacks.read_at(callbacks.user_data, offset + total, dst + total, len - total);
            if (got <= 0) {
                set_error("failed to read the disc image");
                return false;
            }
            total += static_cast<size_t>(got);
        }
        return true;
    }
};

struct Partition {
    std::vector<uint8_t> boot;
    std::vector<uint8_t> bi2;
    std::vector<uint8_t> apploader;
    std::vector<uint8_t> dol;
    std::vector<uint8_t> fst;
    uint32_t fstCount = 0;
    uint32_t stringTableOffset = 0;
};

enum class Kind { Disc, Partition, File };

}  // namespace

struct NodHandle {
    Kind kind;
    std::shared_ptr<Stream> stream;
    std::shared_ptr<const Partition> partition;  // Partition and File handles
    NodDiscHeader header{};                       // Disc handles
    // Readable window in the image, and the current position within it.
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t pos = 0;
};

namespace {

bool read_blob(Stream& stream, uint64_t offset, uint64_t size, std::vector<uint8_t>& out) {
    out.resize(static_cast<size_t>(size));
    return stream.read_exact(offset, out.data(), out.size());
}

std::shared_ptr<Partition> load_partition(Stream& stream) {
    auto part = std::make_shared<Partition>();
    if (!read_blob(stream, 0, kBootSize, part->boot) || !read_blob(stream, kBi2Offset, kBi2Size, part->bi2)) {
        return nullptr;
    }

    uint8_t apploaderHeader[kApploaderHeaderSize];
    if (!stream.read_exact(kApploaderOffset, apploaderHeader, sizeof(apploaderHeader))) {
        return nullptr;
    }
    const uint64_t apploaderSize = kApploaderHeaderSize + be32(apploaderHeader + 0x14) + be32(apploaderHeader + 0x18);
    if (!read_blob(stream, kApploaderOffset, apploaderSize, part->apploader)) {
        return nullptr;
    }

    // The DOL's size is the end of its furthest section (7 text + 11 data).
    const uint64_t dolOffset = be32(part->boot.data() + 0x420);
    uint8_t dolHeader[kDolHeaderSize];
    if (!stream.read_exact(dolOffset, dolHeader, sizeof(dolHeader))) {
        return nullptr;
    }
    uint64_t dolSize = kDolHeaderSize;
    for (int i = 0; i < 18; ++i) {
        const uint64_t sectionOffset = be32(dolHeader + i * 4);
        const uint64_t sectionSize = be32(dolHeader + 0x90 + i * 4);
        if (sectionSize != 0) {
            dolSize = std::max(dolSize, sectionOffset + sectionSize);
        }
    }
    if (!read_blob(stream, dolOffset, dolSize, part->dol)) {
        return nullptr;
    }

    const uint64_t fstOffset = be32(part->boot.data() + 0x424);
    const uint64_t fstSize = be32(part->boot.data() + 0x428);
    if (fstSize < kFstEntrySize || !read_blob(stream, fstOffset, fstSize, part->fst)) {
        if (fstSize < kFstEntrySize) {
            set_error("invalid file system table");
        }
        return nullptr;
    }
    // The root entry's length is the total entry count; the name table follows the entries.
    part->fstCount = be32(part->fst.data() + 8);
    const uint64_t entriesSize = uint64_t(part->fstCount) * kFstEntrySize;
    if (part->fstCount == 0 || entriesSize > part->fst.size()) {
        set_error("invalid file system table");
        return nullptr;
    }
    part->stringTableOffset = static_cast<uint32_t>(entriesSize);
    return part;
}

const uint8_t* fst_entry(const Partition& part, uint32_t index) {
    return part.fst.data() + size_t(index) * kFstEntrySize;
}

std::string fst_name(const Partition& part, uint32_t index) {
    const uint64_t offset = part.stringTableOffset + (be32(fst_entry(part, index)) & 0xFFFFFF);
    if (offset >= part.fst.size()) {
        return "<invalid>";
    }
    const char* begin = reinterpret_cast<const char*>(part.fst.data() + offset);
    const size_t maxLen = part.fst.size() - offset;
    return std::string(begin, strnlen(begin, maxLen));
}

NodBlob blob(const std::vector<uint8_t>& data) { return NodBlob{data.empty() ? nullptr : data.data(), data.size()}; }

}  // namespace

extern "C" {

const char* nod_error_message(void) { return tLastError.empty() ? nullptr : tLastError.c_str(); }

NodResult nod_disc_open(const char* path, const NodDiscOptions* options, NodHandle** out) {
    (void)path;
    (void)options;
    (void)out;
    set_error("opening disc images by path is not supported in this build");
    return NOD_RESULT_ERR_OTHER;
}

NodResult nod_disc_open_stream(const NodDiscStream* stream, const NodDiscOptions* options, NodHandle** out) {
    (void)options;
    tLastError.clear();
    if (stream == nullptr || stream->read_at == nullptr || stream->stream_len == nullptr || stream->close == nullptr ||
        out == nullptr)
    {
        set_error("invalid stream");
        return NOD_RESULT_ERR_OTHER;
    }
    *out = nullptr;

    // From here on the stream is owned, and closed with the last handle.
    auto owned = std::make_shared<Stream>();
    owned->callbacks = *stream;
    const int64_t length = stream->stream_len(stream->user_data);
    if (length < 0) {
        set_error("failed to get the disc image size");
        return NOD_RESULT_ERR_IO;
    }
    owned->length = static_cast<uint64_t>(length);

    auto handle = std::make_unique<NodHandle>();
    handle->kind = Kind::Disc;
    if (owned->length < sizeof(NodDiscHeader) || !owned->read_exact(0, &handle->header, sizeof(NodDiscHeader))) {
        set_error("not a GameCube disc image");
        return NOD_RESULT_ERR_FORMAT;
    }
    if (NOD_MAGIC_EQ(handle->header.wii_magic, WII_MAGIC)) {
        set_error("Wii disc images are not supported in this build; use a GameCube disc image");
        return NOD_RESULT_ERR_FORMAT;
    }
    if (!NOD_MAGIC_EQ(handle->header.gcn_magic, GCN_MAGIC)) {
        set_error("not an uncompressed GameCube disc image (ISO/GCM); convert compressed images first");
        return NOD_RESULT_ERR_FORMAT;
    }

    handle->stream = std::move(owned);
    handle->size = handle->stream->length;
    *out = handle.release();
    return NOD_RESULT_OK;
}

NodResult nod_disc_open_partition_kind(NodHandle* disc, uint32_t kind, const NodPartitionOptions* options,
                                       NodHandle** out) {
    (void)options;
    tLastError.clear();
    if (disc == nullptr || disc->kind != Kind::Disc || out == nullptr) {
        set_error("handle is not a disc");
        return NOD_RESULT_ERR_INVALID_HANDLE;
    }
    *out = nullptr;
    // GameCube discs have no partition table: the data "partition" is the whole disc.
    if (kind != NOD_PARTITION_KIND_DATA) {
        set_error("partition not found");
        return NOD_RESULT_ERR_NOT_FOUND;
    }
    auto partition = load_partition(*disc->stream);
    if (partition == nullptr) {
        return NOD_RESULT_ERR_FORMAT;
    }
    auto handle = std::make_unique<NodHandle>();
    handle->kind = Kind::Partition;
    handle->stream = disc->stream;
    handle->partition = std::move(partition);
    handle->size = disc->size;
    *out = handle.release();
    return NOD_RESULT_OK;
}

NodResult nod_disc_open_partition(NodHandle* disc, uint32_t index, const NodPartitionOptions* options,
                                  NodHandle** out) {
    if (index != 0) {
        set_error("partition not found");
        return NOD_RESULT_ERR_NOT_FOUND;
    }
    return nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, options, out);
}

NodResult nod_partition_open_file(NodHandle* partition, uint32_t fst_index, NodHandle** out) {
    tLastError.clear();
    if (partition == nullptr || partition->kind != Kind::Partition || out == nullptr) {
        set_error("handle is not a partition");
        return NOD_RESULT_ERR_INVALID_HANDLE;
    }
    *out = nullptr;
    const Partition& part = *partition->partition;
    if (fst_index == 0 || fst_index >= part.fstCount) {
        set_error("file not found");
        return NOD_RESULT_ERR_NOT_FOUND;
    }
    const uint8_t* entry = fst_entry(part, fst_index);
    if ((entry[0] & 1) != 0) {
        set_error("not a file");
        return NOD_RESULT_ERR_NOT_FOUND;
    }
    const uint64_t offset = be32(entry + 4);
    const uint64_t size = be32(entry + 8);
    if (offset > partition->stream->length || size > partition->stream->length - offset) {
        set_error("file lies outside the disc image");
        return NOD_RESULT_ERR_FORMAT;
    }
    auto handle = std::make_unique<NodHandle>();
    handle->kind = Kind::File;
    handle->stream = partition->stream;
    handle->partition = partition->partition;
    handle->base = offset;
    handle->size = size;
    *out = handle.release();
    return NOD_RESULT_OK;
}

void nod_free(NodHandle* handle) { delete handle; }

int64_t nod_read(NodHandle* handle, uint8_t* buf, size_t len) {
    if (handle == nullptr || (buf == nullptr && len != 0)) {
        set_error("invalid handle");
        return -1;
    }
    if (handle->pos >= handle->size) {
        return 0;
    }
    const size_t n = static_cast<size_t>(std::min<uint64_t>(len, handle->size - handle->pos));
    if (!handle->stream->read_exact(handle->base + handle->pos, buf, n)) {
        return -1;
    }
    handle->pos += n;
    return static_cast<int64_t>(n);
}

int64_t nod_seek(NodHandle* handle, int64_t offset, int32_t whence) {
    if (handle == nullptr) {
        set_error("invalid handle");
        return -1;
    }
    int64_t origin;
    switch (whence) {
    case 0:
        origin = 0;
        break;
    case 1:
        origin = static_cast<int64_t>(handle->pos);
        break;
    case 2:
        origin = static_cast<int64_t>(handle->size);
        break;
    default:
        set_error("invalid whence");
        return -1;
    }
    const int64_t target = origin + offset;
    if (target < 0) {
        set_error("seek before the start");
        return -1;
    }
    handle->pos = static_cast<uint64_t>(target);
    return target;
}

NodResult nod_disc_header(const NodHandle* disc, NodDiscHeader* out) {
    if (disc == nullptr || disc->kind != Kind::Disc || out == nullptr) {
        set_error("handle is not a disc");
        return NOD_RESULT_ERR_INVALID_HANDLE;
    }
    *out = disc->header;
    return NOD_RESULT_OK;
}

NodResult nod_disc_meta(const NodHandle* disc, NodDiscMeta* out) {
    if (disc == nullptr || disc->kind != Kind::Disc || out == nullptr) {
        set_error("handle is not a disc");
        return NOD_RESULT_ERR_INVALID_HANDLE;
    }
    *out = NodDiscMeta{};
    out->format = NOD_FORMAT_ISO;
    out->lossless = true;
    out->disc_size = disc->size;
    return NOD_RESULT_OK;
}

uint64_t nod_disc_size(const NodHandle* disc) {
    return disc != nullptr && disc->kind == Kind::Disc ? disc->size : 0;
}

size_t nod_disc_partitions(const NodHandle* disc, NodPartitionInfo* out, size_t cap) {
    if (disc == nullptr || disc->kind != Kind::Disc) {
        return 0;
    }
    if (out != nullptr && cap > 0) {
        out[0] = NodPartitionInfo{.index = 0, .kind = NOD_PARTITION_KIND_DATA, .data_size = disc->size};
    }
    return 1;
}

bool nod_partition_is_wii(const NodHandle* partition) {
    (void)partition;
    return false;
}

NodResult nod_partition_meta(const NodHandle* partition, NodPartitionMeta* out) {
    if (partition == nullptr || partition->kind != Kind::Partition || out == nullptr) {
        set_error("handle is not a partition");
        return NOD_RESULT_ERR_INVALID_HANDLE;
    }
    const Partition& part = *partition->partition;
    *out = NodPartitionMeta{};
    out->raw_boot = blob(part.boot);
    out->raw_bi2 = blob(part.bi2);
    out->raw_apploader = blob(part.apploader);
    out->raw_dol = blob(part.dol);
    out->raw_fst = blob(part.fst);
    return NOD_RESULT_OK;
}

uint32_t nod_partition_find_file(const NodHandle* partition, const char* path, NodNodeKind* out_kind,
                                 uint32_t* out_length) {
    if (partition == nullptr || partition->kind != Kind::Partition || path == nullptr) {
        set_error("handle is not a partition");
        return NOD_FST_STOP;
    }
    const Partition& part = *partition->partition;
    // Walk the path one component at a time, searching only the current directory's children.
    uint32_t dirEnd = part.fstCount;
    uint32_t index = 0;
    std::string_view rest(path);
    while (!rest.empty()) {
        while (!rest.empty() && rest.front() == '/') {
            rest.remove_prefix(1);
        }
        if (rest.empty()) {
            break;
        }
        const size_t slash = rest.find('/');
        const std::string_view component = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);

        uint32_t found = NOD_FST_STOP;
        for (uint32_t i = index + 1; i < dirEnd;) {
            const uint8_t* entry = fst_entry(part, i);
            const bool isDir = (entry[0] & 1) != 0;
            const std::string name = fst_name(part, i);
            if (name.size() == component.size() &&
                std::equal(name.begin(), name.end(), component.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                }))
            {
                found = i;
                break;
            }
            i = isDir ? std::max(i + 1, be32(entry + 8)) : i + 1;
        }
        if (found == NOD_FST_STOP) {
            return NOD_FST_STOP;
        }
        index = found;
        const uint8_t* entry = fst_entry(part, index);
        if ((entry[0] & 1) != 0) {
            dirEnd = std::min(be32(entry + 8), part.fstCount);
        } else if (!rest.empty()) {
            return NOD_FST_STOP;
        }
    }
    const uint8_t* entry = fst_entry(part, index);
    if (out_kind != nullptr) {
        *out_kind = (entry[0] & 1) != 0 ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE;
    }
    if (out_length != nullptr) {
        *out_length = be32(entry + 8);
    }
    return index;
}

void nod_partition_iterate_fst(const NodHandle* partition, NodFstCallback callback, void* user_data) {
    if (partition == nullptr || partition->kind != Kind::Partition || callback == nullptr) {
        set_error("handle is not a partition");
        return;
    }
    const Partition& part = *partition->partition;
    // Like nod: skip the root, pass a directory's end index as its size, and continue from whatever
    // index the callback returns.
    uint32_t index = 1;
    while (index < part.fstCount) {
        const uint8_t* entry = fst_entry(part, index);
        const NodNodeKind kind = (entry[0] & 1) != 0 ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE;
        const std::string name = fst_name(part, index);
        index = callback(index, kind, name.c_str(), be32(entry + 8), user_data);
    }
}

}  // extern "C"
