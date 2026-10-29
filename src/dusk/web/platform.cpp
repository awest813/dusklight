#include "platform.hpp"

#include "disc.hpp"

#include <emscripten/emscripten.h>
#include <emscripten/wasmfs.h>

#include <cstdio>

namespace dusk::web {

void initialize() {
    // SDL's preference path on Emscripten is under /libsdl, and aurora and borealis keep config,
    // saves and caches there. Backing it with the origin's private file system (OPFS) keeps them
    // across visits. This must happen before anything asks SDL for that path.
    backend_t opfs = wasmfs_create_opfs_backend();
    if (wasmfs_create_directory("/libsdl", 0777, opfs) != 0) {
        std::fprintf(stderr, "web: failed to mount persistent storage; settings and saves will not be kept\n");
    }

    disc::initialize();
}

void reload() { emscripten_run_script("location.reload()"); }

}  // namespace dusk::web
