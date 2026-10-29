# Building Dusklight

## Dependencies

The following dependencies are required:

* [CMake 3.25+](https://cmake.org)
* [Python 3+](https://python.org)

### Windows

* Install [CMake 3.25+](https://cmake.org) by searching `CMake Tools` in Visual Studio
* Install Python 3 from the [Microsoft Store](https://go.microsoft.com/fwlink?linkID=2082640) and verify it's added to `%PATH%` by typing `python` in `cmd`.

Recommended IDEs:

* [Visual Studio 2026 Community](https://www.visualstudio.com/en-us/products/visual-studio-community-vs.aspx). During installation:
  * Select `C++ Development` and verify the following packages are included:
    * `Windows 11 SDK`
    * `CMake Tools`
    * `C++ Clang Compiler`
    * `C++ Clang-cl`

### macOS

* Make sure [Homebrew](https://brew.sh) is installed
* Install [CMake 3.25+](https://cmake.org)

```sh
brew install cmake
```

* Install Python 3

```sh
brew install python@3
```

Recommended IDEs:

* [Xcode 16.4 or later](https://developer.apple.com/xcode/)
* [Visual Studio Code](https://code.visualstudio.com/download/)
* [CLion](https://www.jetbrains.com/clion/)

### Linux

Actively tested on Ubuntu 24.04, Arch Linux & derivatives.

**Ubuntu 24.04+ packages**

<details>
<summary>Click to expand</summary>

* Run the following command to install the required dependencies:

```sh
sudo apt update && sudo apt install -y \
    build-essential \
    clang \
    cmake \
    curl \
    git \
    libasound2-dev \
    libclang-dev \
    libcurl4-openssl-dev \
    libdbus-1-dev \
    libfreetype-dev \
    libglu1-mesa-dev \
    libgtk-3-dev \
    libncurses5-dev \
    libpng-dev \
    libpulse-dev \
    libudev-dev \
    libvulkan-dev \
    libx11-xcb-dev \
    libxcursor-dev \
    libxi-dev \
    libxinerama-dev \
    libxrandr-dev \
    libxss-dev \
    libxtst-dev \
    lld \
    ninja-build \
    python-is-python3 \
    python3 \
    python3-markupsafe \
    zlib1g-dev
```

</details>
<br>

**Arch Linux packages**

<details>
<summary>Click to expand</summary>

* Run the following command to install the required dependencies:

```sh
sudo pacman -S --needed \
    alsa-lib \
    base-devel \
    clang \
    cmake \
    freetype2 \
    libpulse \
    libxrandr \
    lld \
    llvm \
    ninja \
    python \
    python-markupsafe \
    vulkan-headers
```

</details>
<br>

**Fedora packages**

<details>
<summary>Click to expand</summary>

* Run the following command to install the required dependencies:

```sh
sudo dnf install -y \
    clang-devel \
    cmake \
    libpng-devel \
    llvm-devel \
    ninja-build \
    vulkan-headers
```

* It's also important that you install the developer tools and libraries

```sh
sudo dnf groupinstall \
    "Development Libraries" "Development Tools"
```

</details>
<br>

Recommended IDEs:

* [CLion](https://www.jetbrains.com/clion/)
* [Visual Studio Code](https://code.visualstudio.com/download/)

## Building

* Clone and initialize the Dusklight repository:

```sh
git clone --recursive https://github.com/TwilitRealm/dusklight.git
git pull
cd dusklight
git submodule update --init --recursive
```

**CLion (Windows / macOS / Linux)**

Open the project directory in CLion. Enable the appropriate presets for your platform:

![CLion](../assets/clion.png)

**Visual Studio (Windows)**

Open the project directory in Visual Studio. The CMake configuration will be loaded automatically.

**ninja (macOS)**

```sh
cmake --preset macos-default-relwithdebinfo
cmake --build --preset macos-default-relwithdebinfo
```

Alternate presets available:

* `macos-default-debug`: Clang, Debug
* `macos-default-debug-asan`: Clang, Debug, AddressSanitizer

**ninja (Linux)**

```sh
cmake --preset linux-default-relwithdebinfo
cmake --build --preset linux-default-relwithdebinfo
```

Alternate presets available:

* `linux-default-debug`: GCC, Debug
* `linux-default-debug-asan`: GCC, Debug, AddressSanitizer
* `linux-clang-relwithdebinfo`: Clang, RelWithDebInfo
* `linux-clang-debug`: Clang, Debug
* `linux-clang-debug-asan`: Clang, Debug, AddressSanitizer

**ninja (Windows)**

```sh
cmake --preset windows-msvc-relwithdebinfo
cmake --build --preset windows-msvc-relwithdebinfo
```

Alternate presets available:

* `windows-msvc-debug`: MSVC, Debug
* `windows-msvc-debug-asan`: MSVC, Debug, AddressSanitizer
* `windows-clang-relwithdebinfo`: Clang-cl, RelWithDebInfo
* `windows-clang-debug`: Clang-cl, Debug

**ninja (Web, experimental)**

The browser build is a work in progress; see [the web port plan](web-port-plan.md). It needs
[Emscripten](https://emscripten.org/docs/getting_started/downloads.html) 6.0.13 activated in your
shell:

```sh
git clone https://github.com/emscripten-core/emsdk.git
emsdk/emsdk install 6.0.13
emsdk/emsdk activate 6.0.13
source emsdk/emsdk_env.sh
```

```sh
cmake --preset web
cmake --build --preset web
```

Configuring applies `platforms/web/patches` to `extern/aurora` and `extern/borealis`. The page,
`dusklight.js`, `.wasm` and `.data` end up in `build/web`.

If the build can't download its dependencies' GitHub source archives (some proxies refuse them),
clone the same versions with git and point the build at them:

```sh
platforms/web/fetch-deps.sh deps
cmake --preset web -DDUSK_WEB_DEPS_DIR="$PWD/deps"
```

The host-side tests of the web port need only a C++ compiler and, for the page test, Node and
Playwright's Chromium:

```sh
platforms/web/tests/run-native-tests.sh build/web/_deps/sdl-src
node platforms/web/tests/shell-test.mjs
```

`node platforms/web/tests/web-audio/run.mjs` (with Emscripten activated) plays audio through the
web audio output in headless Chromium.

After building, `platforms/web/tests/run-page.mjs build/web` checks that the prelaunch screen renders
in headless Chromium; with `--disc <image> --expect-log <regex>` it boots a disc image, such as the
data-free one `platforms/web/tests/make-synthetic-disc.py` writes. `platforms/web/tests/profile-page.mjs build/web`
prints where the page's main thread spends its time.

## Running

**Windows / Linux**

* Pass the disc image as a positional argument using the `--dvd` flag. Supported formats are: ISO (GCM), RVZ, WIA, WBFS, CISO, GCZ

```sh
build/{preset}/dusklight --dvd /path/to/game.iso
```

**macOS**

macOS builds an `.app` bundle which contains the executable and all necessary resources.

* Pass the disc image as a positional argument using the `--dvd` flag. Supported formats are: ISO (GCM), RVZ, WIA, WBFS, CISO, GCZ

```sh
build/{preset}/Dusklight.app/Contents/MacOS/Dusklight --dvd /path/to/game.iso
```

**Web**

The page must be served with cross-origin isolation headers (`platforms/web/shell/_headers`). Serve
the build locally, then open http://localhost:8080/ in Chrome or Edge:

```sh
node build/web/serve.mjs build/web 8080
```

The page asks for an uncompressed GameCube disc image (ISO/GCM). You can keep a copy in the
browser's storage, or choose the file again on each visit.
