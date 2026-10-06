#include "follow_camera.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace dingosdk::multiplayer {
namespace {
using Clock = std::chrono::steady_clock;
float seconds(Clock::duration d) { return std::chrono::duration<float>(d).count(); }

// How the game's camera frames the local skater. The starting values are a typical skating
// frame until the first measurements arrive.
struct Profile {
    float distance = 3.4f; // horizontal, behind the skater
    float height = 1.5f;   // camera above the skater's root
    float pitch = -0.15f;  // radians; negative looks down
    float fov = 60.0f;
};
struct State {
    std::mutex mutex;
    Transform current, previous; // the local skater this tick and last tick
    bool have_current{}, have_previous{};
    Profile profile;
    Clock::time_point observed;
    unsigned samples{};
    bool logged{};
    // The spectated skater.
    std::uint64_t id{};
    Clock::time_point at;
    float hx{}, hz{1}; // direction of travel, eased
    std::array<float, 3> last{};
    // The player's own camera.
    CustomCamera custom;
    Clock::time_point custom_at;
    float custom_hx{}, custom_hz{1};
    std::array<float, 3> custom_last{}, custom_eye{};
    bool custom_running{};
};
State &state() {
    static auto *value = new State;
    return *value;
}
} // namespace

void note_local_skater(const Transform &root) noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.previous = s.current;
    s.have_previous = s.have_current;
    s.current = root;
    s.have_current = true;
}

void observe_gameplay_camera(std::uintptr_t base, const GameView &view) noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.have_previous || !view.camera) return;
    // Freecam, noclip, first person and spectate use other cameras: they say nothing about the game's.
    std::uintptr_t vtable{};
    if (!memory::peek(view.camera, vtable) || vtable != base + addr::engine::camera_vtable) return;
    const auto &skater = s.previous.position;
    const float dx = view.world[12] - skater[0], dy = view.world[13] - skater[1], dz = view.world[14] - skater[2];
    const float distance = std::sqrt(dx * dx + dz * dz);
    // Cutscenes, menus and teleports frame something else.
    if (!std::isfinite(distance) || distance < 0.5f || distance > 15.0f || dy < -2.0f || dy > 8.0f ||
        !(view.vertical_fov > 10.0f && view.vertical_fov < 150.0f))
        return;
    // Forward is minus the back row.
    const float pitch = std::asin(std::clamp(-view.world[9], -1.0f, 1.0f));
    const auto now = Clock::now();
    const float dt = s.samples ? std::clamp(seconds(now - s.observed), 0.0f, 0.1f) : 1.0f;
    s.observed = now;
    // A slow average: the game's camera swings about while skating; its usual framing is wanted.
    const float k = s.samples ? 1.0f - std::exp(-dt / 2.5f) : 1.0f;
    auto &p = s.profile;
    p.distance += (distance - p.distance) * k;
    p.height += (dy - p.height) * k;
    p.pitch += (pitch - p.pitch) * k;
    p.fov += (view.vertical_fov - p.fov) * k;
    if (++s.samples == 600 && !s.logged) {
        s.logged = true;
        logging::log(logging::Level::info, logging::Channel::ui,
                     "Spectate camera: the game frames the skater from {:.1f} m behind, {:.1f} m up, pitch {:.0f} degrees, FOV {:.0f}.",
                     p.distance, p.height, p.pitch * 57.2958f, p.fov);
    }
}

std::array<float, 16> follow_camera(std::uint64_t id, const Transform &root, float &fov) noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    const auto now = Clock::now();
    const auto &pos = root.position;
    if (s.id != id || now - s.at > std::chrono::milliseconds(500)) {
        // Start behind the way the skater faces; their travel takes over once they move.
        const auto body = to_matrix(root);
        float hx = body[8], hz = body[10];
        const float length = std::sqrt(hx * hx + hz * hz);
        if (length > .01f) { hx /= length; hz /= length; } else { hx = 0; hz = 1; }
        s.id = id;
        s.hx = hx;
        s.hz = hz;
        s.last = pos;
        s.at = now;
    }
    const float dt = std::clamp(seconds(now - s.at), 0.0005f, 0.1f);
    const float vx = (pos[0] - s.last[0]) / dt, vz = (pos[2] - s.last[2]) / dt;
    const float speed = std::sqrt(vx * vx + vz * vz);
    if (speed > 1.0f && speed < 60.0f) {
        // The heading follows the travel a little behind, so the camera swings round smoothly.
        const float turn = 1.0f - std::exp(-dt / 0.2f);
        float hx = s.hx + (vx / speed - s.hx) * turn, hz = s.hz + (vz / speed - s.hz) * turn;
        const float length = std::sqrt(hx * hx + hz * hz);
        if (length > .01f) { s.hx = hx / length; s.hz = hz / length; }
    }
    s.last = pos;
    s.at = now;
    // Rigidly at the game's own offset from the skater, so it never trails their position.
    const auto &p = s.profile;
    const std::array<float, 3> eye{pos[0] - s.hx * p.distance, pos[1] + p.height, pos[2] - s.hz * p.distance};
    const float cp = std::cos(p.pitch), sp = std::sin(p.pitch);
    const std::array<float, 3> back{-s.hx * cp, -sp, -s.hz * cp};
    std::array<float, 3> right{back[2], 0, -back[0]};
    const float flat = std::sqrt(right[0] * right[0] + right[2] * right[2]);
    if (flat > .001f) { right[0] /= flat; right[2] /= flat; } else right = {1, 0, 0};
    const std::array<float, 3> up{back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2],
                                  back[0] * right[1] - back[1] * right[0]};
    fov = p.fov;
    return {right[0], right[1], right[2], 0, up[0], up[1], up[2], 0, back[0], back[1], back[2], 0, eye[0], eye[1], eye[2], 1};
}

void set_custom_camera(const CustomCamera &camera) noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.custom = camera;
    if (!camera.on) s.custom_running = false;
}
CameraProfile gameplay_camera_profile() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return {s.profile.distance, s.profile.height, s.profile.pitch * 57.2958f, s.profile.fov, s.samples};
}
std::optional<std::array<float, 16>> custom_camera(float &fov) noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    const auto &c = s.custom;
    if (!c.on) return std::nullopt;
    const auto now = Clock::now();
    const auto &pos = c.position;
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) return std::nullopt;
    const bool fresh = !s.custom_running || now - s.custom_at > std::chrono::milliseconds(500);
    if (fresh) {
        // Start behind the way the skater faces; their travel takes over once they move.
        s.custom_hx = std::sin(c.heading / 57.2958f);
        s.custom_hz = std::cos(c.heading / 57.2958f);
        s.custom_last = pos;
        s.custom_at = now;
    }
    const float dt = std::clamp(seconds(now - s.custom_at), 0.0005f, 0.1f);
    const float vx = (pos[0] - s.custom_last[0]) / dt, vz = (pos[2] - s.custom_last[2]) / dt;
    const float speed = std::sqrt(vx * vx + vz * vz);
    if (speed > 1.0f && speed < 120.0f) {
        const float turn = 1.0f - std::exp(-dt / 0.25f);
        float hx = s.custom_hx + (vx / speed - s.custom_hx) * turn, hz = s.custom_hz + (vz / speed - s.custom_hz) * turn;
        const float length = std::sqrt(hx * hx + hz * hz);
        if (length > .01f) { s.custom_hx = hx / length; s.custom_hz = hz / length; }
    }
    s.custom_last = pos;
    s.custom_at = now;
    const float hx = s.custom_hx, hz = s.custom_hz;
    const float distance = std::clamp(c.distance, 0.2f, 30.0f), height = std::clamp(c.height, -2.0f, 20.0f);
    const float side = std::clamp(c.side, -5.0f, 5.0f), pitch = std::clamp(c.pitch, -85.0f, 85.0f) / 57.2958f;
    // Right of the direction of travel, on the ground plane.
    const std::array<float, 3> target{pos[0] - hx * distance - hz * side, pos[1] + height, pos[2] - hz * distance + hx * side};
    // The camera trails its place by `lag`; a teleport (or the first frame) snaps.
    const float gap = std::abs(target[0] - s.custom_eye[0]) + std::abs(target[1] - s.custom_eye[1]) + std::abs(target[2] - s.custom_eye[2]);
    if (fresh || c.lag < 0.005f || gap > 25.0f) s.custom_eye = target;
    else {
        const float k = 1.0f - std::exp(-dt / std::min(c.lag, 2.0f));
        for (std::size_t i = 0; i < 3; ++i) s.custom_eye[i] += (target[i] - s.custom_eye[i]) * k;
    }
    s.custom_running = true;
    const auto &eye = s.custom_eye;
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const std::array<float, 3> back{-hx * cp, -sp, -hz * cp};
    std::array<float, 3> right{back[2], 0, -back[0]};
    const float flat = std::sqrt(right[0] * right[0] + right[2] * right[2]);
    if (flat > .001f) { right[0] /= flat; right[2] /= flat; } else right = {1, 0, 0};
    const std::array<float, 3> up{back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2],
                                  back[0] * right[1] - back[1] * right[0]};
    fov = c.fov >= 20.0f && c.fov <= 150.0f ? c.fov : s.profile.fov;
    return std::array<float, 16>{right[0], right[1], right[2], 0, up[0], up[1], up[2], 0, back[0], back[1], back[2], 0, eye[0], eye[1], eye[2], 1};
}
} // namespace dingosdk::multiplayer
