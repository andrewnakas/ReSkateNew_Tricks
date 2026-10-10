#pragma once
// No comply and boneless heights. The game's trick scripts launch these two through its
// jump-trajectory setter with a fixed upward speed, and no tuning value is read for it:
// the trainer multiplies that speed as the trajectory is set.
#include <cstdint>
// <dev>
#include <string>
// </dev>

namespace dingosdk::trainer {
enum class Trick : std::uint8_t { none, no_comply, boneless };
struct TrickLaunch {
    Trick trick{};
    float up_speed{}, factor{1}; // the game's own upward speed, and what it was multiplied by
};
// Installs the hook once the game's code is recognised. Safe to call every tick.
bool start_trick_heights(std::uintptr_t base) noexcept;
// Height multipliers (1: the game's own height). The launch speed is scaled by their roots.
void set_trick_heights(float no_comply, float boneless) noexcept;
// The pop out of a manual: a height multiplier, and Enum.Intent.Manual as read this tick (above 0: in a manual).
void set_manual_pop(float height, std::int32_t intent) noexcept;
// The scripted launches since the last call, oldest first; false when there are none.
bool take_trick_launch(TrickLaunch &out) noexcept;
// <dev>
// Every call of the trajectory setter since the last asked (at most 32), for finding launches.
std::string trajectory_calls();
// The same calls one at a time, for the capture log (caller as the return address's rva; 0xffff: no state).
struct TrajectoryCall {
    std::uint32_t caller{}, state{}, previous{};
    float up{}, forward{};
};
bool take_trajectory_call(TrajectoryCall &out) noexcept;
// The newest call's first 64 floats (four to a group; the launch velocity is floats 32 to 34).
std::string trajectory_parameters();
// Test levers: scale the ollie launch, and overwrite one float of every call (slot < 0: none).
void trajectory_test(float scale, int slot, float value) noexcept;
// </dev>
} // namespace dingosdk::trainer
