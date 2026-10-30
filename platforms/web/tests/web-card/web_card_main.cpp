// Saves and loads through aurora's memory card (the CARD API the game's memory card thread uses,
// m_Do_MemCard*.cpp) on WasmFS's OPFS backend, the way the web build stores saves. run.mjs runs
// it in headless Chromium, once per card type: "write" creates and writes a save the way the game
// does, then "verify", in a new page as after a reload, reads it back and deletes it.
#include <aurora/aurora.h>
#include <dolphin/types.h>
#include <dolphin/card.h>
#include <emscripten/wasmfs.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Normally set up by aurora_initialize (lib/aurora.cpp), which this test does without.
namespace aurora {
AuroraConfig g_config{};
char g_gameName[4]{};
} // namespace aurora

namespace {

constexpr s32 Channel = 0;
constexpr const char* FileName = "gczelda2";
constexpr u32 FileSize = 0x8000;  // CARD_FILE_SIZE: the header sectors and two save sectors
constexpr u32 SectorSize = 0x2000;
constexpr u32 SavesOffset = 0x4000; // the saves, then their backup at 0x6000
int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) {
        ++g_failures;
    }
}

std::vector<u8> pattern(size_t size, u8 seed) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<u8>(i * 7 + seed);
    }
    return data;
}

double milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// What mDoMemCdRWm_Store does: the header and banner, then the saves and their backup, each read
// back to check it.
void save(CARDFileInfo& file, u8 seed) {
    const auto start = std::chrono::steady_clock::now();
    auto header = pattern(SavesOffset, seed);
    check(CARDWrite(&file, header.data(), header.size(), 0) == CARD_RESULT_READY, "write the header");
    for (u32 offset : {SavesOffset, SavesOffset + SectorSize}) {
        auto sector = pattern(SectorSize, static_cast<u8>(seed + offset / SectorSize));
        std::vector<u8> back(SectorSize);
        check(CARDWrite(&file, sector.data(), SectorSize, offset) == CARD_RESULT_READY &&
                  CARDRead(&file, back.data(), SectorSize, offset) == CARD_RESULT_READY && back == sector,
              "write and read back a save sector");
    }
    std::printf("save took %.0f ms\n", milliseconds_since(start));
}

bool saved(CARDFileInfo& file, u8 seed) {
    std::vector<u8> data(FileSize);
    if (CARDRead(&file, data.data(), FileSize, 0) != CARD_RESULT_READY) {
        return false;
    }
    auto expected = pattern(SavesOffset, seed);
    for (u32 offset : {SavesOffset, SavesOffset + SectorSize}) {
        auto sector = pattern(SectorSize, static_cast<u8>(seed + offset / SectorSize));
        expected.insert(expected.end(), sector.begin(), sector.end());
    }
    return data == expected;
}

} // namespace

int main(int argc, char** argv) {
    backend_t opfs = wasmfs_create_opfs_backend();
    if (wasmfs_create_directory("/libsdl", 0777, opfs) != 0) {
        std::printf("FAIL: mount OPFS\n");
        return 1;
    }
    const std::string type = argc > 1 ? argv[1] : "";
    const std::string phase = argc > 2 ? argv[2] : "";
    const std::string root = "/libsdl/web-card-test-" + type;
    aurora::g_config.userPath = root.c_str();
    CARDSetLoadType(type == "raw" ? CARD_RAWIMAGE : CARD_GCIFOLDER);

    const auto start = std::chrono::steady_clock::now();
    CARDInit("GZ2E", "01");
    static u8 workArea[CARD_WORKAREA_SIZE];
    check(CARDMount(Channel, workArea, nullptr) == CARD_RESULT_READY, "mount the card");
    std::printf("mount took %.0f ms\n", milliseconds_since(start));

    CARDFileInfo file{};
    if (phase == "write") {
        check(CARDCreate(Channel, FileName, FileSize, &file) == CARD_RESULT_READY, "create the save file");
        save(file, 1);
        check(CARDClose(&file) == CARD_RESULT_READY, "close it");
        // Saving again over it, as every later save does.
        check(CARDOpen(Channel, FileName, &file) == CARD_RESULT_READY, "open it again");
        save(file, 2);
        check(saved(file, 2), "the second save replaced the first");
        CARDClose(&file);
    } else if (phase == "verify") {
        check(CARDOpen(Channel, FileName, &file) == CARD_RESULT_READY, "the save is there after a reload");
        check(saved(file, 2), "and holds what was saved");
        CARDClose(&file);
        check(CARDDelete(Channel, FileName) == CARD_RESULT_READY, "delete it");
        check(CARDOpen(Channel, FileName, &file) == CARD_RESULT_NOFILE, "it is gone");
    } else {
        std::printf("FAIL: unknown phase '%s'\n", phase.c_str());
        return 1;
    }
    CARDUnmount(Channel);
    return g_failures == 0 ? 0 : 1;
}
