# Web build (Emscripten) configuration. Included by the top-level CMakeLists.txt before aurora and
# borealis are added; see docs/web-port-plan.md.

if (NOT EMSCRIPTEN)
    return()
endif ()

set(DUSK_WEB_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

# aurora and borealis need small web changes, carried as patches (platforms/web/patches). Applying
# them is idempotent and they only affect web builds, so do it here rather than asking every
# developer to remember.
execute_process(
        COMMAND bash "${DUSK_WEB_DIR}/apply-patches.sh"
        RESULT_VARIABLE _dusk_web_patch_result
)
if (NOT _dusk_web_patch_result EQUAL 0)
    message(FATAL_ERROR "dusklight: failed to apply the web patches to extern/aurora and extern/borealis")
endif ()

# Pre-fetched dependency checkouts (platforms/web/fetch-deps.sh), for hosts that cannot download
# the pinned GitHub source archives. Each one replaces that dependency's download.
set(DUSK_WEB_DEPS_DIR "" CACHE PATH "Pre-fetched web build dependencies (see platforms/web/fetch-deps.sh)")
if (DUSK_WEB_DEPS_DIR)
    foreach (_dep IN ITEMS ABSEIL-CPP:abseil-cpp XXHASH:xxhash FMT:fmt ZLIB:zlib-ng PNG:libpng
            FREETYPE:freetype IMGUI:imgui RMLUI:rmlui TRACY:tracy SDL:sdl3 PICOSHA2:picosha2
            CXXOPTS:cxxopts)
        string(REPLACE ":" ";" _dep "${_dep}")
        list(GET _dep 0 _name)
        list(GET _dep 1 _subdir)
        if (EXISTS "${DUSK_WEB_DEPS_DIR}/${_subdir}")
            set(FETCHCONTENT_SOURCE_DIR_${_name} "${DUSK_WEB_DEPS_DIR}/${_subdir}" CACHE PATH "" FORCE)
        endif ()
    endforeach ()
endif ()

# Everything (including dusk_nod_iso below) is built with wasm threads (game threads are pthreads),
# native wasm exceptions, wasm setjmp/longjmp (libpng) and 128-bit SIMD, which the compiler uses to
# vectorize loops such as the audio DSP and texture conversion. Every browser with WebGPU has SIMD.
add_compile_options(-pthread -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -msimd128)
add_link_options(-pthread -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -msimd128)

# Code mods patch native code at runtime, which wasm cannot do.
set(DUSK_ENABLE_CODE_MODS OFF CACHE BOOL "" FORCE)
set(DUSK_ENABLE_OPUS OFF CACHE BOOL "" FORCE)

# Static libraries only; SDL3 from source (it has an Emscripten backend).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(AURORA_SDL3_PROVIDER vendor CACHE STRING "" FORCE)
set(AURORA_SDL3_LINKAGE static CACHE STRING "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)

# nod (Rust) has no threaded wasm build, so the web build reads plain GameCube ISO/GCM images with
# nod_iso, which implements the same C API. Defining nod::nod first makes aurora and borealis use it.
set(AURORA_NOD_PROVIDER vendor CACHE STRING "" FORCE)
add_library(dusk_nod_iso STATIC "${DUSK_WEB_DIR}/nod_iso/nod_iso.cpp")
target_include_directories(dusk_nod_iso PUBLIC "${DUSK_WEB_DIR}/nod_iso/include")
target_compile_features(dusk_nod_iso PRIVATE cxx_std_20)
add_library(nod::nod ALIAS dusk_nod_iso)

# borealis: no native crash handler, Discord IPC, Sentry or libcurl in a browser.
set(BOREALIS_ENABLE_CRASH OFF CACHE BOOL "" FORCE)
set(BOREALIS_ENABLE_DISCORD OFF CACHE BOOL "" FORCE)
set(BOREALIS_ENABLE_SENTRY OFF CACHE BOOL "" FORCE)
set(BOREALIS_HTTP_BACKEND none CACHE STRING "" FORCE)
set(BOREALIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# Wasm memory, in 64 KiB pages. MEM1 alone is 256 MiB (m_Do_main.cpp).
set(DUSK_WEB_INITIAL_MEMORY_PAGES 16384) # 1 GiB
set(DUSK_WEB_MAXIMUM_MEMORY_PAGES 65536) # 4 GiB

# Configures the dusklight executable target for the browser.
function(dusk_configure_web_target target)
    math(EXPR _initial_bytes "${DUSK_WEB_INITIAL_MEMORY_PAGES} * 65536")
    math(EXPR _maximum_bytes "${DUSK_WEB_MAXIMUM_MEMORY_PAGES} * 65536")
    target_sources(${target} PRIVATE
            "${CMAKE_SOURCE_DIR}/src/dusk/web/audio.cpp"
            "${CMAKE_SOURCE_DIR}/src/dusk/web/disc.cpp"
            "${CMAKE_SOURCE_DIR}/src/dusk/web/keyboard.cpp"
            "${CMAKE_SOURCE_DIR}/src/dusk/web/platform.cpp"
    )
    set_target_properties(${target} PROPERTIES SUFFIX ".js")
    target_link_options(${target} PRIVATE
            # main() never returns to the browser on its own: it runs as a JSPI export, and the
            # places that wait (presenting, GPU progress, frame pacing) suspend to the event loop.
            -sJSPI
            # The page starts main() once the disc worker is attached (shell.js).
            -sINVOKE_RUN=0
            "-sEXPORTED_FUNCTIONS=['_main','_dusk_web_disc_channel']"
            "-sEXPORTED_RUNTIME_METHODS=['callMain']"
            -sINITIAL_MEMORY=${_initial_bytes}
            -sMAXIMUM_MEMORY=${_maximum_bytes}
            -sALLOW_MEMORY_GROWTH=1
            # Not -sGROWABLE_ARRAYBUFFERS: it would spare the JS side a growth check on each memory
            # access, but WebGPU rejects views of resizable buffers ("must not be resizable"), and
            # the WebGPU bindings pass such views on most calls (setBindGroup, writeBuffer, ...).
            -Wno-pthreads-mem-growth
            # Static data stays above 64 KiB: aurora treats texture pointers below 0x10000 as
            # placeholder IDs (lib/gfx/texture.hpp, has_data()).
            -sGLOBAL_BASE=65536
            -sSTACK_SIZE=8MB
            -sDEFAULT_PTHREAD_STACK_SIZE=2MB
            # Game threads (DVD, ARAM, decompression, audio streaming, memory card, movies), the
            # aurora DVD worker, the borealis task pool and WasmFS's OPFS worker. A thread that is
            # not prespawned cannot start while the main thread waits on it.
            -sPTHREAD_POOL_SIZE=24
            -sWASMFS
            -sENVIRONMENT=web,worker
            -sEXIT_RUNTIME=1
            # Function names in the module keep stacks and crash reports readable.
            --profiling-funcs
            "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/res@/res"
            # Exposes the module's memory to the page (Module.duskMemory()).
            "SHELL:--pre-js ${DUSK_WEB_DIR}/shell/pre.js"
    )
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${DUSK_WEB_DIR}/shell/pre.js")

    # The page and its helpers, next to dusklight.js/.wasm/.data.
    set(DUSK_WEB_JS "dusklight.js")
    # A checkout without tags (a shallow clone) has no describable version; name the commit instead.
    set(DUSK_WEB_VERSION "${BOREALIS_APP_DESCRIBE}")
    if (NOT DUSK_WEB_VERSION OR DUSK_WEB_VERSION STREQUAL "UNKNOWN-VERSION")
        string(SUBSTRING "${BOREALIS_APP_REVISION}" 0 9 DUSK_WEB_VERSION)
    endif ()
    configure_file("${DUSK_WEB_DIR}/shell/index.html.in" "${CMAKE_CURRENT_BINARY_DIR}/web/index.html" @ONLY)
    # A separate target that runs on every build, so page changes are copied even when the module
    # does not relink.
    add_custom_target(${target}_web_page ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/web/index.html"
                "${DUSK_WEB_DIR}/shell/shell.js"
                "${DUSK_WEB_DIR}/shell/shell.css"
                "${DUSK_WEB_DIR}/shell/disc-worker.js"
                "${DUSK_WEB_DIR}/shell/audio-worklet.js"
                "${DUSK_WEB_DIR}/shell/coi-sw.js"
                # Page-sized copies of res/logo.png and res/icon.png.
                "${DUSK_WEB_DIR}/shell/logo.png"
                "${DUSK_WEB_DIR}/shell/icon.png"
                "${DUSK_WEB_DIR}/shell/_headers"
                "${DUSK_WEB_DIR}/shell/serve.mjs"
                "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Copying the web page"
            VERBATIM
    )
    add_dependencies(${target} ${target}_web_page)
endfunction()
