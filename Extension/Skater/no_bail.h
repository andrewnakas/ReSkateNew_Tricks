#pragma once
#include <cstddef>
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
// Fast probe: one row for every physics-state choice the game makes for the local skater (many per
// second, where the client tick sees about nine), with the state it was in, the state the game wanted,
// what was returned, the controller buttons and some of the skater's request words. Read-only. Rows are
// queued by the selector hook and drained by the client tick; a full queue drops rows.
struct FastProbeRow {
    std::int64_t ticks{}; // QueryPerformanceCounter
    std::uint32_t current{}, wanted{}, chosen{}, buttons{};
    std::uint32_t words[18]{}; // context+0x13c4..0x13e0, then +0x140c..0x1430
};
void fast_probe_enable(bool on) noexcept;
std::size_t fast_probe_drain(FastProbeRow* rows, std::size_t capacity) noexcept;
// Dark Pop: for each flight from the ground, the selector hook has the game shown D-pad Right let go `after` seconds
// after the takeoff (input_capture.cpp does the showing). A fired flight (RB, then D-pad Right) is protected like No
// Bail, and if the game turns its landing into a bail the pop is chosen instead. Publish from the client tick.
void dark_pop_update(bool enabled, float after) noexcept;
struct DarkPopLast {
    std::uint64_t flights{}; // timed flights finished since the process started
    bool pressed{};          // D-pad Right was pressed in the last one
    std::uint32_t landed{};  // the state used at its landing: 200 = popped again, 100-199 = rode away
    std::uint32_t wanted{};  // what the game chose there (504 dismount, 300 wipeout, 200 pop, ...)
    bool forced{};           // the game wanted a bail and the pop was chosen instead
    std::uint64_t pop_landings{}; // landings of a pop's flight since the process started
    std::uint32_t pop_landed{};   // the state the last of them went to
    std::uint64_t holds{};        // dismounts held off during a pop's flight since the process started
    float seconds{};         // how long it was in the air
    float release_after{};   // when after the takeoff the game was shown the release (negative: not before the landing)
};
DarkPopLast dark_pop_last() noexcept;
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
