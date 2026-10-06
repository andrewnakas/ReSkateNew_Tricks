#pragma once
// The game keeps much of its tuning outside Gameplay/SkatePhysicsTuning, in data-defined classes
// (push speeds, on-foot jumps, flips and rolls, dive and glide, bail speeds...). Their defaults
// and field order ship in the game's data (trainer_classes.inc); a live copy has no name to look
// up, so it is found by searching writable memory for the class's defaults laid out as the
// class lays them out. Game thread, except the search itself, which runs on its own thread.
#include <cstddef>
#include <cstdint>
#include <string>

namespace dingosdk::trainer {
struct ClassField {
    const char *name;
    float stock;
    std::uint16_t offset;
    // The game was seen reading it (hardware read watches on every copy) while a scripted skater
    // rode, pushed, ollied, flipped, ran, jumped and sprinted, or while a player ground, pumped,
    // climbed, vaulted and bailed; or its effect was measured. False: no read turned up in any
    // of that, and no other copy of the numbers was found in memory.
    bool seen;
};
struct ClassSpec {
    const char *group; // shown in the menu
    const char *key;   // in value ids: "<key>.<field>"
    std::uint16_t first, count, anchor;
};
#include "trainer_classes.inc"
inline constexpr std::size_t class_count = sizeof(class_specs) / sizeof(class_specs[0]);
inline constexpr std::size_t class_field_count = sizeof(class_fields) / sizeof(class_fields[0]);

// Starts a search for every class's live copies unless one is running. Copies found earlier
// that still hold what the trainer last wrote are kept.
void find_classes() noexcept;
bool finding_classes() noexcept;
// How many searches have finished (0: none yet).
std::uint64_t class_searches() noexcept;
// Copies of class `index` found.
std::size_t class_copies(std::size_t index) noexcept;
// What field `field` (an index into class_fields) should hold. Written by apply_classes.
void want_class_value(std::size_t field, float value) noexcept;
// Whether anything is wanted that is not the game's own (a field, or the flip speed): until
// then there is nothing to write and no reason to search.
bool classes_wanted() noexcept;
// Writes the wanted values into every copy found; call each tick. Returns the fields written.
std::size_t apply_classes() noexcept;
// Call each tick (it acts once a second): writes again what the game has put back to its own
// numbers. True when something wanted has nowhere left to go, so a new search is due.
bool refresh_classes(std::uint64_t now) noexcept;
std::string classes_summary();
// Board flip trick speed. The game's flip trick tuning is a class of eight curves and no numbers
// (speed against the flick, against the ollie...); the search finds it as eight curve references
// in a row and apply_classes multiplies the outputs of its four speed curves by this.
void want_flip_speed(float factor) noexcept;
// How many flip trick speed curves were found (0: the multiplier does nothing yet).
std::size_t flip_curves() noexcept;
// Where field `field` (an index into class_fields) lives: each copy's address and kind ('c' the
// class's own layout, 's' the eight-byte slots, 'n' native code's copy). Returns how many.
struct FieldCopy {
    std::uintptr_t address{};
    char kind{};
};
std::size_t class_field_copies(std::size_t field, FieldCopy *out, std::size_t capacity) noexcept;
} // namespace dingosdk::trainer
