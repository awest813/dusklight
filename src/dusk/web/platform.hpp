#pragma once

// Browser-specific setup for the web build (Emscripten). Only compiled there.

#include <cstdint>

namespace dusk::web {

// Run first thing in main(): makes user data persistent and attaches the disc image the page
// handed over (see disc.hpp).
void initialize();

// The web build's "restart": reload the page, which reattaches the stored disc image.
void reload();

// Sleeps for `nanoseconds` to within about 0.2 ms, letting the browser run meanwhile. Browsers
// round setTimeout (and so emscripten_sleep and SDL_Delay) up to about 4 ms for short sleeps, which
// is too coarse for frame pacing. Main thread only (it suspends the game loop's JSPI stack).
void precise_sleep(uint64_t nanoseconds);

}  // namespace dusk::web
