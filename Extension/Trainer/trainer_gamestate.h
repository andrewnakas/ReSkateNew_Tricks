#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace dingosdk::trainer {
// The animation system's game states the trainer reads and writes for the local skater.
//
// A flip trick's clip plays at Float.Anim.TrickFlipSpeed, a number the game sets once when the
// board is flicked. The trainer multiplies that number while the trick is in the air: nothing
// that sets the pop reads it, and it works for every trick (EnumGS.Anim.CurrentFlipTrick says
// which one is turning).
struct FlipState {
    std::uintptr_t speed_at{}; // where the live flip speed is; 0: not found
    std::uint32_t trick{};     // CurrentFlipTrick
    float speed{};
    explicit operator bool() const noexcept { return speed_at != 0; }
};
FlipState read_flip_state(std::uintptr_t base, std::uintptr_t entity) noexcept;
// The state objects are loaded with a level: the memory search (trainer_classes.cpp) hands
// every region it reads to this, which keeps the ones it knows. True once both are known.
void find_state_objects(std::uintptr_t start, std::size_t size) noexcept;
bool flip_states_found() noexcept;
// The skater's pages of game states (0 where there is none).
std::array<std::uintptr_t, 6> state_pages(std::uintptr_t base, std::uintptr_t entity) noexcept;
bool write_flip_speed(const FlipState &state, float speed) noexcept;

// Pumping a transition, as the game's trick scripts see it: whether the skater counts as pumping
// this tick and how hard (Bool.Intent.Pumping, Float.Intent.UserTriggeredPumpStrength,
// Float.Anim.PumpingAssistAmount).
struct PumpState {
    bool found{};   // the states are known and the skater has them
    bool pumping{};
    float strength{}, assist{};
};
PumpState read_pump_state(std::uintptr_t base, std::uintptr_t entity) noexcept;

// The tricks that have a speed of their own under "Advanced trick speed". A nollie turns at
// its regular trick's speed.
struct FlipTrick {
    std::string_view key;   // the option and preset name
    std::string_view label; // in the menu
};
inline constexpr std::array<FlipTrick, 16> flip_tricks{{
    {"flip.kickflip", "Kickflip"},
    {"flip.heelflip", "Heelflip"},
    {"flip.popshuvit", "Pop shuvit"},
    {"flip.fspopshuvit", "FS pop shuvit"},
    {"flip.360popshuvit", "360 pop shuvit"},
    {"flip.fs360popshuvit", "FS 360 pop shuvit"},
    {"flip.varialkickflip", "Varial kickflip"},
    {"flip.varialheelflip", "Varial heelflip"},
    {"flip.360flip", "360 flip"},
    {"flip.laserflip", "Laser flip"},
    {"flip.hardflip", "Hardflip"},
    {"flip.inwardheelflip", "Inward heelflip"},
    {"flip.360hardflip", "360 hardflip"},
    {"flip.360inwardheelflip", "360 inward heelflip"},
    {"flip.impossible", "Impossible"},
    {"flip.frontfootimpossible", "Front foot impossible"},
}};
// Which of those a CurrentFlipTrick value belongs to; -1: none (an ollie, a wallie, an underflip).
int flip_trick_index(std::uint32_t trick) noexcept;
// The name the game gives a CurrentFlipTrick value ("Kickflip", "N_360Flip"); empty when unknown.
std::string_view flip_trick_name(std::uint32_t trick) noexcept;
}
