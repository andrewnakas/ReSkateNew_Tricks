#pragma once
#include "Engine/Game/UI/game_view.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include <array>
#include <cstdint>
#include <optional>

// The spectate camera (party Spectate and throwdown turns). It frames another skater the way
// the game's own camera frames the local one: while the player skates, the game camera's
// distance behind the skater, height, pitch and FOV are measured and averaged, and a
// spectated skater gets the same, behind their direction of travel.
namespace dingosdk::multiplayer {
// Client thread, every tick: where the local skater stands.
void note_local_skater(const Transform &root) noexcept;
// Client thread, once per tick after the tick's camera was read (Engine/Game/UI/game_view.h).
// That read holds the camera the game placed after the previous tick, so it is measured
// against the local skater of the previous tick. Only the game's own camera is measured.
void observe_gameplay_camera(std::uintptr_t base, const GameView &view) noexcept;
// The camera for spectating `id` standing at `root` this frame (world matrix rows: right, up,
// back, position), and the FOV to give it.
std::array<float, 16> follow_camera(std::uint64_t id, const Transform &root, float &fov) noexcept;

// The player's own camera (the trainer's Camera tab): a follow camera behind the local skater's
// direction of travel at a distance, height and tilt of the player's choosing, in place of the
// game's. It does not avoid walls.
struct CustomCamera {
    bool on{};
    float distance{3.4f}; // metres behind the skater
    float height{1.5f};   // metres above the skater's root
    float pitch{-9.0f};   // degrees; negative looks down
    float side{};         // metres to the right: over the shoulder
    float fov{};          // degrees; 0: the game's own
    float lag{0.08f};     // seconds the camera trails the skater by
    // Where the local skater is this tick, from whoever switches the camera on.
    std::array<float, 3> position{};
    float heading{}; // degrees, 0 = +Z, clockwise seen from above: used until the skater moves
};
// Any thread.
void set_custom_camera(const CustomCamera &camera) noexcept;
// Client thread, every tick: the camera to hold, or nothing while it is off or there is no skater.
std::optional<std::array<float, 16>> custom_camera(float &fov) noexcept;
// How the game's own camera has been framing the local skater (see observe_gameplay_camera).
struct CameraProfile {
    float distance{}, height{}, pitch{}, fov{}; // pitch in degrees
    unsigned samples{};
};
CameraProfile gameplay_camera_profile() noexcept;
} // namespace dingosdk::multiplayer
