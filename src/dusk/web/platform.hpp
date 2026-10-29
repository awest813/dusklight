#pragma once

// Browser-specific setup for the web build (Emscripten). Only compiled there.

namespace dusk::web {

// Run first thing in main(): makes user data persistent and attaches the disc image the page
// handed over (see disc.hpp).
void initialize();

// The web build's "restart": reload the page, which reattaches the stored disc image.
void reload();

}  // namespace dusk::web
