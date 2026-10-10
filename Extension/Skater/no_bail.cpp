#include "no_bail.h"
#include "Engine/Game/Input/controller_bindings.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include <array>
#include <atomic>
#include <cmath>
#include <intrin.h>

namespace dingosdk {
namespace {
using namespace addr::no_bail;
// Native cause recorder: RCX=collector, EDX=reason, XMM2=magnitude.
using RecordCause = void (*)(std::uintptr_t, std::int32_t, float);
using ChooseState = std::uint32_t (*)(std::uintptr_t, std::uint32_t);
using SkeletonResponse = void (*)(std::uintptr_t, float, bool);
using PublishAnimation = void (*)(std::uintptr_t);
using ResetCauses = void (*)(std::uintptr_t);
constexpr std::uintptr_t highest = memory::highest_user_address;
// The hooks re-resolve the local skater's ownership (some 35 fields) on every
// protected physics step: guarded same-process copies, not a system call each.
template<class T> bool read(std::uintptr_t address, T& value) noexcept { return memory::peek(address, value); }
struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
};
std::uintptr_t pointer(std::uintptr_t object, std::uintptr_t offset = 0) noexcept {
    std::uintptr_t value{};
    if (object < 0x10000 || object > highest - offset || !read(object + offset, value) ||
        value < 0x10000 || value > highest - 0x1000100) return 0;
    return value;
}
struct Owner {
    std::uintptr_t client{}, entity{}, player{}, handle{}, component{}, core{}, context{}, selector{}, causes{}, rig{};
    bool operator==(const Owner&) const = default;
};
struct Lease {
    Owner owner;
    std::uint64_t manual_until{}, flight_until{};
    bool active(std::uint64_t now) const noexcept {
        return now < manual_until || now < flight_until;
    }
};
struct BoardLock {
    Owner owner;
    std::uint64_t until{};
};
struct Protection {
    std::uintptr_t base{};
    bool mount_test{}; // the selector's mount request test is the known one
    BoardLock board;
    RecordCause cause_original{};
    ChooseState choose_original{};
    SkeletonResponse skeleton_original{};
    PublishAnimation publish_original{};
    ResetCauses reset_causes{};
    std::atomic<bool> ready{};
    SRWLOCK lock = SRWLOCK_INIT;
    Lease lease;
    std::atomic<NoBailStepObserver> step_observer{};
};
Protection& protection() { static auto* value = new Protection; return *value; }
struct StateWatch {
    std::atomic<std::uintptr_t> selector{};
    std::atomic<std::uint64_t> until{}, changes{}, wipeouts{};
    std::atomic<std::uint32_t> state{}, previous{};
    std::atomic<std::int64_t> since{}, previous_ticks{}; // performance-counter ticks
};
StateWatch& state_watch() { static auto* value = new StateWatch; return *value; }
// The turn of one skater over each flight (no_bail.h). The published fields are atomics; the
// rest is only touched by that skater's physics selector.
struct SpinWatch {
    std::atomic<std::uintptr_t> selector{}, transform{}, board{};
    std::atomic<std::uint64_t> until{};
    bool airborne{}, have_yaw{}, board_ok{};
    float yaw{}, spin{}, board_yaw{}, board_spin{};
    std::uint32_t steps{}, last_air{};
    std::uint64_t took_off{};
    SRWLOCK lock = SRWLOCK_INIT;
    RevertLanding last;
};
SpinWatch& spin_watch() { static auto* value = new SpinWatch; return *value; }
void track_spin(std::uintptr_t selector, std::uint32_t chosen) noexcept {
    constexpr float pi = 3.14159265f;
    auto& s = spin_watch();
    const auto now = GetTickCount64();
    if (selector != s.selector.load(std::memory_order_acquire)) return;
    if (now >= s.until.load(std::memory_order_acquire)) {
        s.airborne = s.have_yaw = false;
        return;
    }
    std::array<float, 16> m{};
    const auto transform = s.transform.load(std::memory_order_acquire);
    if (!transform || !read(transform, m) || !std::isfinite(m[8]) || !std::isfinite(m[10]) || std::abs(m[8]) + std::abs(m[10]) < 1e-3f) {
        s.airborne = s.have_yaw = false;
        return;
    }
    const float yaw = std::atan2(m[8], m[10]);
    // The deck's heading, read the same way from its three rotation rows; only a set of unit rows counts.
    std::array<float, 12> b{};
    float board_yaw{};
    bool board_ok = false;
    if (const auto board = s.board.load(std::memory_order_acquire); board && read(board + 0x20, b)) {
        const auto length = [&](int r) { return b[r * 4] * b[r * 4] + b[r * 4 + 1] * b[r * 4 + 1] + b[r * 4 + 2] * b[r * 4 + 2]; };
        board_ok = std::isfinite(length(0) + length(1) + length(2)) && std::abs(length(0) - 1) < 0.05f && std::abs(length(1) - 1) < 0.05f &&
                   std::abs(length(2) - 1) < 0.05f && std::abs(b[8]) + std::abs(b[10]) > 1e-3f;
        if (board_ok) board_yaw = std::atan2(b[8], b[10]);
    }
    const auto wrap = [](float d) {
        while (d > pi) d -= 2 * pi;
        while (d < -pi) d += 2 * pi;
        return d;
    };
    const bool air = chosen >= 200 && chosen <= 299;
    if (!s.airborne) {
        if (air && s.have_yaw) {
            // Counted from the last ground pose, so the turn begun on the takeoff step is kept.
            s.airborne = true;
            s.spin = wrap(yaw - s.yaw);
            s.board_spin = board_ok && s.board_ok ? wrap(board_yaw - s.board_yaw) : 0.0f;
            s.board_ok = board_ok && s.board_ok;
            s.steps = 1;
            s.took_off = now;
            s.last_air = chosen;
        }
    } else {
        s.spin += wrap(yaw - s.yaw);
        if (s.board_ok && board_ok) s.board_spin += wrap(board_yaw - s.board_yaw);
        else s.board_ok = false;
        if (air) {
            ++s.steps;
            s.last_air = chosen;
        } else {
            // The pose read here is the one the flight ended with, before the game's revert turns it.
            RevertLanding landing;
            landing.spin_degrees = s.spin * 180.0f / pi;
            landing.board_spin_degrees = s.board_spin * 180.0f / pi;
            landing.board_valid = s.board_ok;
            landing.board_offset_degrees = wrap(board_yaw - yaw) * 180.0f / pi;
            landing.heading_degrees = (s.board_ok ? board_yaw : yaw) * 180.0f / pi;
            landing.from = s.last_air;
            landing.to = chosen;
            landing.steps = s.steps;
            landing.air_ms = now - s.took_off;
            landing.landed_at = now;
            AcquireSRWLockExclusive(&s.lock);
            landing.sequence = s.last.sequence + 1;
            s.last = landing;
            ReleaseSRWLockExclusive(&s.lock);
            s.airborne = false;
        }
    }
    s.yaw = yaw;
    s.have_yaw = true;
    if (!s.airborne) s.board_ok = board_ok; // on the ground: whether the next takeoff has a board pose
    s.board_yaw = board_yaw;
}

// Recheck live local ownership at use time. A retained physics address alone
// must never protect another skater after a respawn or level change.
bool resolve(std::uintptr_t client, std::uintptr_t entity, Owner& o) noexcept {
    const auto base = protection().base;
    unsigned state{}, manager_offset{};
    if (pointer(client) != base + addr::engine::client_vtable || !read(client + 0xc4, state) ||
        (state != 13 && state != 21) || pointer(entity) != base + addr::engine::skater_entity_vtable ||
        !read(base + addr::engine::context_player_manager_offset, manager_offset) || manager_offset > 0x1000000) return false;
    const auto context = pointer(client, 8);
    const auto manager = pointer(context, manager_offset);
    const auto begin = pointer(manager, 0x4c8), end = pointer(manager, 0x4d0);
    if (!context || pointer(entity, 0x20) != context || pointer(manager) != base + addr::engine::local_player_manager_vtable ||
        !begin || end != begin + 8) return false;
    o.player = pointer(begin);
    std::uint8_t local{}, remote{}, teleport{};
    if (pointer(o.player) != base + addr::engine::local_player_vtable || pointer(o.player, 0x78) != context ||
        !read(o.player + 0x45, local) || local != 1 || !read(o.player + 0x44, remote) || remote ||
        pointer(o.player, 0xb8) != entity || pointer(entity, 0xf8) != o.player ||
        !read(entity + 0x7e0, teleport) || teleport) return false;
    o.handle = pointer(o.player, 0xb0);
    const auto collection = pointer(entity, 0x70);
    o.component = pointer(entity, 0x628);
    o.core = pointer(o.component, 0x70);
    o.context = pointer(o.core, 0x3c0);
    o.selector = pointer(o.core, 0x440);
    o.causes = pointer(o.core, 0x428);
    o.rig = pointer(o.core, 0x438);
    if (pointer(o.handle) != entity + 8 || pointer(collection) != entity ||
        pointer(o.component) != base + addr::engine::skater_component_vtable || pointer(o.component, 0x18) != collection ||
        pointer(o.core) != base + bail_core_vtable || !o.context || !o.selector ||
        pointer(o.selector, 8) != o.context || pointer(o.causes, 0x20) != o.context || pointer(o.rig) != o.context || pointer(o.rig, 0x4630) != o.core)
        return false;
    o.client = client;
    o.entity = entity;
    return true;
}
// Dark Pop borrows this protection around its timed landings (defined with Dark Pop below).
bool dark_pop_protecting(std::uint64_t now) noexcept;
bool protected_owner(std::uintptr_t object, std::uintptr_t Owner::* member, Owner* owner = nullptr) noexcept {
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return false;
    // The lock only copies SDK data, and is never held across native code or
    // memory reads. Hooks do not contend for the debug/camera action lock.
    AcquireSRWLockShared(&p.lock);
    const auto lease = p.lease;
    ReleaseSRWLockShared(&p.lock);
    const auto now = GetTickCount64();
    if (!(lease.active(now) || dark_pop_protecting(now)) || object != lease.owner.*member) return false;
    Owner current;
    if (!resolve(lease.owner.client, lease.owner.entity, current) || current != lease.owner) return false;
    if (owner) *owner = current;
    return true;
}
// The local skater this process published (the lease keeps its owner even with No Bail off), checked
// against the live game objects. Says nothing about whether protection is on.
bool local_owner(std::uintptr_t object, std::uintptr_t Owner::* member, Owner* owner = nullptr) noexcept {
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return false;
    AcquireSRWLockShared(&p.lock);
    const auto lease = p.lease;
    ReleaseSRWLockShared(&p.lock);
    if (!lease.owner.entity || !object || object != lease.owner.*member) return false;
    Owner current;
    if (!resolve(lease.owner.client, lease.owner.entity, current) || current != lease.owner) return false;
    if (owner) *owner = current;
    return true;
}
// The published owner, its chain resolved again now; when `rig` is given, only if it is that owner's.
bool published_skater(std::uintptr_t rig, NoBailSkater& skater) noexcept {
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return false;
    AcquireSRWLockShared(&p.lock);
    const auto published = p.lease.owner;
    ReleaseSRWLockShared(&p.lock);
    Owner current;
    if (!published.entity || (rig && rig != published.rig) || !resolve(published.client, published.entity, current) ||
        current != published) return false;
    skater = {p.base, current.entity, current.core, current.context, current.rig};
    return true;
}
bool cancel_request(std::uintptr_t context, std::uintptr_t offset, LONG mask) noexcept {
    const auto address = context + offset;
    if (context < 0x10000 || context > highest - offset - sizeof(LONG) ||
        (address & (alignof(LONG) - 1)) != 0) return false;
    // Consume only the identified request bit. Preserve unrelated native flags
    // even when another producer updates the same word.
    __try {
        _InterlockedAnd(reinterpret_cast<volatile LONG*>(address), ~mask);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool cancel_impact_request(std::uintptr_t context) noexcept {
    return cancel_request(context, impact_request_offset, impact_request_mask);
}
bool cancel_wipeout_requests(std::uintptr_t context) noexcept {
    return cancel_impact_request(context) &&
        cancel_request(context, animation_request_offset, animation_request_mask);
}
bool reset_pending_causes(std::uintptr_t causes) noexcept {
    const auto reset = protection().reset_causes;
    if (!reset) return false;
    __try {
        reset(causes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool filter_requests(std::uintptr_t object, std::uintptr_t Owner::* member) noexcept {
    LastError error;
    Owner owner;
    // Native collision/landing checks also write the collector inline. Reset
    // it at each consumer, after those writes, before either animation's bail
    // or recover/runout query. Filtering record_cause alone misses this path.
    return protected_owner(object, member, &owner) && cancel_wipeout_requests(owner.context) &&
        reset_pending_causes(owner.causes);
}
bool suppress_cause(std::uintptr_t causes, std::int32_t reason, std::uintptr_t caller) noexcept {
    LastError error;
    Owner owner;
    if (!protected_owner(causes, &Owner::causes, &owner)) return false;
    for (const auto& impact : impact_bail_calls) {
        if (caller == protection().base + impact.return_rva && reason == impact.reason)
            return cancel_impact_request(owner.context);
    }
    return true;
}
void record_cause(std::uintptr_t causes, std::int32_t reason, float magnitude) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool protect = suppress_cause(causes, reason, caller);
    // Do not force the native recovery/stumble predicate (recovery_predicate). Its
    // result is also exported to animation at +0x9e, even without a collision.
    // Stop new causes before they reach either the wipeout or runout decision;
    // the native per-step reset still owns clearing the collector's history.
    if (!protect) protection().cause_original(causes, reason, magnitude);
}
// The local skater waiting on foot for its S.K.A.T.E. turn: its mount request goes.
void hold_off_board(std::uintptr_t selector, std::uint32_t current) noexcept {
    auto& p = protection();
    if (current != offboard_physics_state || !p.mount_test || !p.ready.load(std::memory_order_acquire)) return;
    AcquireSRWLockShared(&p.lock);
    const auto board = p.board;
    ReleaseSRWLockShared(&p.lock);
    if (GetTickCount64() >= board.until || selector != board.owner.selector) return;
    Owner current_owner;
    if (!resolve(board.owner.client, board.owner.entity, current_owner) || current_owner != board.owner) return;
    (void)cancel_request(current_owner.context, animation_request_offset, mount_request_mask);
}
// ---- Dark Pop (auto release) ---------------------------------------------------------
// Recordings of a 360 inward heelflip, dark catch (41 flights) showed the pop depends mostly on when, counted from the
// takeoff, the game sees D-pad Right let go: 0.70-0.73 s after it 14 of 17 popped, 0.68 s or earlier 1 of 11. (The
// landing itself is no clock: the game lands a bail at 0.82 s but decides a pop at 0.85 s.) So the mod times the
// release from the takeoff, `after` seconds later.
// Those recordings were made with 0.2.0, which (unnoticed) lent No Bail's protection while D-pad Right was held
// and briefly after: 19 of 32 popped. 0.3.0 and 0.3.1 dropped it and 0 of 20 popped, nearly all wipeouts (300) that
// the protection would have swapped away. So the protection is back: from the moment the game sees D-pad Right
// pressed in a timed flight until shortly after its landing.
// 0.3.3 (protection from the D-pad Right press): no more wipeouts, but 9 of 11 dismounts (504) at the landing tick,
// which No Bail never swaps. The first Dark Pop forced the pop at that tick and was the version that worked. So a fired
// flight whose landing the game turns into a bail (504 or 300) now pops anyway: the dismount request
// (context+0x13d4 bit 0x100) is dropped, and while the pop's flight lasts a dismount is held off like No Bail holds
// off a wipeout. Protection runs until shortly after the pop's own landing.
extern "C" void DingoSDKOverlayDarkPopFlightStart(std::int64_t release_at) noexcept;
extern "C" bool DingoSDKOverlayDarkPopFlightEnd(std::int64_t* shown_release) noexcept;
extern "C" bool DingoSDKOverlayDarkPopLatched() noexcept;
constexpr std::uint64_t dark_pop_settle_ms = 300; // protection after the landing of a timed flight with D-pad Right
constexpr std::uint64_t dark_pop_pop_ms = 2500;   // the longest a pop's flight is protected if its landing is missed
constexpr LONG dark_pop_dismount_mask = 0x100;    // context+0x13d4 (animation_request_offset): the dismount request
bool dark_pop_air(std::uint32_t state) noexcept { return state >= 200 && state < 300; }
bool dark_pop_ground(std::uint32_t state) noexcept { return state >= 100 && state < 200; }
struct DarkPop {
    std::atomic<bool> enabled{};
    std::atomic<float> after{0.715f};
    std::atomic<std::int64_t> takeoff{}; // QPC of the current timed flight (0 = none)
    std::atomic<std::uint64_t> settle_until{}; // GetTickCount64: protection after a timed landing
    std::atomic<std::uint64_t> pop_until{};    // GetTickCount64: a fired flight popped; its flight is protected until then
    std::atomic<std::uint64_t> flights{}, holds{}, pop_landings{};
    std::atomic<bool> pressed{}, forced{};
    std::atomic<std::uint32_t> landed{}, wanted{}, pop_landed{};
    std::atomic<float> seconds{}, release_after{};
};
DarkPop& dark_pop() { static auto* value = new DarkPop; return *value; }
std::int64_t dark_pop_frequency() noexcept {
    static const std::int64_t value = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart > 0 ? std::int64_t{f.QuadPart} : std::int64_t{1}; }();
    return value;
}
float dark_pop_seconds(std::int64_t ticks) noexcept {
    return static_cast<float>(static_cast<double>(ticks) / static_cast<double>(dark_pop_frequency()));
}
bool dark_pop_protecting(std::uint64_t now) noexcept {
    auto& d = dark_pop();
    if (!d.enabled.load(std::memory_order_relaxed)) return false;
    if (now < d.settle_until.load(std::memory_order_relaxed) || now < d.pop_until.load(std::memory_order_relaxed)) return true;
    return d.takeoff.load(std::memory_order_relaxed) && DingoSDKOverlayDarkPopLatched();
}
// After the original selector (and No Bail's wipeout swap) has chosen; returns the state to use.
std::uint32_t dark_pop_choose(std::uintptr_t selector, std::uint32_t current, std::uint32_t chosen) noexcept {
    auto& d = dark_pop();
    if (!d.enabled.load(std::memory_order_relaxed)) {
        if (d.takeoff.exchange(0, std::memory_order_relaxed)) (void)DingoSDKOverlayDarkPopFlightEnd(nullptr);
        d.pop_until.store(0, std::memory_order_relaxed);
        return chosen;
    }
    const auto tick = GetTickCount64();
    const bool popping = tick < d.pop_until.load(std::memory_order_relaxed);
    const bool lands = dark_pop_air(current) && (!dark_pop_air(chosen) || (chosen == 200 && current != 200));
    const bool takes_off = dark_pop_ground(current) && dark_pop_air(chosen);
    const bool dismount_in_pop = popping && dark_pop_air(current) && chosen == offboard_physics_state;
    if (!lands && !takes_off && !dismount_in_pop) return chosen;
    Owner owner;
    if (!local_owner(selector, &Owner::selector, &owner)) return chosen;
    const bool fired = d.takeoff.load(std::memory_order_relaxed) && DingoSDKOverlayDarkPopLatched();
    // A dismount while the pop's flight is protected: stay in the air, as No Bail does with a wipeout.
    // (Not on the landing tick of a fired flight: that is handled below.)
    if (dismount_in_pop && !fired) {
        (void)cancel_request(owner.context, animation_request_offset, dark_pop_dismount_mask);
        d.holds.fetch_add(1, std::memory_order_relaxed);
        return current;
    }
    if (takes_off) {
        LARGE_INTEGER clock{};
        QueryPerformanceCounter(&clock);
        const auto after = static_cast<std::int64_t>(static_cast<double>(d.after.load(std::memory_order_relaxed)) *
            static_cast<double>(dark_pop_frequency()));
        d.takeoff.store(clock.QuadPart, std::memory_order_relaxed);
        DingoSDKOverlayDarkPopFlightStart(clock.QuadPart + after);
        return chosen;
    }
    if (popping && !d.takeoff.load(std::memory_order_relaxed)) {
        // The pop's own flight has ended (it landed, or popped again).
        d.pop_until.store(0, std::memory_order_relaxed);
        d.settle_until.store(tick + dark_pop_settle_ms, std::memory_order_relaxed);
        d.pop_landed.store(chosen, std::memory_order_relaxed);
        d.pop_landings.fetch_add(1, std::memory_order_acq_rel);
        return chosen;
    }
    const auto takeoff = d.takeoff.exchange(0, std::memory_order_relaxed);
    if (!takeoff) return chosen;
    LARGE_INTEGER clock{};
    QueryPerformanceCounter(&clock);
    std::int64_t shown_release{};
    const bool pressed = DingoSDKOverlayDarkPopFlightEnd(&shown_release);
    auto result = chosen;
    const bool forced = pressed && (chosen == offboard_physics_state || chosen == wipeout_physics_state);
    if (forced) {
        (void)cancel_request(owner.context, animation_request_offset, dark_pop_dismount_mask);
        (void)cancel_wipeout_requests(owner.context);
        (void)reset_pending_causes(owner.causes);
        result = 200;
    }
    if (pressed && result == 200) d.pop_until.store(tick + dark_pop_pop_ms, std::memory_order_relaxed);
    else if (pressed) d.settle_until.store(tick + dark_pop_settle_ms, std::memory_order_relaxed);
    d.pressed.store(pressed, std::memory_order_relaxed);
    d.forced.store(forced, std::memory_order_relaxed);
    d.wanted.store(chosen, std::memory_order_relaxed);
    d.landed.store(result, std::memory_order_relaxed);
    d.seconds.store(dark_pop_seconds(clock.QuadPart - takeoff), std::memory_order_relaxed);
    d.release_after.store(shown_release ? dark_pop_seconds(shown_release - takeoff) : -1.0f, std::memory_order_relaxed);
    d.flights.fetch_add(1, std::memory_order_acq_rel);
    return result;
}
extern "C" void DingoSDKOverlayReadControllerInput(dingosdk::ControllerInput* input, bool allow_menu);
extern "C" unsigned DingoSDKOverlayDarkPopInputFlags() noexcept;
constexpr std::size_t fast_probe_capacity = 8192; // rows; about a minute of choices per drain interval at most
struct FastProbe {
    std::atomic<bool> enabled{};
    std::atomic<std::uint64_t> head{}, tail{};
    FastProbeRow rows[fast_probe_capacity];
};
FastProbe& fast_probe() { static auto* value = new FastProbe; return *value; }
void fast_probe_record(std::uintptr_t selector, std::uint32_t current, std::uint32_t wanted, std::uint32_t chosen) noexcept {
    auto& f = fast_probe();
    if (!f.enabled.load(std::memory_order_relaxed)) return;
    Owner owner;
    if (!local_owner(selector, &Owner::selector, &owner)) return;
    const auto head = f.head.load(std::memory_order_relaxed);
    if (head - f.tail.load(std::memory_order_acquire) >= fast_probe_capacity) return;
    auto& row = f.rows[head % fast_probe_capacity];
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    row.ticks = now.QuadPart;
    row.current = current;
    row.wanted = wanted;
    row.chosen = chosen;
    dingosdk::ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad, false);
    row.buttons = (pad.available ? pad.buttons : 0u) | (DingoSDKOverlayDarkPopInputFlags() << 20); // 0x100000 shown held, 0x200000 hidden
    for (int i = 0; i < 8; ++i) { unsigned word{}; row.words[i] = read(owner.context + 0x13c4 + 4 * i, word) ? word : 0u; }
    for (int i = 0; i < 10; ++i) { unsigned word{}; row.words[8 + i] = read(owner.context + 0x140c + 4 * i, word) ? word : 0u; }
    f.head.store(head + 1, std::memory_order_release);
}
std::uint32_t choose_state(std::uintptr_t selector, std::uint32_t current) {
    {
        LastError error;
        hold_off_board(selector, current);
    }
    // Clear shared requests before selection so native ground/air/walking
    // transitions can still run. Some contact tests return Wipeout directly;
    // retain the current state only for that result, never ordinary Offboard.
    const bool filtered = filter_requests(selector, &Owner::selector);
    const auto next = protection().choose_original(selector, current);
    LastError error;
    auto chosen = filtered && next == wipeout_physics_state && protected_owner(selector, &Owner::selector) ? current : next;
    chosen = dark_pop_choose(selector, current, chosen);
    fast_probe_record(selector, current, next, chosen);
    auto& w = state_watch();
    if (selector == w.selector.load(std::memory_order_acquire) && GetTickCount64() < w.until.load(std::memory_order_acquire)) {
        if (const auto before = w.state.exchange(chosen, std::memory_order_acq_rel); before != chosen) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            w.previous.store(before, std::memory_order_relaxed);
            w.previous_ticks.store(now.QuadPart - w.since.exchange(now.QuadPart, std::memory_order_relaxed), std::memory_order_relaxed);
            w.changes.fetch_add(1, std::memory_order_relaxed);
            if (chosen == wipeout_physics_state) w.wipeouts.fetch_add(1, std::memory_order_relaxed);
        }
    }
    track_spin(selector, chosen);
    return chosen;
}
void skeleton_response(std::uintptr_t rig, float seconds, bool wipeout) {
    // The state post-update can raise another request after the selector ran.
    // Filter at this consumer, then let native constraints and recovery run.
    if (filter_requests(rig, &Owner::rig)) wipeout = false;
    if (const auto observer = protection().step_observer.load(std::memory_order_acquire)) {
        LastError error;
        NoBailSkater skater;
        if (published_skater(rig, skater)) observer(skater, seconds, wipeout);
    }
    protection().skeleton_original(rig, seconds, wipeout);
}
bool clear_contact_output(std::uintptr_t contacts) noexcept {
    if (contacts < 0x10000 || contacts > highest - body_contact_output_offset) return false;
    __try {
        _InterlockedExchange8(reinterpret_cast<volatile char*>(contacts + body_contact_output_offset), 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void filter_contact_animation(std::uintptr_t core) noexcept {
    LastError error;
    Owner owner;
    std::uint32_t state{};
    if (!protected_owner(core, &Owner::core, &owner) || !read(owner.context + 0x1414, state)) return;
    // Ground/air and grind state families. Offboard, mounting, handplants and
    // existing ragdolls retain their native contact reporting and recovery.
    if (!((state >= 100 && state < 300) || (state >= 400 && state < 500))) return;
    const auto contacts = pointer(pointer(core, 0x3b8), 0x30);
    std::uint8_t contact{};
    if (!contacts || !read(contacts + body_contact_output_offset, contact) || contact > 1) return;
    // Animation gets this sensitive-body hit independently of the ordinary
    // wipeout output and cause collector. Filter it after native publication,
    // before the animation contact context copies it. Keep the general contact
    // latch, collision timers, per-bone records, impulses and physics state.
    if (clear_contact_output(contacts))
        (void)cancel_request(owner.context, body_contact_context_offset, body_contact_context_mask);
}
void publish_animation(std::uintptr_t core) {
    // Animation also reads requests without consulting the cause collector.
    // Clear flags and pending causes before native publication, including
    // its early recovery query and cause export. Leave the native query result
    // unchanged: forcing it true used to put the skater into a stumbling state.
    (void)filter_requests(core, &Owner::core);
    protection().publish_original(core);
    filter_contact_animation(core);
}
bool compatible(std::uintptr_t base) noexcept {
    if (base < 0x10000 || base > highest - supported_build::game_image_size) return false;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000 ||
        !read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.SizeOfImage != supported_build::game_image_size)
        return false;
    std::array<unsigned char, reset_bail_causes_code.size()> reset_code{};
    if (!read(base + reset_bail_causes_rva, reset_code) || reset_code != reset_bail_causes_code) return false;
    for (const auto& contract : {record_bail_cause_contract, choose_physics_state_contract,
            bail_animation_caller_contract, bail_state_caller_contract, bail_skeleton_contract, bail_publish_contract}) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + contract.rva, actual) || actual != contract.bytes) return false;
    }
    for (const auto& contract : bail_consumer_contracts) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + contract.rva, actual) || actual != contract.bytes) return false;
    }
    for (const auto& contract : body_contact_contracts) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + contract.rva, actual) || actual != contract.bytes) return false;
    }
    for (const auto& impact : impact_bail_calls) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + impact.caller.rva, actual) || actual != impact.caller.bytes) return false;
    }
    return true;
}
}
bool start_no_bail(std::uintptr_t base) noexcept {
    LastError error;
    try {
        auto& p = protection();
        if (p.ready.load()) return p.base == base;
        if (!compatible(base)) {
            logging::write(logging::Level::warning, logging::Channel::skater,
                "No Bail is unavailable: native bail contract did not match.");
            return false;
        }
        p.base = base;
        p.reset_causes = reinterpret_cast<ResetCauses>(base + reset_bail_causes_rva);
        std::array<unsigned char, 32> mount_test{};
        p.mount_test = read(base + mount_request_test_contract.rva, mount_test) &&
                       mount_test == mount_request_test_contract.bytes;
        const std::array targets{
            reinterpret_cast<void*>(base + record_bail_cause_contract.rva),
            reinterpret_cast<void*>(base + choose_physics_state_contract.rva),
            reinterpret_cast<void*>(base + bail_skeleton_contract.rva),
            reinterpret_cast<void*>(base + bail_publish_contract.rva)};
        const std::array replacements{
            reinterpret_cast<void*>(&record_cause), reinterpret_cast<void*>(&choose_state),
            reinterpret_cast<void*>(&skeleton_response), reinterpret_cast<void*>(&publish_animation)};
        std::array<void*, 4> originals{};
        auto status = HookOk;
        std::size_t prepared{};
        for (; prepared < targets.size(); ++prepared) {
            status = hook_prepare(targets[prepared], replacements[prepared], &originals[prepared]);
            if (status != HookOk) break;
            if (!originals[prepared]) { ++prepared; status = HookUnsupportedFunction; break; }
        }
        if (status == HookOk) {
            // Publish every relay before enabling any target.
            p.cause_original = reinterpret_cast<RecordCause>(originals[0]);
            p.choose_original = reinterpret_cast<ChooseState>(originals[1]);
            p.skeleton_original = reinterpret_cast<SkeletonResponse>(originals[2]);
            p.publish_original = reinterpret_cast<PublishAnimation>(originals[3]);
            for (auto target : targets) {
                status = hook_enable(target);
                if (status != HookOk) break;
            }
            if (status == HookOk) {
                p.ready.store(true, std::memory_order_release);
                logging::write(logging::Level::info, logging::Channel::skater,
                    "No Bail ready: collision and landing causes filtered before physics/animation consume them; noclip is protected.");
                return true;
            }
        }
        // Published relays remain callable even if Detours reports an uncertain
        // attach result. With ready=false any remaining hook simply forwards.
        logging::log(logging::Level::warning, logging::Channel::skater,
            "No Bail hook setup failed (status {}); protected noclip is unavailable.", static_cast<LONG>(status));
        while (prepared) (void)hook_remove(targets[--prepared]);
    } catch (...) {}
    return false;
}
bool no_bail_available() noexcept { return protection().ready.load(std::memory_order_acquire); }
bool update_no_bail(std::uintptr_t client, std::uintptr_t entity, bool manual,
    bool flying, std::uint64_t flight_expires) noexcept {
    LastError error;
    auto& p = protection();
    Lease next;
    const bool available = p.ready.load(std::memory_order_acquire) && resolve(client, entity, next.owner);
    if (available) {
        const auto now = GetTickCount64();
        next.manual_until = manual ? now + 500 : 0;
        next.flight_until = flying ? flight_expires : 0;
    }
    AcquireSRWLockExclusive(&p.lock);
    p.lease = available ? next : Lease{};
    ReleaseSRWLockExclusive(&p.lock);
    return available;
}
bool set_teleport_on_board(std::uintptr_t component) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint8_t*>(component + 0xc0) = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void update_board_lock(std::uintptr_t client, std::uintptr_t entity, bool locked) noexcept {
    LastError error;
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return;
    static bool rearm{}; // client tick only
    if (locked) {
        // The owner does not resolve while a teleport is under way: the last one stands
        // until it expires.
        BoardLock next;
        if (resolve(client, entity, next.owner)) {
            next.until = GetTickCount64() + 500;
            AcquireSRWLockExclusive(&p.lock);
            p.board = next;
            ReleaseSRWLockExclusive(&p.lock);
        }
        rearm = true;
        return;
    }
    AcquireSRWLockExclusive(&p.lock);
    p.board = {};
    ReleaseSRWLockExclusive(&p.lock);
    // Released: teleports that keep the skater's own choice (the SDK's /tp) put it on the
    // board again. Once the skater resolves, outside a teleport.
    Owner owner;
    if (rearm && resolve(client, entity, owner) && set_teleport_on_board(owner.component)) rearm = false;
}
void clear_no_bail() noexcept {
    auto& p = protection();
    AcquireSRWLockExclusive(&p.lock);
    p.lease = {};
    ReleaseSRWLockExclusive(&p.lock);
}
bool no_bail_skater(NoBailSkater& skater) noexcept { return published_skater(0, skater); }
void set_no_bail_step_observer(NoBailStepObserver observer) noexcept {
    protection().step_observer.store(observer, std::memory_order_release);
}
void dark_pop_update(bool enabled, float after) noexcept {
    auto& d = dark_pop();
    d.after.store(after, std::memory_order_relaxed);
    d.enabled.store(enabled, std::memory_order_relaxed);
}
DarkPopLast dark_pop_last() noexcept {
    auto& d = dark_pop();
    DarkPopLast last;
    last.flights = d.flights.load(std::memory_order_acquire);
    last.pressed = d.pressed.load(std::memory_order_relaxed);
    last.landed = d.landed.load(std::memory_order_relaxed);
    last.seconds = d.seconds.load(std::memory_order_relaxed);
    last.release_after = d.release_after.load(std::memory_order_relaxed);
    last.forced = d.forced.load(std::memory_order_relaxed);
    last.wanted = d.wanted.load(std::memory_order_relaxed);
    last.pop_landings = d.pop_landings.load(std::memory_order_acquire);
    last.pop_landed = d.pop_landed.load(std::memory_order_relaxed);
    last.holds = d.holds.load(std::memory_order_relaxed);
    return last;
}
void fast_probe_enable(bool on) noexcept {
    auto& f = fast_probe();
    if (on) f.tail.store(f.head.load(std::memory_order_acquire), std::memory_order_release); // start empty
    f.enabled.store(on, std::memory_order_release);
}
std::size_t fast_probe_drain(FastProbeRow* rows, std::size_t capacity) noexcept {
    auto& f = fast_probe();
    auto tail = f.tail.load(std::memory_order_relaxed);
    const auto head = f.head.load(std::memory_order_acquire);
    std::size_t count = 0;
    while (tail != head && count < capacity) rows[count++] = f.rows[tail++ % fast_probe_capacity];
    f.tail.store(tail, std::memory_order_release);
    return count;
}
void watch_physics_state(std::uintptr_t client, std::uintptr_t entity) noexcept {
    auto& w = state_watch();
    Owner owner;
    if (!protection().ready.load(std::memory_order_acquire) || !resolve(client, entity, owner)) return;
    if (w.selector.exchange(owner.selector, std::memory_order_acq_rel) != owner.selector)
        w.state.store(0, std::memory_order_release);
    w.until.store(GetTickCount64() + 500, std::memory_order_release);
}
PhysicsStateWatch watched_physics_state() noexcept {
    auto& w = state_watch();
    PhysicsStateWatch result;
    result.valid = protection().ready.load(std::memory_order_acquire) && w.selector.load(std::memory_order_acquire) &&
        GetTickCount64() < w.until.load(std::memory_order_acquire);
    result.state = w.state.load(std::memory_order_acquire);
    result.previous = w.previous.load(std::memory_order_relaxed);
    static const double frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return static_cast<double>(value.QuadPart); }();
    result.previous_seconds = static_cast<float>(static_cast<double>(w.previous_ticks.load(std::memory_order_relaxed)) / frequency);
    result.changes = w.changes.load(std::memory_order_relaxed);
    result.wipeouts = w.wipeouts.load(std::memory_order_relaxed);
    return result;
}
void watch_revert_spin(std::uintptr_t selector, std::uintptr_t transform, std::uintptr_t board) noexcept {
    auto& s = spin_watch();
    s.transform.store(transform, std::memory_order_release);
    s.board.store(board, std::memory_order_release);
    s.selector.store(selector, std::memory_order_release);
    s.until.store(GetTickCount64() + 500, std::memory_order_release);
}
RevertLanding last_revert_landing() noexcept {
    auto& s = spin_watch();
    AcquireSRWLockShared(&s.lock);
    const auto landing = s.last;
    ReleaseSRWLockShared(&s.lock);
    return landing;
}
void clear_no_bail_flight() noexcept {
    auto& p = protection();
    AcquireSRWLockExclusive(&p.lock);
    p.lease.flight_until = 0;
    ReleaseSRWLockExclusive(&p.lock);
}
}
