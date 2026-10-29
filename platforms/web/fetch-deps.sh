#!/usr/bin/env bash
# Clones the web build's dependencies that are normally downloaded as GitHub source archives (or
# from files.twilitrealm.dev), at the versions the build pins, into a directory to pass as
# DUSK_WEB_DEPS_DIR (platforms/web/cmake/WebOptions.cmake). Only needed where those archives
# can't be downloaded; git checkouts of the same tags are used instead.
#
#   platforms/web/fetch-deps.sh <dir>
#   cmake --preset web -DDUSK_WEB_DEPS_DIR=<dir>
#
# Keep the tags in step with CMakeLists.txt, extern/aurora/extern/CMakeLists.txt,
# extern/aurora/cmake/AuroraDependencyVersions.cmake and extern/borealis/CMakeLists.txt.
set -euo pipefail
dir="${1:?usage: fetch-deps.sh <dir>}"
mkdir -p "$dir"
clone() {
    local name="$1" url="$2" ref="$3"
    if [[ -d "$dir/$name/.git" ]]; then
        echo "fetch-deps: $name already present"
        return
    fi
    if git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$ref" "$url" "$dir/$name" 2>/dev/null; then
        :
    else
        # A commit rather than a tag or branch.
        git init --quiet "$dir/$name"
        git -C "$dir/$name" fetch --quiet --depth 1 "$url" "$ref"
        git -C "$dir/$name" -c advice.detachedHead=false checkout --quiet FETCH_HEAD
    fi
    echo "fetch-deps: $name $ref"
}
clone abseil-cpp https://github.com/abseil/abseil-cpp.git     20240722.0
clone xxhash     https://github.com/Cyan4973/xxHash.git       v0.8.3
clone fmt        https://github.com/fmtlib/fmt.git            12.1.0
clone zlib-ng    https://github.com/zlib-ng/zlib-ng.git       2.3.3
clone libpng     https://github.com/pnggroup/libpng.git       v1.6.58
clone freetype   https://github.com/freetype/freetype.git     VER-2-14-3
clone imgui      https://github.com/ocornut/imgui.git         v1.91.9b-docking
clone rmlui      https://github.com/encounter/RmlUi.git       f00a0fee38391b2f927114e11bea18dc0a7dba1e
clone tracy      https://github.com/wolfpld/tracy.git         v0.14.1
clone sdl3       https://github.com/libsdl-org/SDL.git        release-3.4.10
clone picosha2   https://github.com/okdshin/PicoSHA2.git      v1.0.1
clone cxxopts    https://github.com/jarro2783/cxxopts.git     v3.3.1
