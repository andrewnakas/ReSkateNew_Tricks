#pragma once
// The procedural board: turns added to the pose the animation asks the board to take.
//
// While the board follows its clip (physics states 103, 200, 201) the rig writes a target frame (the animated
// board matrix in the world) into the skater's context, and each step 0x47c3320 copies it to the board's angular
// drive. Hooked, the frame of the local skater is turned about the board's own axes on the way: a roll is a
// kickflip's turn, a yaw a shuvit's, a pitch a dolphin flip's. The clip's own motion stays underneath, so a spin
// adds to whatever trick is playing.
#include <cstdint>

namespace dingosdk::tricks {
// Safe to call every tick; false while the game's code is not the known one.
bool start_board_hook(std::uintptr_t base) noexcept;
// Which skater's board (its physics context, where the target frame is); 0: none. Game thread, every tick.
void set_board_context(std::uintptr_t context) noexcept;
// Turn the board by these angles (degrees; about its long axis, its up axis and its sideways axis) spread evenly
// over `milliseconds`, starting now. A new spin replaces the one running.
void start_board_spin(float roll, float yaw, float pitch, std::uint32_t milliseconds) noexcept;
void stop_board_spin() noexcept;
// After start_board_spin: the board is also moved down its own up axis by this much as the turn goes.
void set_board_drop(float metres) noexcept;
bool board_spin_running() noexcept;
// How often the hook saw the local skater's frame since last asked, and how often it turned it.
void board_hook_counts(std::uint32_t &seen, std::uint32_t &turned) noexcept;
} // namespace dingosdk::tricks
