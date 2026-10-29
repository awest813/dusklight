#!/usr/bin/env bash
# Applies the web build's patches (platforms/web/patches) to the extern/aurora and extern/borealis
# submodules. Idempotent: a patch that is already applied is left alone. Every change in them is
# web-only or harmless natively, so applying them to a native checkout does no harm.
#
# The patches were made against the submodule commits recorded in this repository; after bumping a
# submodule, re-apply and regenerate them (git -C extern/<name> diff > platforms/web/patches/...).
# A submodule with other local changes is left alone, so work on the patched tree isn't disturbed.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
for name in aurora borealis; do
    patch="$root/platforms/web/patches/$name-web.patch"
    dir="$root/extern/$name"
    if git -C "$dir" apply --reverse --check "$patch" 2>/dev/null; then
        echo "apply-patches: $name already patched"
    elif [[ -n "$(git -C "$dir" status --porcelain)" ]]; then
        # Someone is working on the patched tree: leave it be. Regenerate the patch from it when done.
        echo "apply-patches: extern/$name has local changes; leaving it as is" \
             "(update the patch with: git -C extern/$name diff > platforms/web/patches/$name-web.patch)"
    elif git -C "$dir" apply --check "$patch" 2>/dev/null; then
        git -C "$dir" apply "$patch"
        echo "apply-patches: patched $name"
    else
        echo "apply-patches: $patch does not apply to extern/$name ($(git -C "$dir" rev-parse --short HEAD))" >&2
        exit 1
    fi
done
