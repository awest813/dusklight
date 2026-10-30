// Drives borealis::file_select's browser backend (extern/borealis/src/file_select/web.cpp), which
// the game's import and export buttons use. run.mjs runs it in headless Chromium:
//   "open"    (started by a click): choose a file, read it back, export it again as a download;
//   "refused" (started without one): the browser refuses to open its chooser, which must fail
//             cleanly rather than hang.
#include <borealis/file_select.hpp>
#include <borealis/io.hpp>

#include <SDL3/SDL_init.h>

#include <cstdio>
#include <string>

using namespace borealis::file_select;

namespace {

const char* status_name(Status status) {
    switch (status) {
    case Status::Selected:
        return "Selected";
    case Status::Canceled:
        return "Canceled";
    case Status::Unsupported:
        return "Unsupported";
    case Status::Busy:
        return "Busy";
    case Status::Failed:
        return "Failed";
    }
    return "?";
}

std::string read_text(const std::string& location) {
    auto input = borealis::io::open(location, borealis::io::File::Mode::Read);
    if (input.status != borealis::io::Status::Ok) {
        return "<unreadable>";
    }
    std::string text(256, '\0');
    text.resize(input.file.read(text.data(), text.size()));
    return text;
}

} // namespace

int main(int argc, char** argv) {
    SDL_Init(0);
    const std::string phase = argc > 1 ? argv[1] : "";
    const auto capabilities = borealis::file_select::capabilities();
    std::printf("capabilities: open=%d folder=%d export=%d\n", capabilities.canOpenFile,
                capabilities.canOpenFolder, capabilities.canExportFile);

    std::string location;
    open_file({.filters = {{"Save files", "gci;raw"}}}, [&](Result result) {
        std::printf("open: %s %zu\n", status_name(result.status), result.locations.size());
        if (!result.locations.empty()) {
            location = result.locations.front();
            std::printf("content: %s\n", read_text(location).c_str());
        }
    });
    std::printf("busy after open: %d\n", busy());
    if (phase == "open" && !location.empty()) {
        export_file({.sourceLocation = location, .suggestedName = "exported.gci"},
                    [](Result result) { std::printf("export: %s\n", status_name(result.status)); });
    }
    std::fflush(stdout);
    return 0;
}
