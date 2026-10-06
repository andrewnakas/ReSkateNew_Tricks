#pragma once
#include "trainer.h"
#include <cstdint>
#include <string_view>
#include <vector>

namespace dingosdk::trainer {
// One rule of a built-in preset. `pattern` is lower-case words that a value's id must all
// contain ("physicsmode. jump height"); a word starting with '!' must be absent. A rule
// multiplies the stock value, or sets it. Single graph points are never matched.
struct PresetRule {
    std::string_view pattern;
    bool multiply{true};
    double amount{1};
    bool curves{}; // the rule is for curve and graph multipliers instead of plain values
    bool exact{};  // `pattern` is one whole id, and it may be a single graph point
};
struct BuiltinPreset {
    std::string_view name, note;
    std::vector<PresetRule> rules;
};
// The plain name of a value on the Tune tab's short lists, by its lower-case id; empty for the
// rest. `modes`: which lists it is on (mode_realistic, mode_fun).
std::string_view essential_name(std::string_view key, int *rank = nullptr, std::uint8_t *modes = nullptr);
// What a named value does, in a plain sentence with its unit; empty when there is none.
std::string_view essential_help(std::string_view key);
// Whether the game's code was found to read the tuning value at this offset of the asset.
// False means "no use found": changing it will probably do nothing.
bool value_used(std::uint16_t offset);
// Values the game does not read itself but that the trainer makes work: changing one scales
// the graphs that really control the behaviour by the same ratio (value / stock).
struct Link {
    std::string_view value;  // lower-case id of the value the player edits
    std::string_view drives; // lower-case id of a graph multiplier it scales
};
const std::vector<Link> &value_links();
// How a built-in preset shows on the Tune tab: as a dial under `title` when its first rule is a
// multiplier, and on which of the two short lists.
struct PresetDial {
    std::string_view title;
    std::uint8_t modes{};
};
PresetDial preset_dial(std::string_view name);
// Presets stack: each applies on top of what is already changed; Stock clears them.
const std::vector<BuiltinPreset> &builtin_presets();
} // namespace dingosdk::trainer
