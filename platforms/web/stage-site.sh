#!/usr/bin/env bash
# Copies the files of a web build that make up the site (the page, the module and its data, the
# page's scripts, styles and images) into a folder of their own, ready to publish:
#
#   platforms/web/stage-site.sh build/web _site
#
# Used by .github/workflows/pages.yml. Any static host serves the result (see docs/web.md).
set -euo pipefail
build=${1:?usage: stage-site.sh <build-dir> <out-dir>}
out=${2:?usage: stage-site.sh <build-dir> <out-dir>}
mkdir -p "$out"
for file in index.html dusklight.js dusklight.wasm dusklight.data shell.js shell.css disc-worker.js \
    audio-worklet.js coi-sw.js logo.png icon.png _headers; do
    cp "$build/$file" "$out/$file"
done
echo "stage-site: $(du -sh "$out" | cut -f1) in $out"
