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
// ReSkate Tricks: the watched skater wipes out at its next state choice on the ground (a lost casper or primo).
void request_wipeout() noexcept;
// ReSkate Tricks: the watched skater cannot wipe out for this long (a casper or primo being caught, landed or held:
// the game's contact tests throw the skater off a board that is on its side or upside down).
void protect_pose(std::uint64_t milliseconds) noexcept;
// ReSkate Tricks: while it lasts, a grounded board in an air state lands straight into the ground animation state.
void stomp_pose(std::uint64_t milliseconds) noexcept;
// ReSkate Tricks: while it lasts, the ground animation state is not left for a dismount. 0 ends it.
void keep_pose_on_board(std::uint64_t milliseconds) noexcept;
std::uint32_t take_pose_kept() noexcept;
// ReSkate Tricks: while it lasts, riding goes to the ground animation state, where the board follows its target frame. 0 ends it.
void hold_ground_animation(std::uint64_t milliseconds) noexcept;
// How many times that happened since the last call; contact: wheels (low 4 bits), truck 0x10, deck 0x20, body 0x40, trajectory 0x80.
std::uint32_t take_pose_stomps(std::uint32_t& contact) noexcept;
// Stopping flight must not discard the independent manual preference.
void clear_no_bail_flight() noexcept;
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
// Further listeners on the same steps (the grind exit control, grind_exit_control.h),
// independent of the observer above so none displaces Hall of Meat's. Same thread and timing. Adding one already there, or removing one that is not, does nothing;
// false when all slots are taken.
inline constexpr std::size_t no_bail_step_listener_slots = 4;
bool add_no_bail_step_listener(NoBailStepObserver listener) noexcept;
// Any thread: the watched local skater bails at its next physics step, as from a slam of
// `magnitude` (Board Break: a board that snaps on a rail throws the rider). The cause goes in just
// before the game's selector picks the next state, the way the game's own contact tests record
// theirs, so the game makes the wipeout itself; while No Bail protects the skater it is dropped like
// any other. Expires unused after a quarter of a second.
void no_bail_request_wipeout(float magnitude) noexcept;
void remove_no_bail_step_listener(NoBailStepObserver listener) noexcept;
}
