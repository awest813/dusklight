# Web port: audit and plan

Status: plan accepted, implementation started. Decisions are recorded under
[Decisions](#decisions); progress under [Progress](#progress).

This document audits what stands between the current native build and one that runs in a browser
(WebAssembly + WebGPU), and lays out a phased plan. File references are to this repo, to
`extern/aurora` at `3227d76`, and to `extern/borealis` at `4ac5e70`.

It uses [WiiCompiled / MKWiii](https://github.com/awest813/MKWiii) as a reference. That project is
also built on aurora and already renders through it in headless Chromium. Its plan lives in
`docs/web-port-plan.md` and its tooling in `runtime/web/`. Where this plan says "MKWiii", it means
that tree.

## Verdict

A web port is feasible, and in some ways it is easier than MKWiii's. MKWiii is a static
recompiler, so its hardest web problems do not exist here:

- an emulated 4 GiB guest address space with page-fault traps;
- PowerPC floating-point determinism under wasm (FMA, FTZ, NaN payloads);
- stackful guest-thread fibers;
- a translator.

Dusklight is portable C++ that already runs on six platforms, including 32-bit-clean decomp
layouts.

What does decide the outcome:

1. **Threads and the browser main thread.** Every GameCube `OSThread` is a real `std::thread`
   (`src/dusk/OSThread.cpp:2,289,354`). The main thread does blocking round-trips to several of
   them every so often. aurora runs the GX FIFO, the render worker (submit and present), the DVD
   worker and texture-replacement loading on their own threads. In the browser, WebGPU and the
   canvas belong to one thread, and that thread must yield to the event loop to present and to
   receive `mapAsync` callbacks.
2. **The game loop never returns.** `launchUILoop` (`src/m_Do/m_Do_main.cpp:151`) and `main01`
   (`m_Do_main.cpp:232-365`) are `while`/`do…while` loops. MKWiii's answer, JSPI, applies
   directly.
3. **Disc access.** The disc is read through nod, which is written in Rust
   (`extern/aurora/cmake/AuroraNodProvider.cmake`). There is no wasm build of nod and no C
   fallback. `aurora_dvd.cmake:10` links `nod::nod` unconditionally, and `borealis::disc` uses it
   too.
4. **Code mods cannot exist on wasm.** funchook, prepatch, `-fpatchable-function-entry`,
   `dlopen`'d native mods and symgen manifests all assume patchable native code. A web build ships
   with `DUSK_ENABLE_CODE_MODS=OFF`. Data, texture and (eventually) Luau mods can survive.

Unlike MKWiii, the build output contains no translated game code. Like the native releases, it is
only this repo's source plus `res/`. Game data stays on the user's machine. That makes a hosted
"open a URL and pick your disc" build possible in principle. Whether to host one is a maintainer
decision (see [Distribution](#distribution)).

## Constraints

- **Legal posture is unchanged.** No game data is bundled, uploaded or cached anywhere but the
  user's own browser storage.
- **Accuracy bar is unchanged.** The web build is the same code with the same results; anything
  that changes game-visible behaviour (e.g. making DVD callbacks synchronous) needs the same scrutiny
  as on native.
- **Browser floor (first release).** WebGPU, wasm threads (`SharedArrayBuffer`, so the page must be
  cross-origin isolated with COOP/COEP headers), wasm exceptions, and JSPI. That is current Chromium.
  Firefox and Safari are follow-ups. Check their JSPI and WebGPU feature status during Phase 0
  rather than assuming it.
- **Native must not regress.** Every change is behind `__EMSCRIPTEN__` or a CMake option, or is a
  pure portability fix, such as endian detection.
- **Contribution policy.** Upstream does not accept primarily AI-generated contributions (see the
  README). Treat this document as a starting point for a human-led effort, not as a patch series to
  submit as is.

## Audit by subsystem

Effort: S (days), M (1–3 weeks), L (1–2 months), XL (more).

### 1. Build system and toolchain — M

**Today.** There is no Emscripten branch anywhere. This repo, aurora and borealis have zero matches
for `__EMSCRIPTEN__`/`__wasm__`, so every platform ladder falls through to its "other" branch.

**Blockers found:**
- **`include/helpers/endian.h:9`.** `TARGET_LITTLE_ENDIAN` is only set for
  `_WIN32/__x86_64__/__i386__/__aarch64__`. On wasm:
  - `BSWAP16`/`BSWAP32` become no-ops, so every `BE<T>` read is silently wrong;
  - `BSWAP64` is left undefined, so `be64()` fails to compile.

  Switch the test to `std::endian::native` or `__BYTE_ORDER__`, as `include/helpers/bits.hpp:56`
  and `extern/aurora/lib/internal.hpp:19` already do. Its reach is about 1,600 `BE(` uses, 75
  `be_swap(` calls and 21 `#if TARGET_LITTLE_ENDIAN` sites.
- **`sdk/include/mods/api.h:217,254`.** `static_assert(sizeof(ModMetaHookFn) == 24)` and
  `sizeof(ModMetaHookMemExt) == 24` fail on wasm32 (both are 16). `src/dusk/mods/svc/*.cpp`
  compiles them even with code mods off (`files.cmake:1483-1556`), so either gate the asserts on
  pointer size or keep these TUs out of a no-code-mods build.
- **`CMakeLists.txt`:**
  - `DUSK_HAS_FUNCHOOK` turns on for every non-Apple target (`:98-102`) and fetches
    funchook/capstone.
  - `-fpatchable-function-entry` (`:442-465`) is rejected for wasm.
  - `-rdynamic` (`:477`) applies because Emscripten sets `UNIX`.
  - `setup_symbol_manifest` (`:383-392`) has no wasm object-format support.

  All of these go away with `DUSK_ENABLE_CODE_MODS=OFF`, except `-rdynamic`, which needs a guard.
- **`GAME_LIBS`** (`CMakeLists.txt:280-282`) hard-requires every `borealis::` target, so modules
  need web stubs or backends rather than being switched off. See section 9.

**Plan:**
- Add an `x-web` CMake preset (`emcmake`, Ninja, Release and RelWithDebInfo).
- Add an `EMSCRIPTEN` branch in `CMakeLists.txt` that:
  - forces `DUSK_ENABLE_CODE_MODS=OFF` and `BOREALIS_ENABLE_DISCORD=OFF`;
  - includes a `cmake/web/AuroraWebOptions.cmake` modelled on MKWiii's
    `runtime/web/cmake/AuroraWebOptions.cmake`;
  - sets the link flags (section 3);
  - installs a page template next to the `.js`/`.wasm`, like MKWiii's
    `runtime/web/product/WiiCompiled.html`.
- Compile flags:
  - `-pthread` and `-fwasm-exceptions` (about 93 `try` blocks in game code, plus nlohmann and
    cxxopts);
  - `-sSUPPORT_LONGJMP=wasm` (libpng uses `setjmp`, `aurora/lib/gfx/png_io.cpp:59`).
- Keep MKWiii's `fetch-deps.sh` pattern for hosts that cannot download pinned archives.
- Pin one Emscripten version (MKWiii uses 6.0.13) and install it the same way in CI.

### 2. Pointer size: wasm32 vs wasm64 — S

**Today.** The 64-bit switch is `BIT_64` (`extern/aurora/include/dolphin/types.h:4-8`). Only
three structures branch on it:
- `JASTrack.h:20-24` (`JASTrackNodeOffset`, with a `static_assert` at `:275`);
- `JKRHeap.h:22-26` (`MEM_BLOCK_SIZE`);
- `JKRExpHeap.h:47-56`.

On wasm32 these fall back to the original 32-bit GameCube layouts. Most PC fixes key on
`TARGET_PC`, not pointer width (e.g. `JKRArchive.h:93-102`). The 799 `STATIC_ASSERT(sizeof…)` checks
are compiled out off CodeWarrior (`include/global.h:62-72`).

**Decision: wasm32.** It has the widest browser support, matches the original layouts, and
nod's Rust target exists only for wasm32. wasm64 would reuse the desktop-tested 64-bit layouts, but
it has no Safari support and is slower.

**wasm32 risks to fix:**
- `extern/aurora/lib/gfx/texture.hpp:114`: `has_data()` treats any pointer below `0x10000` as "no
  data". Emscripten can place static data below 64 KiB. Either set `-sGLOBAL_BASE` to at least
  64 KiB, or replace the sentinel test.
- `aurora/lib/gfx/texture_replacement.cpp:465,688`: brace-initializes `ByteBuffer(size_t)` from
  `uint64_t`. This is a hard narrowing error on wasm32 (MKWiii's patch fixes it).
- Null dereferences do not trap in wasm. Crashes become silent reads of low memory, so keep a
  debug build with `-sSAFE_HEAP`/ASan for bring-up.
- A handful of `reinterpret_cast<u32>(ptr)` in debug-only code (`d_event_debug.cpp`,
  `d_meter_HIO.cpp`) is harmless.

### 3. Main loop, threads and blocking — L

**Today:**
- aurora hijacks `main` (`extern/aurora/include/aurora/main.h`, `lib/main.cpp:6`). The call chain
  is `DuskMain` → `game_main` (`m_Do_main.cpp:548`) → `launchUILoop` (prelaunch) → `main01`.
- Each frame of `main01`:
  1. `aurora_update` → `aurora_begin_frame` → `game_clock::advance`.
  2. Fixed 30 Hz sim ticks (`game_clock.cpp:73-135`) plus an interpolated draw.
  3. `aurora_end_frame`.
  4. An optional `Limiter` sleep.

**Threads in a running game.** All are real `std::thread`s:

| Thread | Where | Blocks on |
| --- | --- | --- |
| `mDoDvdThd` | `m_Do_dvd_thread.cpp:57-61` | message queue |
| `JKRAram`, `JKRAramStream`, `JKRDecomp` | `JFWSystem.cpp:72`, `JKRAram.cpp:40-41` | message queues |
| `MemCardThread` | `m_Do_MemCard.cpp:108-115` | `OSWaitCond` |
| `JASDvd` (`JASTaskThread`) | `JAUInitializer.cpp:64` | message queue; also blocking `DVDReadPrio` |
| Movie read / video / audio decode | `d_a_movie_player.cpp:2767,2983,3169` | queues; joined from the main thread |
| aurora DVD worker | `aurora/lib/dolphin/dvd/dvd.cpp:354` | cv; nod preloader thread (`:670`) |
| aurora GX FIFO processor | `aurora/lib/gx/fifo.cpp:27,94-103` | atomic wait |
| aurora render worker | `aurora/lib/gfx/render_worker.cpp:199` | owns encoder, **Submit and Present** |
| aurora texture-replacement pool | `aurora/lib/gfx/texture_replacement.cpp:842` | cv |
| aurora pipeline-cache writer (sqlite) | `aurora/lib/gfx/pipeline_cache.cpp:1072` | cv |
| SDL audio callback | `src/dusk/audio/DuskAudioSystem.cpp:84` | holds `JASCriticalSection` for a whole audio frame |
| borealis task pool (≤8), net, ws | `borealis/src/task.cpp:86` etc. | — |

The aurora pipeline-compile thread is already skipped when the backend is WebGPU
(`pipeline_cache.cpp:1143`).

**Main-thread waits on other threads:**
- **Every frame:**
  - `fifo::drain` twice: via `GXDrawDone` (`JFWDisplay.cpp:580`) and at `aurora.cpp:267`.
  - Sleep-polls in `gfx::begin_frame` waiting on `MapAsync` (`frame.cpp:178-183, 580-621`).
  - Limiter sleeps (`src/dusk/time.h`, `JFWDisplay::waitForTick`).
- **Occasionally:**
  - Synchronous `DVDReadPrio` (THP open, `dvd_asset.cpp:112`).
  - `JKRAramPiece::orderSync`, `JKRDecomp::sync`.
  - `daMP_WaitUntilPrepare`.
  - `OSJoinThread` on movie threads.
  - `Z2SoundMgr::stopSync` → `JASDriver::waitSubFrame`, a spin plus yield until the audio thread
    advances (`JASDriverIF.cpp:48-53`).
  - `SDL_WaitEvent` while paused (`aurora/lib/window.cpp:293-300`).

**Choices:**

| Option | Verdict |
| --- | --- |
| Invert the loops into `emscripten_set_main_loop` / SDL callbacks | Reject as the first step. Two loops plus nested blocking calls (sync DVD, movie prepare) would all need restructuring. |
| Asyncify | Reject. Indirect calls everywhere would instrument most of a ~1.5M-line program. |
| **JSPI, game loop on the browser main thread** | **Recommended (MKWiii's model).** `main()` runs as a JSPI export. Present waits on `requestAnimationFrame`, and GPU waits and Limiter sleeps suspend to the event loop. WebGPU, SDL and the canvas stay on the main thread. |
| `PROXY_TO_PTHREAD`, game and WebGPU in a worker with `OffscreenCanvas` | Fallback spike. Blocking is legal in a worker, but present and `mapAsync` still need event-loop turns (so JSPI anyway), and SDL3's input and window code expects the DOM thread. |

**Plan (JSPI model):**
- **aurora on the main thread.** Port MKWiii's aurora changes (section 4):
  - FIFO in `Drain` mode;
  - render worker not started (its `enqueue` already runs inline, `render_worker.cpp:63-77`;
    `is_worker_thread()` must then return true for the asserts at `gpu.cpp:687` and
    `depth_peek.cpp:386`);
  - texture replacements synchronous (`gx/texture.hpp:29`);
  - no sqlite pipeline cache or its writer;
  - `wait_for_gpu_progress` yields to the browser;
  - no `SDL_WaitEvent` pause.
- **Game `OSThread`s stay real threads**, as wasm pthreads from a pre-spawned pool
  (`-sPTHREAD_POOL_SIZE`). They never touch WebGPU or SDL video, so this matches native semantics.
  This is the main difference from MKWiii, which runs its guest threads as JSPI stacks on the
  main thread.
- **Main-thread waits on those threads:**
  - Keep Emscripten's default `ALLOW_BLOCKING_ON_MAIN_THREAD=1`, so a short futex wait spins
    instead of trapping. MKWiii sets it to 0, but it has no cross-thread waits.
  - The rule that makes this safe: **no worker may need the main thread to make progress while
    the main thread waits on it.** That rules out proxied syscalls and JS-on-main-thread I/O on
    the DVD path (section 5), and it rules out audio mixing on the main thread (section 6).
  - Long waits (`daMP_WaitUntilPrepare`, movie `OSJoinThread`) should become JSPI suspends, e.g.
    a helper that polls and yields, so the page stays responsive.
- **Sleeps.** `Limiter::NanoSleep` (`src/dusk/time.h`) on web becomes a JSPI `emscripten_sleep`,
  or is skipped, because `requestAnimationFrame` paces the loop. `game_clock` is already a
  fixed-timestep accumulator and handles variable frame times (`game_clock.cpp:73-135`).
- **Pool size.** About 8 game threads at peak (with movies), the aurora DVD worker, and the
  borealis task pool. Cap the borealis pool on web (`task.cpp:86`, `hardware_concurrency`) and set
  `PTHREAD_POOL_SIZE` to the measured peak, because a thread that is not pre-spawned cannot start
  while the main thread is blocked.
- **Link flags.** Start from MKWiii's, adjusted:
  - `-sJSPI` with `main` exported;
  - `-sINITIAL_MEMORY=1GB -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB` (MEM1 alone is 256 MiB,
    `m_Do_main.cpp:707`);
  - `-sSTACK_SIZE=8MB` and `-sDEFAULT_PTHREAD_STACK_SIZE=1MB`;
  - `--profiling-funcs`.
- **Restart.** `RestartProcess` uses `execv` (`src/dusk/main.cpp:36-111`). On web it becomes
  `location.reload()`.

**Existing bugs this will likely expose** (timing changes on web make races likelier):
- `OSWakeupThread` changes thread state and queue links without holding the condition variable's
  mutex (`OSThread.cpp:406-452`), so a wakeup can be lost.
- `OSDisableInterrupts` is a no-op (`OSThread.cpp:587-597`), so decomp critical sections built on
  it are unprotected (e.g. movie-player counters).
- Several cross-thread flags are plain non-atomics: `mIsDone`, `IsRunning`, `snIntCount`.

Fixing these helps native too.

### 4. Graphics: aurora on emdawnwebgpu — L

**Today.** aurora is closer to web-ready than the fork MKWiii started from. Dawn-native code is
mostly behind `WEBGPU_DAWN`, and this version has no MetalFX, no shared texture memory and no
`Tick()`.

**What MKWiii's `runtime/web/aurora/aurora-web.patch` gives us**, hunk by hunk against `3227d76`:
- **Applies cleanly or with offsets:**
  - `AuroraDawnProvider.cmake` (emdawnwebgpu alias);
  - `aurora_gx.cmake`;
  - `fifo.cpp` (`Drain`);
  - `texture.hpp` (async replacements off);
  - `texture_replacement.cpp` (wasm32 casts);
  - `gx.cpp` and `pipeline.cpp` (immediate-data shims);
  - `gpu.cpp` (guard `wgpu_log`/`SetLoggingCallback`; canvas format);
  - `pipeline_cache.cpp` (`AURORA_PIPELINE_CACHE_DB` gate, needs re-checking);
  - `render_worker.cpp` `initialize`;
  - the new `lib/webgpu/web_extensions.{cpp,hpp}`: canvas surface, present on
    `requestAnimationFrame`, yield via `MessageChannel`, `setImmediates`, and
    `createPipelineLayout` with `immediateSize`.
- **Same intent, edit by hand:**
  - `aurora.cpp` present at `:383`;
  - `frame.cpp` `wait_for_gpu_progress` at `:182`;
  - `render_worker.cpp` `is_worker_thread`;
  - `aurora_core.cmake` (sqlite is linked unconditionally at `:23`; there is no
    `presentation.cpp`);
  - the imgui `-U__EMSCRIPTEN__` trick, which goes in `extern/CMakeLists.txt:249` because there is
    no `AuroraDearImguiProvider.cmake`;
  - the sqlite block in `extern/CMakeLists.txt:269-298`.
- **Do not take:**
  - skipping Freetype, because Dusklight uses RmlUi, which needs it;
  - the `frame_interpolation.cpp`, `Tick` and SharedTextureMemory hunks, whose code does not exist
    here.
- **Not covered by the patch:**
  - `ENABLE_BACKEND_WEBGPU` is never defined (`aurora.cpp:68`). `to_wgpu_backend` already maps it
    (`gpu.cpp:737`).
  - Adapter and device creation use `WaitAny(…, 5 s)` with `TimedWaitAny`
    (`gpu.cpp:777-779, 842, 1037`). MKWiii runs these unchanged under JSPI; confirm that.
  - Features to verify against emdawnwebgpu and the browser, with fallbacks where they are
    missing:
    - `wgpu::CompatibilityModeLimits` / `FeatureLevel::Compatibility` (`gpu.cpp:819,886`);
    - `FeatureName::TextureComponentSwizzle` (`gpu.cpp:934`, `gfx/texture.cpp:33`);
    - BC texture compression, which is usually absent on mobile; a fallback exists.
  - Immediate data (`var<immediate>`, `gx/shader.cpp:2014`) exists in Chromium only. Supporting
    Firefox or Safari later needs a uniform-buffer path.

**Plan:**
- Carry these changes as a patch, the way MKWiii does: `platforms/web/aurora/aurora-web.patch`,
  applied idempotently to `extern/aurora` by `platforms/web/apply-patches.sh`. Every hunk is
  web-only (`__EMSCRIPTEN__`/`WEBGPU_DAWN`), so applying it to a native checkout is harmless.
  borealis changes are carried the same way.
- Build MKWiii's `runtime/web/smoke` app against our aurora as the first CI gate.

**Pipeline compilation stutter.** On WebGPU, aurora builds pipelines synchronously on first use
(`pipeline_cache.cpp:469-490`), and the web has no on-disk pipeline DB. Expect hitches the first
time each material is seen. Mitigations:
- `CreateRenderPipelineAsync`, with the same "skip until ready" behaviour the native pipeline
  thread gives non-blocking requests;
- relying on the browser's own shader cache across sessions;
- optionally pre-warming from a shipped pipeline list.

This is the most likely source of "feels worse than native".

### 5. Disc image, nod and DVD — L

**Today.**
- `aurora_dvd_open` opens the image with `SDL_IOFromFile` and hands nod a `NodDiscStream`
  (`read_at`, `stream_len`, `close`; `dvd.cpp:126-165, 650-710`).
- `borealis::disc` and `src/dusk/iso_validate.cpp:29-60` hash the image (XXH3-128) through nod.
- The prelaunch picker accepts `iso;gcm;ciso;gcz;nfs;rvz;wbfs;wia;tgc`
  (`src/dusk/ui/prelaunch.cpp:88`).

**Blockers:**
- nod is Rust. Its only providers are prebuilt packages (Windows, Linux and macOS) or a
  Corrosion build, and neither targets the web.
- A threaded wasm module requires every object to be built with `+atomics,+bulk-memory`. Rust's
  `wasm32-unknown-emscripten` std is not, so this likely needs nightly `-Zbuild-std`. Spike it
  first.
- nod's compression dependencies (zstd, lzma, bzip2 for WIA/RVZ) must also cross-compile.

**Plan:**
- **Phase 0 spike:** build nod for `wasm32-unknown-emscripten` with atomics (`-Zbuild-std`),
  `preloader_threads = 0`, and link it into a threaded Emscripten test.
- **Fallback if the spike fails (accepted):** a small C++ reader for plain ISO/GCM behind the same nod C API
  subset (`nod_disc_open_stream`, `_partition_*`, `_iterate_fst`, `nod_read/seek`), with a
  "convert to ISO first" message for compressed formats. Note that aurora's DVD overlay hooks
  (`include/aurora/dvd.h`) already show the FST can be served without nod.
- **Getting bytes to nod.** The `read_at` callback is the seam. It must run on a worker and be
  synchronous, so the main thread can safely spin on DVD completion (section 3). The user picks
  the storage mode on the disc screen:
  1. **OPFS.** Copy the image once into the origin-private file system and read it with a
     `FileSystemSyncAccessHandle` from the aurora DVD worker. This is fast, persists, and works in
     every browser with OPFS. It costs a one-time copy of 1.4 GB (GC) or 4.7 GB (Wii) against the
     storage quota.
  2. **File handle.** Keep a `File` (from `<input type=file>` or a drag-and-drop) and read it with
     `FileReaderSync` in the worker. There is no copy, but the user must re-pick the file every
     session unless a File System Access handle is stored in IndexedDB (Chromium only, and it
     needs a permission prompt per session).
  3. Never put the image in MEMFS.
- Keep the aurora DVD worker as a pthread, so `enqueue` does not take the inline path. Inline
  completion would make DVD callbacks fire synchronously inside `DVDRead*Async`, which changes
  reentrancy.
- Validation hashing of a whole disc in wasm is slow. Show progress, and cache the verdict keyed
  by file name, size and mtime.

### 6. Audio — M

**Today.**
- SDL3 pulls audio through `GetNewAudio` (`DuskAudioSystem.cpp:84,153-181`). On the SDL audio
  thread, that callback runs:
  - the JAudio sequencer (`JASDriver::updateDSP` → `subframeCallback`);
  - the software DSP (`DuskDsp.cpp`);
  - THP audio mixing.
- It holds `JASCriticalSection`, one global recursive mutex, for the whole frame.

**Web problem.** SDL3's Emscripten audio callback runs on the browser main thread. With the game
loop also there:
- `Z2SoundMgr::stopSync` → `JASDriver::waitSubFrame` spins waiting for the audio thread to advance
  `sSubFrameCounter`, which deadlocks;
- a JSPI suspend while the main thread holds `JASCriticalSection` lets the audio callback re-enter
  the recursive mutex on the same thread.

**Plan:**
- A dedicated mixer pthread calls the existing `RenderNewAudioFrame` and writes into a
  `SharedArrayBuffer` ring buffer. A small JS `AudioWorklet` reads from it, which can be
  Emscripten's Wasm Audio Worklets or plain JS. The mixer is paced by the ring buffer's fill level.
  This keeps the sequencer off the main thread, exactly as on native.
- Browsers only start audio after a user gesture. The page's "pick disc" or "start" click unlocks
  the `AudioContext`.
- freeverb's FTZ/DAZ (`libs/freeverb/include/freeverb/denormals.h`) falls back to a no-op with a
  `#warning`. Expect slower reverb only; profile it.

### 7. Filesystem, config and saves — M

**Today.**
- User paths come from `borealis::data` (`src/dusk/data.cpp:83-125`, `SDL_GetPrefPath`).
- **User files:**
  - `config.json`, `achievements.json`, `states.json`, `imgui.ini`;
  - pad bindings, `controller_ports.dat`, keyboard bindings;
  - CARD images `MemoryCard{A,B}.{region}.raw` (`aurora/lib/dolphin/card.cpp:69,115,314`);
  - GCI and `.dusksave` files (`src/dusk/save_manager.cpp`);
  - `texture_replacements/`, `mods/` and logs.
- `res/` is read relative to `SDL_GetBasePath()` (`aurora/lib/aurora.cpp:118`). It is 7.7 MB in
  35 files, mostly fonts and PNGs.

**Plan:**
- **WasmFS with the OPFS backend for the user directory.** It persists automatically, and its I/O
  runs on a backing thread, which fits the "workers never wait on the main thread" rule better
  than IDBFS's manual `syncfs`.
- Point `ConfigPath`/`CachePath` at fixed virtual paths, and hide "change/open data folder"
  (`DUSK_CAN_OPEN_DATA_FOLDER` already becomes 0 on unknown platforms, `src/dusk/data.hpp:12-18`).
- Serve `res/` with `--preload-file`, or fetch it lazily.
- Use MEMFS for logs and caches, and offer the log as a download from the fatal-error overlay
  (section 10).
- Expose save export/import prominently, because clearing site data deletes OPFS. The save
  manager's `.dusksave` export already does the hard part.

### 8. Input, window, UI — S

- SDL3's Emscripten backend maps the Gamepad API, keyboard, mouse and touch, so aurora's pad
  layer, the RmlUi menus, ImGui and the existing touch controls (`TouchControls` document) should
  carry over.
- Fullscreen and pointer lock need a user gesture.
- **Drag and drop.** SDL's Emscripten backend writes dropped files into MEMFS. The existing drop
  handling (`src/dusk/ui/ui.cpp:214-218`, `.gci`/`.raw`/`.dusksave`) then works.
- **File dialogs.** `borealis::file_select` uses SDL3 dialogs, which have no Emscripten backend.
  Add a web backend:
  - open: `<input type=file>` or `showOpenFilePicker`, copying into OPFS or keeping a `File`
    (section 5);
  - save/export: a Blob download.
- **Pause on focus loss** must not use `SDL_WaitEvent` (section 3). Pause the game clock and keep
  yielding.

### 9. Borealis modules and networking — M

| Module | Web mapping |
| --- | --- |
| `http` | New fetch backend (`emscripten_fetch` or JS). The mod catalog (`src/dusk/mods/catalog.cpp:24`) and thumbnails (`ui/remote_texture_provider.cpp:46`) need CORS on `twilitrealm.dev`. |
| `ws` | Browser `WebSocket`. |
| `net` | Raw TCP/UDP is impossible: stub it. LiveSplit (`src/dusk/livesplit.cpp`) is dropped, or later reimplemented over WebSocket. Mod `net` services return unsupported. |
| `update` | Off. The page itself is the update channel. |
| `discord` | Off (`BOREALIS_ENABLE_DISCORD=OFF` selects the existing stub). |
| `crash` | Stub (it needs ELF/signals). JS-side handler instead (section 10). |
| `sentry` | Already off by default. Later, optionally the Sentry JS SDK. |
| `data`, `io`, `log`, `cli`, `presentation`, `task` | Build as is on the virtual FS. Take CLI args from the query string (MKWiii's page already does this). Cap the `task` pool. |
| `disc` | Follows nod (section 5). |
| `file_select` | Web backend (section 8). |

### 10. Errors and diagnostics — S

- Build with `-fwasm-exceptions` so existing `try`/`catch` keeps working.
- In JS, catch `WebAssembly.RuntimeError` and `onAbort`, then show an overlay with the log tail
  and a "download log" button. MKWiii's page already records `print`/`printErr`, exit and abort
  status for its harness.
- Use `--profiling-funcs` so stacks keep their function names.
- Use a `-sASSERTIONS=2 -sSAFE_HEAP` build for bring-up, because wasm does not trap on null reads.

### 11. Mods — S (off), M (Luau), XL (code mods)

- **Code mods.** Off on web. `hook.cpp:930-958` and `natives.cpp:388` already report
  `MOD_UNSUPPORTED`/`BuildDisabled`, and the UI handles it (`drop_install_modal.cpp:32`). A wasm
  hooking scheme (patching indirect-call table slots) would only reach virtual or function-pointer
  calls, so it is not worth pursuing.
- **Data and texture mods.** These work through the existing bundle and DVD-overlay paths once
  bundles can be imported into OPFS.
- **Luau script mods.** `mods/luau_runtime` is portable (VM and compiler, no JIT, no platform
  `#if`), but it ships as a native `MODULE`. It needs a "built-in static mod" path that links it
  into the executable and calls `register_service` directly. That path would also benefit native
  platforms that cannot `dlopen` (iOS).
- **Randomizer and cosmetics.** These are external native mods (the submodules were not inspected).
  Unless they are reworked onto service APIs without `DEFINE_HOOK`, they stay native-only.

## Distribution

**A. Hosted static site.** The site is `index.html` plus `.js`, `.wasm` and `res/`, with no game
data. The user picks their disc, which stays in their browser. COOP/COEP headers are required:
GitHub Pages cannot set them (the workaround is `coi-serviceworker`), while Cloudflare Pages,
Netlify or the project's own server can. Serve compressed `.wasm` with versioned file names, and
add a service worker for offline play. This is the zero-install path MKWiii cannot take.

**B. Local-only.** CI publishes a web build as an artifact, and users serve it with a bundled
`serve` script that sets the headers. The legal posture is the same as the native downloads, but
there is little benefit over native.

**Decision: A.** The first release is a hosted site. The upstream maintainers still need to agree
before anything is hosted under the project's name.

## Phased plan

Each phase ends with something testable and adds a CI job so it does not regress. Headless CI uses
MKWiii's harness (`runtime/web/browser-harness.mjs`): Chromium with WebGPU on SwiftShader, served
with COOP/COEP, plus an offscreen canvas shim. Phases that need a disc are verified locally.

| Phase | Work | Exit criterion |
| --- | --- | --- |
| 0. Spikes | (a) nod on `wasm32-unknown-emscripten` with atomics, linked into a threaded test. (b) MKWiii's aurora smoke app against our aurora with the ported patch. (c) JSPI main thread spinning on a pthread that reads a `File` via `FileReaderSync` and an OPFS sync handle. (d) WebGPU feature check (swizzle, immediates, BC, compat limits) in Chrome, Firefox and Safari. | Written go/no-go for each. If (a) fails, switch Phase 4 to the ISO/GCM reader. |
| 1. Portability fixes (native-safe) | Endian detection; `api.h` wasm32 asserts; `has_data` sentinel; `texture_replacement` casts; `-rdynamic` guard; the `OSWakeupThread`/atomics fixes from section 3. | Native CI green; a wasm32 compile of `src/` and `libs/JSystem` gets past these errors. |
| 2. Toolchain | `x-web` preset, aurora web options and patch, borealis web stubs (crash, net, update, discord off, file_select web, http fetch), code mods off, Freetype for RmlUi, link flags, page template. | `dusklight.wasm` links. CI `web_build` job. |
| 3. Main loop and graphics | JSPI `main`, present on rAF, render worker inline, FIFO `Drain`, `wait_for_gpu_progress` yields, Limiter sleeps suspend, no `SDL_WaitEvent` pause, `res/` preloaded, WasmFS + OPFS for the user dir. | The prelaunch UI (RmlUi) renders and accepts input in headless Chromium with no disc. CI `web_prelaunch` job. |
| 4. Disc and boot | nod (or fallback) in the aurora DVD worker over OPFS or a `File`; web file picker; validation with progress; game `OSThread`s on the pthread pool; long main-thread waits made suspendable. | A local build reaches the title screen from a user disc in Chromium. |
| 5. Audio, movies, saves | Mixer pthread plus AudioWorklet ring buffer; gesture unlock; THP movies (read/decode threads, `daMP_WaitUntilPrepare`); CARD saves persisting in OPFS; save export/import; fatal overlay and log download. | Intro movie plays with sound; a save survives a reload. |
| 6. Performance | Profile main-thread budget (sim + GX FIFO + render now share it); async pipeline creation; 60+ fps interpolation; memory sizing; startup time and `.wasm` size. | Steady 30 fps sim with interpolated 60 fps presentation in a heavy area (Hyrule Field, Castle Town) on mid-range desktop hardware in Chromium. |
| 7. Packaging | Static hosted site; COOP/COEP from the host (or `coi-serviceworker`); compressed, versioned `.wasm`; service worker for offline play; unsupported-browser page for non-Chromium browsers; docs. | CI builds the site folder and deploys it to a preview URL, where a disc picked in Chromium boots. |
| 8. Optional | Firefox and Safari (immediate-data fallback, JSPI status); Luau mods as a built-in static mod; mod catalog over fetch with CORS; LiveSplit over WebSocket; mobile browsers and touch. | Per item. |

## Top risks

1. **nod on wasm with threads.** If nightly `-Zbuild-std` is unworkable, the first release
   supports plain ISO/GCM only.
2. **Main-thread budget.** On native, the GX FIFO processing, render submission and audio each
   have their own thread. On web, the first two share the main thread with the game. Profile in
   Phase 3, before investing in packaging.
3. **Pipeline-compile hitches** without a pipeline thread or disk cache (section 4).
4. **Threading bugs surfacing under new timing**: the lost-wakeup race and the no-op interrupt
   disable (section 3).
5. **Cross-thread deadlocks** if anything on a worker's critical path ends up proxied to the main
   thread while the main thread spins on that worker (sections 3, 5 and 6). Design rule: workers
   never wait on the main thread.
6. **Browser coverage.** JSPI and immediate data narrow the first release to Chromium.

## Decisions

- **Distribution:** a hosted static site, where the user opens a URL and picks their disc (option A
  under [Distribution](#distribution)). This needs upstream maintainers' agreement before hosting
  under the project's name.
- **Browsers:** Chromium-only for the first release. Firefox and Safari are Phase 8.
- **Pointer size:** wasm32.
- **Disc formats:** if nod cannot be built for wasm, the first release accepts plain ISO/GCM only,
  and tells users to convert compressed images first.

- **Disc storage:** the user chooses on the disc screen, either "copy into browser storage"
  (OPFS, persists, costs quota) or "use this file for this session" (re-pick each visit).
- **aurora and borealis changes:** carried as patches under `platforms/web/`, as MKWiii does, not
  upstreamed.

## Progress

**The web build builds and runs.** With Emscripten 6.0.13 it compiles and links: `dusklight.wasm`
is 23 MB, plus 0.3 MB of JS and 7.9 MB of preloaded `res/`. In headless Chromium, with WebGPU on
SwiftShader:

- **Without a disc,** it renders the prelaunch screen (RmlUi).
- **With a synthetic GameCube image** (`platforms/web/tests/make-synthetic-disc.py`, no game data),
  the image goes through the page's picker, the disc worker, the shared-memory channel, `nod_iso`
  and aurora's DVD layer. The game then identifies GZ2E01, enters the game loop, creates the logo
  scene and keeps running. It waits on resources that a synthetic disc does not contain.
- **Config, logs and the stored disc image** persist in OPFS across reloads.

A real disc has not been tried; that is the next milestone.

Fixed while getting there (see the commit history for details):

- The page shares the module's own memory with its disc worker (`platforms/web/shell/pre.js`).
  Emscripten ignores `Module.wasmMemory` in threaded builds.
- RmlUi's stencil-only pipelines must not leave `depthWriteEnabled` unset. Browsers reject it and
  the device is lost.
- **Function-pointer signature mismatches,** which trap in wasm, in the process framework, the
  cAPI table, a heap callback and JStudio's composite function values.
  `platforms/web/tests/check-indirect-calls.py` lists the remaining casts whose wasm signatures
  differ. Casts laundered through `void*` only show up at run time, so expect a few more once real
  game data runs more code.
- `nod_iso` must be compiled with the thread flags, and the page's fatal overlay must show the
  real error.

Hosts that can't download GitHub source archives can use `platforms/web/fetch-deps.sh` with
`-DDUSK_WEB_DEPS_DIR`.

### Done

- **Portability fixes (Phase 1), all native-safe:**
  - `include/helpers/endian.h` recognises wasm as little-endian. Before, `BSWAP16/32` were no-ops
    there and `BSWAP64` was undefined.
  - The mod SDK record-size asserts follow pointer size (`sdk/include/mods/api.h`).
  - `OSSleepThread`/`OSWakeupThread` change thread state under the queue mutex, so a wakeup can no
    longer be lost between the sleeper's check and its wait.
  - The mods graphics service no longer calls Dawn-only `Device::GetAdapter`/`Adapter::GetInstance`
    on the web.
- **aurora and borealis patches (Phase 2–3),** carried in `platforms/web/patches` and applied at
  configure time by `platforms/web/apply-patches.sh`. Both apply cleanly to the pinned submodule
  commits.
  - **aurora:** MKWiii's changes ported to `3227d76`, plus:
    - `aurora_dvd_open_io`;
    - no nod preloader thread;
    - no `SDL_WaitEvent` while paused;
    - a skipped frame waits for the next animation frame;
    - `<webgpu/webgpu_cpp.h>` in the RmlUi headers;
    - Freetype kept for RmlUi.
  - **borealis:** `borealis::io::register_scheme`, so apps can expose non-file locations.
- **Disc (Phase 4):**
  - `platforms/web/nod_iso` reads plain GameCube ISO/GCM behind nod's C API, as decided for the
    case where nod has no wasm build. Wii and compressed images are rejected with a message.
  - `src/dusk/web/disc.*`: the disc is `webdisc://<name>`, read through a request slot in shared
    wasm memory. `platforms/web/shell/disc-worker.js` serves it, so any game thread can read
    synchronously and nothing waits on the main thread.
- **Page (Phase 4–5):** `platforms/web/shell`.
  - Browser checks.
  - The disc picker, with the player choosing to keep a copy in OPFS or use the file for the
    session.
  - Save backup and restore (zip).
  - A fatal-error overlay with log download.
  - `serve.mjs` and `_headers` for COOP/COEP.
  - User data persists through WasmFS's OPFS backend mounted at SDL's preference path
    (`/libsdl`).
- **Audio (Phase 5):** a mixer thread renders into a ring in shared memory, played by
  `audio-worklet.js`. Details are in `src/dusk/web/audio.hpp`.
- **Build:**
  - `platforms/web/cmake/WebOptions.cmake` and the `web` presets.
  - Link settings: JSPI, pthreads with a prespawned pool, WasmFS, page-owned memory,
    `GLOBAL_BASE` above aurora's 64 KiB texture sentinel.
  - Code mods off.
  - The CI job `build-web`.
- **Tests:**
  - `platforms/web/tests/run-native-tests.sh` runs `nod_iso`, the disc channel and the audio
    ring/mixer natively under ASan/UBSan. The channel and mixer also ran under TSan.
  - `platforms/web/tests/shell-test.mjs` runs the page in headless Chromium against a stand-in
    module. It covers:
    - both storage modes, reusing a stored disc, and removing it;
    - reads through the channel;
    - image checks;
    - backup/restore;
    - the audio worklet;
    - the unsupported-browser message.

### Found along the way

- After a reload, the previous page's disc worker can still hold the OPFS access handle. The new
  worker retries, and the page terminates its worker on `pagehide`.
- SDL3's Emscripten audio runs the stream callback on the browser main thread. With Dusklight's
  mixer that would deadlock in `Z2SoundMgr::stopSync`, hence the mixer thread.
- In optimized builds, Emscripten places static data at 1024, below aurora's `has_data()`
  sentinel, hence `-sGLOBAL_BASE=65536`.

### Next

- **Boot to the title screen from a real disc (Phase 4 exit).** Watch for:
  - more function-signature traps on code paths the synthetic disc doesn't reach;
  - JSPI and WebGPU feature gaps (`TextureComponentSwizzle`, compatibility limits);
  - V8's native stack limit on deep game call chains;
  - thread pool exhaustion.
- **Frame pacing.** Present waits for `requestAnimationFrame`, and the 30 fps `waitForTick` sleep
  stacks on top of it when interpolation is off.
- **Wii discs and compressed formats**, if a threaded wasm build of nod becomes possible.
- **The borealis HTTP backend over `fetch`,** for the mod catalog. CORS on twilitrealm.dev is
  needed.
