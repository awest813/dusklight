// Tests nod_iso.cpp against a synthetic GameCube disc image built in memory (no game data).
//
//   c++ -std=c++20 -Iinclude nod_iso.cpp nod_iso_test.cpp -o nod_iso_test && ./nod_iso_test

#include "nod.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++gFailures;                                                                           \
        }                                                                                          \
    } while (0)

void put32(std::vector<uint8_t>& image, size_t offset, uint32_t value) {
    image[offset + 0] = uint8_t(value >> 24);
    image[offset + 1] = uint8_t(value >> 16);
    image[offset + 2] = uint8_t(value >> 8);
    image[offset + 3] = uint8_t(value);
}

struct FstNode {
    std::string name;
    bool dir;
    std::string contents;  // files
    std::vector<FstNode> children;  // directories
};

// Layout: header, bi2, apploader at 0x2440, DOL at 0x3000, FST at 0x4000, file data from 0x8000.
std::vector<uint8_t> build_image(const FstNode& root, bool wii = false, bool gcMagic = true) {
    std::vector<uint8_t> image(0x10000, 0);
    std::memcpy(image.data(), "GZ2E01", 6);
    image[6] = 0;  // disc number
    image[7] = 2;  // version
    if (gcMagic) {
        const uint8_t magic[4] = {0xC2, 0x33, 0x9F, 0x3D};
        std::memcpy(image.data() + 0x1C, magic, 4);
    }
    if (wii) {
        const uint8_t magic[4] = {0x5D, 0x1C, 0x9E, 0xA3};
        std::memcpy(image.data() + 0x18, magic, 4);
    }
    std::memcpy(image.data() + 0x20, "Synthetic Disc", 14);

    // Apploader: 0x20 header, code size 0x40, trailer size 0x10.
    put32(image, 0x2440 + 0x14, 0x40);
    put32(image, 0x2440 + 0x18, 0x10);

    // DOL with one text section (0x100..0x180) and one data section (0x180..0x200).
    const uint32_t dolOffset = 0x3000;
    put32(image, 0x420, dolOffset);
    put32(image, dolOffset + 0x00, 0x100);
    put32(image, dolOffset + 0x90, 0x80);
    put32(image, dolOffset + 0x1C, 0x180);
    put32(image, dolOffset + 0xAC, 0x80);
    image[dolOffset + 0x1FF] = 0xAB;

    // Flatten the tree into FST entries + string table, assigning file data offsets.
    std::vector<std::vector<uint8_t>> entries;
    std::string strings;
    uint32_t dataOffset = 0x8000;
    struct Flatten {
        std::vector<std::vector<uint8_t>>& entries;
        std::string& strings;
        uint32_t& dataOffset;
        std::vector<uint8_t>& image;
        void node(const FstNode& n, uint32_t parent) {
            const uint32_t index = uint32_t(entries.size());
            entries.emplace_back(12, 0);
            const uint32_t nameOffset = uint32_t(strings.size());
            strings += n.name;
            strings += '\0';
            auto& e = entries.back();
            e[0] = n.dir ? 1 : 0;
            e[1] = uint8_t(nameOffset >> 16);
            e[2] = uint8_t(nameOffset >> 8);
            e[3] = uint8_t(nameOffset);
            auto set = [&](size_t at, uint32_t v) {
                auto& entry = entries[index];
                entry[at] = uint8_t(v >> 24);
                entry[at + 1] = uint8_t(v >> 16);
                entry[at + 2] = uint8_t(v >> 8);
                entry[at + 3] = uint8_t(v);
            };
            if (n.dir) {
                set(4, parent);
                for (const auto& child : n.children) {
                    node(child, index);
                }
                set(8, uint32_t(entries.size()));
            } else {
                set(4, dataOffset);
                set(8, uint32_t(n.contents.size()));
                std::memcpy(image.data() + dataOffset, n.contents.data(), n.contents.size());
                dataOffset += (uint32_t(n.contents.size()) + 31) & ~31u;
            }
        }
    } flatten{entries, strings, dataOffset, image};
    // The root's name offset is 0 and its "next" is the entry count.
    flatten.node(root, 0);

    const uint32_t fstOffset = 0x4000;
    std::vector<uint8_t> fst;
    for (const auto& e : entries) {
        fst.insert(fst.end(), e.begin(), e.end());
    }
    fst.insert(fst.end(), strings.begin(), strings.end());
    std::memcpy(image.data() + fstOffset, fst.data(), fst.size());
    put32(image, 0x424, fstOffset);
    put32(image, 0x428, uint32_t(fst.size()));
    return image;
}

struct Memory {
    std::vector<uint8_t> data;
    bool closed = false;
};

NodDiscStream stream_for(Memory& memory) {
    return NodDiscStream{
        .user_data = &memory,
        .read_at = [](void* user, uint64_t offset, void* out, size_t len) -> int64_t {
            auto& m = *static_cast<Memory*>(user);
            if (offset >= m.data.size()) {
                return 0;
            }
            // Short reads, to exercise the caller's loop.
            const size_t n = std::min<size_t>({len, size_t(m.data.size() - offset), size_t(0x1234)});
            std::memcpy(out, m.data.data() + offset, n);
            return int64_t(n);
        },
        .stream_len = [](void* user) -> int64_t { return int64_t(static_cast<Memory*>(user)->data.size()); },
        .close = [](void* user) { static_cast<Memory*>(user)->closed = true; },
    };
}

std::string read_all(NodHandle* file) {
    std::string out;
    uint8_t buf[7];
    int64_t n;
    while ((n = nod_read(file, buf, sizeof(buf))) > 0) {
        out.append(reinterpret_cast<char*>(buf), size_t(n));
    }
    CHECK(n == 0);
    return out;
}

void test_reads_a_gamecube_image() {
    const FstNode root{"", true, {}, {
        {"audio", true, {}, {{"bgm.aw", false, "music!", {}}, {"se.aw", false, "effects", {}}}},
        {"res", true, {}, {{"Stage", true, {}, {{"F_SP103.arc", false, std::string(300, 'x'), {}}}}}},
        {"opening.bnr", false, "banner", {}},
    }};
    Memory memory{build_image(root)};
    NodDiscStream stream = stream_for(memory);

    NodHandle* disc = nullptr;
    CHECK(nod_disc_open_stream(&stream, nullptr, &disc) == NOD_RESULT_OK);
    CHECK(disc != nullptr);

    NodDiscHeader header{};
    CHECK(nod_disc_header(disc, &header) == NOD_RESULT_OK);
    CHECK(std::memcmp(header.game_id, "GZ2E01", 6) == 0);
    CHECK(header.disc_version == 2);
    CHECK(std::memcmp(header.game_title, "Synthetic Disc", 14) == 0);
    CHECK(nod_disc_size(disc) == memory.data.size());

    // Whole-disc reads, as borealis::disc hashes the image.
    CHECK(nod_seek(disc, 0, 0) == 0);
    std::vector<uint8_t> whole(memory.data.size());
    size_t got = 0;
    int64_t n;
    while ((n = nod_read(disc, whole.data() + got, whole.size() - got)) > 0) {
        got += size_t(n);
    }
    CHECK(got == memory.data.size());
    CHECK(whole == memory.data);

    NodHandle* partition = nullptr;
    CHECK(nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, nullptr, &partition) == NOD_RESULT_OK);
    NodHandle* missing = nullptr;
    CHECK(nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_UPDATE, nullptr, &missing) ==
          NOD_RESULT_ERR_NOT_FOUND);

    NodPartitionMeta meta{};
    CHECK(nod_partition_meta(partition, &meta) == NOD_RESULT_OK);
    CHECK(meta.raw_boot.size == 0x440);
    CHECK(meta.raw_bi2.size == 0x2000);
    CHECK(meta.raw_apploader.size == 0x20 + 0x40 + 0x10);
    CHECK(meta.raw_dol.size == 0x200);
    CHECK(meta.raw_dol.data[0x1FF] == 0xAB);
    CHECK(meta.raw_fst.size > 0);

    // Iterate like aurora's FST builder: every node, in order, directories reporting their end.
    struct Seen {
        std::vector<std::string> names;
        std::map<std::string, uint32_t> sizes;
        std::map<std::string, uint32_t> indices;
    } seen;
    nod_partition_iterate_fst(
        partition,
        [](uint32_t index, NodNodeKind kind, const char* name, uint32_t size, void* user) -> uint32_t {
            auto& s = *static_cast<Seen*>(user);
            s.names.push_back(std::string(kind == NOD_NODE_KIND_DIRECTORY ? "d:" : "f:") + name);
            s.sizes[name] = size;
            s.indices[name] = index;
            return index + 1;
        },
        &seen);
    const std::vector<std::string> expected{"d:audio", "f:bgm.aw", "f:se.aw", "d:res", "d:Stage", "f:F_SP103.arc",
                                            "f:opening.bnr"};
    CHECK(seen.names == expected);
    CHECK(seen.sizes["audio"] == 4);  // end index of the "audio" subtree
    CHECK(seen.sizes["bgm.aw"] == 6);

    // Returning a directory's size skips its subtree.
    std::vector<std::string> skipped;
    nod_partition_iterate_fst(
        partition,
        [](uint32_t index, NodNodeKind kind, const char* name, uint32_t size, void* user) -> uint32_t {
            static_cast<std::vector<std::string>*>(user)->push_back(name);
            return kind == NOD_NODE_KIND_DIRECTORY ? size : index + 1;
        },
        &skipped);
    CHECK((skipped == std::vector<std::string>{"audio", "res", "opening.bnr"}));

    NodHandle* file = nullptr;
    CHECK(nod_partition_open_file(partition, seen.indices["F_SP103.arc"], &file) == NOD_RESULT_OK);
    CHECK(read_all(file) == std::string(300, 'x'));
    CHECK(nod_seek(file, -5, 2) == 295);
    uint8_t tail[16];
    CHECK(nod_read(file, tail, sizeof(tail)) == 5);
    CHECK(nod_seek(file, 10, 0) == 10);
    CHECK(nod_seek(file, 5, 1) == 15);
    CHECK(nod_seek(file, -100, 1) == -1);
    nod_free(file);

    NodHandle* dir = nullptr;
    CHECK(nod_partition_open_file(partition, seen.indices["audio"], &dir) == NOD_RESULT_ERR_NOT_FOUND);
    CHECK(nod_error_message() != nullptr);

    NodNodeKind kind{};
    uint32_t length = 0;
    CHECK(nod_partition_find_file(partition, "/res/stage/f_sp103.arc", &kind, &length) == seen.indices["F_SP103.arc"]);
    CHECK(kind == NOD_NODE_KIND_FILE && length == 300);
    CHECK(nod_partition_find_file(partition, "audio", &kind, &length) == seen.indices["audio"]);
    CHECK(kind == NOD_NODE_KIND_DIRECTORY);
    CHECK(nod_partition_find_file(partition, "/res/missing", nullptr, nullptr) == NOD_FST_STOP);
    CHECK(nod_partition_find_file(partition, "/opening.bnr/x", nullptr, nullptr) == NOD_FST_STOP);
    // A name that only exists in another directory is not found at the top level.
    CHECK(nod_partition_find_file(partition, "/bgm.aw", nullptr, nullptr) == NOD_FST_STOP);

    // Handles share the stream, which closes with the last one.
    nod_free(disc);
    CHECK(!memory.closed);
    nod_free(partition);
    CHECK(memory.closed);
}

void test_rejects_other_images() {
    const FstNode root{"", true, {}, {{"a", false, "a", {}}}};
    {
        Memory memory{build_image(root, /*wii=*/true, /*gcMagic=*/false)};
        NodDiscStream stream = stream_for(memory);
        NodHandle* disc = nullptr;
        CHECK(nod_disc_open_stream(&stream, nullptr, &disc) == NOD_RESULT_ERR_FORMAT);
        CHECK(disc == nullptr);
        CHECK(std::strstr(nod_error_message(), "Wii") != nullptr);
        CHECK(memory.closed);
    }
    {
        Memory memory{build_image(root, /*wii=*/false, /*gcMagic=*/false)};
        NodDiscStream stream = stream_for(memory);
        NodHandle* disc = nullptr;
        CHECK(nod_disc_open_stream(&stream, nullptr, &disc) == NOD_RESULT_ERR_FORMAT);
        CHECK(std::strstr(nod_error_message(), "convert") != nullptr);
    }
    {
        Memory memory{std::vector<uint8_t>(100, 0)};
        NodDiscStream stream = stream_for(memory);
        NodHandle* disc = nullptr;
        CHECK(nod_disc_open_stream(&stream, nullptr, &disc) == NOD_RESULT_ERR_FORMAT);
    }
}

}  // namespace

int main() {
    test_reads_a_gamecube_image();
    test_rejects_other_images();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    std::puts("nod_iso: all tests passed");
    return 0;
}
