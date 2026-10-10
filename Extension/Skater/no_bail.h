#pragma once
#include <cstdint>

namespace dingosdk {
bool start_no_bail(std::uintptr_t image_base) noexcept;
bool no_bail_available() noexcept;
// Publish from the validated local client tick. Returns owner availability even
// when both controls are off. Manual protection expires if ticks stop arriving.
bool update_no_bail(std::uintptr_t client, std::uintptr_t entity, bool manual,
    bool flying, std::uint64_t flight_expires) noexcept;
void clear_no_bail() noexcept;
// S.K.A.T.E.: while `locked`, the local skater cannot get back on the board once it is off
// (its mount request is dropped). Publish from the client tick; it expires if ticks stop.
// Releasing it also leaves the skater's teleport option on the board again, since a turn's
// teleport may have set it off (skater component +0xc0) and the SDK's own teleports keep it.
void update_board_lock(std::uintptr_t client, std::uintptr_t entity, bool locked) noexcept;
// The physics state the local skater's selector last chose, for the trainer (air time, bail
// markers). Publish the skater to watch from the client tick; it expires if ticks stop.
struct PhysicsStateWatch {
    bool valid{};
    std::uint32_t state{};
    std::uint32_t previous{}; // the state before this one
    float previous_seconds{}; // how long that one lasted
    std::uint64_t changes{}, wipeouts{}; // counted since the process started
};
void watch_physics_state(std::uintptr_t client, std::uintptr_t entity) noexcept;
PhysicsStateWatch watched_physics_state() noexcept;
// Stopping flight must not discard the independent manual preference.
void clear_no_bail_flight() noexcept;
// The local skater's turn over each flight, for the trainer's revert boost: measured in the
// physics state hook, every physics step, so the start and the end of the turn are not lost.
// From AutoRevertBoost by Sivaes, jaq and OVM (github.com/Sivaes/AutoRevertBoost, GPL-3.0).
struct RevertLanding {
    std::uint64_t sequence{};     // 0: no landing yet; one more per finished flight
    float spin_degrees{};         // signed turn about the vertical axis from takeoff to landing
    float board_spin_degrees{};   // the same for the board's deck (if board_valid)
    float board_offset_degrees{}; // the deck's heading minus the skater's at touchdown (if board_valid)
    float heading_degrees{};      // where the deck (the skater, without a deck pose) points at touchdown: 0 = +Z, as atan2(x, z)
    bool board_valid{};
    std::uint32_t from{}, to{};   // the last air state, and the state chosen on landing
    std::uint32_t steps{};        // physics steps in the air
    std::uint64_t air_ms{}, landed_at{}; // flight time, and GetTickCount64() at the landing
};
// Which skater to measure: its physics selector, its world matrix and the deck's physics body
// (rotation rows from +0x20). Asked again at least twice a second, or the measuring stops.
void watch_revert_spin(std::uintptr_t selector, std::uintptr_t transform, std::uintptr_t board) noexcept;
RevertLanding last_revert_landing() noexcept;
// The local skater as No Bail follows it (update_no_bail publishes it), for a feature that reads it
// (Hall of Meat): any thread, its ownership chain resolved again now.
struct NoBailSkater {
    std::uintptr_t base{}, entity{}, core{}, context{}, rig{};
};
bool no_bail_skater(NoBailSkater& skater) noexcept;
// Every physics step of that skater, whether or not No Bail protects it: No Bail's hook on the
// skeleton response runs once per step, right after the step's body contacts, and hands it on after
// its own filtering, so a wipeout it filtered never happened. `seconds` is the step's simulated time.
// One observer, nullptr for none; it runs on the physics thread.
using NoBailStepObserver = void (*)(const NoBailSkater& skater, float seconds, bool wipeout) noexcept;
void set_no_bail_step_observer(NoBailStepObserver observer) noexcept;
}
