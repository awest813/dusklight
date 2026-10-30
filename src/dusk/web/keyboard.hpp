#pragma once

// The web build's default keyboard layout. Only compiled there.
//
// Dusklight has no keyboard layout of its own, but in a browser most players have only a keyboard,
// so port 1 starts with this one (docs/web.md lists it):
//
//   Control stick  W A S D          A      Space      L      Left Shift
//   C-stick        Arrow keys       B      E          R      C
//   D-pad          1 2 3 4          X      Q          Z      Z
//                  (up down         Y      F          Start  Enter
//                   left right)
//
// It leaves out Ctrl, Alt and Cmd: Ctrl+W or Cmd+W would close the tab, which a page can't
// prevent. L and R also press their analog triggers fully, as a GameCube trigger does when clicked.

#include <cstdint>

namespace dusk::web {

// Binds the layout to `port`'s keyboard bindings.
void apply_keyboard_layout(uint32_t port);

// On a first run (no keyboard bindings saved yet), turns the keyboard on for port 1 with the
// layout, and saves it. Call after PADInit, which loads the saved bindings.
void use_default_keyboard_layout();

}  // namespace dusk::web
