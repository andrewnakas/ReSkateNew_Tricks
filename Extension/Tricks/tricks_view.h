#pragma once
// What the Tricks page of the Insert menu shows and edits: every trick the mod adds, grouped in families, and the
// settings for their controls and animation. The tables are shared by the page (overlay thread) and the game side
// (tricks.cpp); the page reads the snapshot the game thread publishes and changes things with `tricks ...` console
// commands, never by touching the game itself.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace dingosdk::tricks {
enum class Family : std::uint8_t { poses, flickouts, ground, nocomply, air, flickit };
inline constexpr std::size_t family_count = 6;
struct FamilyInfo {
    const char *id;
    const char *name;
    const char *hint;
};
inline constexpr FamilyInfo family_catalog[family_count]{
    {"poses", "Freestyle poses", "Caspers, primos and anti-caspers: caught in the air or from a manual, held on the right stick"},
    {"flickouts", "Flick-outs", "A swipe of the right stick from a held pose flips the board out of it"},
    {"ground", "Ground tricks", "Tricks done on the ground without a pop"},
    {"nocomply", "No comply and boneless flips", "A flick after a no comply, boneless or footplant flips the board in its air"},
    {"air", "In-air extras", "A tap of the left stick's click in the air turns the board once more"},
    {"flickit", "Flick-it flips", "New flip tricks popped from riding with their own flick"},
};

struct TrickInfo {
    const char *id;
    Family family;
    const char *name;
    const char *hint;
    int points; // what the trick is worth in the score line
};
inline constexpr TrickInfo trick_catalog[]{
    {"casper", Family::poses, "Casper", "L3 + right stick down at the catch", 250},
    {"primo", Family::poses, "Primo", "L3 + right stick to either side at the catch", 250},
    {"anticasper", Family::poses, "Anti Casper", "L3 + right stick up at the catch", 300},
    {"casperflip", Family::poses, "Casper Flip", "Let go of L3 in a casper: the board flips back over and rides away", 200},
    {"manual_entry", Family::poses, "Poses from manuals", "A manual pushed into its brake, then L3 + the stick: tail -> casper or primo, nose -> anti casper", 0},
    {"flick_kickflip", Family::flickouts, "Kickflip", "Swipe up-left from a held pose", 400},
    {"flick_heelflip", Family::flickouts, "Heelflip", "Swipe up-right from a held pose", 400},
    {"flick_treflip", Family::flickouts, "Tre Flip", "Swipe straight up from a held pose", 500},
    {"flick_shuvit", Family::flickouts, "Pop Shuvit", "Quick swipe to the left rim from a casper", 350},
    {"flick_fsshuvit", Family::flickouts, "FS Pop Shuvit", "Quick swipe to the right rim from a casper", 350},
    {"groundshuv", Family::ground, "Ground Shuvit", "L3, stick down at the rim, roll to a lower diagonal, let go", 150},
    {"nc_kickflip", Family::nocomply, "No Comply Kickflip", "Tap A for the no comply, pop it with the kickflip flick (down, then up-left)", 300},
    {"nc_heelflip", Family::nocomply, "No Comply Heelflip", "Tap A, pop it with the heelflip flick (down, then up-right)", 300},
    {"nc_shuvit", Family::nocomply, "No Comply Shuvit", "Tap A, pop it with the pop shuvit flick (down, rolled to the left rim)", 250},
    {"nc_fsshuvit", Family::nocomply, "No Comply FS Shuvit", "Tap A, pop it with the FS pop shuvit flick (down, rolled to the right rim)", 250},
    {"air_extraflip", Family::air, "Extra flip", "Tap L3 in the air, stick at rest: double flip, nightmare, daydream", 300},
    {"air_lateshuv", Family::air, "Late Shuvit", "Tap L3 in the air with the stick left or right", 250},
    {"flipgrab", Family::air, "Flip Grab", "A trigger during a flip: the flip turns into the game's grab with the knees tucked up (RT indy, LT melon)", 400},
    {"grindgrab", Family::air, "Grind Grab", "Press a trigger while grinding: RT indy, LT melon, both a double grab, on any grind", 200},
    {"fandango", Family::air, "Fandango", "Hold A in a crooked grind: the back foot comes off the board", 300},
    {"dolphin", Family::flickit, "Dolphin Flip", "Flick down, up and straight back down: a forward varial (one front flip, half a turn)", 500},
    {"dragon", Family::flickit, "Dragon Flip", "Flick down, up and back down to a lower diagonal: a forward 360 (one front flip, a full turn)", 650},
};
inline constexpr std::size_t trick_count = std::size(trick_catalog);
constexpr int trick_index(std::string_view id) {
    for (std::size_t i = 0; i < trick_count; ++i)
        if (id == trick_catalog[i].id) return static_cast<int>(i);
    return -1;
}

struct SettingInfo {
    const char *key;
    Family family;
    const char *label;
    const char *hint;
    float low, high, step;
    const char *format;
};
inline constexpr SettingInfo setting_catalog[]{
    {"zone_left", Family::poses, "Casper place starts at", "Right stick angle (270 is straight down) where the casper's place begins; below it is the primo", 200, 270, 1, "%.0f deg"},
    {"zone_right", Family::poses, "Casper place ends at", "Right stick angle where the casper's place ends; above it is the other primo", 270, 340, 1, "%.0f deg"},
    {"anti_steady", Family::poses, "Anti casper catch time", "How long the stick rests up before the anti casper is caught (a flick passes the top)", 60, 400, 10, "%.0f ms"},
    {"height_edge", Family::poses, "Height with the stick at the rim", "The casper's loop blend (0 low, 1 high) with the right stick at its rim", 0, 1, 0.05f, "%.2f"},
    {"height_middle", Family::poses, "Height with the stick half out", "The casper's loop blend with the right stick eased back toward the middle", 0, 1, 0.05f, "%.2f"},
    {"snap_casper", Family::poses, "Casper way-in starts at tick", "How far into the casper's way-in clip an air catch starts (0: the whole clip)", 0, 69, 1, "%.0f"},
    {"snap_primo", Family::poses, "Primo way-in starts at tick", "How far into the primo's way-in clip an air catch starts", 0, 94, 1, "%.0f"},
    {"slide", Family::poses, "Slide slowdown", "Speed a held pose scrubs off each second", 0, 10, 0.1f, "%.1f m/s"},
    {"rigid", Family::poses, "Board hold", "How firmly a held pose's board keeps its pose (the game's own is 1)", 1, 100, 1, "%.0f"},
    {"out_spin_ms", Family::flickouts, "Flick-out flip time", "How long the board takes to flip after a flick-out", 150, 800, 10, "%.0f ms"},
    {"flickout_flip", Family::flickouts, "Flick-out flip size", "Kickflip / heelflip out of a casper: 0 a half flip (the roll back over is the flip), 1 one and a half", 0, 1, 1, "%.0f"},
    {"flickout_up", Family::flickouts, "Flick-out stick up", "0 tre half flip, 1 tre with the bigger flip, 2 a straight half flip (no shuv)", 0, 2, 1, "%.0f"},
    {"exit_guard_ms", Family::flickouts, "Exit guard", "How long a way out of a pose is kept on the board at most", 500, 4000, 100, "%.0f ms"},
    {"groundshuv_ms", Family::ground, "Ground shuvit time", "How long the board takes to turn", 200, 800, 10, "%.0f ms"},
    {"groundshuv_hop", Family::ground, "Ground shuvit lift", "How far the board comes off the ground in the middle of the turn", 0, 0.2f, 0.01f, "%.2f m"},
    {"nc_spin_ms", Family::nocomply, "No comply flip time", "How long the board takes to flip in a no comply's air", 200, 800, 10, "%.0f ms"},
    {"air_spin_ms", Family::air, "Extra flip time", "How long an in-air extra takes", 150, 800, 10, "%.0f ms"},
    {"flickit_spin_ms", Family::flickit, "Flick-it flip time", "How long a dolphin, dragon or big spin takes to turn the board", 250, 900, 10, "%.0f ms"},
};
inline constexpr std::size_t setting_count = std::size(setting_catalog);

struct View {
    bool ready{};
    std::array<bool, trick_count> trick{};
    std::array<bool, family_count> family{};
    std::array<float, setting_count> setting{};
    bool names{};     // the mod's own notices
    bool score{};     // tricks added to the game's score line
    bool height{};    // the casper's height on the right stick
    bool anti_turn{}; // the anti casper turned half round with the mirror
    bool two_primos{};
};
// Thread-safe snapshot (tricks_view.cpp, part of the overlay library).
std::shared_ptr<const View> view() noexcept;
void publish(std::shared_ptr<const View>) noexcept;
} // namespace dingosdk::tricks
