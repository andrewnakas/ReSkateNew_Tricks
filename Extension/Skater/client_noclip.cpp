#include "client_source_spawn.h"
#include "client_source_spawn_internal.h"
#include "no_bail.h"
#include "offboard_flight.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/offboard_flight.h"
#include "free_flight.h"
#include <cmath>
#include <format>
#include <mutex>
#include <string>

namespace dingosdk::client_source::detail {
namespace {
// Physics bodies already verified writable (a VirtualQuery each), so the
// every-frame and every-simulation-step body reads below skip that system call.
// A body at a new address, and every body once a second, is checked again.
// Only touched with SourceState::busy held, which every caller holds.
struct WritableBodies {
    std::array<std::uintptr_t, 32> bodies{};
    ULONGLONG until{};
};
WritableBodies& writable_bodies() { static WritableBodies value; return value; }
bool body_writable(std::size_t index, std::uintptr_t body) {
    auto& cache = writable_bodies();
    const auto now = GetTickCount64();
    if (now >= cache.until) {
        cache.bodies = {};
        cache.until = now + 1000;
    }
    if (cache.bodies[index] == body) return true;
    if (!source_writable(body + 0x60, 0x20)) return false;
    cache.bodies[index] = body;
    return true;
}
bool copy_to(std::uintptr_t address, const void* data, std::size_t size) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void*>(address), data, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// debug_write for a physics body field inside the region debug_noclip_bodies
// verified writable: a guarded copy and read-back instead of VirtualQuery,
// WriteProcessMemory and a ReadProcessMemory on every simulation step.
template<class T> void body_write(std::uintptr_t address, const T& value) {
    source_require(source_range(address, sizeof(T)) && copy_to(address, &value, sizeof(T)), "Debug setting write failed.");
    T written{};
    source_require(memory::peek(address, written) && written == value, "Debug setting write could not be verified.");
}
}
void debug_stop_noclip(InteractiveDebug& debug) noexcept {
    debug.noclip = false;
    debug.noclip_entity = 0;
    debug.noclip_velocity.valid = false;
    clear_no_bail_flight();
}
NoclipBodies debug_noclip_bodies(std::uintptr_t base, std::uintptr_t client, std::uintptr_t entity) {
    overlay::DebugModel skater;
    (void)debug_skater(base, client, skater);
    source_require(skater.skater_identity == entity, "Skater changed; flight stopped.");
    SourceReader reader;
    const auto collection = reader.pointer(entity, 0x70);
    const auto component = reader.pointer(entity, 0x628);
    source_require(reader.pointer(component) == base + addr::engine::skater_component_vtable && reader.pointer(component, 0x18) == collection,
        "Skater physics ownership changed.");
    NoclipBodies result;
    result.root = skater.skater_position;
    result.core = reader.pointer(component, 0x70);
    source_require(reader.pointer(result.core) == base + addr::no_bail::bail_core_vtable, "Skater physics is unavailable.");
    const auto board = reader.pointer(reader.pointer(result.core, 0x430), 0x18);
    result.rig_wrapper = reader.pointer(result.core, 0x438);
    const auto rig = reader.pointer(result.rig_wrapper, 0x2f10);
    result.context = reader.pointer(result.core, 0x3c0);
    source_require(reader.pointer(result.rig_wrapper) == result.context && reader.pointer(result.rig_wrapper, 0x4630) == result.core,
        "Skater motion ownership changed.");
    result.seconds = reader.value<float>(result.context, 0x17ec);
    source_require(std::isfinite(result.seconds) && result.seconds >= 0 && result.seconds <= .1f, "Invalid physics timestep.");
    result.offboard = reader.pointer(reader.pointer(result.core, 0x3b0)) == base + addr::offboard_flight::offboard_flight_vtable;
    // Read the committed native state, not the selector's proposed next state
    // (which is remapped by its caller and may be stale between selections).
    const auto physics = reader.value<std::uint32_t>(result.context, 0x1414);
    result.wipeout = physics == addr::no_bail::wipeout_physics_state ||
        physics == addr::no_bail::offboard_physics_state;
    source_require(reader.pointer(board) == base + spawn::board_physics_vtable && reader.pointer(rig) == base + spawn::rig_physics_vtable,
        "Unsupported board or skeleton physics.");
    const auto board_parts = reader.pointer(board, 0x20), rig_parts = reader.pointer(rig, 0x20);
    source_require(reader.value<std::uint32_t>(board_parts, 0) == 9 && reader.value<std::uint32_t>(rig_parts, 0) == 26,
        "Unsupported physics body layout.");
    for (std::size_t i = 0; i < result.parts.size(); ++i) {
        const auto body = i < 9 ? board_parts + i * 0x130 : rig_parts + (i - 8) * 0x130;
        source_require(reader.pointer(body, 0x10) == (i < 9 ? board : rig) && body_writable(i, body),
            "Physics body ownership changed.");
        const auto velocity = reader.value<std::array<float, 3>>(body, 0x70);
        for (const auto v : velocity) source_require(std::isfinite(v) && std::abs(v) <= 100000,
            "Invalid physics velocity.");
        result.parts[i] = body;
    }
    result.board_height = reader.value<float>(board_parts, 0x54);
    source_require(std::isfinite(result.board_height) && std::abs(result.board_height) <= 1000000, "Invalid board altitude.");
    reader.verify();
    return result;
}
namespace {
void noclip_apply_velocity(std::uintptr_t core) noexcept {
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || !state.velocity_guard_active.load(std::memory_order_acquire) ||
        state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    auto& debug = state.trial.debug;
    const auto apply_boost = [&](InteractiveDebug::VelocityRequest& boost, std::uint64_t& updates, bool up, bool offboard = false) {
        if (!boost.valid || boost.core != core) return;
        try {
            if (offboard && boost.applied && GetTickCount64() >= boost.expires) {
                boost.valid = false;
                state.offboard_boost_core.store(0, std::memory_order_release);
                return;
            }
            source_require(GetTickCount64() < boost.expires, "Velocity boost timed out.");
            const auto bodies = debug_noclip_bodies(state.trial.base, boost.client, boost.entity);
            source_require(bodies.core == core, "Skater physics was replaced before the velocity boost could apply.");
            source_require(offboard ? (boost.applied || bodies.offboard || bodies.wipeout) : !bodies.offboard,
                offboard ? "Off-board Up Boost requires walking, falling or gliding." :
                "Velocity boosts require the skater to be on the board.");
            source_require(!debug.noclip && !debug.park_editor, "Velocity boost cancelled while editing or flying.");
            SourceReader reader;
            source_require(reader.value<std::uint8_t>(boost.entity, 0x7e0) == 0,
                "Wait for the current teleport before using velocity boosts.");
            std::array<std::array<float, 3>, 32> velocities{};
            std::array<std::uint32_t, 32> flags{};
            const std::size_t first = offboard ? 9 : 0; // Leave the carried/detached board alone.
            if (offboard && !boost.applied) {
                const auto current_up = reader.value<float>(bodies.parts[9], 0x74);
                boost.velocity[1] = std::max(boost.velocity[1], current_up + boost.velocity[1]);
            }
            for (std::size_t i = first; i < bodies.parts.size(); ++i) {
                velocities[i] = reader.value<std::array<float, 3>>(bodies.parts[i], 0x70);
                flags[i] = reader.value<std::uint32_t>(bodies.parts[i], 0x60);
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    if (offboard && axis == 1) velocities[i][axis] = std::max(velocities[i][axis], boost.velocity[1]);
                    else velocities[i][axis] += boost.velocity[axis];
                    source_require(std::isfinite(velocities[i][axis]) && std::abs(velocities[i][axis]) <= 100000,
                        "Velocity boost produced an invalid physics velocity.");
                }
            }
            reader.verify();
            if (offboard && bodies.offboard)
                source_require(ensure_offboard_up_velocity(state.trial.base, core, bodies.context,
                    bodies.rig_wrapper, boost.velocity[1]), "Off-board boost motion ownership changed.");
            for (std::size_t i = first; i < bodies.parts.size(); ++i) {
                body_write(bodies.parts[i] + 0x70, velocities[i]);
                body_write(bodies.parts[i] + 0x60, flags[i] | 8u);
            }
            if (offboard) {
                if (!boost.applied) {
                    ++updates;
                    boost.applied = true;
                    boost.expires = GetTickCount64() + 180;
                    debug.status = "Off-board up velocity added.";
                }
            } else {
                boost.valid = false;
                ++updates;
                debug.status = up ? "Up velocity added." : "Forward velocity added.";
            }
        } catch (const SourceGuard& issue) {
            boost.valid = false;
            if (offboard) state.offboard_boost_core.store(0, std::memory_order_release);
            debug.status = issue.message;
            if (offboard) logging::write(logging::Level::warning, logging::Channel::skater, issue.message);
        } catch (...) {
            boost.valid = false;
            if (offboard) state.offboard_boost_core.store(0, std::memory_order_release);
            debug.status = "Velocity boost failed during the physics update.";
            if (offboard) logging::write(logging::Level::warning, logging::Channel::skater, debug.status);
        }
    };
    apply_boost(debug.forward_velocity, debug.forward_velocity_updates, false);
    apply_boost(debug.up_velocity, debug.up_velocity_updates, true);
    apply_boost(debug.offboard_up_velocity, debug.offboard_up_velocity_updates, true, true);
    const auto& request = debug.noclip_velocity;
    if (!debug.noclip || !request.valid || request.core != core) return;
    try {
        if (GetTickCount64() >= request.expires) { debug_stop_noclip(debug); debug.status = "Flight input timed out."; return; }
        const auto bodies = debug_noclip_bodies(state.trial.base, request.client, request.entity);
        source_require(bodies.core == core, "Skater physics was replaced; flight stopped.");
        SourceReader reader;
        source_require(reader.value<std::uint8_t>(request.entity, 0x7e0) == 0,
            "A teleport started; flight stopped.");
        for (float v : request.velocity) source_require(std::isfinite(v) && std::abs(v) <= 6000, "Invalid flight input.");
        reader.verify();
        if (bodies.seconds == 0) { debug.noclip_altitude_valid = false; return; }
        // Off-board the native drive only chases the motion target through a
        // spring, at a capped walking speed while grounded. Give the skeleton
        // bodies the flight velocity directly so the root (and the camera) keeps
        // up at any speed and stops dead on release. The carried/detached board
        // is left alone; its bodies only receive velocity while riding.
        const float height = bodies.offboard ? bodies.root[1] : bodies.board_height;
        source_require(std::isfinite(height) && std::abs(height) <= 1000000, "Invalid flight altitude.");
        if (!debug.noclip_altitude_valid || debug.noclip_altitude_offboard != bodies.offboard) {
            debug.noclip_altitude = height;
            debug.noclip_altitude_offboard = bodies.offboard;
        }
        debug.noclip_altitude_valid = true;
        debug.noclip_altitude = std::clamp(debug.noclip_altitude, height - 3.0f, height + 3.0f);
        auto velocity = request.velocity;
        // Correct gravity drift without changing world gravity or teleporting.
        velocity[1] += std::clamp((debug.noclip_altitude - height) * 12.0f, -8.0f, 8.0f);
        debug.noclip_altitude += request.velocity[1] * bodies.seconds;
        // Match native velocity writers: XYZ at +70 and dirty bit 8 at +60.
        // Preserve W, angular velocity, transforms, contacts and other flags.
        // Runs after the simulation's movement-state update.
        for (std::size_t i = bodies.offboard ? 9 : 0; i < bodies.parts.size(); ++i) {
            const auto body = bodies.parts[i];
            const auto flags = reader.value<std::uint32_t>(body, 0x60);
            body_write(body + 0x70, velocity);
            body_write(body + 0x60, flags | 8u);
        }
        ++debug.noclip_velocity_updates;
    } catch (const SourceGuard& issue) { debug_stop_noclip(debug); debug.status = issue.message; }
      catch (...) { debug_stop_noclip(debug); debug.status = "Flight stopped after a physics error."; }
}
struct JumpScale {
    std::atomic<bool> pending{};
    std::uintptr_t client{}, entity{}, core{};
    ULONGLONG expires{};
    float factor{1};
    std::atomic<int> outcome{};
    std::atomic<float> up_speed{};
};
JumpScale& jump_scale() { static auto* value = new JumpScale; return *value; }
// The trainer's hippy jump height: scale the upward velocity the game gave the off-board skater.
void trainer_apply_jump_scale(std::uintptr_t core) noexcept {
    auto& j = jump_scale();
    if (!j.pending.load(std::memory_order_acquire) || j.core != core) return;
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    j.pending.store(false, std::memory_order_release);
    try {
        if (GetTickCount64() >= j.expires) { j.outcome.store(-3); return; }
        const auto bodies = debug_noclip_bodies(state.trial.base, j.client, j.entity);
        source_require(bodies.core == core, "Skater physics was replaced.");
        // On the board the game drives a jump along its own path and undoes a velocity change.
        if (!bodies.offboard) { j.outcome.store(-2); return; }
        float before{};
        const bool scaled = scale_offboard_up_velocity(state.trial.base, core, bodies.context, bodies.rig_wrapper, j.factor, &before);
        j.up_speed.store(before);
        j.outcome.store(scaled ? 2 : before < 0.5f ? -1 : -2);
    } catch (...) { j.outcome.store(-3); }
}
// The trainer's push speed. On the board the game steers the skater to the speed its trick
// scripts ask for (context +0x17f4) and never runs its own code for the push tuning values.
// Measured: a tapped push asks for the skater's own speed kept between 4 and about 9.5 m/s, a
// held one walks up 4.95, 6.98, 8.5 and 9.1 m/s, and the game holds that speed for about six
// seconds after the last push before the skater coasts. A multiplier on that target would
// feed back (the target follows the speed: x2 ran away to 17 m/s), so after each physics step
// of a riding skater:
//  - `factor` over 1: once the game has the skater at its tap speed it is carried on to
//    4 m/s x factor; a held push that has reached the game's last step is carried on to that
//    step x factor, and stays there while the game holds its own;
//  - `factor` under 1: the skater is held down to the game's target x factor (it then never
//    reaches the speed the next step would need, so nothing feeds back).
// Auto push: the game hands its flag to the animation and nothing comes of it (measured: the
// same coast-down with it on). With `cruise` set, a rolling skater that is not braking gains
// speed up to it.
constexpr float push_gain = 0.2f;    // m/s the game's own push gains each physics step (0 to 4 m/s in 19 steps)
constexpr float tap_speed = 4.0f;    // m/s the game's tapped push settles at
constexpr float last_step = 0.95f;   // of the stock top pushing speed: the game's last step is 9.1 of 9.25 m/s
constexpr float cruise_gain = 0.15f; // m/s each physics step: the game steers the speed back toward its own, so less does nothing
constexpr int held_steps = 36;       // a push flagged this long is held (a tap lasts 19 steps)
constexpr int hold_steps = 360;      // how long after a push the game holds its speed
struct PushSpeed {
    std::atomic<float> factor{1}, stock{9.25f}, cruise{};
    std::uintptr_t client{}, entity{};
    std::atomic<ULONGLONG> expires{};
    // Physics thread only.
    int pushing{}, since_push{hold_steps};
    bool carried{}; // a held push is being carried past the game's last step
};
PushSpeed& push_speed() { static auto* value = new PushSpeed; return *value; }
void trainer_push_speed(std::uintptr_t core) noexcept {
    auto& p = push_speed();
    const float factor = p.factor.load(std::memory_order_relaxed), cruise = p.cruise.load(std::memory_order_relaxed);
    if ((factor == 1 && cruise <= 0) || GetTickCount64() >= p.expires.load(std::memory_order_acquire)) return;
    const auto watch = watched_physics_state();
    if (!watch.valid || watch.state != 100) return; // riding the ground
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    try {
        const auto bodies = debug_noclip_bodies(state.trial.base, p.client, p.entity);
        if (bodies.core != core || bodies.offboard || bodies.parts.empty()) return;
        SourceReader reader;
        const float target = reader.value<float>(bodies.context, 0x17f4);
        // What the game's brake code tests: a brake held (bit 2) or one squeezed by some amount (bit 1, +0x1880).
        const auto requests = reader.value<std::uint32_t>(bodies.context, 0x13c4);
        const bool braking = (requests & 4u) != 0 || ((requests & 2u) != 0 && std::abs(reader.value<float>(bodies.context, 0x1880)) > 0.05f);
        std::array<std::array<float, 3>, 32> velocities{};
        std::array<std::uint32_t, 32> flags{};
        const auto count = std::min<std::size_t>(bodies.parts.size(), velocities.size());
        for (std::size_t i = 0; i < count; ++i) {
            velocities[i] = reader.value<std::array<float, 3>>(bodies.parts[i], 0x70);
            flags[i] = reader.value<std::uint32_t>(bodies.parts[i], 0x60);
        }
        reader.verify();
        if (requests & 0x40u) { // the push request the game's own push code tests
            ++p.pushing;
            p.since_push = 0;
        } else {
            p.pushing = 0;
            if (p.since_push < hold_steps) ++p.since_push;
        }
        const float speed = std::sqrt(velocities[0][0] * velocities[0][0] + velocities[0][2] * velocities[0][2]);
        if (!(speed >= 0.5f) || !(speed < 1000.0f)) return;
        const float stock = p.stock.load(std::memory_order_relaxed);
        const bool pushed = target > 0.5f && target < 100.0f && p.since_push < hold_steps; // the game is holding a push speed
        const bool at_last_step = pushed && target >= stock * last_step && speed >= target * 0.9f;
        if (braking || !at_last_step) p.carried = false;
        else if (p.pushing >= held_steps) p.carried = true;
        float change = 0;
        if (braking) {
        } else if (pushed && factor > 1 && p.carried) {
            change = std::min(push_gain, target * factor - speed);
        } else if (pushed && factor > 1 && speed >= tap_speed * 0.85f && speed < tap_speed * factor) {
            change = std::min(push_gain, tap_speed * factor - speed);
        } else if (pushed && factor < 1 && speed > target * factor && speed <= target * 1.1f) {
            change = -std::min(push_gain, speed - target * factor); // pushed speed only: a hill's is faster than the target
        } else if (cruise > 0 && speed >= 1.0f && speed < cruise) {
            change = std::min(cruise_gain, cruise - speed);
        }
        if (std::abs(change) < 0.005f) return;
        const float scale = (speed + change) / speed;
        // Like the native velocity writers: XYZ at +70 and the dirty bit 8 at +60.
        for (std::size_t i = 0; i < count; ++i) {
            velocities[i][0] *= scale;
            velocities[i][2] *= scale;
            body_write(bodies.parts[i] + 0x70, velocities[i]);
            body_write(bodies.parts[i] + 0x60, flags[i] | 8u);
        }
    } catch (...) {}
}
// ReSkate Tricks: a casper or primo slides on the board's edge or its grip tape. While freestyle holds the board
// to its clip (GROUND_ANIMATION, 103) the game keeps the speed it came in with; this takes `deceleration` m/s off
// the speed over the ground each second, so a slide carries speed and scrubs it off harder than a grind.
struct FreestyleSlide {
    std::atomic<float> deceleration{};
    std::uintptr_t client{}, entity{};
    std::atomic<ULONGLONG> expires{};
    // Physics thread only: the speed the slide should have now (below 0: none yet), and when it was last stepped.
    float target{-1};
    ULONGLONG last_step{}, started{};
    std::atomic<bool> level{true};
    std::atomic<float> upright{0.0f};
    // The rocking of the slide: the board's turn rate about its way of travel, and that rate summed to an angle.
    float roll_angle{};
    std::atomic<float> roll_peak{}, roll_swing{};
    std::atomic<bool> hold{}, stable{true};
    // A pose landed from the air: for this long from the touchdown no part moves up or down, so the board is not
    // knocked out of the pose by its own landing (a primo's board lands on its edge).
    // 300 ms by default: a casper lands deck first, 15 cm into the ground, and was thrown 1.7 m back up (measured
    // 2026-10-13, the owner's "bounce on casper landings").
    std::atomic<std::uint32_t> soft_ms{300};
    std::atomic<bool> mean{};
    bool was_hold{}; // how much of the parts' motion against each other is taken out each step // take the pitch out of the board's turning while it slides
};
FreestyleSlide& freestyle_slide() { static auto* value = new FreestyleSlide; return *value; }
// ReSkate Tricks: the skater's root kept upright while a pose is held. The board's target is held level by the
// board hook, but the root the body is drawn on still rolled over a quarter turn about the board's length at speed
// (the owner: "bends at the ankles until his face or back hits the ground", 2026-10-13). The rig's root frame (rig
// + 0x4720: rows side, up, forward, position) is built from the board each step by the rig's root update (0x47784b0,
// called by the rig-follow of states 103, 200 and 202; hooked in tricks_board.cpp). Right after that update its up
// row is set straight up; the heading is the one it had while it still stood. (Measured 2026-10-13: with the board
// and the clip's board pose both still, the root rolled to 88 degrees in 0.6 s and came back in 1.3 s.)
struct RootUpright {
    std::atomic<int> mode{};            // 0 off, 1 watched only, 2 held upright
    std::atomic<std::uintptr_t> rig{};  // the local skater's, from the slide
    std::atomic<ULONGLONG> seen{};
    std::atomic<float> low{2.0f}, high{-2.0f};
    std::atomic<std::uint32_t> calls{}, fixed{}, source_fixed{}, source_calls{};
    std::atomic<float> source_low{2.0f}, source_sum{};
    std::atomic<bool> turn{};               // the root's source turned half round (the anti-casper)
    std::atomic<bool> hold{};               // held upright whatever the physics state (a pose's way out, in the air too)
    // The heading held as it was when the lock began (the board spun under the feet: a no comply flip, a ground shuvit):
    // the frame follows the board, and a board turned, or upside down, dragged the body round with it.
    std::atomic<bool> heading_lock{};
    bool lock_latched{};      // root hook only
    float lock_heading[3]{};
    std::atomic<std::uintptr_t> turn_rig{}; // the local skater's rig, from the game thread
    std::atomic<std::uint32_t> turned{};
    // Motion hook only.
    bool latched{};
    float heading[3]{};
};
RootUpright& root_upright() { static auto* value = new RootUpright; return *value; }
// <dev> The first three seconds of a held pose on the ground, a sample every 50 ms: ms, the height of the root's up
// row, the board's height, the board's speed up, the height of the root's side and forward rows.
struct RootTrace {
    std::array<std::array<float, 6>, 64> samples{};
    std::atomic<std::uint32_t> count{};
    ULONGLONG started{}, last{};
};
std::atomic<std::uintptr_t> trace_part{};
std::atomic<bool> parts_log_wanted{}; // <dev> `tricks parts`: every physics part's place in the skater's root frame, once
std::atomic<float> target_up{}, target_right{};
RootTrace& root_trace() { static auto* value = new RootTrace; return *value; }
void trace_root(std::uintptr_t rig, float high, float low, float mean) noexcept {
    auto& t = root_trace();
    const auto now = GetTickCount64();
    if (now - t.last > 400) { t.started = now; t.count.store(0, std::memory_order_relaxed); }
    const auto count = t.count.load(std::memory_order_relaxed);
    if (count && now - t.last < 50 && count < t.samples.size()) return;
    if (count >= t.samples.size()) { t.last = now; return; }
    std::array<float, 12> frame{};
    __try {
        std::memcpy(frame.data(), reinterpret_cast<const void*>(rig + 0x4720), sizeof(frame));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    // The animated board matrix (rig + 0x49e0, model space): the heights of its right, up and forward rows.
    std::array<float, 12> board{};
    __try {
        std::memcpy(board.data(), reinterpret_cast<const void*>(trace_part.load(std::memory_order_relaxed) + 0x20), sizeof(board)); // <dev> the board's first physics part: its own frame
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    // The frame the root is built from: the height of its up and side rows (the clip's board at rig + 0x49e0 was
    // measured steady through a casper's flop; this one rolled).
    std::array<float, 12> model{};
    __try {
        std::memcpy(model.data(), reinterpret_cast<const void*>(*reinterpret_cast<const std::uintptr_t*>(rig + 0x59b0) + 0x5b0), sizeof(model));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    (void)mean; (void)board;
    t.samples[count] = {static_cast<float>(now - t.started), frame[5], high, low, model[5], model[1]};
    t.count.store(count + 1, std::memory_order_relaxed);
    t.last = now;
}
// </dev>
bool upright_root(std::uintptr_t rig) noexcept {
    auto& u = root_upright();
    const int mode = u.mode.load(std::memory_order_relaxed);
    if (!mode) {
        u.latched = false;
        return false;
    }
    std::array<float, 12> target{};
    __try {
        std::memcpy(target.data(), reinterpret_cast<const void*>(rig + 0x4720), sizeof(target));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    float *side = target.data(), *up = target.data() + 4, *forward = target.data() + 8;
    // Not the frame this code knows (three unit rows): left alone.
    for (const float *row : {side, up, forward}) {
        const float length = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
        if (!(length > 0.9f && length < 1.1f)) return false;
    }
    u.calls.fetch_add(1, std::memory_order_relaxed);
    if (up[1] < u.low.load(std::memory_order_relaxed)) u.low.store(up[1], std::memory_order_relaxed);
    if (up[1] > u.high.load(std::memory_order_relaxed)) u.high.store(up[1], std::memory_order_relaxed);
    if (mode < 2) return false;
    // The heading: the forward row over the ground while the root stands (or the first one there is).
    if (up[1] > 0.8f || !u.latched) {
        const float *from = std::abs(forward[1]) < 0.8f ? forward : nullptr;
        if (from) {
            const float length = std::sqrt(from[0] * from[0] + from[2] * from[2]);
            if (length > 0.3f) {
                u.heading[0] = from[0] / length; u.heading[1] = 0.0f; u.heading[2] = from[2] / length;
                u.latched = true;
            }
        }
    }
    if (!u.latched) return false;
    const float cross[3]{up[1] * forward[2] - up[2] * forward[1], up[2] * forward[0] - up[0] * forward[2], up[0] * forward[1] - up[1] * forward[0]};
    const float hand = cross[0] * side[0] + cross[1] * side[1] + cross[2] * side[2] < 0 ? -1.0f : 1.0f;
    up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
    for (int i = 0; i < 3; ++i) forward[i] = u.heading[i];
    side[0] = hand * (up[1] * forward[2] - up[2] * forward[1]);
    side[1] = 0.0f;
    side[2] = hand * (up[0] * forward[1] - up[1] * forward[0]);
    if (!copy_to(rig + 0x4720, target.data(), sizeof(target))) return false;
    // The same update ends by storing the root's inverse (rig + 0x4760: three transposed rows, + 0x4790: the position
    // taken back through them). Left as the game had it, everything placed from the inverse (the feet on the board)
    // was placed from the rolled root: the legs bent backwards and twisted under an upright body (owner, 2026-10-13).
    std::array<float, 4> position{};
    __try {
        std::memcpy(position.data(), reinterpret_cast<const void*>(rig + 0x4750), sizeof(position));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    std::array<float, 16> inverse{};
    __try {
        std::memcpy(inverse.data(), reinterpret_cast<const void*>(rig + 0x4760), sizeof(inverse));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    for (int i = 0; i < 3; ++i) {
        inverse[i * 4 + 0] = side[i];
        inverse[i * 4 + 1] = up[i];
        inverse[i * 4 + 2] = forward[i];
    }
    for (int j = 0; j < 3; ++j)
        inverse[12 + j] = -(position[0] * inverse[j] + position[1] * inverse[4 + j] + position[2] * inverse[8 + j]);
    (void)copy_to(rig + 0x4760, inverse.data(), sizeof(inverse));
    u.fixed.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// ReSkate Tricks: the rider's drives to the deck. The solver world of the skater's deck (w = core[0x430][0x718][0x80]
// [0x28]) holds five drives that pull the rider's bodies to the deck and put nothing back on it (MassBias 1): the
// pelvis to a point 0.905 m up the deck's own up axis, the feet to points on its top, each with a target orientation
// in the deck's frame (record [w+0xe68] + row * 0xc0: point on B +0x30, orientation in B +0x6c (w,x,y,z), their
// previous values +0x88 / +0xa4; ReSkateRustRewrite docs/re/world-step.md "Round twelve", drive-compile.md). With the
// deck upside down (a casper) the pelvis' point is under the board: the drawn body sinks onto the ground (measured
// 2026-10-13: the rider's parts from 0.86 to 0.2 m over the board). While a pose is held they are given an upright
// deck instead: the deck's own place and heading, standing up.
struct RiderDrives {
    std::atomic<int> mode{};   // 0 the game's, 1 watched, 2 turned upright
    std::atomic<int> feet{1};  // the feet's rows: 0 the game's, 1 orientation turned, 2 orientation and point turned
    std::atomic<std::uintptr_t> core{}, part{};
    std::atomic<ULONGLONG> seen{};
    // Physics thread only.
    std::uintptr_t w{};
    int deck{-1}, pelvis{-1}, count{};
    std::array<int, 12> rows{}, bodies{};
    struct Saved {
        std::array<float, 3> point{}, wrote_point{};
        std::array<float, 4> turn{}, wrote_turn{};
        bool valid{};
        // The game sets this row again every step (measured 2026-10-13: a second pelvis row follows the skater's
        // root): it is the game's, never turned.
        bool live{};
    };
    std::array<Saved, 12> saved{};
    ULONGLONG started{}, last{}, next_sample{};
    std::atomic<std::uint32_t> writes{}, regame{};
    std::mutex text_lock;
    std::string text;
};
RiderDrives& rider_drives() { static auto* value = new RiderDrives; return *value; }
template<class T> bool peek_at(std::uintptr_t at, T& value) noexcept { return at && memory::peek(at, value); }
// The rows whose body B is the deck and whose drive is the rider's (limit 10000; the trucks' are 39600). The deck is
// the B of the row whose point is the pelvis' (0.905 m up).
bool find_rider_rows(RiderDrives& d) noexcept {
    std::uint32_t count{};
    std::uintptr_t pairs{}, records{};
    if (!peek_at(d.w + 0x90 + 0x1c, count) || !peek_at(d.w + 0x90 + 0x50, pairs) || !peek_at(d.w + 0xe68, records) || !pairs || !records || count > 4096) return false;
    auto pair = [&](std::uint32_t row, std::uint32_t& a, std::uint32_t& b) {
        const auto at = pairs + 4 * ((row & ~3u) + row);
        return peek_at(at, a) && peek_at(at + 0x10, b);
    };
    int deck = -1, pelvis = -1;
    for (std::uint32_t row = 0; row < count && deck < 0; ++row) {
        std::array<float, 3> point{};
        float limit{};
        std::uint32_t a{}, b{};
        if (!peek_at(records + row * 0xc0 + 0x30, point) || !peek_at(records + row * 0xc0 + 0x10, limit) || !pair(row, a, b)) continue;
        if (std::abs(limit - 10000.0f) < 1.0f && std::abs(point[1] - 0.905f) < 0.05f && std::abs(point[0]) < 0.3f && std::abs(point[2]) < 0.3f && a != b) {
            deck = static_cast<int>(b);
            pelvis = static_cast<int>(row);
        }
    }
    if (deck < 0) return false;
    d.count = 0;
    for (std::uint32_t row = 0; row < count && d.count < static_cast<int>(d.rows.size()); ++row) {
        float limit{};
        std::uint32_t a{}, b{};
        if (!pair(row, a, b) || static_cast<int>(b) != deck || a == b || !peek_at(records + row * 0xc0 + 0x10, limit) || std::abs(limit - 10000.0f) >= 1.0f) continue;
        d.rows[d.count] = static_cast<int>(row);
        d.bodies[d.count] = static_cast<int>(a);
        ++d.count;
    }
    d.deck = deck;
    d.pelvis = pelvis;
    for (auto& s : d.saved) s = {};
    return d.count > 0;
}
// q = a (x) b, both (w, x, y, z).
std::array<float, 4> quat_mul(const std::array<float, 4>& a, const std::array<float, 4>& b) noexcept {
    return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3], a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
            a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1], a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}
// The rotation m (column vectors: v' = m v) as (w, x, y, z).
std::array<float, 4> quat_of(const float m[3][3]) noexcept {
    std::array<float, 4> q{};
    const float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {0.25f * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q = {(m[2][1] - m[1][2]) / s, 0.25f * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
    } else if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q = {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25f * s, (m[1][2] + m[2][1]) / s};
    } else {
        const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q = {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25f * s};
    }
    return q;
}
// The turn from the deck's frame to an upright one at the same heading, in the deck's own axes: rows of `deck` are its
// axes x, y (up), z (forward) in the world.
bool upright_turn(const std::array<float, 12>& deck, float m[3][3]) noexcept {
    const float *x = deck.data(), *y = deck.data() + 4, *z = deck.data() + 8;
    for (const float* row : {x, y, z}) {
        const float length = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
        if (!(length > 0.9f && length < 1.1f)) return false;
    }
    const float run = std::sqrt(z[0] * z[0] + z[2] * z[2]);
    if (!(run > 0.3f)) return false; // the deck stands on its nose or tail: no heading to keep
    const float uz[3]{z[0] / run, 0.0f, z[2] / run}, uy[3]{0.0f, 1.0f, 0.0f};
    const float cross[3]{y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2], y[0] * z[1] - y[1] * z[0]};
    const float hand = cross[0] * x[0] + cross[1] * x[1] + cross[2] * x[2] < 0 ? -1.0f : 1.0f;
    const float ux[3]{hand * (uy[1] * uz[2] - uy[2] * uz[1]), hand * (uy[2] * uz[0] - uy[0] * uz[2]), hand * (uy[0] * uz[1] - uy[1] * uz[0])};
    const float* mine[3]{x, y, z};
    const float* upright[3]{ux, uy, uz};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = mine[i][0] * upright[j][0] + mine[i][1] * upright[j][1] + mine[i][2] * upright[j][2];
    return true;
}
void put_back_rider_rows(RiderDrives& d) noexcept {
    std::uintptr_t records{};
    if (!peek_at(d.w + 0xe68, records) || !records) return;
    for (int k = 0; k < d.count; ++k) {
        auto& s = d.saved[k];
        s.live = false;
        if (!s.valid) continue;
        s.valid = false;
        const auto at = records + static_cast<std::uintptr_t>(d.rows[k]) * 0xc0;
        std::array<float, 3> point{};
        std::array<float, 4> turn{};
        // Only what this code wrote is put back: a row the game set since is the game's.
        if (!peek_at(at + 0x30, point) || !peek_at(at + 0x6c, turn) || point != s.wrote_point || turn != s.wrote_turn) continue;
        (void)copy_to(at + 0x30, s.point.data(), sizeof(s.point));
        (void)copy_to(at + 0x88, s.point.data(), sizeof(s.point));
        (void)copy_to(at + 0x6c, s.turn.data(), sizeof(s.turn));
        (void)copy_to(at + 0xa4, s.turn.data(), sizeof(s.turn));
    }
}
// The feet let go of the deck while the mod turns the board under them: the rider-to-deck drives of the feet (anchored
// near the deck; the pelvis's sits 0.9 m up) have their five force limits (record +0x10 .. +0x20) scaled down, so the
// legs keep the clip's pose and the board turns clear of the shoes instead of dragging the feet through itself
// (stills 2026-10-14: a no comply kickflip's board went through the back foot). Put back when the spin ends.
struct FeetFree {
    std::atomic<std::uintptr_t> core{};
    std::atomic<ULONGLONG> until{};
    std::atomic<float> scale{0.05f};
    std::atomic<std::uint32_t> writes{};
    // Physics thread only.
    RiderDrives rows;
    std::array<std::array<float, 5>, 12> saved{}, wrote{};
    std::array<bool, 12> held{};
};
FeetFree& feet_free() { static auto* value = new FeetFree; return *value; }
void put_back_feet(FeetFree& f) noexcept {
    std::uintptr_t records{};
    if (!peek_at(f.rows.w + 0xe68, records) || !records) return;
    for (int k = 0; k < f.rows.count; ++k) {
        if (!f.held[k]) continue;
        f.held[k] = false;
        const auto at = records + static_cast<std::uintptr_t>(f.rows.rows[k]) * 0xc0 + 0x10;
        std::array<float, 5> now{};
        if (peek_at(at, now) && now == f.wrote[k]) (void)copy_to(at, f.saved[k].data(), sizeof(f.saved[k]));
    }
}
void tricks_feet_free(std::uintptr_t core) noexcept {
    auto& f = feet_free();
    // The hold comes back over 150 ms after the spin (all at once, the feet were yanked to the deck).
    const auto now = GetTickCount64(), until = f.until.load(std::memory_order_relaxed);
    const bool wanted = core && core == f.core.load(std::memory_order_relaxed) && now < until + 150;
    const bool holding = std::any_of(f.held.begin(), f.held.end(), [](bool h) { return h; });
    if (!wanted) {
        if (holding && core == f.core.load(std::memory_order_relaxed)) put_back_feet(f);
        return;
    }
    std::uintptr_t a{}, b{}, c{}, w{};
    if (!peek_at(core + 0x430, a) || !peek_at(a + 0x718, b) || !peek_at(b + 0x80, c) || !peek_at(c + 0x28, w) || !w) return;
    if (w != f.rows.w) {
        f.held.fill(false);
        f.rows.w = w;
        f.rows.deck = -1;
        f.rows.count = 0;
    }
    if (f.rows.deck < 0 && !find_rider_rows(f.rows)) return;
    std::uintptr_t records{};
    if (!peek_at(w + 0xe68, records) || !records) return;
    const float lowest = std::clamp(f.scale.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float scale = now < until ? lowest : lowest + (1.0f - lowest) * static_cast<float>(now - until) / 150.0f;
    for (int k = 0; k < f.rows.count; ++k) {
        const auto row = records + static_cast<std::uintptr_t>(f.rows.rows[k]) * 0xc0;
        std::array<float, 3> point{};
        std::array<float, 5> limits{};
        if (!peek_at(row + 0x30, point) || !peek_at(row + 0x10, limits)) continue;
        if (point[1] > 0.5f) continue; // the pelvis's row: the body stays over the board
        if (!f.held[k] || limits != f.wrote[k]) f.saved[k] = limits; // the game's own (first, or set again by the game)
        std::array<float, 5> mine{};
        for (std::size_t i = 0; i < 5; ++i) mine[i] = f.saved[k][i] * scale;
        if (copy_to(row + 0x10, mine.data(), sizeof(mine))) {
            f.wrote[k] = mine;
            f.held[k] = true;
            f.writes.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
void tricks_rider_drives(std::uintptr_t core) noexcept {
    auto& d = rider_drives();
    if (!core || core != d.core.load(std::memory_order_relaxed)) return;
    const auto now = GetTickCount64();
    const int mode = d.mode.load(std::memory_order_relaxed);
    const bool posed = mode && now - d.seen.load(std::memory_order_relaxed) < 100;
    std::uintptr_t a{}, b{}, c{}, w{};
    if (!peek_at(core + 0x430, a) || !peek_at(a + 0x718, b) || !peek_at(b + 0x80, c) || !peek_at(c + 0x28, w) || !w) return;
    if (w != d.w) {
        d.w = w;
        d.deck = -1;
        d.count = 0;
    }
    if (!posed) {
        put_back_rider_rows(d);
        d.started = 0;
        return;
    }
    if (d.deck < 0 && !find_rider_rows(d)) return;
    std::uintptr_t records{};
    if (!peek_at(w + 0xe68, records) || !records) return;
    std::array<float, 12> deck{};
    if (!peek_at(d.part.load(std::memory_order_relaxed) + 0x20, deck)) return;
    if (!d.started) { d.started = now; d.next_sample = now; }
    // <dev> the rows as the game has them, at the pose's start and 0.3, 0.6, 1.2 and 2 s into it
    if (now >= d.next_sample) {
        const auto since = now - d.started;
        std::string line = std::format(" | {}ms deck {} up {:.2f},{:.2f},{:.2f} fwd {:.2f},{:.2f},{:.2f}:", since, d.deck, deck[4], deck[5], deck[6], deck[8], deck[9], deck[10]);
        for (int k = 0; k < d.count; ++k) {
            std::array<float, 3> point{};
            std::array<float, 4> turn{};
            std::array<float, 4> k4{};
            const auto at = records + static_cast<std::uintptr_t>(d.rows[k]) * 0xc0;
            (void)peek_at(at + 0x30, point); (void)peek_at(at + 0x6c, turn); (void)peek_at(at, k4);
            const bool mine = d.saved[k].valid && point == d.saved[k].wrote_point;
            line += std::format(" r{}/b{}{}{} p(", d.rows[k], d.bodies[k], mine ? "*" : "", d.saved[k].live ? "~" : "");
            line += std::format("{:.2f},{:.2f},{:.2f}) q({:.2f},{:.2f},{:.2f},{:.2f}) k{:.0f}", point[0], point[1], point[2], turn[0], turn[1], turn[2], turn[3], k4[0]);
        }
        // Every other drive that holds a rider body to something that is not the rider or the deck (its root, the
        // world): point on A, point on B, orientation in B.
        std::uint32_t count{};
        std::uintptr_t pairs{};
        int pelvis_body = -1;
        for (int k = 0; k < d.count; ++k)
            if (d.rows[k] == d.pelvis) pelvis_body = d.bodies[k];
        if (pelvis_body >= 0 && peek_at(w + 0x90 + 0x1c, count) && peek_at(w + 0x90 + 0x50, pairs) && pairs && count <= 4096) {
            line += " / others:";
            auto rider = [&](std::uint32_t body) { return static_cast<int>(body) >= pelvis_body - 8 && static_cast<int>(body) <= pelvis_body + 30; };
            for (std::uint32_t row = 0; row < count; ++row) {
                std::uint32_t ra{}, rb{};
                const auto at = pairs + 4 * ((row & ~3u) + row);
                if (!peek_at(at, ra) || !peek_at(at + 0x10, rb)) continue;
                if (static_cast<int>(rb) == d.deck || static_cast<int>(ra) == d.deck || (!rider(ra) && !rider(rb)) || (rider(ra) && rider(rb) && ra != static_cast<std::uint32_t>(pelvis_body) && rb != static_cast<std::uint32_t>(pelvis_body))) continue;
                std::array<float, 3> pa{}, pb{};
                std::array<float, 4> qb{}, k4{};
                const auto rec = records + static_cast<std::uintptr_t>(row) * 0xc0;
                (void)peek_at(rec + 0x24, pa); (void)peek_at(rec + 0x30, pb); (void)peek_at(rec + 0x6c, qb); (void)peek_at(rec, k4);
                line += std::format(" r{}:{}-{} a({:.2f},{:.2f},{:.2f}) b({:.2f},{:.2f},{:.2f}) q({:.2f},{:.2f},{:.2f},{:.2f}) k{:.0f}/{:.0f}", row, ra, rb, pa[0], pa[1], pa[2], pb[0], pb[1], pb[2], qb[0], qb[1], qb[2], qb[3], k4[0], k4[2]);
            }
        }
        {
            std::lock_guard lock(d.text_lock);
            if (d.text.size() < 30000) d.text += line;
        }
        d.next_sample = d.started + (since < 300 ? 300 : since < 600 ? 600 : since < 1200 ? 1200 : since < 2000 ? 2000 : 100000);
    }
    // </dev>
    if (mode < 2) return;
    float m[3][3]{};
    if (!upright_turn(deck, m)) return;
    const auto upright = quat_of(m);
    const int feet = d.feet.load(std::memory_order_relaxed);
    for (int k = 0; k < d.count; ++k) {
        const bool is_pelvis = d.rows[k] == d.pelvis;
        if (!is_pelvis && feet == 0) continue;
        auto& s = d.saved[k];
        const auto at = records + static_cast<std::uintptr_t>(d.rows[k]) * 0xc0;
        std::array<float, 3> point{};
        std::array<float, 4> turn{};
        if (!peek_at(at + 0x30, point) || !peek_at(at + 0x6c, turn)) continue;
        // A row the game set since the last step (or not yet seen) is the game's new value: turned from there.
        if (s.live) continue;
        if (!s.valid || point != s.wrote_point || turn != s.wrote_turn) {
            if (s.valid) {
                d.regame.fetch_add(1, std::memory_order_relaxed);
                s.live = true;
                s.valid = false;
                continue;
            }
            s.point = point;
            s.turn = turn;
            s.valid = true;
        }
        std::array<float, 3> new_point = s.point;
        if (is_pelvis || feet == 2)
            for (int i = 0; i < 3; ++i) new_point[i] = m[i][0] * s.point[0] + m[i][1] * s.point[1] + m[i][2] * s.point[2];
        const auto new_turn = quat_mul(upright, s.turn);
        if (!copy_to(at + 0x30, new_point.data(), sizeof(new_point)) || !copy_to(at + 0x88, new_point.data(), sizeof(new_point)) ||
            !copy_to(at + 0x6c, new_turn.data(), sizeof(new_turn)) || !copy_to(at + 0xa4, new_turn.data(), sizeof(new_turn))) continue;
        // Read back: the stored form (a quaternion's sign, a rounding) is what the next step compares with.
        (void)peek_at(at + 0x30, s.wrote_point);
        (void)peek_at(at + 0x6c, s.wrote_turn);
        d.writes.fetch_add(1, std::memory_order_relaxed);
    }
}
// The frame the root update builds the root from ([rig + 0x59b0] + 0x5b0: rows side, up, forward, + 0x5e0 position).
// It, not the board or the clip, is what rolls in a casper (measured 2026-10-13: its up row from 1.00 to 0.06 and
// back over 1.5 s with its side row going straight up, the root copying it and the drawn body following 60 ms later,
// while the board's part and the clip's board stayed still). Set upright on its heading before the update, so the
// root and everything placed from it start upright.
bool upright_root_source(std::uintptr_t rig) noexcept {
    auto& u = root_upright();
    const bool locked = u.heading_lock.load(std::memory_order_relaxed);
    if (!locked) u.lock_latched = false;
    if (u.mode.load(std::memory_order_relaxed) < 2 && !locked) return false;
    std::uintptr_t source{};
    if (!memory::peek(rig + 0x59b0, source) || !source) return false;
    std::array<float, 12> rows{};
    __try {
        std::memcpy(rows.data(), reinterpret_cast<const void*>(source + 0x5b0), sizeof(rows));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    float *side = rows.data(), *up = rows.data() + 4, *forward = rows.data() + 8;
    for (const float* row : {side, up, forward}) {
        const float length = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
        if (!(length > 0.9f && length < 1.1f)) return false;
    }
    // <dev> how far it had tipped since the last step, and over what
    {
        const float tip = up[1];
        if (tip < u.source_low.load(std::memory_order_relaxed)) u.source_low.store(tip, std::memory_order_relaxed);
        u.source_sum.store(u.source_sum.load(std::memory_order_relaxed) + (1.0f - tip), std::memory_order_relaxed);
        u.source_calls.fetch_add(1, std::memory_order_relaxed);
    }
    if (up[1] > 0.999f && !locked) return false; // already upright
    // The heading: the forward row over the ground (the roll is about it, so it stays level), or the one latched.
    const float run = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    if (locked && !u.lock_latched) {
        if (!(run > 0.3f)) return false;
        u.lock_heading[0] = forward[0] / run; u.lock_heading[1] = 0.0f; u.lock_heading[2] = forward[2] / run;
        u.lock_latched = true;
    }
    if (!locked && !(run > 0.3f)) return false;
    const float cross[3]{up[1] * forward[2] - up[2] * forward[1], up[2] * forward[0] - up[0] * forward[2], up[0] * forward[1] - up[1] * forward[0]};
    const float hand = cross[0] * side[0] + cross[1] * side[1] + cross[2] * side[2] < 0 ? -1.0f : 1.0f;
    if (locked) { forward[0] = u.lock_heading[0]; forward[1] = 0.0f; forward[2] = u.lock_heading[2]; }
    else { forward[0] /= run; forward[1] = 0.0f; forward[2] /= run; }
    up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
    side[0] = hand * (up[1] * forward[2] - up[2] * forward[1]);
    side[1] = 0.0f;
    side[2] = hand * (up[0] * forward[1] - up[1] * forward[0]);
    if (!copy_to(source + 0x5b0, rows.data(), sizeof(rows))) return false;
    u.source_fixed.fetch_add(1, std::memory_order_relaxed);
    return true;
}
struct BoardTurnBack {
    std::atomic<std::uintptr_t> client{}, entity{};
    std::atomic<void (*)() noexcept> after{};
    std::atomic<std::uint64_t> expires{};
    std::atomic<std::uint32_t> done{};
    std::atomic<float> last{};
    std::atomic<std::uint64_t> watch_until{}; // <dev> the deck's heading against the root, measured every step
    std::atomic<float> watch_angle{}, watch_up{}, watch_spin{};
};
BoardTurnBack& board_turn_back() { static auto* value = new BoardTurnBack; return *value; }
// A hand brought to the board (the mod's grabs): the rider is the drawn body, so one of its physics parts (a hand) is
// given, every step, the velocity that takes it a share of the way to a point on the deck (in the deck's frame),
// on top of the deck's own; the arm follows through its joints.
struct HandPull {
    std::atomic<std::uintptr_t> core{};
    std::atomic<ULONGLONG> until{};
    std::atomic<int> part{}, part2{-1};
    std::atomic<float> x{}, y{}, z{}, gain{0.35f}, crouch{};
    std::atomic<std::uint32_t> steps{};
    std::atomic<float> gap{};
};
HandPull& hand_pull() { static auto* value = new HandPull; return *value; }
BoardTurnBack& board_turn_back();
void tricks_hand_pull(std::uintptr_t core) noexcept {
    auto& h = hand_pull();
    if (!core || core != h.core.load(std::memory_order_relaxed) || GetTickCount64() >= h.until.load(std::memory_order_relaxed)) return;
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    try {
        auto& t = board_turn_back();
        const auto bodies = debug_noclip_bodies(state.trial.base, t.client.load(std::memory_order_relaxed), t.entity.load(std::memory_order_relaxed));
        if (bodies.core != core || bodies.offboard || bodies.wipeout || bodies.parts.size() < 32 || !(bodies.seconds > 0)) return;
        SourceReader reader;
        const auto deck = bodies.parts[8];
        const auto right = reader.value<std::array<float, 4>>(deck, 0x20), up = reader.value<std::array<float, 4>>(deck, 0x30),
                   forward = reader.value<std::array<float, 4>>(deck, 0x40);
        const auto centre = reader.value<std::array<float, 3>>(deck, 0x50), deck_speed = reader.value<std::array<float, 3>>(deck, 0x70);
        reader.verify();
        const float x = h.x.load(std::memory_order_relaxed), y = h.y.load(std::memory_order_relaxed), z = h.z.load(std::memory_order_relaxed);
        float target[3];
        for (int i = 0; i < 3; ++i) target[i] = centre[i] + right[i] * x + up[i] * y + forward[i] * z;
        const float share = std::clamp(h.gain.load(std::memory_order_relaxed), 0.0f, 1.0f) / bodies.seconds;
        const auto pull = [&](int index, float weight) {
            if (index < 0 || static_cast<std::size_t>(index) >= bodies.parts.size()) return;
            const auto part = bodies.parts[static_cast<std::size_t>(index)];
            SourceReader r2;
            const auto at = r2.value<std::array<float, 3>>(part, 0x50);
            const auto flags = r2.value<std::uint32_t>(part, 0x60);
            r2.verify();
            std::array<float, 3> v{};
            float gap = 0;
            for (int i = 0; i < 3; ++i) {
                const float d = target[i] - at[i];
                gap += d * d;
                v[i] = d * share * weight;
            }
            // At most 15 m/s toward the board (the flip clips fling the arms out; a slower pull lost to the arm's drives).
            const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (speed > 15.0f)
                for (auto& c : v) c *= 15.0f / speed;
            for (int i = 0; i < 3; ++i) v[i] += deck_speed[i];
            body_write(part + 0x70, v);
            body_write(part + 0x60, flags | 8u);
            if (weight == 1.0f) h.gap.store(std::sqrt(gap), std::memory_order_relaxed);
        };
        // The whole arm: the hand, its forearm (part - 1... given as part2) and the upper arm (part2 + 1), all at full pull:
        // the hand alone stayed 0.8 m off, held out by the arm's own drives (measured 2026-10-14).
        pull(h.part.load(std::memory_order_relaxed), 1.0f);
        const int forearm = h.part2.load(std::memory_order_relaxed);
        pull(forearm, 1.0f);
        if (forearm >= 0) pull(forearm + 1, 0.8f);
        // A crouch: the pelvis (part 31) and the chest (part 10) pulled down toward the board, so a short arm reaches it.
        if (const float crouch = h.crouch.load(std::memory_order_relaxed); crouch > 0)
            for (const int index : {31, 10}) {
                const auto part = bodies.parts[static_cast<std::size_t>(index)];
                SourceReader r3;
                auto v = r3.value<std::array<float, 3>>(part, 0x70);
                const auto flags = r3.value<std::uint32_t>(part, 0x60);
                r3.verify();
                for (int i = 0; i < 3; ++i) v[i] -= up[i] * crouch;
                body_write(part + 0x70, v);
                body_write(part + 0x60, flags | 8u);
            }
        h.steps.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {}
}
// A flip grab's board carried on its path: the board's target frame only turns it (its place in the air is not
// driven: a lift through the target did nothing, 2026-10-14), so its nine parts are given, every step, the skater's
// own travel plus the velocity that takes the deck most of the way to its place on the path. The path is in the
// skater's root frame (+0x4720 side, +0x4730 up, +0x4750 place): from where the deck was at the start (under the feet)
// up by `rise` and out along the side row by `reach`, there from `top` to `hold` of the time, back by the end.
struct BoardCarry {
    std::atomic<std::uintptr_t> core{};
    std::atomic<std::uint64_t> start{}, length{}; // QueryPerformanceCounter
    std::atomic<float> rise{}, reach{}, top{0.42f}, hold{0.58f}, gain{0.5f};
    // Physics thread only: the deck's place in the root frame at the start.
    bool started{}, logged{};
    float side0{}, up0{}, forward0{};
    std::atomic<std::uint32_t> steps{};
    std::atomic<float> gap{};
};
BoardCarry& board_carry() { static auto* value = new BoardCarry; return *value; }
float carry_smooth(float x) noexcept {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
void tricks_board_carry(std::uintptr_t core) noexcept {
    auto& c = board_carry();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const auto start = c.start.load(std::memory_order_acquire), length = c.length.load(std::memory_order_relaxed);
    if (!core || core != c.core.load(std::memory_order_relaxed) || !start || !length || static_cast<std::uint64_t>(now.QuadPart) >= start + length) {
        c.started = false;
        c.logged = false;
        return;
    }
    const float p = static_cast<float>(static_cast<double>(static_cast<std::uint64_t>(now.QuadPart) - start) / static_cast<double>(length));
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    try {
        auto& t = board_turn_back();
        const auto bodies = debug_noclip_bodies(state.trial.base, t.client.load(std::memory_order_relaxed), t.entity.load(std::memory_order_relaxed));
        if (bodies.core != core || bodies.offboard || bodies.wipeout || bodies.parts.size() < 32 || !(bodies.seconds > 0)) return;
        SourceReader reader;
        const auto side = reader.value<std::array<float, 4>>(bodies.rig_wrapper, 0x4720), up = reader.value<std::array<float, 4>>(bodies.rig_wrapper, 0x4730),
                   forward = reader.value<std::array<float, 4>>(bodies.rig_wrapper, 0x4740), place = reader.value<std::array<float, 4>>(bodies.rig_wrapper, 0x4750);
        const auto deck_at = reader.value<std::array<float, 3>>(bodies.parts[8], 0x50);
        const auto travel = reader.value<std::array<float, 3>>(bodies.parts[31], 0x70); // the pelvis: the skater's own travel
        reader.verify();
        const auto dot = [](const float* a, const std::array<float, 4>& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        if (!c.started) {
            const float d[3]{deck_at[0] - place[0], deck_at[1] - place[1], deck_at[2] - place[2]};
            c.side0 = dot(d, side);
            c.up0 = dot(d, up);
            c.forward0 = dot(d, forward);
            c.started = true;
        }
        const float top = std::clamp(c.top.load(std::memory_order_relaxed), 0.1f, 0.9f), hold = std::clamp(c.hold.load(std::memory_order_relaxed), top, 0.95f);
        const float out = p < top ? carry_smooth(p / top) : p < hold ? 1.0f : 1.0f - carry_smooth((p - hold) / (1.0f - hold));
        const float s = c.side0 + c.reach.load(std::memory_order_relaxed) * out, u = c.up0 + c.rise.load(std::memory_order_relaxed) * out, f = c.forward0;
        float push[3], gap = 0;
        const float share = std::clamp(c.gain.load(std::memory_order_relaxed), 0.0f, 1.0f) / bodies.seconds;
        for (int i = 0; i < 3; ++i) {
            const float target = place[i] + side[i] * s + up[i] * u + forward[i] * f;
            const float d = target - deck_at[i];
            gap += d * d;
            push[i] = travel[i] + d * share;
        }
        for (std::size_t i = 0; i < 9; ++i) {
            SourceReader r2;
            const auto flags = r2.value<std::uint32_t>(bodies.parts[i], 0x60);
            r2.verify();
            const std::array<float, 3> v{push[0], push[1], push[2]};
            body_write(bodies.parts[i] + 0x70, v);
            body_write(bodies.parts[i] + 0x60, flags | 8u);
        }
        c.gap.store(std::sqrt(gap), std::memory_order_relaxed);
        c.steps.fetch_add(1, std::memory_order_relaxed);
        if (p > 0.5f && !c.logged) { // <dev> the axes: the deck's rows and the back hand's place, in the root frame
            c.logged = true;
            SourceReader r3;
            const auto dr = r3.value<std::array<float, 4>>(bodies.parts[8], 0x20), du = r3.value<std::array<float, 4>>(bodies.parts[8], 0x30),
                       df = r3.value<std::array<float, 4>>(bodies.parts[8], 0x40);
            const auto hand = r3.value<std::array<float, 3>>(bodies.parts[11], 0x50), hand2 = r3.value<std::array<float, 3>>(bodies.parts[15], 0x50),
                       head = r3.value<std::array<float, 3>>(bodies.parts[9], 0x50), feet = r3.value<std::array<float, 3>>(bodies.parts[23], 0x50);
            r3.verify();
            const auto rel = [&](const std::array<float, 3>& at) {
                const float d[3]{at[0] - place[0], at[1] - place[1], at[2] - place[2]};
                return std::format("({:.2f},{:.2f},{:.2f})", dot(d, side), dot(d, up), dot(d, forward));
            };
            const auto axis = [&](const std::array<float, 4>& a) { return std::format("({:.2f},{:.2f},{:.2f})", dot(a.data(), side), dot(a.data(), up), dot(a.data(), forward)); };
            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: carry at {:.2f}: deck right {} up {} forward {}; deck {} back hand {} front hand {} head {} back foot {} (root frame side,up,forward).", p,
                         axis(dr), axis(du), axis(df), rel(deck_at), rel(hand), rel(hand2), rel(head), rel(feet));
        }
    } catch (...) {}
}
void tricks_board_turn_back(std::uintptr_t core) noexcept {
    auto& t = board_turn_back();
    const auto after = t.after.load(std::memory_order_acquire);
    const bool watching = GetTickCount64() < t.watch_until.load(std::memory_order_relaxed);
    const bool log_parts = parts_log_wanted.load(std::memory_order_relaxed);
    if (!after && !watching && !log_parts) return;
    const auto finish = [&] {
        if (after && t.after.exchange(nullptr, std::memory_order_acq_rel)) after();
    };
    if (after && GetTickCount64() >= t.expires.load(std::memory_order_relaxed)) { finish(); return; }
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    try {
        const auto bodies = debug_noclip_bodies(state.trial.base, t.client.load(std::memory_order_relaxed), t.entity.load(std::memory_order_relaxed));
        if (bodies.core != core) return; // another skater's step: the local one's comes
        if (log_parts && parts_log_wanted.exchange(false)) { // <dev> which part is which: places in the root frame
            SourceReader parts;
            const auto r0 = parts.value<std::array<float, 4>>(bodies.rig_wrapper, 0x4720), r1 = parts.value<std::array<float, 4>>(bodies.rig_wrapper, 0x4730),
                       r2 = parts.value<std::array<float, 4>>(bodies.rig_wrapper, 0x4740), r3 = parts.value<std::array<float, 4>>(bodies.rig_wrapper, 0x4750);
            std::string text;
            for (std::size_t i = 0; i < bodies.parts.size(); ++i) {
                const auto p = parts.value<std::array<float, 3>>(bodies.parts[i], 0x50);
                const float d[3]{p[0] - r3[0], p[1] - r3[1], p[2] - r3[2]};
                text += std::format(" {}:({:.2f},{:.2f},{:.2f})", i, d[0] * r0[0] + d[1] * r0[1] + d[2] * r0[2], d[0] * r1[0] + d[1] * r1[1] + d[2] * r1[2],
                                    d[0] * r2[0] + d[1] * r2[1] + d[2] * r2[2]);
            }
            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: physics parts in the root frame (index:(side,up,forward)):{}", text);
        }
        if (!after && !watching) return;
        if (bodies.offboard || bodies.wipeout || bodies.parts.size() < 9) { finish(); return; }
        SourceReader reader;
        // The deck (board part 8: the one the root update takes the board from) against the root, in the ground plane.
        const auto deck = bodies.parts[8];
        const auto board_forward = reader.value<std::array<float, 4>>(deck, 0x40);
        const auto root_forward = reader.value<std::array<float, 4>>(bodies.rig_wrapper, 0x4720 + 0x20);
        const auto centre = reader.value<std::array<float, 3>>(deck, 0x50);
        reader.verify();
        const float cross = board_forward[2] * root_forward[0] - board_forward[0] * root_forward[2];
        const float dot = board_forward[0] * root_forward[0] + board_forward[2] * root_forward[2];
        const float a = std::atan2(cross, dot);
        if (watching) {
            t.watch_angle.store(-a * 57.2958f, std::memory_order_relaxed);
            t.watch_up.store(reader.value<std::array<float, 4>>(deck, 0x30)[1], std::memory_order_relaxed);
            const auto w = reader.value<std::array<float, 3>>(deck, 0x90);
            t.watch_spin.store(std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * 57.2958f, std::memory_order_relaxed);
        }
        if (!after) return;
        if (!std::isfinite(a) || std::abs(a) < 60.0f / 57.2958f) { finish(); return; }
        const float c = std::cos(a), s = std::sin(a);
        const auto turn = [&](float& x, float& z) {
            const float nx = c * x + s * z, nz = -s * x + c * z;
            x = nx;
            z = nz;
        };
        for (std::size_t i = 0; i < 9; ++i) {
            const auto part = bodies.parts[i];
            std::array<std::array<float, 4>, 3> rows{reader.value<std::array<float, 4>>(part, 0x20), reader.value<std::array<float, 4>>(part, 0x30),
                reader.value<std::array<float, 4>>(part, 0x40)};
            auto at = reader.value<std::array<float, 3>>(part, 0x50);
            auto spin = reader.value<std::array<float, 3>>(part, 0x90);
            const auto flags = reader.value<std::uint32_t>(part, 0x60);
            reader.verify();
            for (auto& row : rows) turn(row[0], row[2]);
            float dx = at[0] - centre[0], dz = at[2] - centre[2];
            turn(dx, dz);
            at[0] = centre[0] + dx;
            at[2] = centre[2] + dz;
            turn(spin[0], spin[2]);
            for (std::size_t row = 0; row < 3; ++row) body_write(part + 0x20 + row * 0x10, rows[row]);
            body_write(part + 0x50, at);
            body_write(part + 0x90, spin);
            body_write(part + 0x60, flags | 1u | 4u | 0x20u);
        }
        t.last.store(a * 57.2958f, std::memory_order_relaxed);
        t.done.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {}
    finish();
}
void tricks_freestyle_slide(std::uintptr_t core) noexcept {
    auto& f = freestyle_slide();
    const float deceleration = f.deceleration.load(std::memory_order_relaxed);
    if (deceleration <= 0 || GetTickCount64() >= f.expires.load(std::memory_order_acquire)) return;
    const auto watch = watched_physics_state();
    if (!watch.valid || watch.state != 103) return;
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    try {
        const auto bodies = debug_noclip_bodies(state.trial.base, f.client, f.entity);
        if (bodies.core != core || bodies.offboard || bodies.parts.empty() || !(bodies.seconds > 0 && bodies.seconds < 0.1f)) return;
        trace_part.store(bodies.parts[0], std::memory_order_relaxed);
        if (rider_drives().mode.load(std::memory_order_relaxed)) {
            rider_drives().core.store(core, std::memory_order_relaxed);
            rider_drives().part.store(bodies.parts[0], std::memory_order_relaxed);
            rider_drives().seen.store(GetTickCount64(), std::memory_order_relaxed);
        }
        if (root_upright().mode.load(std::memory_order_relaxed))
        {
            // <dev> the rider's physics parts: highest, lowest and mean height over the board's
            SourceReader parts;
            float high = -100.0f, low = 100.0f, sum = 0.0f;
            for (std::size_t i = 9; i < bodies.parts.size(); ++i) {
                const float h = parts.value<float>(bodies.parts[i], 0x54) - bodies.board_height;
                high = std::max(high, h); low = std::min(low, h); sum += h;
            }
            // <dev> which physics part the root update takes its position from (rig + 0x48a0 rows, + 0x48d0 position:
            // board part 8, distance 0, measured 2026-10-13); shown in place of the lowest part
            std::array<float, 3> source{};
            float nearest = 1e9f;
            if (memory::peek(bodies.rig_wrapper + 0x48d0, source))
                for (std::size_t i = 0; i < bodies.parts.size(); ++i) {
                    std::array<float, 3> at{};
                    if (!memory::peek(bodies.parts[i] + 0x50, at)) continue;
                    const float dx = at[0] - source[0], dy = at[1] - source[1], dz = at[2] - source[2], dd = dx * dx + dy * dy + dz * dz;
                    if (dd < nearest) { nearest = dd; low = static_cast<float>(i) + std::min(std::sqrt(dd), 0.99f); }
                }
            trace_root(bodies.rig_wrapper, high, low, bodies.parts.size() > 9 ? sum / static_cast<float>(bodies.parts.size() - 9) : 0.0f);
            if (parts_log_wanted.exchange(false)) { // <dev> which part is which (a hand for the grabs): their places in the root frame
                std::array<float, 16> root{};
                if (memory::peek(bodies.rig_wrapper + 0x4720, root)) {
                    std::string text;
                    for (std::size_t i = 0; i < bodies.parts.size(); ++i) {
                        const auto p = parts.value<std::array<float, 3>>(bodies.parts[i], 0x50);
                        const float d[3]{p[0] - root[12], p[1] - root[13], p[2] - root[14]};
                        // side, up, forward: the root's rows
                        const float x = d[0] * root[0] + d[1] * root[1] + d[2] * root[2], y = d[0] * root[4] + d[1] * root[5] + d[2] * root[6], z = d[0] * root[8] + d[1] * root[9] + d[2] * root[10];
                        text += std::format(" {}:({:.2f},{:.2f},{:.2f})", i, x, y, z);
                    }
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: physics parts in the root frame (index:(side,up,forward)):{}", text);
                }
            }
        }
        root_upright().rig.store(bodies.rig_wrapper, std::memory_order_relaxed);
        root_upright().seen.store(GetTickCount64(), std::memory_order_relaxed);
        SourceReader reader;
        std::array<std::array<float, 3>, 32> velocities{};
        std::array<std::uint32_t, 32> flags{};
        const auto count = std::min<std::size_t>(bodies.parts.size(), velocities.size());
        for (std::size_t i = 0; i < count; ++i) {
            velocities[i] = reader.value<std::array<float, 3>>(bodies.parts[i], 0x70);
            flags[i] = reader.value<std::uint32_t>(bodies.parts[i], 0x60);
        }
        reader.verify();
        // The way and speed of travel: the first part's, or (a ground shuvit, whose board turns under the feet: one
        // part's own speed then has the turn in it, and holding that to the slide's speed drove the skater faster)
        // the mean of all the parts'.
        float travel_x = velocities[0][0], travel_z = velocities[0][2];
        if (f.mean.load(std::memory_order_relaxed) && count) {
            travel_x = travel_z = 0.0f;
            for (std::size_t i = 0; i < count; ++i) { travel_x += velocities[i][0]; travel_z += velocities[i][2]; }
            travel_x /= static_cast<float>(count);
            travel_z /= static_cast<float>(count);
        }
        const float speed = std::sqrt(travel_x * travel_x + travel_z * travel_z);
        if (!(speed < 1000.0f)) return;
        // The game's own friction in this state stops the board within a second (measured on flat ground: 15 km/h to
        // under 2 in one second), so the slide's speed is the mod's: it starts at the speed the pose was entered
        // with and falls by `deceleration` each second, and the board is held to it along the way it is going.
        const auto now = GetTickCount64();
        if (f.target < 0 || now - f.last_step > 250) { f.target = speed; f.started = now; f.roll_angle = 0; } // a new slide
        const bool hold = f.hold.load(std::memory_order_relaxed);
        if (hold != f.was_hold) f.roll_angle = 0; // the rocking is measured over the held pose only
        f.was_hold = hold;
        if (speed > 0.5f && hold) {
            const auto turn0 = reader.value<std::array<float, 3>>(bodies.parts[0], 0x90);
            const float rate = (turn0[0] * travel_x + turn0[2] * travel_z) / speed;
            if (std::isfinite(rate) && std::abs(rate) < 100.0f) {
                f.roll_angle += rate * bodies.seconds;
                if (std::abs(rate) > f.roll_peak.load(std::memory_order_relaxed)) f.roll_peak.store(std::abs(rate), std::memory_order_relaxed);
                const float swing = std::abs(f.roll_angle) * 57.2958f;
                if (swing > f.roll_swing.load(std::memory_order_relaxed)) f.roll_swing.store(swing, std::memory_order_relaxed);
            }
        }
        f.last_step = now;
        f.target = std::max(0.0f, f.target - deceleration * bodies.seconds);
        // Much slower than it should be: it hit something. The slide goes on from the speed it has now.
        if (speed < f.target * 0.5f) f.target = speed;
        if (f.target < 0.3f || speed < 0.05f) return; // nearly stopped: the game's own
        const float scale = f.target / speed;
        // Sliding on the deck, the ground's friction pitches the board up about the axis across its travel and the
        // skater goes over backwards with it (seen at 15 km/h: on its back for the first second). That turn about
        // the sideways axis is taken out of each part's angular velocity (+0x90, pending flag 0x20); the roll that
        // turns the board into the pose is along the travel and stays.
        const float side[3]{travel_z / speed, 0.0f, -travel_x / speed}; // up x travel
        const bool level = f.level.load(std::memory_order_relaxed) && speed > 1.0f;
        // Held upright: every part is brought toward the one velocity of the slide, so the skater does not rock on
        // the board when it touches down in the pose, and what is left of the parts' tipping is damped.
        const float keep = 1.0f - std::clamp(f.upright.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const float slide_x = travel_x * scale, slide_z = travel_z * scale;
        for (std::size_t i = 0; i < count; ++i) {
            velocities[i][0] = slide_x + (velocities[i][0] * scale - slide_x) * keep;
            velocities[i][2] = slide_z + (velocities[i][2] * scale - slide_z) * keep;
            if (now - f.started < f.soft_ms.load(std::memory_order_relaxed)) velocities[i][1] = 0.0f;
            body_write(bodies.parts[i] + 0x70, velocities[i]);
            std::uint32_t pending = flags[i] | 8u;
            if (hold && f.stable.load(std::memory_order_relaxed)) {
                // The pose is in: nothing tips any more. Only the turn about the vertical (steering) is left.
                auto turn = reader.value<std::array<float, 3>>(bodies.parts[i], 0x90);
                if (std::isfinite(turn[0]) && std::isfinite(turn[2])) {
                    turn[0] = turn[2] = 0.0f;
                    body_write(bodies.parts[i] + 0x90, turn);
                    pending |= 0x20u;
                }
            } else if (level) {
                auto turn = reader.value<std::array<float, 3>>(bodies.parts[i], 0x90);
                const float about = turn[0] * side[0] + turn[2] * side[2];
                if (std::isfinite(about) && std::abs(about) < 1000.0f) {
                    turn[0] -= about * side[0];
                    turn[2] -= about * side[2];
                    // Once the board has turned into the pose (the clip's way in is over), the roll goes too: the
                    // skater is held upright for the rest of the slide and only turns about the vertical.
                    // (Taking all of it out once the pose is in made the skater step off the board 0.7 s into every slide:
                    // measured 2026-10-12. Only the damping is left, and it is off unless asked for.)
                    turn[0] *= keep;
                    turn[2] *= keep;
                    body_write(bodies.parts[i] + 0x90, turn);
                    pending |= 0x20u;
                }
            }
            body_write(bodies.parts[i] + 0x60, pending);
        }
    } catch (...) {}
}
std::atomic<void (*)() noexcept>& tricks_step() { static std::atomic<void (*)() noexcept> value{}; return value; }
void noclip_physics_update(std::uintptr_t core) {
    const auto original = source_state().velocity_update_original.load(std::memory_order_acquire);
    if (original) original(core);
    noclip_apply_velocity(core);
    trainer_apply_jump_scale(core);
    trainer_push_speed(core);
    tricks_board_turn_back(core);
    tricks_feet_free(core);
    tricks_board_carry(core);
    tricks_hand_pull(core);
    tricks_freestyle_slide(core);
    tricks_rider_drives(core);
    if (const auto step = tricks_step().load(std::memory_order_acquire)) step();
}
bool noclip_motion_target(std::uintptr_t rig, std::uintptr_t context,
    const std::array<float,16>* supplied, std::array<float,16>& target) noexcept {
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || !state.velocity_guard_active.load(std::memory_order_acquire) ||
        state.busy.test_and_set(std::memory_order_acquire)) return false;
    SourceBusyScope scope{state.busy};
    auto& debug = state.trial.debug;
    auto& boost = debug.offboard_up_velocity;
    if (!debug.noclip && boost.valid && boost.applied) {
        try {
            SourceReader reader;
            if (reader.pointer(boost.core, 0x438) != rig || reader.pointer(boost.core, 0x3c0) != context)
                return false;
            if (GetTickCount64() >= boost.expires) return false;
            const auto bodies = debug_noclip_bodies(state.trial.base, boost.client, boost.entity);
            source_require(bodies.core == boost.core && bodies.rig_wrapper == rig && bodies.context == context,
                "Off-board boost skater changed.");
            source_require(!debug.park_editor && reader.value<std::uint8_t>(boost.entity, 0x7e0) == 0,
                "Off-board boost interrupted by editing or teleporting.");
            source_require(reader.raw(reinterpret_cast<std::uintptr_t>(supplied), target.data(), sizeof(target)) &&
                valid_flight_transform(target), "Invalid off-board motion target.");
            // Native off-board movement drives the skeleton toward this target.
            // Body velocity alone is undone by that drive on the same step.
            // Lead only Y for the launch; keep native X/Z, rotation and collision flags.
            target[13] = std::max(target[13], bodies.root[1] + boost.velocity[1] * bodies.seconds);
            source_require(valid_flight_transform(target), "Off-board boost target is out of bounds.");
            reader.verify();
            if (bodies.offboard)
                source_require(ensure_offboard_up_velocity(state.trial.base, bodies.core, context, rig, boost.velocity[1]),
                    "Off-board boost motion ownership changed.");
            return bodies.seconds > 0;
        } catch (const SourceGuard& issue) { debug.status = issue.message; }
          catch (...) { debug.status = "Off-board boost motion failed."; }
        boost.valid = false;
        state.offboard_boost_core.store(0, std::memory_order_release);
        return false;
    }
    const auto& request = debug.noclip_velocity;
    if (!debug.noclip || !request.valid) return false;
    try {
        SourceReader reader;
        // Foreign native calls forward without altering the local flight session.
        if (reader.pointer(request.core, 0x438) != rig || reader.pointer(request.core, 0x3c0) != context) return false;
        if (GetTickCount64() >= request.expires) { debug_stop_noclip(debug); debug.status = "Flight input timed out."; return false; }
        const auto bodies = debug_noclip_bodies(state.trial.base, request.client, request.entity);
        source_require(bodies.core == request.core && bodies.rig_wrapper == rig && bodies.context == context,
            "Skater motion changed; flight stopped.");
        if (!bodies.offboard || bodies.seconds == 0) return false;
        source_require(reader.value<std::uint8_t>(request.entity, 0x7e0) == 0, "A teleport started; flight stopped.");
        std::array<float,16> previous{};
        source_require(reader.raw(reinterpret_cast<std::uintptr_t>(supplied), target.data(), sizeof(target)) &&
            reader.raw(rig + 0x4720, previous.data(), sizeof(previous)) &&
            valid_flight_transform(target) && valid_flight_transform(previous), "Invalid skater motion target.");
        // The new build moved the simulation counter to +180. +170 now holds
        // state flags and can stay constant, suppressing every subsequent move.
        (void)reader.value<std::uint32_t>(request.core, 0x180);
        // Lead the skater's current root by one step instead of integrating the
        // previous target. The native drive is a spring toward this target: a
        // free-running target stretched it without bound at high speed (the
        // skater outran the camera) and the ragdoll snapped back on release.
        // Anchoring to the root bounds the stretch to one step and leaves no
        // stored energy when the input stops.
        for (std::size_t i = 0; i < 3; ++i) {
            source_require(std::isfinite(request.velocity[i]) && std::abs(request.velocity[i]) <= 6000, "Invalid flight input.");
            source_require(std::isfinite(bodies.root[i]) && std::abs(bodies.root[i]) <= 1000000, "Invalid skater root position.");
            target[12+i] = bodies.root[i] + request.velocity[i] * bodies.seconds;
        }
        source_require(valid_flight_transform(target), "Flight motion target is out of bounds.");
        reader.verify();
        source_require(sync_offboard_flight_velocity(state.trial.base, bodies.core, context, rig, request.velocity),
            "Off-board flight velocity ownership changed; flight stopped.");
        ++debug.noclip_motion_updates;
        return true;
    } catch (const SourceGuard& issue) { debug_stop_noclip(debug); debug.status = issue.message; }
      catch (...) { debug_stop_noclip(debug); debug.status = "Flight stopped after a motion error."; }
    return false;
}
void noclip_skater_motion(std::uintptr_t rig, std::uintptr_t context,
    const std::array<float,16>* supplied, std::uint8_t flags) {
    const auto original = source_state().motion_original.load(std::memory_order_acquire);
    alignas(16) std::array<float,16> target{};
    const auto* motion = noclip_motion_target(rig, context, supplied, target) ? &target : supplied;
    if (original) original(rig, context, motion, flags);
    if (source_state().offboard_boost_core.load(std::memory_order_acquire)) {
        std::uintptr_t core{};
        if (memory::peek(rig + 0x4630, core) &&
            core == source_state().offboard_boost_core.load(std::memory_order_acquire))
            noclip_apply_velocity(core);
    }
}
}
}

namespace dingosdk {
using namespace client_source::detail;

bool queue_jump_scale(std::uintptr_t client, std::uintptr_t entity, float factor) noexcept {
    SourceLastError error;
    try {
        auto& state = source_state();
        auto& j = jump_scale();
        if (!state.initialized.load(std::memory_order_acquire) || !state.velocity_guard_active.load(std::memory_order_acquire) ||
            !std::isfinite(factor) || factor <= 0 || factor > 20 || j.pending.load(std::memory_order_acquire)) return false;
        const auto bodies = debug_noclip_bodies(state.trial.base, client, entity);
        j.client = client;
        j.entity = entity;
        j.core = bodies.core;
        j.factor = factor;
        j.expires = GetTickCount64() + 150;
        j.outcome.store(0);
        j.pending.store(true, std::memory_order_release);
        return true;
    } catch (...) { return false; }
}
void set_push_speed(std::uintptr_t client, std::uintptr_t entity, float factor, float stock, float cruise) noexcept {
    auto& p = push_speed();
    p.client = client;
    p.entity = entity;
    p.cruise.store(cruise > 0 && cruise < 100 ? cruise : 0.0f, std::memory_order_relaxed);
    p.stock.store(stock > 1 && stock < 100 ? stock : 9.25f, std::memory_order_relaxed);
    p.factor.store(factor > 0.02f && factor <= 50 ? factor : 1.0f, std::memory_order_relaxed);
    p.expires.store(GetTickCount64() + 500, std::memory_order_release);
}
void set_freestyle_slide_upright(float strength) noexcept { freestyle_slide().upright.store(strength, std::memory_order_relaxed); }
std::uint32_t take_freestyle_root_trace(std::array<std::array<float, 6>, 64>& samples) noexcept {
    auto& t = root_trace();
    samples = t.samples;
    return t.count.exchange(0, std::memory_order_relaxed);
}
void freestyle_root_updated(std::uintptr_t rig) noexcept {
    auto& u = root_upright();
    const bool held = u.hold.load(std::memory_order_relaxed) && rig && rig == u.turn_rig.load(std::memory_order_relaxed);
    if (!held && (u.rig.load(std::memory_order_relaxed) != rig || GetTickCount64() - u.seen.load(std::memory_order_relaxed) > 200)) return;
    (void)upright_root(rig);
}
void freestyle_root_updating(std::uintptr_t rig) noexcept {
    auto& u = root_upright();
    const bool held = (u.hold.load(std::memory_order_relaxed) || u.heading_lock.load(std::memory_order_relaxed)) && rig && rig == u.turn_rig.load(std::memory_order_relaxed);
    if (held || (u.rig.load(std::memory_order_relaxed) == rig && GetTickCount64() - u.seen.load(std::memory_order_relaxed) <= 200)) (void)upright_root_source(rig);
    // The anti-casper: the game's mirror turns the skater over to the board's other side (the casper as seen from
    // behind: owner, 2026-10-14); turned half round about the vertical as well, the skater faces its own side again
    // with the front foot on the end that is down.
    if (u.turn.load(std::memory_order_relaxed) && rig && rig == u.turn_rig.load(std::memory_order_relaxed)) {
        std::uintptr_t source{};
        if (!memory::peek(rig + 0x59b0, source) || !source) return;
        std::array<float, 12> rows{};
        __try {
            std::memcpy(rows.data(), reinterpret_cast<const void*>(source + 0x5b0), sizeof(rows));
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        for (int i : {0, 1, 2, 8, 9, 10}) rows[i] = -rows[i]; // side and forward rows; up stays
        if (copy_to(source + 0x5b0, rows.data(), sizeof(rows))) u.turned.fetch_add(1, std::memory_order_relaxed);
    }
}
void request_parts_log() noexcept { parts_log_wanted.store(true); }
void set_freestyle_root_hold(std::uintptr_t rig, bool on) noexcept {
    root_upright().turn_rig.store(rig, std::memory_order_relaxed);
    root_upright().hold.store(on, std::memory_order_relaxed);
}
void set_freestyle_root_heading_lock(std::uintptr_t rig, bool on) noexcept {
    if (on) root_upright().turn_rig.store(rig, std::memory_order_relaxed);
    root_upright().heading_lock.store(on, std::memory_order_relaxed);
}
void set_freestyle_root_turn(std::uintptr_t rig, bool on) noexcept {
    root_upright().turn_rig.store(rig, std::memory_order_relaxed);
    root_upright().turn.store(on, std::memory_order_relaxed);
}
std::uint32_t take_freestyle_root_turned() noexcept { return root_upright().turned.exchange(0, std::memory_order_relaxed); }
std::uint32_t take_freestyle_root_source(float& lowest, float& mean_tip, std::uint32_t& calls) noexcept {
    auto& u = root_upright();
    lowest = u.source_low.exchange(2.0f, std::memory_order_relaxed);
    calls = u.source_calls.exchange(0, std::memory_order_relaxed);
    const float sum = u.source_sum.exchange(0.0f, std::memory_order_relaxed);
    mean_tip = calls ? sum / static_cast<float>(calls) : 0.0f;
    return u.source_fixed.exchange(0, std::memory_order_relaxed);
}
void set_freestyle_root_upright(int mode) noexcept { root_upright().mode.store(mode, std::memory_order_relaxed); }
void set_freestyle_target_heights(float up, float right) noexcept {
    target_up.store(up, std::memory_order_relaxed);
    target_right.store(right, std::memory_order_relaxed);
}
void set_freestyle_rider_drives(int mode, int feet) noexcept {
    rider_drives().mode.store(mode, std::memory_order_relaxed);
    rider_drives().feet.store(feet, std::memory_order_relaxed);
}
std::string take_freestyle_rider_drives(std::uint32_t& writes, std::uint32_t& regame) noexcept {
    auto& d = rider_drives();
    writes = d.writes.exchange(0, std::memory_order_relaxed);
    regame = d.regame.exchange(0, std::memory_order_relaxed);
    try {
        std::lock_guard lock(d.text_lock);
        return std::exchange(d.text, {});
    } catch (...) { return {}; }
}
std::uint32_t take_freestyle_root_upright(float& low, float& high, std::uint32_t& fixed) noexcept {
    auto& u = root_upright();
    low = u.low.exchange(2.0f, std::memory_order_relaxed);
    high = u.high.exchange(-2.0f, std::memory_order_relaxed);
    fixed = u.fixed.exchange(0, std::memory_order_relaxed);
    return u.calls.exchange(0, std::memory_order_relaxed);
}
void set_freestyle_slide_hold(bool held) noexcept { freestyle_slide().hold.store(held, std::memory_order_relaxed); }
void set_freestyle_slide_stable(bool stable) noexcept { freestyle_slide().stable.store(stable, std::memory_order_relaxed); }
void take_freestyle_slide_roll(float& peak_rate, float& swing) noexcept {
    peak_rate = freestyle_slide().roll_peak.exchange(0, std::memory_order_relaxed);
    swing = freestyle_slide().roll_swing.exchange(0, std::memory_order_relaxed);
}
void set_tricks_step(void (*step)() noexcept) noexcept { tricks_step().store(step, std::memory_order_release); }
void set_freestyle_slide_mean(bool mean) noexcept { freestyle_slide().mean.store(mean, std::memory_order_relaxed); }
void set_freestyle_slide_soft(std::uint32_t milliseconds) noexcept { freestyle_slide().soft_ms.store(milliseconds, std::memory_order_relaxed); }
void set_freestyle_slide_level(bool level) noexcept { freestyle_slide().level.store(level, std::memory_order_relaxed); }
void set_board_turn_owner(std::uintptr_t client, std::uintptr_t entity) noexcept {
    board_turn_back().client.store(client, std::memory_order_relaxed);
    board_turn_back().entity.store(entity, std::memory_order_relaxed);
}
void set_hand_pull(std::uintptr_t core, std::uint32_t milliseconds, int part, int part2, float x, float y, float z, float gain, float crouch) noexcept {
    auto& h = hand_pull();
    h.core.store(core, std::memory_order_relaxed);
    h.part.store(part, std::memory_order_relaxed);
    h.part2.store(part2, std::memory_order_relaxed);
    h.x.store(x, std::memory_order_relaxed);
    h.y.store(y, std::memory_order_relaxed);
    h.z.store(z, std::memory_order_relaxed);
    h.gain.store(gain, std::memory_order_relaxed);
    h.crouch.store(crouch, std::memory_order_relaxed);
    h.until.store(milliseconds ? GetTickCount64() + milliseconds : 0, std::memory_order_relaxed);
}
std::uint32_t take_hand_pull(float& gap) noexcept {
    gap = hand_pull().gap.load(std::memory_order_relaxed);
    return hand_pull().steps.exchange(0, std::memory_order_relaxed);
}
void set_board_carry(std::uintptr_t core, std::uint32_t milliseconds, float rise, float reach, float top, float hold) noexcept {
    auto& c = board_carry();
    c.start.store(0, std::memory_order_release);
    if (!milliseconds) return;
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    c.core.store(core, std::memory_order_relaxed);
    c.rise.store(rise, std::memory_order_relaxed);
    c.reach.store(reach, std::memory_order_relaxed);
    c.top.store(top, std::memory_order_relaxed);
    c.hold.store(hold, std::memory_order_relaxed);
    c.length.store(static_cast<std::uint64_t>(frequency.QuadPart) * milliseconds / 1000, std::memory_order_relaxed);
    c.start.store(static_cast<std::uint64_t>(now.QuadPart), std::memory_order_release);
}
std::uint32_t take_board_carry(float& gap) noexcept {
    gap = board_carry().gap.load(std::memory_order_relaxed);
    return board_carry().steps.exchange(0, std::memory_order_relaxed);
}
void set_feet_free(std::uintptr_t core, std::uint32_t milliseconds, float scale) noexcept {
    auto& f = feet_free();
    f.core.store(core, std::memory_order_relaxed);
    f.scale.store(scale, std::memory_order_relaxed);
    f.until.store(milliseconds ? GetTickCount64() + milliseconds : 0, std::memory_order_relaxed);
}
std::uint32_t take_feet_free_writes() noexcept { return feet_free().writes.exchange(0, std::memory_order_relaxed); }
bool request_board_turn_back(void (*after)() noexcept) noexcept {
    auto& t = board_turn_back();
    if (!after || !t.client.load(std::memory_order_relaxed) || !t.entity.load(std::memory_order_relaxed)) return false;
    t.expires.store(GetTickCount64() + 200, std::memory_order_relaxed);
    t.after.store(after, std::memory_order_release);
    return true;
}
float watched_board_spin() noexcept { return board_turn_back().watch_spin.load(std::memory_order_relaxed); }
bool watch_board_heading(float& angle, float& up) noexcept {
    auto& t = board_turn_back();
    const bool was = GetTickCount64() < t.watch_until.load(std::memory_order_relaxed);
    t.watch_until.store(GetTickCount64() + 300, std::memory_order_relaxed);
    angle = t.watch_angle.load(std::memory_order_relaxed);
    up = t.watch_up.load(std::memory_order_relaxed);
    return was;
}
std::uint32_t take_board_turn_backs(float& last) noexcept {
    last = board_turn_back().last.load(std::memory_order_relaxed);
    return board_turn_back().done.exchange(0, std::memory_order_relaxed);
}
void set_freestyle_slide(std::uintptr_t client, std::uintptr_t entity, float deceleration) noexcept {
    auto& f = freestyle_slide();
    f.client = client;
    f.entity = entity;
    f.deceleration.store(deceleration > 0 && deceleration < 100 ? deceleration : 0.0f, std::memory_order_relaxed);
    f.expires.store(GetTickCount64() + 500, std::memory_order_release);
}
JumpScaleResult take_jump_scale_result() noexcept {
    auto& j = jump_scale();
    if (j.pending.load(std::memory_order_acquire)) return {};
    return {j.outcome.exchange(0), j.up_speed.load()};
}

bool start_client_noclip_velocity(std::uintptr_t base) noexcept {
    SourceLastError error;
    try {
        auto& state = source_state();
        std::lock_guard lock(state.initialization_mutex);
        if (!state.initialized.load() || state.trial.base != base) return false;
        if (state.velocity_guard_attempted) return state.velocity_guard_active.load();
        state.velocity_guard_attempted = true;
        SourceReader reader;
        std::array<unsigned char, 32> bytes{};
        if (!reader.raw(base + spawn::physics_update, bytes.data(), bytes.size()) || bytes != spawn::physics_update_prefix ||
            reader.pointer(base + addr::no_bail::bail_core_vtable, 0x58) != base + spawn::physics_update ||
            !reader.raw(base + spawn::skater_motion, bytes.data(), bytes.size()) || bytes != spawn::skater_motion_prefix ||
            !offboard_flight_compatible(base)) return false;
        reader.verify();
        auto* target = reinterpret_cast<void*>(base + spawn::physics_update);
        auto* motion_target = reinterpret_cast<void*>(base + spawn::skater_motion);
        void* original{};
        if (hook_prepare(target, reinterpret_cast<void*>(&noclip_physics_update), &original) != HookOk) return false;
        if (!original) { (void)hook_remove(target); return false; }
        state.velocity_update_original.store(reinterpret_cast<SourcePhysicsUpdate>(original), std::memory_order_release);
        original = nullptr;
        if (hook_prepare(motion_target, reinterpret_cast<void*>(&noclip_skater_motion), &original) != HookOk) {
            (void)hook_remove(target); state.velocity_update_original.store(nullptr); return false;
        }
        if (!original) {
            (void)hook_remove(motion_target); (void)hook_remove(target);
            state.velocity_update_original.store(nullptr); return false;
        }
        state.motion_original.store(reinterpret_cast<SourceSkaterMotion>(original), std::memory_order_release);
        // Keep trampolines on uncertain enable results; handlers still forward.
        if (hook_enable(target) != HookOk || hook_enable(motion_target) != HookOk) return false;
        state.velocity_guard_active.store(true, std::memory_order_release);
        return true;
    } catch (...) { return false; }
}
}
