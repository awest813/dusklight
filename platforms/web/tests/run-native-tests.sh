#!/usr/bin/env bash
# Builds and runs the web port's host-side unit tests natively: the ISO reader (nod_iso) and the
# web disc channel. They need only a C++20 compiler; the disc channel test also needs SDL3:
#
#   platforms/web/tests/run-native-tests.sh [SDL3 include dir] [libSDL3.a]
#
# Without SDL3 arguments, only the nod_iso test runs.
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

if [[ $# -ge 2 ]]; then
    "$cxx" "${flags[@]}" -I"$1" -I"$root/src" -I"$root/extern/borealis/include" -I"$root/extern/borealis/src" \
        -DBOREALIS_BUILD_TYPE='"Test"' \
        "$root/src/dusk/web/disc.cpp" "$root/extern/borealis/src/io.cpp" \
        "$root/platforms/web/tests/web_disc_test.cpp" "$2" -ldl -o "$out/web_disc_test"
    "$out/web_disc_test"
fi
