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

# Everything is built with wasm threads (game threads are pthreads), native wasm exceptions and
# wasm setjmp/longjmp (libpng).
add_compile_options(-pthread -fwasm-exceptions -sSUPPORT_LONGJMP=wasm)
add_link_options(-pthread -fwasm-exceptions -sSUPPORT_LONGJMP=wasm)

# Wasm memory: the page creates it (so its disc worker can see it) with these sizes, in 64 KiB
# pages. MEM1 alone is 256 MiB (m_Do_main.cpp).
set(DUSK_WEB_INITIAL_MEMORY_PAGES 16384) # 1 GiB
set(DUSK_WEB_MAXIMUM_MEMORY_PAGES 65536) # 4 GiB

# Configures the dusklight executable target for the browser.
function(dusk_configure_web_target target)
    math(EXPR _initial_bytes "${DUSK_WEB_INITIAL_MEMORY_PAGES} * 65536")
    math(EXPR _maximum_bytes "${DUSK_WEB_MAXIMUM_MEMORY_PAGES} * 65536")
    target_sources(${target} PRIVATE
            "${CMAKE_SOURCE_DIR}/src/dusk/web/audio.cpp"
            "${CMAKE_SOURCE_DIR}/src/dusk/web/disc.cpp"
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
            # Static data stays above 64 KiB: aurora treats texture pointers below 0x10000 as
            # placeholder IDs (lib/gfx/texture.hpp, has_data()).
            -sGLOBAL_BASE=65536
            -sSTACK_SIZE=8MB
            -sDEFAULT_PTHREAD_STACK_SIZE=1MB
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
    )

    # The page and its helpers, next to dusklight.js/.wasm/.data.
    set(DUSK_WEB_JS "dusklight.js")
    configure_file("${DUSK_WEB_DIR}/shell/index.html.in" "${CMAKE_CURRENT_BINARY_DIR}/web/index.html" @ONLY)
    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/web/index.html"
                "${DUSK_WEB_DIR}/shell/shell.js"
                "${DUSK_WEB_DIR}/shell/shell.css"
                "${DUSK_WEB_DIR}/shell/disc-worker.js"
                "${DUSK_WEB_DIR}/shell/audio-worklet.js"
                "${DUSK_WEB_DIR}/shell/_headers"
                "${DUSK_WEB_DIR}/shell/serve.mjs"
                "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Copying the web page"
            VERBATIM
    )
endfunction()
