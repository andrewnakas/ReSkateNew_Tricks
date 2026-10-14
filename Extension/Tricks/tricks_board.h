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
// Keeps a held pose's target level and on the heading it was taken with: 1 a casper, 2 a primo, 0 off.
void set_board_level(int mode) noexcept;
// The height of a held casper's forward row (the pose's own tilt; 0.30 as measured).
void set_board_level_tilt(float rise) noexcept;
std::uint32_t take_board_level() noexcept;
// <dev> The range of the height (y) of the target frame's right, up and forward rows while watched: min, max of each.
void set_board_lean_watch(bool on) noexcept;
bool take_board_lean(float values[6]) noexcept;
// </dev>
// Holds the board's angular drive to its animated pose at this strength (the game's own is 1; 0 gives it back).
void set_board_rigid(float strength) noexcept;
// How many steps it was held since the last call, and the strength found there the first time (below 0: never seen).
std::uint32_t take_board_rigid(float &first_seen) noexcept;
// Lifts the board up its own up axis, most at the middle of the running turn (0 at both ends).
void set_board_hop(float metres) noexcept;
bool board_spin_running() noexcept;
// <dev> Calls of the second root update (0x4779330, any skater) since the last call.
std::uint32_t take_root_update2_calls() noexcept;
// How often the hook saw the local skater's frame since last asked, and how often it turned it.
void board_hook_counts(std::uint32_t &seen, std::uint32_t &turned) noexcept;
} // namespace dingosdk::tricks
