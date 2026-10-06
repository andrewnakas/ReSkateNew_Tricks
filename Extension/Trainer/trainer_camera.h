#pragma once
// The game's own follow cameras, moved: where each one sits is a handful of numbers in the
// game's camera framing data (the offsets SunJay's Low Cam mod changes on disk). The trainer
// finds the loaded copies and writes the player's numbers into them, so the game's camera keeps
// its smoothing and wall avoidance and simply sits somewhere else.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dingosdk::trainer {
// The three cameras a player sees: the low and the high one on the board, and the one on foot.
enum class CameraRig : std::uint8_t { low, high, foot, count };
inline constexpr std::size_t camera_rigs = static_cast<std::size_t>(CameraRig::count);
struct RigSetting {
    float distance{1}; // x the game's own distance behind the skater
    float height{};    // metres added to the point the camera turns about (camera and aim together)
    float raise{};     // metres added to the camera alone: it looks further down, or up
    float side{1};     // x the game's own sideways offset
    bool operator==(const RigSetting &) const = default;
};
// What each camera should be. Any thread.
void want_camera_rigs(const std::array<RigSetting, camera_rigs> &settings) noexcept;
// Game thread, every tick while a level is loaded: starts the search for the framing data when
// something is wanted and none is known, and writes what is wanted. Returns the numbers written.
std::size_t apply_camera_rigs(std::uint64_t now) noexcept;
// A level change frees the framing data: forget it.
void forget_camera_rigs() noexcept;
// "low 6, high 8, foot 6 framings found" for the menu and the log.
std::string camera_rigs_summary();
} // namespace dingosdk::trainer
