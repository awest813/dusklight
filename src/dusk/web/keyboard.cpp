#include "keyboard.hpp"

#include "dusk/main.h"

#include <SDL3/SDL_scancode.h>
#include <dolphin/pad.h>

#include <array>
#include <filesystem>
#include <system_error>

namespace dusk::web {
namespace {

std::array<PADKeyButtonBinding, PAD_BUTTON_COUNT> layout_buttons() {
    return {{
        {SDL_SCANCODE_SPACE, PAD_BUTTON_A},
        {SDL_SCANCODE_E, PAD_BUTTON_B},
        {SDL_SCANCODE_Q, PAD_BUTTON_X},
        {SDL_SCANCODE_F, PAD_BUTTON_Y},
        {SDL_SCANCODE_RETURN, PAD_BUTTON_START},
        {SDL_SCANCODE_Z, PAD_TRIGGER_Z},
        {SDL_SCANCODE_LSHIFT, PAD_TRIGGER_L},
        {SDL_SCANCODE_C, PAD_TRIGGER_R},
        {SDL_SCANCODE_1, PAD_BUTTON_UP},
        {SDL_SCANCODE_2, PAD_BUTTON_DOWN},
        {SDL_SCANCODE_3, PAD_BUTTON_LEFT},
        {SDL_SCANCODE_4, PAD_BUTTON_RIGHT},
    }};
}

std::array<PADKeyAxisBinding, PAD_AXIS_COUNT> layout_axes() {
    return {{
        {SDL_SCANCODE_D, PAD_AXIS_LEFT_X_POS, 0},
        {SDL_SCANCODE_A, PAD_AXIS_LEFT_X_NEG, 0},
        {SDL_SCANCODE_W, PAD_AXIS_LEFT_Y_POS, 0},
        {SDL_SCANCODE_S, PAD_AXIS_LEFT_Y_NEG, 0},
        {SDL_SCANCODE_RIGHT, PAD_AXIS_RIGHT_X_POS, 0},
        {SDL_SCANCODE_LEFT, PAD_AXIS_RIGHT_X_NEG, 0},
        {SDL_SCANCODE_UP, PAD_AXIS_RIGHT_Y_POS, 0},
        {SDL_SCANCODE_DOWN, PAD_AXIS_RIGHT_Y_NEG, 0},
        {SDL_SCANCODE_LSHIFT, PAD_AXIS_TRIGGER_L, 0},
        {SDL_SCANCODE_C, PAD_AXIS_TRIGGER_R, 0},
    }};
}

}  // namespace

void apply_keyboard_layout(uint32_t port) {
    auto buttons = layout_buttons();
    auto axes = layout_axes();
    PADSetKeyButtonBindings(port, buttons.data());
    PADSetKeyAxisBindings(port, axes.data());
}

void use_default_keyboard_layout() {
    // aurora keeps every port's keyboard bindings in this file (lib/dolphin/pad/pad.cpp), written
    // whenever the player changes them, so its absence means a first run.
    std::error_code ec;
    if (std::filesystem::exists(ConfigPath / "keyboard_bindings.dat", ec) || ec) {
        return;
    }
    apply_keyboard_layout(PAD_CHAN0);
    PADSetKeyboardActive(PAD_CHAN0, TRUE);
    PADSerializeMappings();
}

}  // namespace dusk::web
