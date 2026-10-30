// Checks the filesystem operations that Dusklight's saves and settings rely on, against WasmFS's
// OPFS backend mounted the way the web build mounts it (src/dusk/web/platform.cpp). run.mjs runs
// it in two pages of one browser profile: "write" makes the changes, then "verify", in a new page
// as after a reload, checks that they persisted.
//
// The operations are the ones the game's writers use, through SDL (a thin layer over stdio and
// POSIX on Emscripten), std::filesystem and std::ofstream:
// - aurora::io::open_atomic_file: exclusive create ("wb+x"), write, flush, rename over the target;
// - aurora's memory card: a 16 MiB raw card image, updated at an offset;
// - dusk::config: write a temporary file with std::ofstream, rename it over config.json;
// - dusk::save_manager: replace a directory of mod save data, remove_all. WasmFS cannot move
//   directories on OPFS, so save_manager.cpp's rename_directory moves their files instead; this
//   checks both that limitation (to notice when it goes away) and the workaround.
#include <emscripten/wasmfs.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

std::vector<uint8_t> pattern(size_t size, uint8_t seed) {
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>((i * 31 + seed) ^ (i >> 12));
    }
    return data;
}

bool write_to(FILE* file, const std::vector<uint8_t>& data) {
    return std::fwrite(data.data(), 1, data.size(), file) == data.size() && std::fflush(file) == 0;
}

std::vector<uint8_t> read_all(const fs::path& path) {
    std::vector<uint8_t> data;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return data;
    }
    std::vector<uint8_t> buffer(65536); // not on the stack: Emscripten's default stack is 64 KiB
    size_t n;
    while ((n = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
        data.insert(data.end(), buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(n));
    }
    std::fclose(file);
    return data;
}

// The web branch of rename_directory in src/dusk/save_manager.cpp.
void rename_directory(const fs::path& from, const fs::path& to) {
    struct Entry {
        fs::path name;
        bool isDirectory;
    };
    std::vector<Entry> entries;
    for (const auto& entry : fs::directory_iterator(from)) {
        entries.push_back({entry.path().filename(), entry.is_directory()});
    }
    const auto move = [](const Entry& entry, const fs::path& source, const fs::path& target) {
        if (entry.isDirectory) {
            rename_directory(source / entry.name, target / entry.name);
        } else {
            fs::rename(source / entry.name, target / entry.name);
        }
    };
    fs::create_directory(to);
    size_t moved = 0;
    try {
        for (; moved < entries.size(); ++moved) {
            move(entries[moved], from, to);
        }
    } catch (...) {
        while (moved > 0) {
            try {
                move(entries[--moved], to, from);
            } catch (...) {
                // Keep undoing the others.
            }
        }
        std::error_code ec;
        fs::remove(to, ec);
        throw;
    }
    fs::remove(from);
}

bool write_text(const fs::path& path, const std::string& text) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << text;
    out.close();
    return out.good();
}

std::string read_text(const fs::path& path) {
    const auto data = read_all(path);
    return {data.begin(), data.end()};
}

// The card image after the "write" phase: the second full image, with one block rewritten.
constexpr size_t CardSize = 16 * 1024 * 1024;
constexpr size_t BlockOffset = 5 * 8192;

std::vector<uint8_t> expected_card() {
    auto card = pattern(CardSize, 2);
    const auto block = pattern(8192, 3);
    std::memcpy(card.data() + BlockOffset, block.data(), block.size());
    return card;
}

void write_phase(const fs::path& root) {
    std::error_code ec;
    fs::remove_all(root, ec);
    check(fs::create_directories(root / "card", ec) && !ec, "create_directories");

    // A new file, created exclusively as open_atomic_file does, then renamed into place.
    const fs::path card = root / "card" / "MemoryCardA.raw";
    const fs::path temporary = card.string() + ".tmp-1";
    FILE* file = std::fopen(temporary.c_str(), "wb+x");
    check(file != nullptr, "exclusive create");
    FILE* again = std::fopen(temporary.c_str(), "wb+x");
    check(again == nullptr && errno == EEXIST, "exclusive create refuses an existing file");
    if (again != nullptr) {
        std::fclose(again);
    }
    check(file != nullptr && write_to(file, pattern(CardSize, 1)), "write and flush 16 MiB");
    check(file != nullptr && std::fclose(file) == 0, "close");
    check(std::rename(temporary.c_str(), card.c_str()) == 0, "rename to a new name");

    // Replacing an existing file the same way.
    const fs::path replacement = card.string() + ".tmp-2";
    file = std::fopen(replacement.c_str(), "wb+x");
    check(file != nullptr && write_to(file, pattern(CardSize, 2)) && std::fclose(file) == 0,
          "write the replacement");
    check(std::rename(replacement.c_str(), card.c_str()) == 0, "rename over an existing file");
    check(!fs::exists(replacement) && !fs::exists(temporary), "no temporary files left");

    // Updating part of it in place.
    file = std::fopen(card.c_str(), "r+b");
    check(file != nullptr && std::fseek(file, BlockOffset, SEEK_SET) == 0 && write_to(file, pattern(8192, 3)) &&
              std::fclose(file) == 0,
          "update a block in place");
    check(fs::file_size(card, ec) == CardSize && !ec, "file_size");
    check(read_all(card) == expected_card(), "read back the card image");

    // Settings: std::ofstream to a temporary name, renamed over config.json.
    const fs::path config = root / "config.json";
    const fs::path configTemporary = root / ".config.json.tmp";
    check(write_text(configTemporary, "{\"version\":1}") && (fs::rename(configTemporary, config, ec), !ec),
          "write config.json");
    check(write_text(configTemporary, "{\"version\":2}") && (fs::rename(configTemporary, config, ec), !ec),
          "replace config.json");

    // Mod save data: a staged directory replaces the current one, which is kept until then.
    const fs::path mods = root / "mods" / "example";
    const fs::path staging = root / "mods" / "example.staging";
    const fs::path previous = root / "mods" / "example.staging.old";
    fs::create_directories(mods, ec);
    check(write_text(mods / "a.json", "old a") && write_text(mods / "b.json", "old b"), "write mod data");
    fs::create_directories(staging, ec);
    check(write_text(staging / "a.json", "new a") && write_text(staging / "c.json", "new c"), "stage mod data");
    fs::rename(mods, previous, ec);
    check(ec == std::errc::device_or_resource_busy, "WasmFS still cannot rename directories on OPFS");
    try {
        rename_directory(mods, previous);
        rename_directory(staging, mods);
        check(true, "move directories file by file");
    } catch (const fs::filesystem_error& error) {
        std::printf("%s\n", error.what());
        check(false, "move directories file by file");
    }
    fs::remove_all(previous, ec);
    check(!ec && !fs::exists(previous), "remove_all");

    // A move that fails partway (here, a file held open, which OPFS won't move) is undone.
    {
        const fs::path held = root / "held";
        fs::create_directories(held / "nested", ec);
        check(write_text(held / "a", "a") && write_text(held / "nested" / "b", "b") && write_text(held / "z", "z"),
              "write a directory to move");
        FILE* open = std::fopen((held / "z").c_str(), "r+b");
        bool threw = false;
        try {
            rename_directory(held, root / "held.moved");
        } catch (const fs::filesystem_error&) {
            threw = true;
        }
        if (open != nullptr) {
            std::fclose(open);
        }
        if (threw) {
            check(read_text(held / "a") == "a" && read_text(held / "nested" / "b") == "b" &&
                      read_text(held / "z") == "z" && !fs::exists(root / "held.moved"),
                  "a failed directory move leaves the directory as it was");
        } else {
            std::printf("note: OPFS moved a file held open; the undo path was not exercised\n");
            fs::remove_all(root / "held.moved", ec);
        }
        fs::remove_all(held, ec);
    }

    // Something to delete in the next phase.
    fs::create_directories(root / "doomed" / "nested", ec);
    check(write_text(root / "doomed" / "nested" / "file", "x"), "write a nested file");
}

void verify_phase(const fs::path& root) {
    std::error_code ec;
    const fs::path card = root / "card" / "MemoryCardA.raw";
    check(read_all(card) == expected_card(), "the card image persisted");
    check(read_text(root / "config.json") == "{\"version\":2}", "config.json persisted");
    check(!fs::exists(root / ".config.json.tmp"), "no temporary config left");
    const fs::path mods = root / "mods" / "example";
    check(read_text(mods / "a.json") == "new a" && read_text(mods / "c.json") == "new c" &&
              !fs::exists(mods / "b.json"),
          "the replaced mod data persisted");
    check(!fs::exists(root / "mods" / "example.staging") && !fs::exists(root / "mods" / "example.staging.old"),
          "no staging directories left");
    size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        (void)entry;
        ++entries;
    }
    check(!ec && entries == 4, "directory listing");
    fs::remove_all(root / "doomed", ec);
    check(!ec && !fs::exists(root / "doomed"), "remove_all of a persisted tree");
    fs::remove_all(root, ec);
    check(!ec && !fs::exists(root), "clean up");
}

} // namespace

int main(int argc, char** argv) {
    backend_t opfs = wasmfs_create_opfs_backend();
    if (wasmfs_create_directory("/libsdl", 0777, opfs) != 0) {
        std::printf("FAIL: mount OPFS\n");
        return 1;
    }
    const std::string phase = argc > 1 ? argv[1] : "";
    const fs::path root = "/libsdl/web-storage-test";
    if (phase == "write") {
        write_phase(root);
    } else if (phase == "verify") {
        verify_phase(root);
    } else {
        std::printf("FAIL: unknown phase '%s'\n", phase.c_str());
        return 1;
    }
    return g_failures == 0 ? 0 : 1;
}
