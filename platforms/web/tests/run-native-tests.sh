#!/usr/bin/env bash
# Builds and runs the web port's host-side unit tests natively: the ISO reader (nod_iso), the web
# audio ring and mixer, and the web disc channel. They need only a C++20 compiler; the disc
# channel test also needs SDL3, either built already or as a source tree to build here:
#
#   platforms/web/tests/run-native-tests.sh [<SDL3 include dir> <libSDL3.a> | <SDL3 source dir>]
#
# Without SDL3, the disc channel test is skipped. It also needs the borealis web patch, which is
# applied first (platforms/web/apply-patches.sh).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cxx="${CXX:-c++}"
flags=(-std=c++20 -O1 -g -Wall -Wextra -pthread)
if [[ "${SANITIZE:-1}" == 1 ]]; then flags+=(-fsanitize=address,undefined); fi

"$cxx" "${flags[@]}" -I"$root/platforms/web/nod_iso/include" \
    "$root/platforms/web/nod_iso/nod_iso.cpp" "$root/platforms/web/nod_iso/nod_iso_test.cpp" -o "$out/nod_iso_test"
"$out/nod_iso_test"

"$cxx" "${flags[@]}" -I"$root/src" "$root/src/dusk/web/audio.cpp" "$root/platforms/web/tests/web_audio_test.cpp" \
    -o "$out/web_audio_test"
"$out/web_audio_test"

sdl_include="" sdl_lib=""
if [[ $# -ge 2 ]]; then
    sdl_include="$1" sdl_lib="$2"
elif [[ $# -eq 1 ]]; then
    # A headless static SDL3 is all the test needs.
    cmake -S "$1" -B "$out/sdl" -DCMAKE_BUILD_TYPE=Release -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_SHARED=OFF \
        -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_VIDEO=OFF \
        -DSDL_AUDIO=OFF -DSDL_RENDER=OFF -DSDL_GPU=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF \
        -DSDL_SENSOR=OFF -DSDL_CAMERA=OFF -DSDL_DIALOG=OFF -DSDL_HIDAPI=OFF -DSDL_POWER=OFF \
        -DSDL_TRAY=OFF > "$out/sdl-configure.log"
    cmake --build "$out/sdl" --parallel > "$out/sdl-build.log"
    sdl_include="$1/include" sdl_lib="$out/sdl/libSDL3.a"
fi

if [[ -n "$sdl_lib" ]]; then
    "$root/platforms/web/apply-patches.sh"
    "$cxx" "${flags[@]}" -I"$sdl_include" -I"$root/src" -I"$root/extern/borealis/include" \
        -I"$root/extern/borealis/src" -DBOREALIS_BUILD_TYPE='"Test"' \
        "$root/src/dusk/web/disc.cpp" "$root/extern/borealis/src/io.cpp" \
        "$root/platforms/web/tests/web_disc_test.cpp" "$sdl_lib" -ldl -o "$out/web_disc_test"
    "$out/web_disc_test"
else
    echo "run-native-tests: no SDL3 given; skipped the disc channel test"
fi
