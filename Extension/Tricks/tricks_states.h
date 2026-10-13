#pragma once
// The animation system's game states ReSkate Tricks drives for the local skater.
//
// Game build 20260929. A game state is an asset object in memory: its 16-byte asset id, then the
// object, with its name at + 0x18 and at + 0x28 the handle the game finds its value by. The
// objects are found by a memory search; the skater's values are reached through its pages.
// (Layout and handle rules as found for the ReSkate Trainer.)
#include <cstddef>
#include <cstdint>

namespace dingosdk::tricks {
enum class State : std::uint8_t {
    wants_freestyle,      // Bool.Intent.WantsFreestyle: into the freestyle node from riding, out when false
    freestyle_trick,      // EnumGS.Intent.FreestyleTrick: 0 casper, 1 primo, 2 primo2
    freestyle_flip_trick, // EnumGS.Intent.FreeStyleFlipTrick: non-zero on leaving a casper = flip out of it
    enable_caspers,       // Bool.Intent.EnableCaspers: set by the game from the enablefreestyle setting
    ground_trick,         // Enum.GS.CurrentGroundTrick: 7 keeps physics state 103 (the board follows the clip)
    count
};
inline constexpr std::size_t state_count = static_cast<std::size_t>(State::count);

// Searches committed read-write memory for the state objects. Slow (a second or more): call off
// the game thread. True once every state object is known.
bool search_state_objects() noexcept;
bool state_objects_known() noexcept;
// Forget them (a level change unloads them).
void forget_state_objects() noexcept;

// Why a state reads nothing: 0 its object is not found, 1 found but its handle is not filled in, 2 the skater has
// no page or no value for it, 3 it has a value. `handle` is the object's handle word.
int state_reason(std::uintptr_t base, std::uintptr_t entity, State state, std::uint32_t &handle) noexcept;
// The skater's value of a state; false while it has none.
bool read_state(std::uintptr_t base, std::uintptr_t entity, State state, std::uint32_t &value) noexcept;

// The game finds every state's value through one lookup (0x11dcdd0). Hooked, a held state is
// answered with ours for every asker: the animation graph and the scripts alike. Safe to call
// every tick; false while the game's code is not the known one.
bool start_holds(std::uintptr_t base) noexcept;
// Hold a state at `value`, or (`hold` false) give it back to the game.
void hold(State state, bool hold, std::uint32_t value = 0) noexcept;
// Each tick: holds lapse half a second after the last call, so a stalled mod gives the game its
// own values back.
void keep_holds_alive() noexcept;
// Write an enumeration state's value in place, in the skater's own states block (any thread; `before`: what was there).
bool poke_state_value(std::uintptr_t base, std::uintptr_t entity, std::uintptr_t state, std::uint32_t value, std::uint32_t *before = nullptr) noexcept;
bool peek_state_value(std::uintptr_t base, std::uintptr_t entity, std::uintptr_t at, std::uint32_t &value) noexcept;
// A state (by its object, as find_state gives it) answered at the lookup with `value` for this long; how often it was asked for.
void hold_object(std::uintptr_t object, std::uint32_t value, std::uint64_t milliseconds) noexcept;
std::uint32_t held_object_asked() noexcept;
// How often the game asked for a held state since last asked here.
std::uint32_t held_asked(State state) noexcept;

// Writing through the game's own setters (SetBoolGameState 0x1090600, SetIntGameState 0x1090650), as its
// scripts do: a state nothing has written has no slot in the skater's store (handle 0), and the animation
// graph then sees its default whatever a lookup answers. A write gives it one. The setters take the entity
// key the scripts pass as TheKey; it is taken from the game's own write of Bool.Intent.EnableCaspers (its
// ExperimentalSettings script, on every mount), so the key is the skater's whose settings ran last.
bool start_setter_watch(std::uintptr_t base) noexcept;
// Once the key is known: finds the other states by name through the game's own asset lookup, in the asset domain of
// the EnableCaspers object the game passed (game thread). Returns how many it found; below 0: not possible yet.
int resolve_state_objects(std::uintptr_t base) noexcept;
// Any game state by its asset name ("Animation/Dingo/Bool.Anim.ForceAnimSkateboard"), from the same asset domains;
// 0 when it is not loaded there. Game thread, key known.
std::uintptr_t find_state(std::uintptr_t base, const char *name) noexcept;
// Through the game's setters / getters, for a state found with find_state. `kind`: 0 bool, 1 int / enum, 2 float.
bool write_state_at(std::uintptr_t base, std::uintptr_t state, int kind, double value) noexcept;
bool read_state_at(std::uintptr_t base, std::uintptr_t state, int kind, double &value) noexcept;
bool state_objects_resolved() noexcept;
// A state-flow transition, by its asset name, made to start its destination clip `tick` ticks in (0: as shipped).
// Returns the phase mode that was there (0 or 7; `before`: the tick offset), or below 0 when nothing was written.
int set_transition_start(std::uintptr_t base, const char *name, float tick, float *before = nullptr) noexcept;
bool skater_key_known() noexcept;
std::uint64_t skater_key() noexcept;
// Forget the key (the skater is gone).
void forget_skater_key() noexcept;
bool write_state(std::uintptr_t base, State state, std::uint32_t value) noexcept;
// What the game's own getters (GetBoolGameState, GetEnumGameState) return for the key; false without a key.
bool game_read_state(std::uintptr_t base, State state, std::int32_t &value) noexcept;
} // namespace dingosdk::tricks
