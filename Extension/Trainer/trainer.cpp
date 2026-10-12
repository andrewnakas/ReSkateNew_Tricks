#include "trainer.h"
#include "trainer_classes.h"
#include "trainer_gamestate.h"
#include "trainer_jump.h"
#include "trainer_presets.h"
#include "trainer_session.h"
#include "trainer_landing.h"
#include "trainer_waypoint.h"
// <dev>
#include "trainer_probe.h"
// </dev>
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/physics_tuning.h"
#include "Engine/Game/Multiplayer/session_physics.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Skater/physics_tuning.h"
#include "Extension/Skater/skater_observer.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <shlobj.h>
#include <xinput.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <numbers>
#include <optional>

// <dev>
extern "C" void DingoSDKOverlayScriptController(const void *gamepad);
extern "C" void DingoSDKOverlayScriptControllerPolls(std::uint64_t *polls, std::uint64_t *served);
extern "C" std::uint64_t DingoSDKOverlayScriptControllerBlanked();
// </dev>

// Game thread only. See trainer.h for how the menu reaches this.
extern "C" std::uint64_t DingoSDKOverlayGrindStickMirrored() noexcept;
extern "C" void DingoSDKOverlaySetGrindStickTurn(bool on, float x, float y_add, float y_scale) noexcept;
extern "C" void DingoSDKOverlayGrindStickSent(float *values) noexcept;
extern "C" void DingoSDKOverlaySetGrindStickLean(bool on, float x, float y) noexcept;
extern "C" bool DingoSDKOverlayGrindStickLeanRaw(float *values) noexcept;
extern "C" std::uint64_t DingoSDKOverlayGrindStickLeaned() noexcept;
namespace dingosdk::trainer {
namespace {
namespace tuning = dingosdk::physics_tuning;
using Vec3 = std::array<float, 3>;
constexpr float max_coordinate = 1.0e6f;
constexpr std::uint32_t air_states_first = 200, air_states_end = 300;
constexpr std::uint32_t xinput_up = 0x1, xinput_down = 0x2, xinput_left = 0x4, xinput_right = 0x8,
                        xinput_lb = 0x100, xinput_rb = 0x200;

struct Entry {
    std::string id, key, label, group; // key: lower-case id
    Kind kind{};
    std::uint16_t offset{}, size{};
    std::size_t curve{};
    double value{}, stock{};
    bool touched{}, frozen{};
    bool detail{};
    bool used{}; // the game's code was found to read it (trainer_used.inc), or it is linked
    std::vector<std::size_t> drives; // graph multipliers this value scales (value_links)
    std::vector<std::size_t> members; // graph: its outputs (Y0.., Min_Y, Max_Y)
    int class_field{-1};              // a field of one of the game's data-defined classes (trainer_classes.h), not of the tuning asset
};
struct MapData {
    std::array<Marker, marker_slots> markers{};
    std::string preset;
};
struct MapFile { // Mods/<mod>/trainer.json
    std::string note, preset_name;
    std::vector<std::pair<std::string, double>> preset;
    std::vector<Spot> spots;
};
struct Saved {
    double value{};
    bool frozen{};
};
struct SelfTest {
    int step{}; // 0: idle
    std::uint64_t until{};
    std::size_t entry{};
    double before{}, target{};
    Vec3 origin{};
    int failures{};
};
struct Motion {
    bool valid{};
    Vec3 position{}, velocity{};
    std::int64_t stamp{};
    bool airborne{};
    Vec3 takeoff{}, takeoff_velocity{};
    std::int64_t takeoff_stamp{};
    float apex{};
    bool axes_valid{};
    Vec3 forward{}, up{};
    float spin{}, spin_rate{}, flip{};
    float board_turn{}; // the board's fastest turn rate in this jump, degrees per second
    std::uint32_t air_state{}, ground_state{};
};
// The trick multipliers and auto push in force: x of the game's own, and for auto push the
// speed a rolling skater is carried up to (m/s, 0: off).
struct Boosts {
    float flip{1}, hippy{1}, nocomply{1}, boneless{1}, offboard{1}, cruise{};
};
// <dev>
// `trainer narrow`: the floats that sit inside a range every time they are sampled, a set number of
// ticks into a flight (a game state with a known value: which address is it?).
struct Narrow {
    std::vector<std::uintptr_t> at;
    int pending{}; // 1 start, 2 keep
    int tick{8}, air_ticks{};
    float low{}, high{};
    std::string last{"nothing sampled yet"};
};
Narrow dev_narrow;
// `trainer hold`: one number written to a few addresses on every tick of a flight.
std::vector<std::uintptr_t> dev_hold_at;
float dev_hold_value{};
void __try_poke(std::uintptr_t address, float value) noexcept;
// `option footlog`: off the board, each burst of body rotation (a flip in the air, a roll on the ground).
struct FootLog {
    bool on{};
    bool turning{};
    int serial{};
    std::int64_t last{}, since{}, quiet{};
    float peak{}, turned{}, up_at_start{}, height_at_start{};
    std::size_t part{};
};
FootLog dev_foot{.on = true}; // on for playtest captures
// `trainer airdump`: the skater's game state blocks written to files on set ticks of the next flight.
std::vector<int> dev_airdump_ticks;
std::string dev_airdump_name;
int dev_airdump_tick{};
std::string narrow_run(Narrow &n) {
    const auto started = GetTickCount64();
    const auto inside = [&](float v) { return v >= n.low && v <= n.high; };
    if (n.pending == 1) {
        n.at.clear();
        std::vector<float> chunk(1 << 18);
        MEMORY_BASIC_INFORMATION info{};
        for (std::uintptr_t at = 0x10000; at < 0x7fffffff0000ull && VirtualQuery(reinterpret_cast<void *>(at), &info, sizeof(info)) == sizeof(info);
             at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize) {
            if (info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE || info.Type != MEM_PRIVATE) continue;
            const auto begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress), end = begin + info.RegionSize;
            for (auto part = begin; part < end && n.at.size() < (8u << 20); part += chunk.size() * 4) {
                const auto bytes = std::min<std::size_t>(end - part, chunk.size() * 4);
                SIZE_T got{};
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(part), chunk.data(), bytes, &got)) continue;
                for (std::size_t i = 0; i < got / 4; ++i)
                    if (inside(chunk[i])) n.at.push_back(part + i * 4);
            }
        }
    } else {
        std::erase_if(n.at, [&](std::uintptr_t address) {
            float v{};
            return !memory::peek(address, v) || !inside(v);
        });
    }
    std::string result = std::format("{} floats in {:.4g}..{:.4g} ({} ms)", n.at.size(), n.low, n.high, GetTickCount64() - started);
    for (std::size_t i = 0; i < std::min<std::size_t>(n.at.size(), 40); ++i) {
        float v{};
        (void)memory::peek(n.at[i], v);
        result += std::format(" {:x}={:.5g}", n.at[i], v);
    }
    n.pending = 0;
    return result;
}
// `trainer fakesession`: the menu's session messages without a session (1 host's physics, 2 boosts off).
int dev_fake_session = 0;
// A scripted controller: each step holds one controller state for a time (`trainer pad`).
struct PadStep {
    std::array<std::uint8_t, 12> pad{}; // XINPUT_GAMEPAD
    double seconds{};
};
struct PadScript {
    std::vector<PadStep> steps;
    std::size_t index{};
    std::int64_t step_start{};
    bool running{};
    std::uint64_t served_before{};
    int mount_tries{}; // `pad mount`: play again until the skater is on the board
    bool loop{};       // `pad loop`: play again until `pad off`
};
// </dev>
// How long a new skater alone waits for a memory search: doubled while searches find nothing new.
constexpr std::uint64_t class_skater_wait_low = 20000, class_skater_wait_high = 600000;
struct State {
    bool loaded{}; // the store was read
    // <dev>
    PadScript pad;
    bool state_log{};
    std::int64_t state_since{};
    std::uintptr_t client{};
    std::array<std::array<float, 4>, 9> body_peaks{};
    // One sample of the board in the air, for the flip trace (trainer option fliptrace 1).
    struct FlipSample {
        float time{}, turn[3]{}, up_speed{}, height{};
        std::uint32_t state{};
    };
    bool flip_trace{};
    // The pump log (trainer option pumplog 1): each time the skater's speed tops out and falls
    // away again (the bottom of a transition), that top speed is logged.
    bool pump_log{};
    // The swing being ridden: its lowest point so far and the speed there, then the highest point after it.
    float pump_low{}, pump_low_speed{}, pump_high{}, pump_last_top{};
    bool pump_rising{}, pump_started{};
    int pump_serial{};
    std::vector<FlipSample> flip_samples;
    int flip_serial{};
    float flip_takeoff_up{};
    // </dev>
    std::shared_ptr<const tuning::Model> model;
    std::vector<Entry> entries;
    std::map<std::string, std::size_t, std::less<>> index;
    std::uintptr_t base{}, asset{}, entity{};
    tuning::Values own, want;
    bool write_due{}, view_due{true}, save_due{};
    bool profile_due{}; // the loaded map's preset is still to be applied
    bool refresh_due{}; // values were written while the skater had no cached copy to refresh
    std::uint64_t next_hold{}, next_save{}, next_skater{}, revision{};
    std::string status = "Waiting for the game.", last;
    bool ready{};
    std::map<std::string, Saved, std::less<>> saved;                                    // by key
    std::map<std::string, std::map<std::string, double, std::less<>>, std::less<>> user; // presets
    std::vector<std::string> active;                                                    // presets applied since Stock
    std::map<std::string, MapData, std::less<>> maps;
    std::string map;
    MapFile map_file;
    int slot{};
    bool auto_return{}, pad_shortcuts{}, hud{}, hud_jump{}, logging{};
    bool pad_menu{true}; // LB + RB + R3 opens and closes the menu
    // The hippy jump's height is set by the game's trick scripts, not by tuning: the trainer
    // scales the upward velocity when it sees one start.
    float hippy_height{1};
    // An on-foot jump: the same scaling, when a skater standing off the board starts to rise.
    float offboard_height{1};
    // Board flip tricks: a multiplier on the game's flip speed curves (trainer_classes.h).
    float flip_speed{1};
    // "Advanced trick speed": each flip trick also turns at a speed of its own (flip_tricks, trainer_gamestate.h).
    bool flip_advanced{};
    bool flip_pop{}; // a slowed flip trick pops higher, so it has the air time it needs
    bool flip_flick{}; // a slow flick of the stick turns the board as fast as a full one
    bool grind_watch{}; // the grind card is asking what the game's switches read (`trainer grind watch`)
    std::map<std::string, bool, std::less<>> folds; // the menu's sections and cards the player has opened or closed
    std::array<float, flip_tricks.size()> flip_trick = [] {
        std::array<float, flip_tricks.size()> ones{};
        ones.fill(1.0f);
        return ones;
    }();
    // The flip speed in the air now: the game's own number, what the trainer made of it, and whether it can be reached at all.
    struct FlipDrive {
        bool active{};
        float game{}, written{};
        float clamped{}; // the impossible's own speed state as the flight began
        float factor{1}; // the flip speed times the turning trick's own
        std::uint32_t trick{}; // the trick it was last driven for
    } flip_drive;
    float flip_game_seen{}; // the game's own number in the last driven flight (for the flight log)
    float flip_clamped_seen{-1}; // and what the impossible's own speed state read (-1: not found)
    bool flip_live{};
    std::uint32_t flip_trick_seen{}; // the last trick that turned (CurrentFlipTrick)
    bool offboard_grounded{}; // off the board and not moving up or down at the last tick
    const char *boost_name{""};
    // The no comply and the boneless are launched by the trick scripts too (trainer_jump.cpp).
    float nocomply_height{1}, boneless_height{1};
    float manual_height{1}; // the pop out of a manual
    float grind_side{1};    // the sideways part of a pop out of a grind or slide
    float grind_push{};     // Skate 3's pop: this many m/s sideways at full stick, the way the stick points (0: none)
    // Skate 3's turn on a grind: the left stick alone turns the board, up to this many degrees either way (0: off, the
    // game's own, which follows where the right stick points). Negative turns the other way.
    float grind_range{};
    float grind_yaw{};          // Skate 3's eased stick, -0.614..0.614
    float grind_heading{};      // what the game is answered with (radians)
    // The angle the game turns the right stick by before it steers the board (it reads the stick from the camera's side):
    // measured from what it was shown against what its GrindControlLR / UD then read.
    float grind_frame_cos{1}, grind_frame_sin{};
    std::uint64_t grind_yaw_at{};
    // The grind exit switches (grind_switches, trainer_gamestate.h): -1 the game's own, 0 held off, 1 held on; and what each read.
    std::array<std::int8_t, grind_switches.size()> grind_mode = [] { std::array<std::int8_t, grind_switches.size()> a{}; a.fill(-1); return a; }();
    std::array<std::int8_t, grind_switches.size()> grind_live = grind_mode;
    // The game's own grind settings (grind_settings, trainer_gamestate.h): -1 the game's own, 0 forced off, 1 forced on.
    std::array<std::int8_t, grind_settings.size()> grind_setting = [] { std::array<std::int8_t, grind_settings.size()> a{}; a.fill(-1); return a; }();
    // What the game had a held switch at before the trainer first wrote it (-1: not held, or not read yet). The game
    // sets most of them once and never again (measured 2026-10-11: released, a switch stayed where it was held), so
    // "the game's own" has to put this back.
    std::array<std::int8_t, grind_switches.size()> grind_stock = grind_mode;
    // Transition skating (bowls, quarter pipes, vert): the trainer's own help, each 0 / false when off.
    float tr_lean{};         // 0..1: how hard the skater leans into the ramp on a coping grind
    float tr_carry{};        // 0..1: how much of the speed along the coping is kept rolling off it
    float tr_drop{1.0f};     // m/s: the most speed across the coping, into the ramp, while that speed is kept
    float tr_unstick{};      // strength of the push out of a hang-up
    float tr_lockout{};      // seconds the coping cannot be caught again after a coping grind
    bool tr_spine{};         // a flight straight up and over a lip does not reach for the coping
    float tr_plant{};        // 0..1: a handplant leaves the lip the way Skate 3's did
    // The game's own transition settings (transition_settings, trainer_gamestate.h): -1 the game's own, 0 forced off, 1 forced on.
    // The game's own Settings options shown with them (transition_options): what the game holds, read twice a second.
    std::array<int, transition_options.size()> tr_option = [] {
        std::array<int, transition_options.size()> a{};
        for (std::size_t i = 0; i < a.size(); ++i) a[i] = transition_options[i].stock;
        return a;
    }();
    std::uint64_t tr_option_at{};
    // What the player's own game settings hold for the settings the trainer can force (read with the options above):
    // a row that is not forced shows this, not the game's default.
    std::array<std::int8_t, grind_settings.size()> grind_own = [] {
        std::array<std::int8_t, grind_settings.size()> a{};
        for (std::size_t i = 0; i < a.size(); ++i) a[i] = grind_settings[i].stock;
        return a;
    }();
    std::array<std::int8_t, transition_settings.size()> tr_own = [] {
        std::array<std::int8_t, transition_settings.size()> a{};
        for (std::size_t i = 0; i < a.size(); ++i) a[i] = transition_settings[i].stock;
        return a;
    }();
    std::array<std::int8_t, transition_settings.size()> tr_setting = [] { std::array<std::int8_t, transition_settings.size()> a{}; a.fill(-1); return a; }();
    // What the trainer knows of the transition the skater is on, from one tick to the next.
    struct TransitionTrack {
        std::uint64_t steep_at{};          // when the skater last rode steep ground
        std::array<float, 2> steep_out{};  // level direction out of that wall, into the ramp
        bool coping{};                     // on a coping grind or stall now
        bool grinding{}, flying{};         // at the last tick
        std::array<float, 2> edge{}, ramp{}; // level unit vectors: along the coping, and from it into the ramp
        float in_angle{}, in_speed{};      // how the grind began: degrees between the travel and the coping, m/s
        std::uint64_t left_at{};           // when the last coping grind ended without a pop
        std::uint64_t flight_off_until{};  // a flight up and over a lip: no reaching for edges until then
        bool flight_off{};
        bool air_grinds_off{};             // the air grind reach is held at 0 now
        float lean{};                      // the lean in force, eased
        std::uint64_t reports{};           // physics-step reports told so far
        std::uint64_t log_at{};
    } tr;
    float revert_boost{};            // 0: off
    float pump_power{1};             // x what pumping a transition gains
    bool pump_live{};                // the game's pumping states are found
    // <dev>
    std::uint32_t pump_seen{};       // pump log: the pumps told so far
    bool pump_was{};
    float pump_strength{}, pump_gained{}, pump_given{}, pump_speed{};
    std::uint32_t pump_steps{};
    // </dev>
    RevertTuning revert;
    std::uint64_t revert_boosts_seen{};
    std::uint32_t last_state{};
    float boost_factor{}; // velocity factor of the jump now starting, 0: none
    std::uint64_t boost_until{};
    float return_delay{1.5f};
    std::uint64_t seen_wipeouts{}, return_at{}, open_serial{};
    std::uint64_t class_search_at{}; // when to look for the game's tuning classes (0: not due)
    int class_search_tries{};
    std::uint64_t class_search_seen{};      // the searches that had finished when this level loaded
    bool class_list_wanted{};               // the player has the list of every value open on this level
    float flip_applied{1};                  // the flip speed the catch times were last set for
    std::uint64_t class_entity{};           // the skater the last search was for
    std::uint64_t class_research_after{};   // no new search before this, however many skaters come and go
    std::uint64_t class_creations{};        // the skaters the game had built at the last look
    bool class_research_due{};              // a new skater came while a search could not start: look when it can
    bool class_skater_due{};                // the same, for a new skater alone (nothing looked lost)
    bool class_skater_judged{true};         // the search a new skater asked for has been compared with what was known
    std::uint64_t class_skater_after{};     // no search for a new skater alone before this
    std::uint64_t class_skater_wait{class_skater_wait_low};
    std::uint64_t class_found{};            // class_search_found() before that search
    std::vector<std::size_t> class_entries; // the entries that are fields of those classes
    // A session whose host sets everyone's physics (session_physics.h): what the host shares
    // beyond its tuning, and what each class field holds for it. The player's own stand down.
    bool enforced{};
    std::uint64_t host_revision{};
    SessionExtras host;
    std::array<float, class_field_count> host_classes{};
    Boosts boosts;         // what is in force this tick
    std::string share_text; // the last exported preset, for the menu to put on the clipboard
    std::uint64_t share_serial{};
    bool extras_due{true}; // the player's own extras are to be published for a session again
    bool wipeouts_known{}; // the wipeout count the session started with was read
    int open_tab{};
    std::uint32_t pad_previous{};
    // The pause map's waypoint. The registry is read only while the player has a use for the
    // answer (the TELEPORT card is on screen, or `trainer waypoint`), and the waypoint is
    // forgotten on a new level: it belonged to the old one.
    std::optional<Vec3> waypoint;
    std::string waypoint_detail, waypoint_level;
    std::uint64_t waypoint_wanted_until{}, waypoint_next_read{};
    // A ground-snapped arrival being watched (trainer_landing.h).
    landing::Watch landing;
    Motion motion;
    Telemetry telemetry;
    std::ofstream log;
    std::int64_t log_start{};
    std::map<std::uint32_t, std::uint64_t> states; // physics state -> ticks seen
    SelfTest test;
};
State &state() {
    static auto *value = new State;
    return *value;
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
// "MaxPushSpeed" -> "Max Push Speed"
std::string spaced(std::string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        const bool upper = std::isupper(c) != 0;
        if (i && upper) {
            const auto before = static_cast<unsigned char>(text[i - 1]);
            const bool next_lower = i + 1 < text.size() && std::islower(static_cast<unsigned char>(text[i + 1]));
            if (std::islower(before) || std::isdigit(before) || (std::isupper(before) && next_lower)) result += ' ';
        }
        result += text[i] == '.' ? ' ' : text[i];
        if (text[i] == '.') result += "/ ";
    }
    return result;
}
std::optional<double> number(std::string_view text) {
    double value{};
    const auto *last = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last || !std::isfinite(value)) return std::nullopt;
    return value;
}
void say(logging::Level level, const std::string &text) { logging::write(level, logging::Channel::skater, text); }

// ---- storage ---------------------------------------------------------------------------
std::filesystem::path data_directory() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"trainer";
    if (raw) CoTaskMemFree(raw);
    return result;
}
std::filesystem::path game_directory() {
    std::vector<wchar_t> exe(32768);
    const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!length || length >= exe.size()) return {};
    return std::filesystem::path(exe.data()).parent_path();
}
std::optional<Json> read_json(const std::filesystem::path &path) {
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (text.empty() || text.size() > 1024 * 1024) return std::nullopt;
        return Json::parse(text);
    } catch (...) {
        return std::nullopt;
    }
}
std::optional<Vec3> read_position(const Json &value) {
    if (!value.is_array() || value.size() != 3) return std::nullopt;
    Vec3 result{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!value.at(i).is_number()) return std::nullopt;
        result[i] = value.at(i).get<float>();
        if (!std::isfinite(result[i]) || std::abs(result[i]) > max_coordinate) return std::nullopt;
    }
    return result;
}
Json write_position(const Vec3 &p) { return Json::array({Json(p[0]), Json(p[1]), Json(p[2])}); }

struct RevertField {
    std::string_view name;
    float RevertTuning::*member;
    float low, high;
};
constexpr RevertField revert_fields[]{
    {"spin", &RevertTuning::min_spin, 0.0f, 100000.0f},     {"slip", &RevertTuning::min_slip, 0.0f, 90.0f},
    {"twist", &RevertTuning::min_twist, 0.0f, 90.0f},       {"bend", &RevertTuning::bend_boost, -1000.0f, 1000.0f},
    {"full", &RevertTuning::full_boost, -1000.0f, 1000.0f}, {"auto", &RevertTuning::auto_boost, -1000.0f, 1000.0f},
    {"max_speed", &RevertTuning::max_speed, 0.0f, 1000.0f}, {"cooldown", &RevertTuning::cooldown, 0.0f, 60.0f},
    {"min_air", &RevertTuning::min_air, 0.0f, 60.0f},
};
RevertTuning sane_revert(RevertTuning tuning) {
    const RevertTuning stock;
    for (const auto &field : revert_fields) {
        auto &value = tuning.*field.member;
        value = std::isfinite(value) ? std::clamp(value, field.low, field.high) : stock.*field.member;
    }
    return tuning;
}
void load_store() {
    auto &s = state();
    s.loaded = true;
    const auto json = read_json(data_directory() / L"trainer.json");
    if (!json || !json->is_object()) return;
    try {
        if (json->contains("options") && json->at("options").is_object()) {
            const auto &o = json->at("options");
            s.auto_return = o.value("auto_return", false);
            s.pad_shortcuts = o.value("pad_shortcuts", false);
            s.pad_menu = o.value("pad_menu", true);
            s.hud = o.value("hud", false);
            s.hud_jump = o.value("hud_jump", false);
            s.return_delay = std::clamp(o.value("return_delay", 1.5f), 0.0f, 10.0f);
            s.hippy_height = std::clamp(o.value("hippy_height", 1.0f), height_low, height_high);
            s.nocomply_height = std::clamp(o.value("nocomply_height", 1.0f), height_low, height_high);
            s.manual_height = std::clamp(o.value("manual_height", 1.0f), height_low, height_high);
            s.grind_side = std::clamp(o.value("grind_side", 1.0f), 0.0f, 1000.0f);
            s.grind_push = std::clamp(o.value("grind_push", 0.0f), -50.0f, 50.0f);
            // (grind_lean, grind_turn and grind_tip were levers of earlier builds that did nothing: read past.)
            s.grind_range = std::clamp(o.value("grind_range", 0.0f), -360.0f, 360.0f);
            for (std::size_t i = 0; i < grind_settings.size(); ++i)
                s.grind_setting[i] = static_cast<std::int8_t>(std::clamp(o.value("grind.set." + std::string(grind_settings[i].key), -1), -1, 1));
            for (std::size_t i = 0; i < grind_switches.size(); ++i)
                s.grind_mode[i] = grind_switches[i].read_only ? std::int8_t{-1}
                                                               : static_cast<std::int8_t>(std::clamp(o.value("grind." + std::string(grind_switches[i].key), -1), -1, 1));
            s.tr_lean = std::clamp(o.value("transition.lean", 0.0f), -1.0f, 1.0f);
            s.tr_carry = std::clamp(o.value("transition.carry", 0.0f), 0.0f, 1.0f);
            s.tr_drop = std::clamp(o.value("transition.drop", 1.0f), 0.0f, 100.0f);
            s.tr_unstick = std::clamp(o.value("transition.unstick", 0.0f), 0.0f, 100.0f);
            s.tr_lockout = std::clamp(o.value("transition.lockout", 0.0f), 0.0f, 60.0f);
            s.tr_spine = o.value("transition.spine", false);
            s.tr_plant = std::clamp(o.value("transition.plant", 0.0f), 0.0f, 1.0f);
            for (std::size_t i = 0; i < transition_settings.size(); ++i)
                s.tr_setting[i] = static_cast<std::int8_t>(std::clamp(o.value("transition.set." + std::string(transition_settings[i].key), -1), -1, 1));
            s.offboard_height = std::clamp(o.value("offboard_height", 1.0f), height_low, height_high);
            s.flip_speed = std::clamp(o.value("flip_speed", 1.0f), flip_low, flip_high);
            s.flip_advanced = o.value("flip_advanced", false);
            s.flip_pop = o.value("flip_pop", false);
            s.flip_flick = o.value("flip_flick", false);
            for (std::size_t i = 0; i < flip_tricks.size(); ++i)
                s.flip_trick[i] = std::clamp(o.value(std::string(flip_tricks[i].key), 1.0f), flip_low, flip_high);
            s.boneless_height = std::clamp(o.value("boneless_height", 1.0f), height_low, height_high);
            s.revert_boost = std::clamp(o.value("revert_boost", 0.0f), 0.0f, revert_boost_high);
            s.pump_power = std::clamp(o.value("pump_power", 1.0f), 0.0f, pump_high);
            s.slot = std::clamp(o.value("slot", 0), 0, static_cast<int>(marker_slots) - 1);
        }
        if (json->contains("folds") && json->at("folds").is_object())
            for (const auto &[id, open] : json->at("folds").items())
                if (open.is_boolean() && id.size() <= 64) s.folds[id] = open.get<bool>();
        if (json->contains("values") && json->at("values").is_object())
            for (const auto &[key, row] : json->at("values").items())
                if (row.is_object() && row.contains("value") && row.at("value").is_number())
                    s.saved[lower(key)] = {row.at("value").get<double>(), row.value("frozen", false)};
        if (json->contains("presets") && json->at("presets").is_object())
            for (const auto &[name, values] : json->at("presets").items()) {
                if (!values.is_object() || name.empty() || name.size() > 48) continue;
                auto &preset = s.user[name];
                for (const auto &[key, value] : values.items())
                    if (value.is_number()) preset[lower(key)] = value.get<double>();
            }
        if (json->contains("active") && json->at("active").is_array())
            for (const auto &name : json->at("active"))
                // (Fast Parkour Flips and Pump Power were presets of earlier versions that changed nothing.)
                if (name.is_string() && name.string() != "Fast Parkour Flips" && name.string() != "Pump Power") s.active.push_back(name.string());
        if (json->contains("revert") && json->at("revert").is_object()) {
            for (const auto &field : revert_fields)
                if (const auto name = std::string(field.name); json->at("revert").contains(name) && json->at("revert").at(name).is_number())
                    s.revert.*field.member = json->at("revert").at(name).get<float>();
            // 0.4.1 saved its own default for the off-the-travel rule (12 degrees), which paid ordinary
            // 180s: a file from before the rules were numbered that still holds it gets today's (off).
            if (!json->at("revert").contains("rules") && s.revert.min_slip == 12.0f) s.revert.min_slip = RevertTuning{}.min_slip;
            s.revert = sane_revert(s.revert);
        }
        if (json->contains("maps") && json->at("maps").is_object())
            for (const auto &[level, row] : json->at("maps").items()) {
                if (!row.is_object()) continue;
                auto &map = s.maps[lower(level)];
                map.preset = row.value("preset", "");
                if (row.contains("markers") && row.at("markers").is_array())
                    for (std::size_t i = 0; i < marker_slots && i < row.at("markers").size(); ++i)
                        if (const auto position = read_position(row.at("markers").at(i))) map.markers[i] = {true, *position};
            }
    } catch (...) {
        say(logging::Level::warning, "Trainer: trainer.json is damaged; starting from what could be read.");
    }
}
void save_store() {
    auto &s = state();
    s.save_due = false;
    try {
        Json json = Json::object();
        json["schema"] = 1;
        Json options = Json::object();
        options["auto_return"] = s.auto_return;
        options["pad_shortcuts"] = s.pad_shortcuts;
        options["pad_menu"] = s.pad_menu;
        options["hud"] = s.hud;
        options["hud_jump"] = s.hud_jump;
        options["return_delay"] = s.return_delay;
        options["hippy_height"] = s.hippy_height;
        options["nocomply_height"] = s.nocomply_height;
        options["manual_height"] = s.manual_height;
        options["grind_side"] = s.grind_side;
        options["grind_push"] = s.grind_push;
        options["grind_range"] = s.grind_range;
        for (std::size_t i = 0; i < grind_settings.size(); ++i)
            if (s.grind_setting[i] >= 0) options["grind.set." + std::string(grind_settings[i].key)] = static_cast<int>(s.grind_setting[i]);
        for (std::size_t i = 0; i < grind_switches.size(); ++i)
            if (s.grind_mode[i] >= 0) options["grind." + std::string(grind_switches[i].key)] = static_cast<int>(s.grind_mode[i]);
        options["transition.lean"] = s.tr_lean;
        options["transition.carry"] = s.tr_carry;
        options["transition.drop"] = s.tr_drop;
        options["transition.unstick"] = s.tr_unstick;
        options["transition.lockout"] = s.tr_lockout;
        options["transition.spine"] = s.tr_spine;
        options["transition.plant"] = s.tr_plant;
        for (std::size_t i = 0; i < transition_settings.size(); ++i)
            if (s.tr_setting[i] >= 0) options["transition.set." + std::string(transition_settings[i].key)] = static_cast<int>(s.tr_setting[i]);
        options["offboard_height"] = s.offboard_height;
        options["flip_speed"] = s.flip_speed;
        options["flip_advanced"] = s.flip_advanced;
        options["flip_pop"] = s.flip_pop;
        options["flip_flick"] = s.flip_flick;
        for (std::size_t i = 0; i < flip_tricks.size(); ++i)
            if (s.flip_trick[i] != 1.0f) options[std::string(flip_tricks[i].key)] = s.flip_trick[i];
        options["boneless_height"] = s.boneless_height;
        options["revert_boost"] = s.revert_boost;
        options["pump_power"] = s.pump_power;
        options["slot"] = s.slot;
        json["options"] = std::move(options);
        Json folds = Json::object();
        for (const auto &[id, open] : s.folds) folds[id] = open;
        json["folds"] = std::move(folds);
        Json values = Json::object();
        // Values of fields this build's tuning does not have are kept as they were read.
        for (const auto &[key, saved] : s.saved) {
            Json row = Json::object();
            row["value"] = saved.value;
            row["frozen"] = saved.frozen;
            values[key] = std::move(row);
        }
        json["values"] = std::move(values);
        Json presets = Json::object();
        for (const auto &[name, preset] : s.user) {
            Json row = Json::object();
            for (const auto &[key, value] : preset) row[key] = value;
            presets[name] = std::move(row);
        }
        json["presets"] = std::move(presets);
        Json active = Json::array();
        for (const auto &name : s.active) active.push_back(name);
        json["active"] = std::move(active);
        Json maps = Json::object();
        for (const auto &[level, map] : s.maps) {
            if (level.empty()) continue;
            Json row = Json::object();
            Json markers = Json::array();
            for (const auto &marker : map.markers) markers.push_back(marker.set ? write_position(marker.position) : Json(nullptr));
            row["markers"] = std::move(markers);
            row["preset"] = map.preset;
            maps[level] = std::move(row);
        }
        json["maps"] = std::move(maps);
        Json revert = Json::object();
        for (const auto &field : revert_fields) revert[std::string(field.name)] = s.revert.*field.member;
        revert["rules"] = 2;
        json["revert"] = std::move(revert);
        const auto directory = data_directory();
        if (directory.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const auto target = directory / L"trainer.json", temporary = directory / L"trainer.json.tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file << json.dump(2);
            if (!file) return;
        }
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(target, error);
            std::filesystem::rename(temporary, target, error);
        }
    } catch (...) {
        say(logging::Level::warning, "Trainer: could not save trainer.json.");
    }
}
void changed(bool save = true) {
    auto &s = state();
    s.view_due = true;
    s.extras_due = true;
    if (save) {
        s.save_due = true;
        s.next_save = GetTickCount64() + 1500;
    }
}

// ---- values ----------------------------------------------------------------------------
double read_field(const std::vector<std::uint8_t> &image, const Entry &e) {
    const auto *at = image.data() + e.offset;
    if (e.kind == Kind::real) {
        float value;
        std::memcpy(&value, at, 4);
        return value;
    }
    if (e.kind == Kind::flag) return *at ? 1 : 0;
    switch (e.size) {
    case 1: { std::int8_t v; std::memcpy(&v, at, 1); return v; }
    case 2: { std::int16_t v; std::memcpy(&v, at, 2); return v; }
    case 4: { std::int32_t v; std::memcpy(&v, at, 4); return v; }
    default: { std::int64_t v; std::memcpy(&v, at, 8); return static_cast<double>(v); }
    }
}
void write_field(std::vector<std::uint8_t> &image, const Entry &e, double value) {
    auto *at = image.data() + e.offset;
    if (e.kind == Kind::real) {
        const auto v = static_cast<float>(value);
        std::memcpy(at, &v, 4);
    } else if (e.kind == Kind::flag) {
        *at = value != 0 ? 1 : 0;
    } else {
        switch (e.size) {
        case 1: { const auto v = static_cast<std::int8_t>(std::clamp(value, -128.0, 127.0)); std::memcpy(at, &v, 1); break; }
        case 2: { const auto v = static_cast<std::int16_t>(std::clamp(value, -32768.0, 32767.0)); std::memcpy(at, &v, 2); break; }
        case 4: { const auto v = static_cast<std::int32_t>(std::clamp(value, -2147483648.0, 2147483647.0)); std::memcpy(at, &v, 4); break; }
        default: { const auto v = static_cast<std::int64_t>(value); std::memcpy(at, &v, 8); break; }
        }
    }
}
// A curve's outputs times `factor`: a point is 0x1c bytes with its output at +0x14.
tuning::Curve scaled_curve(const tuning::Curve &source, double factor) {
    auto result = source;
    for (std::size_t p = 0; p + addr::physics_tuning::curve_point_size <= result.points.size(); p += addr::physics_tuning::curve_point_size) {
        float y;
        std::memcpy(&y, result.points.data() + p + 0x14, 4);
        y = static_cast<float>(y * factor);
        if (std::isfinite(y)) std::memcpy(result.points.data() + p + 0x14, &y, 4);
    }
    return result;
}
double sane(const Entry &e, double value) {
    if (!std::isfinite(value)) return e.stock;
    if (e.kind == Kind::flag) return value != 0 ? 1 : 0;
    if (e.kind == Kind::integer) return std::round(value);
    // No limits of the trainer's own beyond keeping a number a number: what a value may sensibly
    // be is the player's call (a slider's ends are only where the slider stops).
    if (e.kind == Kind::curve || e.kind == Kind::graph) return std::clamp(value, -1.0e9, 1.0e9);
    return std::clamp(value, -1.0e12, 1.0e12);
}
// The held flip class's catch times: how long each kind of flip may take to come round.
// <dev>
// `trainer flipext 0|1`: the catch extension times follow the flip speed too (a trial).
bool dev_slow_extensions = false;
// </dev>
bool slowed_catch_time(std::string_view key) {
    // <dev>
    if (dev_slow_extensions && key.starts_with("heldflip.") && key.ends_with("catchextensiontime")) return true;
    // </dev>
    return key.starts_with("heldflip.") && key.ends_with("catchtime");
}
// What a class field should hold: the player's value, or the host's while a session's host
// sets everyone's physics.
void want_class(const Entry &e) {
    auto &s = state();
    const auto field = static_cast<std::size_t>(e.class_field);
    auto value = s.enforced ? s.host_classes[field] : static_cast<float>(e.touched ? e.value : e.stock);
    // The game hurries any flip that would not come round inside its catch time (measured: at
    // every flip speed below about x0.9 a kickflip turned at 360 degrees per 0.2 s, the stock
    // catch time). A flip slowed by the trick slider is given that much longer.
    if (s.boosts.flip < 1.0f && slowed_catch_time(e.key)) value /= std::max(s.boosts.flip, flip_low);
    want_class_value(field, value);
}
void apply_entry(Entry &e) {
    auto &s = state();
    if (e.class_field >= 0) {
        want_class(e);
        return;
    }
    if (e.kind == Kind::graph) {
        // A point the player set by hand keeps their value.
        for (const auto index : e.members) {
            const auto &member = s.entries[index];
            if (!member.touched && static_cast<std::size_t>(member.offset) + member.size <= s.want.image.size())
                write_field(s.want.image, member, e.touched ? member.stock * e.value : member.stock);
        }
    } else if (e.kind == Kind::curve) {
        if (e.curve < s.own.curves.size() && s.own.curves[e.curve] && e.curve < s.want.curves.size())
            s.want.curves[e.curve] = e.touched ? scaled_curve(*s.own.curves[e.curve], e.value) : *s.own.curves[e.curve];
    } else if (static_cast<std::size_t>(e.offset) + e.size <= s.want.image.size()) {
        write_field(s.want.image, e, e.touched ? e.value : e.stock);
    }
    s.write_due = true;
}
void remember(const Entry &e) {
    auto &s = state();
    if (e.touched || e.frozen) s.saved[e.key] = {e.value, e.frozen};
    else s.saved.erase(e.key);
}
void set_entry(Entry &e, double value) {
    e.value = sane(e, value);
    e.touched = e.value != e.stock;
    apply_entry(e);
    remember(e);
    // A linked value scales its graphs by the same ratio (the graphs are what the game reads).
    if (!e.drives.empty() && e.stock != 0) {
        const auto ratio = e.value / e.stock;
        const auto targets = e.drives; // set_entry may not touch `e` again, but keep the list stable
        auto &s = state();
        for (const auto index : targets)
            if (index < s.entries.size() && !s.entries[index].frozen) {
                auto &target = s.entries[index];
                const bool multiplier = target.kind == Kind::curve || target.kind == Kind::graph;
                set_entry(target, multiplier ? ratio : target.stock * ratio);
            }
    }
}
// A value a locked linked value drives (its graphs, the push class's speeds) is locked with it:
// resetting it alone would leave the lock showing a number that no longer does anything.
bool is_locked(const Entry &e) {
    if (e.frozen) return true;
    auto &s = state();
    const auto index = static_cast<std::size_t>(&e - s.entries.data());
    return std::ranges::any_of(s.entries, [&](const Entry &other) {
        return other.frozen && std::ranges::find(other.drives, index) != other.drives.end();
    });
}
Entry *find_entry(std::string_view id) {
    auto &s = state();
    const auto found = s.index.find(lower(id));
    return found == s.index.end() ? nullptr : &s.entries[found->second];
}
void build_entries() {
    auto &s = state();
    s.entries.clear();
    s.index.clear();
    s.class_entries.clear();
    const auto &m = *s.model;
    const auto add = [&](std::string name, Kind kind, std::uint16_t offset, std::uint16_t size, std::size_t curve) {
        if (name.empty()) name = std::format("Unnamed.0x{:x}", offset);
        Entry e;
        e.id = kind == Kind::curve || kind == Kind::graph ? name + " x" : name;
        e.key = lower(e.id);
        if (s.index.contains(e.key)) { // arrays of structures repeat names
            e.id += std::format(" @0x{:x}", offset);
            e.key = lower(e.id);
        }
        const auto dot = name.find('.');
        auto group = dot == std::string::npos ? std::string("General") : name.substr(0, dot);
        for (const std::string_view prefix : {"SkatePhysics", "Physics"})
            if (group.size() > prefix.size() && group.starts_with(prefix)) {
                group.erase(0, prefix.size());
                break;
            }
        e.group = spaced(group);
        e.label = spaced(dot == std::string::npos ? name : name.substr(dot + 1));
        if (kind == Kind::curve || kind == Kind::graph) e.label += " (x)";
        e.kind = kind;
        e.offset = offset;
        e.size = size;
        e.curve = curve;
        e.used = value_used(offset);
        s.index.emplace(e.key, s.entries.size());
        s.entries.push_back(std::move(e));
    };
    for (std::size_t i = 0; i < m.fields.size(); ++i) {
        const auto &f = m.fields[i];
        add(i < m.field_names.size() ? m.field_names[i] : std::string{}, f.real ? Kind::real : f.flag ? Kind::flag : Kind::integer,
            f.offset, f.size, 0);
    }
    for (std::size_t i = 0; i < m.curve_slots.size(); ++i)
        add(i < m.curve_names.size() ? m.curve_names[i] : std::string{}, Kind::curve, m.curve_slots[i], 8, i);
    // Graphs stored in the asset: sixteen X and Y values, a point count and bounds. Each
    // becomes one multiplier row; its points stay editable but out of the way.
    const auto plain = s.entries.size();
    for (std::size_t i = 0; i < plain; ++i) {
        if (!s.entries[i].id.ends_with(".Y0") || s.entries[i].kind != Kind::real) continue;
        const auto parent = s.entries[i].id.substr(0, s.entries[i].id.size() - 3);
        const auto offset = s.entries[i].offset;
        std::vector<std::size_t> outputs;
        for (std::size_t j = 0; j < plain; ++j) {
            auto &other = s.entries[j];
            if (other.id.size() <= parent.size() + 1 || !other.id.starts_with(parent) || other.id[parent.size()] != '.') continue;
            const std::string_view leaf(other.id.c_str() + parent.size() + 1);
            if (leaf.find('.') != std::string_view::npos) continue;
            other.detail = true;
            const bool output = (leaf.starts_with("Y") && leaf.size() <= 3) || leaf == "Min_Y" || leaf == "Max_Y";
            if (output && other.kind == Kind::real) outputs.push_back(j);
        }
        add(parent, Kind::graph, offset, 0, 0);
        s.entries.back().members = std::move(outputs);
    }
    // The game's data-defined classes: one row per number field, under the class's name.
    for (std::size_t c = 0; c < class_count; ++c)
        for (std::size_t i = 0; i < class_specs[c].count; ++i) {
            const auto &field = class_fields[class_specs[c].first + i];
            Entry e;
            e.id = std::format("{}.{}", class_specs[c].key, field.name);
            e.key = lower(e.id);
            if (s.index.contains(e.key)) continue;
            e.group = class_specs[c].group;
            e.label = spaced(field.name);
            e.kind = Kind::real;
            e.used = field.seen;
            e.class_field = static_cast<int>(class_specs[c].first + i);
            e.stock = e.value = field.stock;
            s.index.emplace(e.key, s.entries.size());
            s.class_entries.push_back(s.entries.size());
            s.entries.push_back(std::move(e));
        }
    for (const auto &link : value_links()) {
        const auto from = s.index.find(link.value), to = s.index.find(link.drives);
        if (from == s.index.end() || to == s.index.end()) continue;
        s.entries[from->second].drives.push_back(to->second);
        s.entries[from->second].used = true;
    }
    // A host's values may have arrived before there were entries to hold them.
    for (const auto index : s.class_entries) want_class(s.entries[index]);
    s.extras_due = true;
    say(logging::Level::info, std::format("Trainer: {} tuning values and {} curves are editable.", m.fields.size(), m.curve_slots.size()));
}
// A new copy of the asset (first sight, or a level load): its values are the stock ones.
void adopt(const tuning::Values &live) {
    auto &s = state();
    s.own = live;
    s.want = live;
    for (auto &e : s.entries) {
        e.stock = e.class_field >= 0 ? class_fields[e.class_field].stock : e.kind == Kind::curve || e.kind == Kind::graph ? 1.0 : read_field(s.own.image, e);
        const auto found = s.saved.find(e.key);
        if (found != s.saved.end()) {
            e.frozen = found->second.frozen;
            e.value = sane(e, found->second.value);
            e.touched = e.value != e.stock;
        } else if (!e.touched) {
            e.value = e.stock;
        }
        if (e.touched) apply_entry(e);
    }
    // A linked value saved by itself (before links existed, or edited in the file) still
    // drives its graphs.
    for (auto &e : s.entries) {
        if (e.drives.empty() || !e.touched || e.stock == 0) continue;
        for (const auto index : e.drives) {
            auto &target = s.entries[index];
            if (target.touched || target.frozen) continue;
            const bool multiplier = target.kind == Kind::curve || target.kind == Kind::graph;
            target.value = sane(target, multiplier ? e.value / e.stock : target.stock * e.value / e.stock);
            target.touched = target.value != target.stock;
            if (target.touched) {
                apply_entry(target);
                remember(target);
            }
        }
    }
    s.write_due = true;
    changed(false);
}
std::string apply_preset(std::string_view name);
bool editable(std::string *why = nullptr) {
    // A guest's physics are the host's while the session enforces them: the tuning
    // (physics_tuning::enforce), and what the host shares beyond it (sync_session). Writing
    // over either would only fight the session, so the player's own stand down.
    if (session_tuning_enforced()) {
        if (why) *why = "You are in a multiplayer game where the host controls physics. This menu will not work in this multiplayer game.";
        return false;
    }
    return true;
}
// Auto push is the trainer's doing: the game's flag reaches only its animation.
float own_cruise() {
    const auto *auto_push = find_entry("physicsmode.autopushenabled"), *auto_speed = find_entry("physicspush.maxspeedforautopush");
    return auto_push && auto_speed && auto_push->touched && auto_push->value != 0 ? static_cast<float>(auto_speed->value) : 0.0f;
}
// The host's where a session's host sets everyone's physics, whatever the boosts switch says
// (that switch is about a guest's own); otherwise the player's, and the game's own in a
// session that has turned boosts off.
// A session where the player's own trick extras do not count: its host sets the physics, or has turned boosts off.
bool extras_blocked() { return state().enforced || (multiplayer_session_active() && !session_boosts_allowed()); }
Boosts boosts_in_force() {
    auto &s = state();
    if (s.enforced)
        return {s.host.flip_speed, s.host.hippy_height, s.host.nocomply_height, s.host.boneless_height, s.host.offboard_height, s.host.cruise};
    if (multiplayer_session_active() && !session_boosts_allowed()) return {};
    return {s.flip_speed, s.hippy_height, s.nocomply_height, s.boneless_height, s.offboard_height, own_cruise()};
}
// The player's own, for a session they host to share with its guests.
void publish_extras() {
    auto &s = state();
    s.extras_due = false;
    SessionExtras own;
    own.flip_speed = s.flip_speed;
    own.hippy_height = s.hippy_height;
    own.nocomply_height = s.nocomply_height;
    own.boneless_height = s.boneless_height;
    own.offboard_height = s.offboard_height;
    own.cruise = own_cruise();
    for (const auto index : s.class_entries)
        if (const auto &e = s.entries[index]; e.touched)
            own.classes.emplace_back(static_cast<std::uint16_t>(e.class_field), static_cast<float>(e.value));
    set_local_physics_extras(encode_session_extras(own));
}
// Follows a session into and out of its host's physics, and the host's changes while there.
void sync_session() {
    auto &s = state();
    const bool enforced = session_tuning_enforced();
    std::vector<std::uint8_t> bytes;
    bool fresh = false;
    if (enforced) fresh = host_physics_extras(s.host_revision, bytes);
    else s.host_revision = 0; // read again the next time a host sets the physics
    if (enforced == s.enforced && !fresh) return;
    if (fresh) {
        auto decoded = decode_session_extras(bytes);
        if (!decoded) say(logging::Level::warning, "Trainer: what the host shares beyond its tuning could not be read; the game's own is used for it.");
        s.host = decoded ? std::move(*decoded) : SessionExtras{};
        for (std::size_t i = 0; i < class_field_count; ++i) s.host_classes[i] = class_fields[i].stock;
        for (const auto &[index, value] : s.host.classes) s.host_classes[index] = value;
        if (!s.host.stock())
            say(logging::Level::info, std::format("Trainer: skating with the host's trick multipliers and {} of its class values.", s.host.classes.size()));
    }
    s.enforced = enforced;
    for (const auto index : s.class_entries) want_class(s.entries[index]);
    changed(false);
}
void hold(std::uint64_t now) {
    auto &s = state();
    if (!s.base) return;
    if (!s.model) {
        tuning::prepare();
        std::string error;
        s.model = tuning::game_tuning(&error);
        if (!s.model) {
            const auto next = error.empty() ? std::string("Reading the game's physics tuning...") : "Physics tuning is unavailable: " + error + ".";
            if (next != s.status) { s.status = next; changed(false); }
            return;
        }
        build_entries();
    }
    if (!s.write_due && now < s.next_hold) return;
    s.next_hold = now + 500;
    tuning::Values live;
    std::uintptr_t asset{};
    if (!tuning::read_live(s.base, live, &asset)) {
        if (s.ready) { s.ready = false; s.status = "Waiting for a level."; changed(false); }
        return;
    }
    if (asset != s.asset || !s.ready) {
        s.asset = asset;
        adopt(live);
        s.ready = true;
        s.status = "Ready.";
    }
    if (!editable()) return;
    if (s.profile_due) {
        s.profile_due = false;
        if (const auto found = s.maps.find(s.map); found != s.maps.end() && !found->second.preset.empty())
            say(logging::Level::info, "Trainer: this map's preset - " + apply_preset(found->second.preset));
    }
    const bool pending = s.write_due || std::ranges::any_of(s.entries, [](const Entry &e) { return e.touched; });
    s.write_due = false;
    if (!pending) return;
    const auto written = tuning::write_live(s.base, s.entity, s.want);
    if (written.failed) {
        s.status = "The physics tuning could not be written.";
        changed(false);
    }
    // The skater skates from its own copy of part of the tuning: keep asking until it took it.
    if (written.values || written.curves) {
        s.refresh_due = !written.refreshed;
        say(logging::Level::info, std::format("Trainer: wrote {} values and {} curves; skater copy refreshed: {}.", written.values, written.curves,
                                              written.refreshed ? "yes" : "not yet"));
    } else if (s.refresh_due && tuning::refresh_skater(s.base, s.entity)) {
        s.refresh_due = false;
        say(logging::Level::info, "Trainer: skater copy refreshed.");
    }
}

// ---- presets ---------------------------------------------------------------------------
bool matches(const std::string &key, std::string_view pattern) {
    std::size_t start = 0;
    while (start < pattern.size()) {
        auto end = pattern.find(' ', start);
        if (end == std::string_view::npos) end = pattern.size();
        const auto token = pattern.substr(start, end - start);
        if (!token.empty()) {
            const bool exclude = token.front() == '!';
            const auto word = exclude ? token.substr(1) : token;
            if ((key.find(word) != std::string::npos) == exclude) return false;
        }
        start = end + 1;
    }
    return true;
}
// What each built-in preset's rules reach in the running game: (entry, rule) pairs, worked out
// once per table of entries.
// Whether a preset rule is about this value (`scale`: it is a curve or graph multiplier).
bool rule_matches(const Entry &e, bool scale, const PresetRule &rule) {
    if (rule.exact) return !scale && e.key == rule.pattern;
    return !e.detail && scale == rule.curves && matches(e.key, rule.pattern);
}
const std::vector<std::vector<std::pair<std::size_t, std::size_t>>> &preset_targets() {
    static std::vector<std::vector<std::pair<std::size_t, std::size_t>>> cache;
    static const Entry *cached_for{};
    static std::size_t cached_count{};
    auto &s = state();
    if (cached_for == s.entries.data() && cached_count == s.entries.size() && !cache.empty()) return cache;
    cached_for = s.entries.data();
    cached_count = s.entries.size();
    const auto &presets = builtin_presets();
    cache.assign(presets.size(), {});
    for (std::size_t p = 0; p < presets.size(); ++p)
        for (std::size_t r = 0; r < presets[p].rules.size(); ++r) {
            const auto &rule = presets[p].rules[r];
            for (std::size_t i = 0; i < s.entries.size(); ++i) {
                const auto &e = s.entries[i];
                const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                if (!rule_matches(e, scale, rule)) continue;
                if (e.kind == Kind::flag && rule.multiply) continue;
                cache[p].emplace_back(i, r);
            }
        }
    return cache;
}
// A preset as a dial: `factor` is where its first value goes (x of stock); the others follow in
// proportion, so 1 is the game's own and the first rule's amount is the preset as it ships.
std::string apply_dial(std::string_view name, double factor) {
    auto &s = state();
    const auto &presets = builtin_presets();
    const auto wanted = lower(name);
    std::size_t p = 0;
    while (p < presets.size() && lower(presets[p].name) != wanted) ++p;
    if (p == presets.size()) return "error: no built-in preset is called \"" + std::string(name) + "\".";
    const auto &rules = presets[p].rules;
    if (rules.empty() || !rules[0].multiply || rules[0].amount <= 0 || rules[0].amount == 1)
        return "error: " + std::string(presets[p].name) + " is a switch, not a dial.";
    if (!(factor > 0) || !std::isfinite(factor)) return "error: a dial needs a multiplier above 0; 1 is the game's own.";
    const double t = std::log(factor) / std::log(rules[0].amount);
    std::size_t count{}, locked{};
    for (const auto &[index, r] : preset_targets()[p]) {
        auto &e = s.entries[index];
        const auto &rule = rules[r];
        if (is_locked(e)) {
            ++locked;
            continue;
        }
        if (rule.multiply) set_entry(e, rule.amount > 0 ? e.stock * std::pow(rule.amount, t) : e.stock * (1 + (rule.amount - 1) * t));
        else set_entry(e, t > 0.01 ? rule.amount : e.stock); // a switch that goes with the preset's direction
        ++count;
    }
    std::erase(s.active, std::string(presets[p].name));
    if (std::abs(t - 1) < 1e-3) s.active.emplace_back(presets[p].name);
    changed();
    return std::format("{} x{:.2f}: {} values set{}.", presets[p].name, factor, count, locked ? std::format("; {} locked values left alone", locked) : "");
}
float *trick_option(std::string_view name) {
    auto &s = state();
    for (std::size_t i = 0; i < flip_tricks.size(); ++i)
        if (name == flip_tricks[i].key) return &s.flip_trick[i];
    return name == "flip_speed" ? &s.flip_speed : name == "hippy_height" ? &s.hippy_height : name == "nocomply_height" ? &s.nocomply_height
         : name == "boneless_height" ? &s.boneless_height : name == "offboard_height" ? &s.offboard_height
         : name == "revert_boost" ? &s.revert_boost : name == "pump_power" ? &s.pump_power : name == "manual_height" ? &s.manual_height : nullptr;
}
// Everything of the trainer's that changes how the game plays and is not a tuning value: one number each, under the
// key a preset carries it by. The STOCK badge, every reset and the player's presets all go through this list, so a new
// option cannot be counted by one of them and missed by another.
struct Setup {
    std::string key;
    std::uint8_t topic{}; // the section of the Tune tab that shows it (trainer.h: topic_*)
    double stock{};
    std::function<double()> get;
    std::function<void(double)> set;
    std::function<bool()> in_force; // empty: always. Off stock but not in force (a trick's own speed with Advanced unticked) is not "modified".
};
const std::vector<Setup> &setups() {
    static const auto *list = [] {
        auto *all = new std::vector<Setup>;
        auto &s = state();
        const auto real = [&](std::string key, std::uint8_t topic, float *value, float stock, float low, float high) {
            all->push_back({std::move(key), topic, stock, [value] { return static_cast<double>(*value); },
                            [value, low, high](double v) { *value = std::isfinite(v) ? std::clamp(static_cast<float>(v), low, high) : *value; }, {}});
        };
        const auto toggle = [&](std::string key, std::uint8_t topic, bool *value) {
            all->push_back({std::move(key), topic, 0.0, [value] { return *value ? 1.0 : 0.0; }, [value](double v) { *value = v != 0; }, {}});
        };
        const auto mode = [&](std::string key, std::uint8_t topic, std::int8_t *value) {
            all->push_back({std::move(key), topic, -1.0, [value] { return static_cast<double>(*value); },
                            [value](double v) { *value = static_cast<std::int8_t>(v < 0 ? -1 : v != 0 ? 1 : 0); }, {}});
        };
        real("trick.flip_speed", topic_pop, &s.flip_speed, 1.0f, flip_low, flip_high);
        real("trick.manual_height", topic_pop, &s.manual_height, 1.0f, height_low, height_high);
        real("trick.nocomply_height", topic_pop, &s.nocomply_height, 1.0f, height_low, height_high);
        real("trick.boneless_height", topic_pop, &s.boneless_height, 1.0f, height_low, height_high);
        real("trick.hippy_height", topic_pop, &s.hippy_height, 1.0f, height_low, height_high);
        real("trick.offboard_height", topic_foot, &s.offboard_height, 1.0f, height_low, height_high);
        real("trick.pump_power", topic_pumping, &s.pump_power, 1.0f, 0.0f, pump_high);
        real("trick.revert_boost", topic_trickline, &s.revert_boost, 0.0f, 0.0f, revert_boost_high);
        toggle("trick.flip_advanced", topic_pop, &s.flip_advanced);
        toggle("trick.flip_pop", topic_pop, &s.flip_pop);
        toggle("trick.flip_flick", topic_pop, &s.flip_flick);
        for (std::size_t i = 0; i < flip_tricks.size(); ++i) {
            real("trick." + std::string(flip_tricks[i].key), topic_pop, &s.flip_trick[i], 1.0f, flip_low, flip_high);
            all->back().in_force = [&s] { return s.flip_advanced; };
        }
        real("grind.side", topic_grinds, &s.grind_side, 1.0f, 0.0f, 1000.0f);
        real("grind.push", topic_grinds, &s.grind_push, 0.0f, -50.0f, 50.0f);
        real("grind.range", topic_grinds, &s.grind_range, 0.0f, -360.0f, 360.0f);
        for (std::size_t i = 0; i < grind_settings.size(); ++i) mode("grind.set." + std::string(grind_settings[i].key), topic_grinds, &s.grind_setting[i]);
        for (std::size_t i = 0; i < grind_switches.size(); ++i)
            if (!grind_switches[i].read_only) mode("grind.hold." + std::string(grind_switches[i].key), topic_grinds, &s.grind_mode[i]);
        real("transition.lean", topic_transition, &s.tr_lean, 0.0f, -1.0f, 1.0f);
        real("transition.carry", topic_transition, &s.tr_carry, 0.0f, 0.0f, 1.0f);
        real("transition.drop", topic_transition, &s.tr_drop, 1.0f, 0.0f, 100.0f);
        all->back().in_force = [&s] { return s.tr_carry != 0.0f; }; // the limit on what the row above keeps
        real("transition.unstick", topic_transition, &s.tr_unstick, 0.0f, 0.0f, 100.0f);
        real("transition.lockout", topic_transition, &s.tr_lockout, 0.0f, 0.0f, 60.0f);
        real("transition.plant", topic_transition, &s.tr_plant, 0.0f, 0.0f, 1.0f);
        toggle("transition.spine", topic_transition, &s.tr_spine);
        for (std::size_t i = 0; i < transition_settings.size(); ++i)
            mode("transition.set." + std::string(transition_settings[i].key), transition_settings[i].card == transition_card_pump ? topic_pumping : topic_transition,
                 &s.tr_setting[i]);
        for (const auto &field : revert_fields) {
            const auto member = field.member;
            const float low = field.low, high = field.high;
            all->push_back({"revert." + std::string(field.name), topic_trickline, static_cast<double>(RevertTuning{}.*member),
                            [&s, member] { return static_cast<double>(s.revert.*member); },
                            [&s, member, low, high](double v) { if (std::isfinite(v)) s.revert.*member = std::clamp(static_cast<float>(v), low, high); }, {}});
        }
        return all;
    }();
    return *list;
}
const Setup *find_setup(std::string_view key) {
    for (const auto &one : setups())
        if (one.key == key) return &one;
    return nullptr;
}
bool off_stock(const Setup &one) { return one.get() != one.stock; }
bool modified(const Setup &one) { return off_stock(one) && (!one.in_force || one.in_force()); }
// Puts back everything on that list (`topic` 0), or what one section of the Tune tab shows.
std::size_t reset_setups(std::uint8_t topic = 0) {
    std::size_t count{};
    for (const auto &one : setups())
        if ((!topic || one.topic == topic) && off_stock(one)) {
            one.set(one.stock);
            ++count;
        }
    return count;
}
// Sets one of the setups named by a preset key; false when the key is not one (it is a tuning value's, then).
// (trick.flip_gate, trick.catch_at and trick.catch_percent come in presets of earlier versions and are read past.)
bool set_setup_key(std::string_view key, double value) {
    if (key == "trick.flip_gate" || key == "trick.catch_at" || key == "trick.catch_percent") return true;
    const auto *one = find_setup(key);
    if (!one) return false;
    one->set(value);
    return true;
}
// Whether a built-in preset is on: the values say so, whatever switched them (a slider, a dial or another preset).
bool builtin_on(std::size_t p);
// `matching`: only the values that still hold what the built-in preset set go back (a feel taken off must not wipe a
// value the player has since set themselves).
std::string remove_preset(std::string_view name, bool matching = false) {
    auto &s = state();
    const auto wanted = lower(name);
    std::size_t count{};
    std::string removed;
    const auto put_back = [&](Entry &e) {
        if (is_locked(e)) return;
        if (e.touched) {
            set_entry(e, e.stock);
            ++count;
            return;
        }
        // A linked value at stock whose graphs were set directly (by an older version's preset).
        for (const auto index : e.drives)
            if (auto &target = s.entries[index]; target.touched && !is_locked(target)) {
                set_entry(target, target.stock);
                ++count;
            }
    };
    for (const auto &[user_name, values] : s.user) {
        if (lower(user_name) != wanted) continue;
        for (const auto &saved : values) {
            if (const auto *one = find_setup(saved.first)) {
                one->set(one->stock);
                ++count;
            }
            else if (auto *e = find_entry(saved.first)) put_back(*e);
        }
        removed = user_name;
        break;
    }
    if (removed.empty())
        for (const auto &preset : builtin_presets()) {
            if (lower(preset.name) != wanted) continue;
            for (const auto &rule : preset.rules)
                for (auto &e : s.entries) {
                    const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                    if (!rule_matches(e, scale, rule)) continue;
                    if (matching && std::abs(e.value - sane(e, rule.multiply ? e.stock * rule.amount : rule.amount)) > 1e-6 * std::max(1.0, std::abs(e.value))) continue;
                    put_back(e);
                }
            removed = preset.name;
            break;
        }
    if (removed.empty()) return "error: no preset is called \"" + std::string(name) + "\".";
    std::erase(s.active, removed);
    // Presets that are still on may share values with the one switched off: set theirs again.
    const auto still_on = s.active;
    for (const auto &other : still_on) (void)apply_preset(other);
    changed();
    return std::format("{} off: {} values put back.", removed, count);
}
std::size_t reset_all() {
    auto &s = state();
    std::size_t count{};
    for (auto &e : s.entries) {
        if (!e.touched || is_locked(e)) continue;
        set_entry(e, e.stock);
        ++count;
    }
    s.active.clear();
    return count;
}
std::string apply_preset(std::string_view name) {
    auto &s = state();
    const auto wanted = lower(name);
    if (wanted == "stock") {
        const auto count = reset_all();
        changed();
        return std::format("Stock: {} values put back (frozen values kept).", count);
    }
    std::size_t count{}, locked{};
    std::string applied;
    if (wanted == "map") {
        if (s.map_file.preset.empty()) return "error: this map ships no trainer preset.";
        for (const auto &[id, value] : s.map_file.preset)
            if (auto *e = find_entry(id); e && !is_locked(*e)) { set_entry(*e, value); ++count; }
        applied = s.map_file.preset_name.empty() ? "Map preset" : s.map_file.preset_name;
    }
    if (applied.empty())
        for (const auto &[user_name, values] : s.user) {
            if (lower(user_name) != wanted) continue;
            for (const auto &[key, value] : values) {
                if (set_setup_key(key, value)) ++count;
                else if (auto *e = find_entry(key); e && !is_locked(*e)) { set_entry(*e, value); ++count; }
            }
            applied = user_name;
            break;
        }
    if (applied.empty())
        for (const auto &preset : builtin_presets()) {
            if (lower(preset.name) != wanted) continue;
            for (const auto &rule : preset.rules)
                for (auto &e : s.entries) {
                    const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                    if (!rule_matches(e, scale, rule)) continue;
                    if (e.kind == Kind::flag && rule.multiply) continue;
                    if (is_locked(e)) {
                        ++locked;
                        continue;
                    }
                    set_entry(e, rule.multiply ? e.stock * rule.amount : rule.amount);
                    ++count;
                }
            applied = preset.name;
            break;
        }
    if (applied.empty()) return "error: no preset is called \"" + std::string(name) + "\".";
    if (std::ranges::find(s.active, applied) == s.active.end()) s.active.push_back(applied);
    changed();
    return std::format("{}: {} values set{}.", applied, count,
                       locked ? std::format("; {} locked values left alone (untick their boxes in Tune to let presets change them)", locked) : "");
}

// ---- maps ------------------------------------------------------------------------------
MapFile read_map_file(const std::string &level) {
    MapFile result;
    const auto mods = game_directory() / L"Mods";
    std::error_code error;
    for (std::filesystem::directory_iterator it(mods, error), end; it != end && !error; it.increment(error)) {
        if (!it->is_directory(error)) continue;
        const auto json = read_json(it->path() / L"trainer.json");
        if (!json || !json->is_object()) continue;
        try {
            // The file names its level, or it stands for every level its mod adds.
            bool mine = false;
            if (json->contains("level") && json->at("level").is_string()) mine = lower(json->at("level").string()) == level;
            else if (const auto levels = read_json(it->path() / L"reskate-levels.json"); levels && levels->contains("levels"))
                for (const auto &row : levels->at("levels"))
                    if (row.is_object() && lower(row.value("asset", "")) == level) mine = true;
            if (!mine) continue;
            result.note = json->value("note", "");
            if (json->contains("preset") && json->at("preset").is_object()) {
                const auto &preset = json->at("preset");
                result.preset_name = preset.value("name", "");
                if (preset.contains("values") && preset.at("values").is_object())
                    for (const auto &[id, value] : preset.at("values").items())
                        if (value.is_number() && result.preset.size() < 512) result.preset.emplace_back(id, value.get<double>());
            }
            if (json->contains("spots") && json->at("spots").is_array())
                for (const auto &row : json->at("spots")) {
                    if (!row.is_object() || !row.contains("position") || result.spots.size() >= 64) continue;
                    if (const auto position = read_position(row.at("position")))
                        result.spots.push_back({row.value("name", "Spot"), *position});
                }
            return result;
        } catch (...) {}
    }
    return result;
}
void enter_map(const std::string &level) {
    auto &s = state();
    s.map = level;
    s.map_file = level.empty() ? MapFile{} : read_map_file(level);
    s.motion = {};
    s.telemetry.last = {};
    s.telemetry.best = {};
    s.landing = {};
    s.telemetry.top_speed = 0;
    s.return_at = 0;
    s.profile_due = false;
    // A level brings fresh copies of the game's tuning classes: look for them once it has settled.
    s.class_search_at = level.empty() ? 0 : GetTickCount64() + 10000;
    s.class_search_tries = 0;
    s.class_search_seen = class_searches();
    s.class_skater_wait = class_skater_wait_low;
    s.class_skater_after = 0;
    s.class_skater_due = false;
    s.class_skater_judged = true;
    s.class_list_wanted = false;
    if (!level.empty()) {
        const auto found = s.maps.find(level);
        s.profile_due = found != s.maps.end() && !found->second.preset.empty();
        if (!s.map_file.spots.empty() || !s.map_file.preset.empty())
            say(logging::Level::info, std::format("Trainer: the map ships {} spots and {} preset values.", s.map_file.spots.size(),
                                                    s.map_file.preset.size()));
    }
    changed(false);
}
MapData &current_map() { return state().maps[state().map]; }
bool teleport_allowed(std::string &why) {
    if (multiplayer_session_active() && !session_noclip_allowed()) {
        why = "The host has turned off teleporting in this session.";
        return false;
    }
    return true;
}
std::string go_to(const Vec3 &position, const std::string &what) {
    std::string why;
    if (!teleport_allowed(why)) return "error: " + why;
    if (!teleport_local_skater(position)) return "error: teleporting is unavailable right now.";
    state().motion.valid = false;
    return "Teleporting to " + what + ".";
}

// ---- ground-safe arrivals -------------------------------------------------------------
// The topmost collision surface at x, z (trainer_landing.h for the ray), from the client
// physics world. Empty while collision there is still streaming in.
std::optional<float> topmost_ground(float x, float z, float hint_y) {
    const auto first = landing::first_ray(hint_y);
    if (const auto hit = local_ground_height(x, z, first.top, first.bottom)) return hit;
    if (const auto retry = landing::retry_ray(first)) return local_ground_height(x, z, retry->top, retry->bottom);
    return std::nullopt;
}

// Teleports onto the surface at x, z rather than to a raw height: a map waypoint's height is
// not the ground, and arriving under it drops the skater through the map.
std::string land_at(float x, float z, float hint_y, const std::string &what) {
    auto &s = state();
    const auto ground = topmost_ground(x, z, hint_y);
    // Collision far away may not be loaded yet: go to the hint's height, and the landing
    // watch puts the skater on the surface once it streams in.
    const Vec3 to{x, ground ? *ground + landing::stand_height : hint_y + 2.0f, z};
    const auto answer = go_to(to, what);
    if (answer.starts_with("error")) return answer;
    s.landing = landing::start(x, z, to[1], GetTickCount64());
    say(logging::Level::info, ground ? std::format("Trainer: landing on the surface at {:.1f}, {:.1f}, {:.1f}.", to[0], to[1], to[2])
                                     : std::format("Trainer: no collision at {:.1f}, {:.1f} yet; going to height {:.1f} and waiting for the ground.", x, z, to[1]));
    const auto stem = answer.substr(0, answer.size() - 1);
    return ground ? stem + std::format(" (ground at {:.1f}).", *ground)
                  : stem + " (the ground there is still loading; you'll be put on it when it arrives).";
}

void update_landing(std::uint64_t now) {
    auto &s = state();
    auto &w = s.landing;
    if (!w.active || now < w.next || !s.telemetry.skater) return;
    const auto &p = s.telemetry.position;
    landing::Sample sample{.now = now, .position = p, .vertical = s.telemetry.vertical, .ground = std::nullopt, .may_resend = true};
    const float dx = p[0] - w.x, dz = p[2] - w.z;
    if (dx * dx + dz * dz <= landing::arrived_radius * landing::arrived_radius) sample.ground = topmost_ground(p[0], p[2], p[1]);
    switch (landing::step(w, sample)) {
    case landing::Step::resend: {
        const auto ground = topmost_ground(w.x, w.z, w.y);
        (void)go_to({w.x, ground ? *ground + landing::stand_height : w.y, w.z}, "the same spot");
        say(logging::Level::info, "Trainer: the teleport hadn't arrived; sent it again.");
        break;
    }
    case landing::Step::put_back:
        (void)go_to({p[0], *sample.ground + landing::stand_height, p[2]}, "the surface");
        say(logging::Level::info, std::format("Trainer: sank to {:.1f} under the surface at {:.1f}; put back on it.", p[1], *sample.ground));
        break;
    case landing::Step::landed:
        say(logging::Level::info, std::format("Trainer: landed at {:.1f}, {:.1f}, {:.1f}.", p[0], p[1], p[2]));
        break;
    case landing::Step::ended:
        say(logging::Level::info, std::format("Trainer: landing watch ended at {:.1f}, {:.1f}, {:.1f} after {} correction(s).", p[0], p[1], p[2], w.fixes));
        break;
    case landing::Step::wait: break;
    }
}

void refresh_waypoint() {
    auto &s = state();
    const auto read = read_map_waypoint(s.base);
    s.waypoint_detail = read.detail;
    if (const auto next = landing::remembered(s.waypoint, read.reading); next != s.waypoint) {
        s.waypoint = next;
        s.view_due = true;
    }
}

// ---- telemetry -------------------------------------------------------------------------
std::int64_t clock_now() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
double clock_seconds(std::int64_t ticks) {
    static const double frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return static_cast<double>(ticks) / frequency;
}
float horizontal(const Vec3 &v) { return std::sqrt(v[0] * v[0] + v[2] * v[2]); }
void finish_jump(const Vec3 &landing, float landing_speed, std::int64_t landed_at) {
    auto &s = state();
    auto &m = s.motion;
    const auto air = static_cast<float>(clock_seconds(landed_at - m.takeoff_stamp));
    m.airborne = false;
    s.telemetry.airborne = false;
    if (air < 0.25f) return;
    Jump jump;
    jump.serial = s.telemetry.last.serial + 1;
    jump.air_time = air;
    jump.takeoff = m.takeoff;
    jump.landing = landing;
    jump.takeoff_speed = std::sqrt(m.takeoff_velocity[0] * m.takeoff_velocity[0] + m.takeoff_velocity[1] * m.takeoff_velocity[1] +
                                   m.takeoff_velocity[2] * m.takeoff_velocity[2]);
    jump.takeoff_angle = std::atan2(m.takeoff_velocity[1], std::max(horizontal(m.takeoff_velocity), 0.01f)) * 180.0f / std::numbers::pi_v<float>;
    jump.height = m.apex - m.takeoff[1];
    jump.distance = horizontal({landing[0] - m.takeoff[0], 0, landing[2] - m.takeoff[2]});
    jump.drop = m.takeoff[1] - landing[1];
    jump.landing_speed = landing_speed;
    jump.spin = std::abs(m.spin);
    jump.spin_rate = m.spin_rate;
    jump.flip = m.flip;
    jump.board_turn = m.board_turn;
    jump.state = m.air_state;
    s.telemetry.last = jump;
    if (jump.distance > s.telemetry.best.distance) s.telemetry.best = jump;
    // In the log for a player who is measuring: the read-out is on, or telemetry is recorded.
    if (!s.hud_jump && !s.logging) return;
    say(logging::Level::info,
        std::format("Trainer jump {}: takeoff {:.1f} km/h at {:.1f} deg, air {:.2f} s, height {:.2f} m, distance {:.2f} m, drop {:.2f} m, "
                    "landing {:.1f} km/h at ({:.1f}, {:.1f}, {:.1f}); spin {:.0f} deg (peak {:.0f} deg/s), flip {:.0f} deg, "
                    "board {:.0f} deg/s, states {}>{}.",
                    jump.serial, jump.takeoff_speed * 3.6f, jump.takeoff_angle, jump.air_time, jump.height, jump.distance, jump.drop,
                    jump.landing_speed * 3.6f, landing[0], landing[1], landing[2], jump.spin, jump.spin_rate, jump.flip, jump.board_turn, m.ground_state,
                    m.air_state));
}
// Board flip tricks turn at the game's speed times the player's. The trick scripts set
// Float.Anim.TrickFlipSpeed when the board is flicked; while the skater is in the air the
// trainer keeps it at that number times the flip speed (and, with "Advanced trick speed", times
// the speed of the trick that is turning). On the ground the game's own number is put back.
// <dev>
// `trainer bodyflip`: the off-board flip's two states, what they have been since last asked, and a hold for tests.
struct DevBodyFlip {
    bool hold_speed{}, hold_rate{}, found{};
    float speed{}, rate{};
    float low_speed{1e9f}, high_speed{-1e9f}, low_rate{1e9f}, high_rate{-1e9f};
    BodyFlipState last{};
    float logged_speed{}, logged_rate{};
    std::uint64_t logged_at{};
} dev_body_flip;
bool dev_capture = true; // `option capture 0|1`: log launches and off-board flip states while someone plays
void drive_dev_body_flip(std::uintptr_t base) {
    auto &b = dev_body_flip;
    const auto now = read_body_flip_state(base, static_cast<std::uintptr_t>(state().entity));
    b.last = now;
    if (dev_capture) {
        state().flip_trace = true; // each flight's turn and height lines
        dingosdk::ControllerInput pad;
        DingoSDKOverlayReadControllerInput(&pad);
        const auto buttons = pad.available ? pad.buttons : 0u;
        const auto tick = GetTickCount64();
        if ((now.speed_at || now.rate_at) && (std::abs(now.speed - b.logged_speed) > 0.001f || std::abs(now.rate - b.logged_rate) > 0.001f) &&
            tick - b.logged_at > 50) {
            say(logging::Level::info, std::format("Trainer capture: body flip speed {:.4g} rate {:.4g} (were {:.4g}, {:.4g}), buttons {:x}, physics state {}.",
                                                  now.speed, now.rate, b.logged_speed, b.logged_rate, buttons, watched_physics_state().state));
            b.logged_speed = now.speed;
            b.logged_rate = now.rate;
            b.logged_at = tick;
        }
        TrajectoryCall call;
        for (int i = 0; i < 32 && take_trajectory_call(call); ++i) {
            if (call.caller == 0x482f64b && call.up <= 0.05f) continue; // the falling updates
            say(logging::Level::info, std::format("Trainer capture: launch from {:x}, state {} after {}, up {:.2f} m/s, buttons {:x}, trick then {} (live {}).", call.caller,
                                                  call.state, call.previous, call.up, buttons, call.trick, call.live));
        }
    }
    if (now.speed_at) {
        b.low_speed = std::min(b.low_speed, now.speed);
        b.high_speed = std::max(b.high_speed, now.speed);
        if (b.hold_speed) (void)write_state_number(now.speed_at, b.speed);
    }
    if (now.rate_at) {
        b.low_rate = std::min(b.low_rate, now.rate);
        b.high_rate = std::max(b.high_rate, now.rate);
        if (b.hold_rate) (void)write_state_number(now.rate_at, b.rate);
    }
}
// </dev>
// The left stick's sideways position on the controller, -1 (left) to 1 (right), 0 inside a small dead zone: Skate 3's
// grind pop reads it raw (its PhysGrindStabilityNudge is left stick X). The game state of the same name in skate.
// (Float.Intent.GrindStabilityNudge) read 0 at every pop in the playtest of 2026-10-11, so the controller is asked.
bool left_stick_raw(float &x, float &y) noexcept;
// And how far the left stick turns the board at its end. Skate 3's own code reads as about 24 degrees; the owner
// remembers turning all the way to the next grind, so the preset starts at a quarter turn.
constexpr float skate3_grind_range = 90.0f;
float left_stick_x() noexcept {
    float x{}, y{};
    if (!left_stick_raw(x, y)) return 0;
    constexpr float dead = 0.2f;
    return std::abs(x) <= dead ? 0.0f : (x - std::copysign(dead, x)) / (1.0f - dead);
}
// The left stick as the controller reports it, each axis -1 to 1 (up and right are positive). False without a controller.
bool pad_sticks(float &x, float &y, float &right_x, float &right_y) noexcept {
    using GetState = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
    static const GetState get_state = []() -> GetState {
        for (const auto name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            const auto module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module) continue;
            if (const auto entry = GetProcAddress(module, "XInputGetState")) {
                GetState function{};
                std::memcpy(&function, &entry, sizeof(function));
                return function;
            }
            FreeLibrary(module);
        }
        return nullptr;
    }();
    x = y = right_x = right_y = 0;
    if (!get_state) return false;
    bool any = false;
    float most = -1;
    for (DWORD user = 0; user < 4; ++user) {
        XINPUT_STATE pad{};
        if (get_state(user, &pad) != ERROR_SUCCESS) continue;
        any = true;
        const float px = std::clamp(static_cast<float>(pad.Gamepad.sThumbLX) / 32767.0f, -1.0f, 1.0f);
        const float py = std::clamp(static_cast<float>(pad.Gamepad.sThumbLY) / 32767.0f, -1.0f, 1.0f);
        if (const float size = px * px + py * py; size > most) {
            most = size;
            x = px;
            y = py;
            right_x = std::clamp(static_cast<float>(pad.Gamepad.sThumbRX) / 32767.0f, -1.0f, 1.0f);
            right_y = std::clamp(static_cast<float>(pad.Gamepad.sThumbRY) / 32767.0f, -1.0f, 1.0f);
        }
    }
    // While the trainer leans the skater into a ramp the game's polls see a made-up left stick, and so would this
    // read: the player's own stick is what the last poll had before that.
    if (float own[2]{}; DingoSDKOverlayGrindStickLeanRaw(own)) {
        x = own[0];
        y = own[1];
    }
    return any;
}
bool left_stick_raw(float &x, float &y) noexcept {
    float right_x{}, right_y{};
    return pad_sticks(x, y, right_x, right_y);
}
// Test value (`trainer flipland <seconds>`): this long before the landing a slowed flip goes back to the game's own speed.
float flip_land_window = 0;
// Test value (`trainer flipturn <seconds>`): the turn time below, for every trick.
float flip_turn_test = 0;
// With "Flick tricks at full speed" the game's own flip speed counts as at least this (`trainer flipleast <n>` to
// test). The game sets that speed from how fast the stick was flicked, and every trick's clip turns for the same 20
// ticks, so a trick with a slow flick (the front foot impossible, the nollie impossibles, the 360 pop shuvits: 11 of 11
// still turning at the landing in the playtest of 2026-10-11, at under half a clean impossible's turn rate) needs more
// air than "Slow flips pop higher" allows for. Held up to this, they turn in the time that pop gives every other trick.
float flip_least_game = 1.0f;
// The least upward speed a pop into `trick`, slowed to `speed`, needs to land on time (0: the game's own will do).
// Every flip trick turns for about 0.31 s at the game's speed and 0.31 s / speed slowed (playtest of 2026-10-10: 14
// tricks, speeds 0.1 to 0.5), and scripted kickflips landed clean with 0.2 s more air than that. Shuvits and impossibles
// were still turning when they landed in that playtest (scripted pop shuvits were not), so they get a little more. The air a launch gives on flat ground is
// 0.04 s more than twice its upward speed over 9.8.
float slow_flip_up(std::uint32_t trick, float speed) {
    if (!(speed < 1.0f)) return 0;
    const auto name = flip_trick_name(trick);
    float turn = name.find("Shuvit") != name.npos || name.find("Impossible") != name.npos ? 0.38f : 0.34f;
    if (flip_turn_test > 0) turn = flip_turn_test;
    const float air = 0.20f + turn / std::max(speed, 0.05f);
    return std::min(4.9f * (air - 0.04f), 16.5f);
}
// CurrentFlipTrick values that are a pop with no flip in it (the nollie, the ollie pops, the boned ollie, the wallies):
// the flip speed leaves them alone, as it does the ollie.
constexpr bool plain_pop(std::uint32_t trick) { return trick <= 1 || trick == 16 || trick == 31 || trick == 32 || (trick >= 33 && trick <= 35) || trick == 42; }
// The ollies among those (ollie, nollie, their pops, the boned ollie). With "Slow flips pop higher" they are slowed and
// popped like a flip trick (the owner's ask of 2026-10-11): the pop gives them the air a slowed clip hangs for.
constexpr bool ollie_pop(std::uint32_t trick) { return trick == 1 || trick == 16 || trick == 31 || trick == 32 || trick == 42; }
void drive_flip_speed(std::uintptr_t base, bool airborne, float to_landing) {
    auto &s = state();
    auto &drive = s.flip_drive;
    const auto now = read_flip_state(base, static_cast<std::uintptr_t>(s.entity));
    s.flip_live = static_cast<bool>(now);
    set_launch_trick(now ? now.trick : 0xffu, now ? now.trick_at : 0); // for the launch hook: the trick the game has in mind this tick
    // A plain ollie (trick 1) plays the same clip and reads the same number: slowed, it hangs in
    // the air (measured: 0.60 s of air at 1, 0.65 s at 0.5, 0.77 to 0.88 s at 0.25). Only a flip
    // trick is driven, and the ollies where "Slow flips pop higher" gives them that air.
    const bool flipping = now && (!plain_pop(now.trick) || (ollie_pop(now.trick) && s.flip_pop && !extras_blocked()));
    if (!now || !airborne || !flipping) {
        if (now && drive.active && now.speed == drive.written && drive.written != drive.game) {
            (void)write_flip_speed(now, drive.game);
            if (now.clamped_at) (void)write_state_number(now.clamped_at, drive.clamped);
        }
        drive = {};
        return;
    }
    if (now.trick) s.flip_trick_seen = now.trick;
    float factor = s.boosts.flip;
    if (s.flip_advanced && !extras_blocked())
        if (const auto index = flip_trick_index(now.trick); index >= 0) factor *= s.flip_trick[static_cast<std::size_t>(index)];
    // "Slow flips pop higher" for a trick the game chose after the pop (seen out of manuals and grinds).
    if (s.flip_pop && !extras_blocked() && (!drive.active || drive.trick != now.trick)) {
        float was{}, time{};
        if (const float up = slow_flip_up(now.trick, factor); up > 0 && raise_flight(up, was, time))
            say(logging::Level::info, std::format("Trainer: slow flip chosen {:.3f} s after the pop: up speed {:.2f} -> {:.2f} m/s.", time, was, up));
        // <dev>
        else if (dev_capture && up > was + 0.05f) say(logging::Level::info, "Trainer capture: no raise; " + flight_dump());
        // </dev>
    }
    drive.trick = now.trick;
    // <dev>
    if (const int raised = take_flight_raise()) say(logging::Level::info, raised > 0 ? "Trainer capture: launched again." : "Trainer capture: launching again failed.");
    // </dev>
    // A slowed clip also plays its landing slowly, so the skater lands late and sinks (measured: 0.07 s late at x 0.5,
    // 0.25 to 0.57 s at x 0.25). Close to the ground the clip goes back to the game's own speed.
    if (factor < 1 && flip_land_window > 0 && to_landing < flip_land_window) factor = 1;
    // A number that is not the trainer's own is the game's: the flight's first, or a new trick's.
    if (!drive.active || now.speed != drive.written) drive.game = now.speed;
    if (!drive.active) drive.clamped = now.clamped;
    drive.active = true;
    drive.factor = factor;
    const bool held_up = s.flip_flick && !extras_blocked();
    s.flip_game_seen = drive.game;
    s.flip_clamped_seen = now.clamped_at ? now.clamped : -1.0f;
    drive.written = std::clamp((held_up ? std::max(drive.game, flip_least_game) : drive.game) * factor, 0.0f, 1000.0f);
    if (now.speed != drive.written || (now.clamped_at && now.clamped != drive.written)) (void)write_flip_speed(now, drive.written);
}
void observe(std::uintptr_t base, std::uintptr_t client, std::uint64_t now) {
    auto &s = state();
    auto &t = s.telemetry;
    auto &m = s.motion;
    overlay::DebugModel skater;
    std::array<float, 16> matrix{};
    bool found = false;
    if (now >= s.next_skater) {
        try {
            matrix = client_source::detail::debug_skater(base, client, skater);
            found = skater.skater_position_valid;
        } catch (...) {
            s.next_skater = now + 250; // no skater: menus, loading, a respawn
        }
    }
    if (!found) {
        if (t.skater) { t.skater = false; m.valid = false; s.entity = 0; }
        return;
    }
    s.entity = skater.skater_identity;
    watch_physics_state(client, s.entity);
    const auto watch = watched_physics_state();
    const auto stamp = clock_now();
    const Vec3 position = skater.skater_position;
    t.skater = true;
    t.position = position;
    // Rows: right, up, forward, position.
    t.heading = std::atan2(matrix[8], matrix[10]) * 180.0f / std::numbers::pi_v<float>;
    if (t.heading < 0) t.heading += 360.0f;
    t.physics_state = watch.valid ? watch.state : 0;
    if (watch.valid) ++s.states[watch.state];
    // A hippy jump leaves the board (504) straight from riding (100): measured. A plain
    // dismount does the same but is not rising, so it is left alone.
    if (watch.valid && watch.state != s.last_state) {
        // <dev>
        if (s.state_log)
            say(logging::Level::info, std::format("Trainer state: {} -> {} after {:.3f} s; y {:.3f}, up {:.2f} m/s, {:.1f} km/h.", s.last_state,
                                                  watch.state, clock_seconds(stamp - s.state_since), position[1], s.telemetry.vertical,
                                                  s.telemetry.speed * 3.6f));
        s.state_since = stamp;
        // </dev>
        // Off a grind or a slide the board is in a 400s state (401 and 403 seen in a playtest) and the hippy leaves it the same
        // way; that case is built from the riding one and not measured yet.
        const bool from_board = s.last_state == 100 || (s.last_state >= 400 && s.last_state < 500);
        // <dev>
        if (dev_capture && (watch.state >= 400 || s.last_state >= 400))
            say(logging::Level::info, std::format("Trainer capture: state {} -> {}, up {:.2f} m/s, {:.1f} km/h.", s.last_state, watch.state,
                                                  s.telemetry.vertical, s.telemetry.speed * 3.6f));
        // </dev>
        if (watch.state == addr::no_bail::offboard_physics_state && from_board && s.boosts.hippy != 1.0f) {
            s.boost_factor = std::sqrt(s.boosts.hippy); // height goes with the square of the speed
            s.boost_until = now + 250;
            s.boost_name = "hippy jump";
        }
        s.last_state = watch.state;
    }
    if (s.boost_factor > 0) {
        const auto result = take_jump_scale_result();
        if (result.outcome > 0) {
            say(logging::Level::info, std::format("Trainer: {} boosted: up speed {:.2f} -> {:.2f} m/s.", s.boost_name, result.up_speed,
                                                  result.up_speed * s.boost_factor));
            s.boost_factor = 0;
        } else if (now >= s.boost_until || result.outcome == -2) {
            s.boost_factor = 0;
        } else {
            // The factor comes from the multipliers in force, so a session's rules are already in it.
            (void)queue_jump_scale(client, s.entity, s.boost_factor); // again each tick until the skater is rising
        }
    }
    const auto seconds = m.valid ? clock_seconds(stamp - m.stamp) : 0.0;
    const Vec3 step{position[0] - m.position[0], position[1] - m.position[1], position[2] - m.position[2]};
    const bool jumped = std::sqrt(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]) > 25.0f;
    if (!m.valid || seconds <= 0.0005 || seconds > 0.25 || jumped) {
        // First sample, a hitch, or a teleport: nothing can be measured across it.
        m = {};
        m.valid = true;
        m.position = position;
        m.stamp = stamp;
        t.airborne = false;
        t.air_time = t.height = 0;
        return;
    }
    const auto dt = static_cast<float>(seconds);
    const Vec3 raw{step[0] / dt, step[1] / dt, step[2] / dt};
    const Vec3 velocity{m.velocity[0] * 0.5f + raw[0] * 0.5f, m.velocity[1] * 0.5f + raw[1] * 0.5f, m.velocity[2] * 0.5f + raw[2] * 0.5f};
    // An on-foot jump: off the board, level at the last tick and now rising faster than stairs
    // or a slope can lift a runner.
    if (watch.valid && watch.state == addr::no_bail::offboard_physics_state) {
        if (s.offboard_grounded && raw[1] > 2.2f && s.boost_factor == 0 && s.boosts.offboard != 1.0f) {
            s.boost_factor = std::sqrt(s.boosts.offboard);
            s.boost_until = now + 250;
            s.boost_name = "off-board jump";
        }
        s.offboard_grounded = std::abs(raw[1]) < 0.6f;
    } else {
        s.offboard_grounded = false;
    }
    // The skater's physics says when it is in the air: its states 200-299 are the pop and
    // the flight (100s ride the ground, 300 is a wipeout, 500s are off the board).
    const bool flying = watch.valid && watch.state >= air_states_first && watch.state < air_states_end;
    // Rotation in the air: about the vertical (spins) and of the up axis (flips and rolls).
    const Vec3 forward{matrix[8], matrix[9], matrix[10]}, up{matrix[4], matrix[5], matrix[6]};
    if (m.airborne && m.axes_valid) {
        const float yaw = std::atan2(m.forward[2] * forward[0] - m.forward[0] * forward[2], m.forward[0] * forward[0] + m.forward[2] * forward[2]);
        const float degrees = yaw * 180.0f / std::numbers::pi_v<float>;
        // A ragdoll or a pose snap turns further in one tick than any trick does: leave those out.
        if (std::abs(degrees) < 45.0f) {
            m.spin += degrees;
            m.spin_rate = std::max(m.spin_rate, std::abs(degrees) / dt);
        }
        const float cosine = std::clamp(m.up[0] * up[0] + m.up[1] * up[1] + m.up[2] * up[2], -1.0f, 1.0f);
        if (const float tumble = std::acos(cosine) * 180.0f / std::numbers::pi_v<float>; tumble < 45.0f) m.flip += tumble;
    }
    m.forward = forward;
    m.up = up;
    m.axes_valid = true;
    if (!flying && watch.valid) m.ground_state = watch.state;
    if (flying && !m.airborne) {
        m.spin = m.spin_rate = m.flip = m.board_turn = 0;
        m.air_state = watch.state;
        m.airborne = true;
        m.takeoff = m.position;
        m.takeoff_velocity = velocity;
        m.takeoff_stamp = m.stamp;
        m.apex = std::max(m.position[1], position[1]);
    }
    {
        // Seconds until the skater is back at the height they left from, by the flight's own arc.
        float to_landing = 1e9f;
        if (m.airborne) {
            const float rise = velocity[1], above = position[1] - m.takeoff[1], under = rise * rise + 2 * 9.8f * above;
            to_landing = under > 0 ? (rise + std::sqrt(under)) / 9.8f : 0.0f;
        }
        drive_flip_speed(base, m.airborne, to_landing);
    }
// <dev>
    drive_dev_body_flip(base);
// </dev>
    if (m.airborne) {
        m.apex = std::max(m.apex, position[1]);
        t.air_time = static_cast<float>(clock_seconds(stamp - m.takeoff_stamp));
        t.height = m.apex - m.takeoff[1];
        if (!flying)
            finish_jump(position, std::sqrt(m.velocity[0] * m.velocity[0] + m.velocity[1] * m.velocity[1] + m.velocity[2] * m.velocity[2]),
                        stamp);
    }
    t.airborne = m.airborne;
    t.speed = horizontal(velocity);
    t.vertical = velocity[1];
    t.top_speed = std::max(t.top_speed, t.speed);
    // <dev>
    if (auto &own = state(); own.pump_log) {
        const float whole = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
        // One line a swing: the speed at the bottom, told once the skater has climbed 0.4 m out of it.
        constexpr float climb = 0.4f;
        if (whole < 1000.0f) {
            const float height = position[1];
            if (!own.pump_started) {
                own.pump_started = true;
                own.pump_low = own.pump_high = own.pump_last_top = height;
                own.pump_low_speed = whole;
            } else if (!own.pump_rising) {
                if (height < own.pump_low) {
                    own.pump_low = height;
                    own.pump_low_speed = whole;
                } else if (height > own.pump_low + climb) {
                    say(logging::Level::info, std::format("Trainer pump #{}: bottom {:.2f} m/s at height {:.2f} m, dropped {:.2f} m into it.", ++own.pump_serial,
                                                          own.pump_low_speed, own.pump_low, own.pump_last_top - own.pump_low));
                    own.pump_rising = true;
                    own.pump_high = height;
                }
            } else if (height > own.pump_high) {
                own.pump_high = height;
            } else if (height < own.pump_high - climb) {
                own.pump_last_top = own.pump_high;
                own.pump_rising = false;
                own.pump_low = height;
                own.pump_low_speed = whole;
            }
        }
    }
    // </dev>
    m.velocity = velocity;
    m.position = position;
    m.stamp = stamp;
    // How fast the board turns (flip tricks, shuvits): body 0's angular velocity (+0x90), read while a jump
    // is measured or telemetry is recorded.
    float board_turn = 0;
    bool turning = m.airborne || (s.logging && s.log);
    // <dev>
    turning = turning || s.state_log || s.flip_trace;
    if (m.airborne)
        for (const auto address : dev_hold_at) __try_poke(address, dev_hold_value);
    if (auto &foot = dev_foot; foot.on) {
        float rate = 0;
        std::size_t fastest = 0;
        if (watch.valid && watch.state >= 500 && watch.state < 600) {
            try {
                const auto bodies = client_source::detail::debug_noclip_bodies(base, client, s.entity);
                for (std::size_t i = 0; i < std::min<std::size_t>(bodies.parts.size(), 9); ++i) {
                    std::array<float, 3> spin{};
                    if (!memory::peek(bodies.parts[i] + 0x90, spin)) continue;
                    const float size = std::sqrt(spin[0] * spin[0] + spin[1] * spin[1] + spin[2] * spin[2]) * 180.0f / std::numbers::pi_v<float>;
                    if (std::isfinite(size) && size < 1.0e5f && size > rate) { rate = size; fastest = i; }
                }
            } catch (...) {}
        }
        if (rate > 120.0f) {
            if (!foot.turning) {
                foot = {.on = true, .turning = true, .serial = foot.serial + 1, .last = stamp, .since = stamp, .up_at_start = s.telemetry.vertical, .height_at_start = position[1]};
            }
            foot.quiet = stamp;
            foot.turned += rate * static_cast<float>(dt);
            if (rate > foot.peak) { foot.peak = rate; foot.part = fastest; }
        } else if (foot.turning && clock_seconds(stamp - foot.quiet) > 0.3) {
            say(logging::Level::info, std::format("Trainer foot #{}: turned {:.0f} deg in {:.2f} s, peak {:.0f} deg/s (body {}), started rising {:.1f} m/s at y {:.1f}.", foot.serial,
                                                  foot.turned, clock_seconds(foot.quiet - foot.since), foot.peak, foot.part, foot.up_at_start, foot.height_at_start));
            foot.turning = false;
        }
    }
    if (!m.airborne) {
        if (dev_airdump_tick) dev_airdump_ticks.clear();
        dev_airdump_tick = 0;
    } else if (!dev_airdump_ticks.empty() && std::ranges::find(dev_airdump_ticks, ++dev_airdump_tick) != dev_airdump_ticks.end()) {
        const auto buffers = state_pages(base, static_cast<std::uintptr_t>(s.entity));
        for (std::size_t k = 0; k < buffers.size(); ++k) {
            std::vector<char> bytes(0x1800);
            SIZE_T got{};
            if (!buffers[k] || !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(buffers[k]), bytes.data(), bytes.size(), &got)) continue;
            std::ofstream(data_directory() / std::format("{}_t{}_{}.bin", dev_airdump_name, dev_airdump_tick, k), std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(got));
        }
    }
    if (!m.airborne) dev_narrow.air_ticks = 0;
    else if (dev_narrow.pending && ++dev_narrow.air_ticks == dev_narrow.tick) {
        dev_narrow.last = narrow_run(dev_narrow);
        say(logging::Level::info, "Trainer narrow: " + dev_narrow.last.substr(0, 400));
    }
    // </dev>
    if (turning) {
        try {
            const auto bodies = client_source::detail::debug_noclip_bodies(base, client, s.entity);
            std::array<float, 3> spin{};
            if (!bodies.parts.empty() && memory::peek(bodies.parts[0] + 0x90, spin)) {
                board_turn = std::sqrt(spin[0] * spin[0] + spin[1] * spin[1] + spin[2] * spin[2]) * 180.0f / std::numbers::pi_v<float>;
                if (!std::isfinite(board_turn) || board_turn > 1.0e5f) board_turn = 0;
            }
        } catch (...) {}
        if (m.airborne) m.board_turn = std::max(m.board_turn, board_turn);
        // <dev>
        // The flip trace: how the board turned through one flight, summed up when it lands.
        if (s.flip_trace && m.airborne) {
            try {
                const auto bodies = client_source::detail::debug_noclip_bodies(base, client, s.entity);
                std::array<float, 3> spin{};
                if (!bodies.parts.empty() && memory::peek(bodies.parts[0] + 0x90, spin)) {
                    if (s.flip_samples.empty()) s.flip_takeoff_up = m.takeoff_velocity[1];
                    State::FlipSample sample;
                    sample.time = t.air_time;
                    for (std::size_t k = 0; k < 3; ++k) sample.turn[k] = spin[k] * 180.0f / std::numbers::pi_v<float>;
                    sample.up_speed = velocity[1];
                    sample.height = position[1] - m.takeoff[1];
                    sample.state = watch.valid ? static_cast<std::uint32_t>(watch.state) : 0;
                    if (s.flip_samples.size() < 4000) s.flip_samples.push_back(sample);
                }
            } catch (...) {}
        } else if (!m.airborne && !s.flip_samples.empty()) {
            const auto rate = [](const State::FlipSample &x) { return std::sqrt(x.turn[0] * x.turn[0] + x.turn[1] * x.turn[1] + x.turn[2] * x.turn[2]); };
            const auto &f = s.flip_samples;
            std::size_t first = f.size(), last = 0;
            float peak = 0;
            for (std::size_t i = 0; i < f.size(); ++i) {
                peak = std::max(peak, rate(f[i]));
                if (rate(f[i]) > 200.0f) {
                    if (first == f.size()) first = i;
                    last = i;
                }
            }
            ++s.flip_serial;
            const float air = f.back().time;
            if (first < f.size() && last > first) {
                float turned = 0;
                for (std::size_t i = first + 1; i <= last; ++i) turned += 0.5f * (rate(f[i]) + rate(f[i - 1])) * (f[i].time - f[i - 1].time);
                const float took = f[last].time - f[first].time;
                // The same turn split by axis: about the line of travel (a flip), about the world's
                // up (a shuv) and about the sideways axis (the ollie's own pitch).
                {
                    const float vx = m.takeoff_velocity[0], vz = m.takeoff_velocity[2], along = std::sqrt(vx * vx + vz * vz);
                    const float fx = along > 0.3f ? vx / along : 1.0f, fz = along > 0.3f ? vz / along : 0.0f;
                    float degrees[3]{}, peaks[3]{}, ends[3]{};
                    for (std::size_t i = 1; i < f.size(); ++i) {
                        const float parts[3]{f[i].turn[0] * fx + f[i].turn[2] * fz, f[i].turn[1], f[i].turn[0] * fz - f[i].turn[2] * fx};
                        for (std::size_t k = 0; k < 3; ++k) {
                            degrees[k] += std::abs(parts[k]) * (f[i].time - f[i - 1].time);
                            peaks[k] = std::max(peaks[k], std::abs(parts[k]));
                            if (std::abs(parts[k]) > 150.0f) ends[k] = f[i].time;
                        }
                    }
                    say(logging::Level::info,
                        std::format("Trainer flipaxes #{}: flip {:.0f} deg (peak {:.0f}, ends {:.0f}% of air), shuv {:.0f} deg (peak {:.0f}, ends {:.0f}%), "
                                    "pitch {:.0f} deg (peak {:.0f}); all turning ends at {:.0f}% of air.",
                                    s.flip_serial, degrees[0], peaks[0], air > 0 ? 100 * ends[0] / air : 0.0f, degrees[1], peaks[1],
                                    air > 0 ? 100 * ends[1] / air : 0.0f, degrees[2], peaks[2], air > 0 ? 100 * f[last].time / air : 0.0f));
                }
                say(logging::Level::info,
                    std::format("Trainer flip #{}: air {:.3f} s, up {:.2f} m/s, turning from {:.3f} to {:.3f} s ({:.3f} s), turned {:.0f} deg, mean {:.0f} peak {:.0f} "
                                "deg/s, air left at start {:.3f} s, {} samples, trick {} ({}).",
                                s.flip_serial, air, s.flip_takeoff_up, f[first].time, f[last].time, took, turned, took > 0 ? turned / took : 0.0f, peak,
                                air - f[first].time, f.size(), s.flip_trick_seen, flip_trick_name(s.flip_trick_seen)));
            } else {
                say(logging::Level::info, std::format("Trainer flip #{}: air {:.3f} s, up {:.2f} m/s, no board turn (peak {:.0f} deg/s).", s.flip_serial, air,
                                                      s.flip_takeoff_up, peak));
            }
            {
                // How the flight went up and down: a slowed flip lands late and the skater sinks below where a stock one lands.
                float apex = -1e9f, apex_at = 0, lowest = 1e9f, lowest_at = 0;
                for (const auto &x : f) {
                    if (x.height > apex) { apex = x.height; apex_at = x.time; }
                    if (x.time > apex_at && x.height < lowest) { lowest = x.height; lowest_at = x.time; }
                }
                say(logging::Level::info, std::format("Trainer flipheight #{}: air {:.3f} s, up {:.2f} m/s, apex {:.2f} m at {:.2f} s, lowest {:.2f} m at {:.2f} s, "
                                                      "flip speed {:.2f}, trick {}, game's speed {:.3f}, clamped {:.3f}.", s.flip_serial, air, s.flip_takeoff_up, apex, apex_at, lowest, lowest_at,
                                                      s.boosts.flip, s.flip_trick_seen, s.flip_game_seen, s.flip_clamped_seen));
            }
            try {
                std::ofstream out(data_directory() / "flips.csv", std::ios::app);
                for (const auto &x : f)
                    out << std::format("{},{:.4f},{:.1f},{:.1f},{:.1f},{:.3f},{:.3f},{}", s.flip_serial, x.time, x.turn[0], x.turn[1], x.turn[2], x.up_speed,
                                       x.height, x.state)
                        << '\n';
            } catch (...) {}
            s.flip_samples.clear();
        }
        if (m.airborne && s.state_log) {
            try {
                const auto bodies = client_source::detail::debug_noclip_bodies(base, client, s.entity);
                for (std::size_t i = 0; i < std::min<std::size_t>(bodies.parts.size(), 9); ++i)
                    for (std::size_t k = 0; k < 4; ++k) {
                        std::array<float, 3> v{};
                        if (memory::peek(bodies.parts[i] + 0x80 + k * 0x10, v)) {
                            const float size = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                            if (std::isfinite(size)) s.body_peaks[i][k] = std::max(s.body_peaks[i][k], size);
                        }
                    }
            } catch (...) {}
        } else if (!m.airborne && s.state_log && s.body_peaks[0][0] + s.body_peaks[0][1] + s.body_peaks[1][0] > 0) {
            std::string line = "Trainer bodies (peak size of the vectors at +80 +90 +a0 +b0):";
            for (std::size_t i = 0; i < 9; ++i)
                line += std::format(" {}:[{:.1f} {:.1f} {:.1f} {:.1f}]", i, s.body_peaks[i][0], s.body_peaks[i][1], s.body_peaks[i][2], s.body_peaks[i][3]);
            say(logging::Level::info, line);
            s.body_peaks = {};
        }
        // </dev>
    }
    if (s.logging && s.log) {
        s.log << std::format("{:.4f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.1f},{},{},{:.0f}\n", clock_seconds(stamp - s.log_start), position[0],
                             position[1], position[2], t.speed, t.vertical, t.heading, t.airborne ? 1 : 0, t.physics_state, board_turn);
    }
    // A bail: back to the selected marker once the skater has had time to fall.
    if (watch.valid && (!s.wipeouts_known || watch.wipeouts != s.seen_wipeouts)) {
        if (s.wipeouts_known && s.auto_return) {
            const bool marked = current_map().markers[static_cast<std::size_t>(s.slot)].set;
            if (marked) s.return_at = now + static_cast<std::uint64_t>(s.return_delay * 1000.0f);
            say(logging::Level::info, marked ? std::format("Trainer: bail seen; back to marker {} in {:.1f} s.", s.slot + 1, s.return_delay)
                                             : std::format("Trainer: bail seen, but marker {} is empty.", s.slot + 1));
        }
        s.wipeouts_known = true;
        s.seen_wipeouts = watch.wipeouts;
    }
}
void set_logging(bool on) {
    auto &s = state();
    if (s.log.is_open()) s.log.close();
    s.logging = false;
    if (!on) return;
    const auto directory = data_directory() / L"telemetry";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::string name = s.map.empty() ? "unknown" : s.map.substr(s.map.find_last_of('/') + 1);
    for (auto &c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
    SYSTEMTIME time;
    GetLocalTime(&time);
    const auto file = directory / std::format("{}-{:04}{:02}{:02}-{:02}{:02}{:02}.csv", name, time.wYear, time.wMonth, time.wDay, time.wHour,
                                              time.wMinute, time.wSecond);
    s.log.open(file, std::ios::binary | std::ios::trunc);
    if (!s.log) return;
    s.log << "seconds,x,y,z,speed_mps,vertical_mps,heading_deg,airborne,physics_state,board_turn_dps\n";
    s.log_start = clock_now();
    s.logging = true;
}
void shortcuts() {
    auto &s = state();
    ControllerInput input;
    DingoSDKOverlayReadControllerInput(&input);
    const auto buttons = input.available ? input.buttons : 0;
    const auto pressed = buttons & ~s.pad_previous;
    s.pad_previous = buttons;
    if (!s.pad_shortcuts || (buttons & (xinput_lb | xinput_rb)) != (xinput_lb | xinput_rb)) return;
    std::string result;
    if (pressed & xinput_up) result = command("marker", {"save"});
    else if (pressed & xinput_down) result = command("marker", {"go"});
    else if (pressed & xinput_left) result = command("slot", {std::to_string((s.slot + static_cast<int>(marker_slots) - 1) % static_cast<int>(marker_slots) + 1)});
    else if (pressed & xinput_right) result = command("slot", {std::to_string((s.slot + 1) % static_cast<int>(marker_slots) + 1)});
    if (!result.empty()) overlay::notify(overlay::NoticeLevel::info, "TRAINER", result);
}

// <dev>

// ---- read sweep: which class values does the game's code read? ---------------------------------
struct Sweep {
    bool running{};
    std::size_t field{}, end{};
    std::vector<std::string> only; // class keys, empty: all
    std::array<FieldCopy, 16> copies{};
    std::size_t copy_count{}, round{}; // copies of the field, and which four are watched now
    bool armed{};
    std::string line;
    std::uint64_t field_reads{};
    std::uint64_t until{};
    std::ofstream file;
    std::size_t read{}, total{};
    std::uintptr_t image_size{};
    int dwell{450};
    // Live: someone is playing. Passes repeat over the values not read yet until `sweep stop`.
    bool live{}, unseen{};
    std::string tag;
    std::size_t passes{};
    std::uintptr_t field_reader{};
    std::vector<std::uint64_t> game, other;
    std::vector<std::uintptr_t> reader;
    std::vector<std::uint8_t> found; // 1: copies were found at least once
    // The game reads one copy of a class (the local skater's): once a read shows which, only
    // that copy is watched, so a pass over what is left takes a second or two and the short
    // bursts of reads a move causes are not missed.
    struct Unit {
        std::size_t field;
        std::uintptr_t address;
        int copy;
    };
    std::vector<Unit> queue;
    std::size_t at{};
    std::vector<int> live_copy; // per class, -1: not known yet
};
Sweep &sweep() {
    static auto *value = new Sweep;
    return *value;
}
std::size_t sweep_class(std::size_t field) {
    std::size_t c = 0;
    while (c + 1 < class_count && field >= static_cast<std::size_t>(class_specs[c].first) + class_specs[c].count) ++c;
    return c;
}
bool sweep_listed(std::size_t field) {
    auto &w = sweep();
    if (w.unseen && class_fields[field].seen) return false;
    return w.only.empty() || std::ranges::find(w.only, std::string(class_specs[sweep_class(field)].key)) != w.only.end();
}
bool sweep_wants(std::size_t field) {
    auto &w = sweep();
    return sweep_listed(field) && !(w.live && w.game[field] + w.other[field]);
}
std::string sweep_reader(std::uintptr_t rip, std::uintptr_t base, std::uintptr_t image_size) {
    if (!rip) return "@-";
    return rip >= base && rip < base + image_size ? std::format("@{:x}", rip - base) : std::format("@script:{:x}", rip);
}
// The live sweep's file, whole, after every pass: value, reads by the game + by other code, first reader.
std::string write_live_sweep() {
    auto &w = sweep();
    auto &s = state();
    std::ofstream file(data_directory() / ("sweep-" + w.tag + ".tsv"), std::ios::binary | std::ios::trunc);
    std::size_t listed{}, read{};
    std::string missing;
    for (std::size_t f = 0; f < class_field_count; ++f) {
        if (!sweep_listed(f)) continue;
        ++listed;
        const auto name = std::format("{}.{}", class_specs[sweep_class(f)].key, class_fields[f].name);
        if (w.game[f] + w.other[f]) ++read;
        else missing += (missing.empty() ? "" : " ") + name;
        if (!w.found[f]) file << name << "\tnot found\n";
        else file << name << "\tc:" << w.game[f] << "+" << w.other[f] << "\t" << sweep_reader(w.reader[f], s.base, w.image_size) << "\n";
    }
    return std::format("{} of {} read after {} passes{}", read, listed, w.passes, missing.empty() ? "." : "; not read: " + missing);
}
void run_live_sweep(std::uint64_t now) {
    auto &w = sweep();
    auto &s = state();
    if (w.armed) { // collect the four just watched
        std::array<std::uint64_t, 4> counts{}, others{};
        std::array<std::uintptr_t, 4> readers{};
        watch_four_counts(counts.data(), others.data());
        watch_four_readers(readers.data());
        for (std::size_t i = 0; i < 4 && w.at + i < w.queue.size(); ++i) {
            const auto &unit = w.queue[w.at + i];
            if (!(counts[i] + others[i])) continue;
            const bool first = !(w.game[unit.field] + w.other[unit.field]);
            w.game[unit.field] += counts[i];
            w.other[unit.field] += others[i];
            if (!first) continue;
            w.reader[unit.field] = readers[i];
            w.live_copy[sweep_class(unit.field)] = unit.copy;
            const auto text = std::format("{}.{} is read ({} reads, copy {}, {})", class_specs[sweep_class(unit.field)].key, class_fields[unit.field].name,
                                          counts[i] + others[i], unit.copy, sweep_reader(readers[i], s.base, w.image_size));
            say(logging::Level::info, "Trainer sweep: " + text);
            overlay::notify(overlay::NoticeLevel::info, "SWEEP", text);
        }
        w.at += 4;
    }
    if (w.at >= w.queue.size()) { // the next pass
        if (w.armed) {
            ++w.passes;
            const auto summary = write_live_sweep();
            if (w.passes % 60 == 1) say(logging::Level::info, "Trainer sweep: " + summary);
        }
        w.queue.clear();
        w.at = 0;
        const bool every_copy = w.passes % 10 == 9; // now and then, in case a second copy is read as well
        std::size_t wanted{};
        for (std::size_t f = 0; f < w.end; ++f) {
            if (!sweep_wants(f)) continue;
            ++wanted;
            const auto count = class_field_copies(f, w.copies.data(), w.copies.size());
            if (count) w.found[f] = 1;
            const auto live = w.live_copy[sweep_class(f)];
            for (std::size_t i = 0; i < count; ++i)
                if (every_copy || live < 0 || live >= static_cast<int>(count) || live == static_cast<int>(i))
                    w.queue.push_back({f, w.copies[i].address, static_cast<int>(i)});
        }
        if (w.queue.empty()) {
            std::array<std::uintptr_t, 4> none{};
            (void)watch_four(none.data(), s.base, w.image_size);
            w.armed = false;
            if (!wanted) {
                w.running = false;
                say(logging::Level::info, "Trainer sweep: finished; " + write_live_sweep());
            }
            w.until = now + 1000; // no copies yet: the classes are still being found
            return;
        }
    }
    std::array<std::uintptr_t, 4> addresses{};
    for (std::size_t i = 0; i < 4 && w.at + i < w.queue.size(); ++i) addresses[i] = w.queue[w.at + i].address;
    (void)watch_four(addresses.data(), s.base, w.image_size);
    w.armed = true;
    w.until = now + static_cast<std::uint64_t>(w.dwell);
}
void run_sweep(std::uint64_t now) {
    auto &w = sweep();
    auto &s = state();
    if (!w.running || now < w.until) return;
    if (w.live) {
        run_live_sweep(now);
        return;
    }
    if (w.armed) { // collect the round just watched
        std::array<std::uint64_t, 4> counts{}, others{};
        std::array<std::uintptr_t, 4> readers{};
        watch_four_counts(counts.data(), others.data());
        watch_four_readers(readers.data());
        for (std::size_t i = 0; i < 4 && w.round * 4 + i < w.copy_count; ++i) {
            w.line += std::format("\t{}:{}", w.copies[w.round * 4 + i].kind, counts[i]);
            if (others[i]) w.line += std::format("+{}", others[i]);
            w.field_reads += counts[i] + others[i];
            if (w.live) {
                w.game[w.field] += counts[i];
                w.other[w.field] += others[i];
                if (!w.field_reader) w.field_reader = readers[i];
            }
        }
        ++w.round;
        if (w.round * 4 >= w.copy_count) { // the field is done
            if (w.live) {
                w.found[w.field] = 1;
                if (w.field_reads) {
                    w.reader[w.field] = w.field_reader;
                    const auto text = std::format("{}.{} is read ({} reads, {})", class_specs[sweep_class(w.field)].key, class_fields[w.field].name,
                                                  w.field_reads, sweep_reader(w.field_reader, s.base, w.image_size));
                    say(logging::Level::info, "Trainer sweep: " + text);
                    overlay::notify(overlay::NoticeLevel::info, "SWEEP", text);
                }
            } else {
                w.file << w.line << "\n";
                w.file.flush();
            }
            ++w.total;
            w.read += w.field_reads != 0;
            ++w.field;
            w.copy_count = 0;
        }
    }
    if (!w.copy_count) { // the next field
        while (w.field < w.end && !sweep_wants(w.field)) ++w.field;
        if (w.live && w.field >= w.end) { // a pass is over: write it down and go round again
            ++w.passes;
            const auto summary = write_live_sweep();
            if (w.passes % 5 == 1) say(logging::Level::info, "Trainer sweep: " + summary);
            w.field = 0;
            while (w.field < w.end && !sweep_wants(w.field)) ++w.field;
            if (w.field >= w.end) say(logging::Level::info, "Trainer sweep: every listed value has been read.");
        }
        if (w.field < w.end) {
            w.copy_count = class_field_copies(w.field, w.copies.data(), w.copies.size());
            w.round = 0;
            w.field_reads = 0;
            w.field_reader = 0;
            w.line = std::format("{}.{}", class_specs[sweep_class(w.field)].key, class_fields[w.field].name);
            if (!w.copy_count) {
                if (!w.live) w.file << w.line << "\tnot found\n";
                ++w.field;
                w.armed = false;
                if (w.live) w.until = now + 20; // a class with no copies must not spin the pass loop
                return; // next tick
            }
        }
    }
    std::array<std::uintptr_t, 4> addresses{};
    if (w.field < w.end)
        for (std::size_t i = 0; i < 4 && w.round * 4 + i < w.copy_count; ++i) addresses[i] = w.copies[w.round * 4 + i].address;
    (void)watch_four(addresses.data(), s.base, w.image_size);
    w.armed = w.field < w.end;
    if (!w.armed) {
        w.running = false;
        w.file.close();
        say(logging::Level::info, w.live ? "Trainer sweep: finished; " + write_live_sweep()
                                         : std::format("Trainer sweep: finished; the game read {} of {} values.", w.read, w.total));
        return;
    }
    w.until = now + static_cast<std::uint64_t>(w.dwell);
}
// Commands from outside while someone plays: each line of devcmd.txt in the data folder is a
// trainer command; the answers go to the log.
void run_dev_commands(std::uint64_t now) {
    static std::uint64_t next = 0;
    if (now < next) return;
    next = now + 300;
    const auto path = data_directory() / "devcmd.txt";
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return;
    std::vector<std::string> lines;
    {
        std::ifstream file(path);
        for (std::string line; std::getline(file, line);) lines.push_back(line);
    }
    if (!std::filesystem::remove(path, error)) return; // still being written: next time
    for (const auto &line : lines) {
        std::vector<std::string> words;
        for (std::size_t start = 0; start < line.size();) {
            const auto first = line.find_first_not_of(" \t\r", start);
            if (first == std::string::npos) break;
            auto last = line.find_first_of(" \t\r", first);
            if (last == std::string::npos) last = line.size();
            words.push_back(line.substr(first, last - first));
            start = last;
        }
        if (!words.empty() && lower(words[0]) == "trainer") words.erase(words.begin());
        if (words.empty()) continue;
        const std::vector<std::string> arguments(words.begin() + 1, words.end());
        say(logging::Level::info, std::format("Trainer dev> {} => {}", line, command(words[0], arguments)));
    }
}
void __try_poke(std::uintptr_t address, float value) noexcept {
    __try {
        *reinterpret_cast<volatile float *>(address) = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// ---- scripted controller ---------------------------------------------------------------
// "a+ry=-1:300" -> hold A with the right stick down for 300 ms. "n:500" -> nothing held.
std::optional<PadStep> pad_step(std::string_view text) {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos) return std::nullopt;
    const auto time = number(text.substr(colon + 1));
    if (!time || *time < 0 || *time > 60000) return std::nullopt;
    PadStep step;
    step.seconds = *time / 1000.0;
    std::uint16_t buttons{};
    std::array<std::int16_t, 4> sticks{};
    std::array<std::uint8_t, 2> triggers{};
    static constexpr std::pair<std::string_view, std::uint16_t> names[]{
        {"up", 0x1}, {"down", 0x2}, {"left", 0x4}, {"right", 0x8}, {"start", 0x10}, {"back", 0x20}, {"ls", 0x40},
        {"rs", 0x80}, {"lb", 0x100}, {"rb", 0x200}, {"a", 0x1000}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000}};
    static constexpr std::string_view axes[]{"lx", "ly", "rx", "ry"};
    auto rest = text.substr(0, colon);
    while (!rest.empty()) {
        const auto plus = rest.find('+');
        const auto word = lower(rest.substr(0, plus));
        rest = plus == std::string_view::npos ? std::string_view{} : rest.substr(plus + 1);
        const auto equals = word.find('=');
        const auto name = word.substr(0, equals);
        const auto amount = equals == std::string::npos ? std::optional<double>(1.0) : number(std::string_view(word).substr(equals + 1));
        if (!amount || std::abs(*amount) > 1) return std::nullopt;
        if (name == "n" || name.empty()) continue;
        bool known = false;
        for (const auto &[button, bit] : names)
            if (name == button) { buttons |= bit; known = true; }
        for (std::size_t i = 0; i < 4; ++i)
            if (name == axes[i]) { sticks[i] = static_cast<std::int16_t>(std::lround(*amount * 32767.0)); known = true; }
        if (name == "lt" || name == "rt") {
            triggers[name == "rt"] = static_cast<std::uint8_t>(std::lround(std::abs(*amount) * 255.0));
            known = true;
        }
        if (!known) return std::nullopt;
    }
    std::memcpy(step.pad.data(), &buttons, 2);
    step.pad[2] = triggers[0];
    step.pad[3] = triggers[1];
    std::memcpy(step.pad.data() + 4, sticks.data(), 8);
    return step;
}
bool game_in_front() {
    DWORD process{};
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}
void run_pad() {
    auto &p = state().pad;
    if (!p.running) return;
    const auto stamp = clock_now();
    while (p.index < p.steps.size() && clock_seconds(stamp - p.step_start) >= p.steps[p.index].seconds) {
        p.step_start += static_cast<std::int64_t>(p.steps[p.index].seconds / clock_seconds(1));
        ++p.index;
    }
    if (p.index < p.steps.size()) {
        DingoSDKOverlayScriptController(p.steps[p.index].pad.data());
        return;
    }
    // The pad stays connected, with nothing held, until `trainer pad off`: a pad that comes and
    // goes drops the game to keyboard mode, and it takes a while to notice the pad again.
    if (p.loop && !p.steps.empty()) {
        p.index = 0;
        p.step_start = stamp;
        return;
    }
    static constexpr std::array<std::uint8_t, 12> neutral{};
    DingoSDKOverlayScriptController(neutral.data());
    if (p.mount_tries > 0 && state().telemetry.physics_state == addr::no_bail::offboard_physics_state) {
        --p.mount_tries; // still on foot: the hub menu or a card was in the way
        p.index = 0;
        p.step_start = stamp;
        return;
    }
    if (p.mount_tries) say(logging::Level::info, std::format("Trainer pad: mounted (state {}).", state().telemetry.physics_state));
    p.mount_tries = 0;
    p.running = false;
    std::uint64_t polls{}, served{};
    DingoSDKOverlayScriptControllerPolls(&polls, &served);
    say(logging::Level::info, std::format("Trainer pad: script finished; the game read it {} times ({} blanked by the open menu); game window in front: {}.",
                                          served - p.served_before, DingoSDKOverlayScriptControllerBlanked(), game_in_front() ? "yes" : "no"));
}

// </dev>
// ---- self test -------------------------------------------------------------------------
void test_line(bool ok, const std::string &text) {
    auto &s = state();
    if (!ok) ++s.test.failures;
    say(ok ? logging::Level::info : logging::Level::error, std::format("trainer selftest: {} {}", ok ? "OK" : "FAIL", text));
}
void run_selftest(std::uint64_t now) {
    auto &s = state();
    auto &t = s.test;
    if (!t.step || now < t.until) return;
    tuning::Values live;
    switch (t.step) {
    case 1: {
        test_line(s.model != nullptr, std::format("game tuning read ({} values, {} curves)", s.model ? s.model->fields.size() : 0,
                                                  s.model ? s.model->curve_slots.size() : 0));
        std::size_t named{};
        if (s.model) named = static_cast<std::size_t>(std::ranges::count_if(s.model->field_names, [](const std::string &n) { return !n.empty(); }));
        test_line(s.model && named == s.model->fields.size(), std::format("field names ({} of {})", named, s.model ? s.model->fields.size() : 0));
        test_line(s.ready && s.asset, "running tuning asset found");
        test_line(s.telemetry.skater, "local skater found");
        // The first plain real that is not zero and that the player has not touched.
        t.entry = s.entries.size();
        for (std::size_t i = 0; i < s.entries.size(); ++i)
            if (s.entries[i].kind == Kind::real && !s.entries[i].detail && !s.entries[i].touched && std::abs(s.entries[i].stock) > 0.01) {
                t.entry = i;
                break;
            }
        if (!s.ready || t.entry == s.entries.size() || !editable()) {
            test_line(false, "no value to write (tuning not ready or locked by a session)");
            t.step = 4;
            break;
        }
        auto &e = s.entries[t.entry];
        t.before = e.stock;
        t.target = e.stock * 1.01;
        set_entry(e, t.target);
        t.step = 2;
        t.until = now + 1200;
        break;
    }
    case 2: {
        auto &e = s.entries[t.entry];
        const bool read = tuning::read_live(s.base, live);
        test_line(read && std::abs(read_field(live.image, e) - static_cast<float>(t.target)) < 1e-4 * std::max(1.0, std::abs(t.target)),
                  std::format("write and hold {} = {:.5g}", e.id, t.target));
        set_entry(e, t.before);
        t.step = 3;
        t.until = now + 1200;
        break;
    }
    case 3: {
        auto &e = s.entries[t.entry];
        const bool read = tuning::read_live(s.base, live);
        test_line(read && read_field(live.image, e) == static_cast<float>(t.before) && !e.touched, std::format("restore {} = {:.5g}", e.id, t.before));
        t.step = 4;
        break;
    }
    case 4: {
        if (!s.telemetry.skater) {
            test_line(false, "teleport (no skater)");
            t.step = 6;
            break;
        }
        t.origin = s.telemetry.position;
        const Vec3 target{t.origin[0] + 3.0f, t.origin[1] + 1.0f, t.origin[2]};
        const auto result = go_to(target, "a test point");
        test_line(!result.starts_with("error"), "teleport request: " + result);
        t.origin = target;
        t.step = result.starts_with("error") ? 6 : 5;
        t.until = now + 700;
        break;
    }
    case 5: {
        const auto &p = s.telemetry.position;
        // The skater may already be rolling away from where it was put.
        const float away = horizontal({p[0] - t.origin[0], 0, p[2] - t.origin[2]});
        test_line(s.telemetry.skater && away < 25.0f, std::format("teleport put the skater {:.2f} m from the target", away));
        t.step = 6;
        t.until = now + 1500;
        break;
    }
    default:
        say(t.failures ? logging::Level::error : logging::Level::info,
            std::format("trainer selftest: DONE {} failures", t.failures));
        t = {};
        changed(false);
        break;
    }
}

// ---- transition skating ------------------------------------------------------------------------
// Bowls, quarter pipes, vert and spines: the trainer's own help back into the ramp, and Skate 3's numbers for it.
// What is known of skate. here comes from its data, not from play (2026-10-11): the timed lean into the ramp Skate 3
// ended a coping grind with is still in the game but switched off (the exit timers are -1), a grind's exit points
// straight across the coping, and nothing measured says what a hang-up is. So every lever below reports what it saw
// (`Trainer transition:` lines) and each is a first setting.
constexpr std::string_view transition_preset = "Transition: Skate 3";
// "Transition feels like Skate 3": the lean is the share of the stick the game is shown (0.8 at most: past 0.9 the
// game takes the lean as the wish to leave the grind); Skate 3 kept all the speed along the coping and nudged the board
// at most 1 m/s across it; its hang-up push; 70 frames before the same edge could be caught again; its handplant exit.
constexpr float skate3_tr_lean = 0.6f, skate3_tr_carry = 1.0f, skate3_tr_drop = 1.0f, skate3_tr_unstick = 1.0f, skate3_tr_lockout = 1.17f, skate3_tr_plant = 1.0f;
// The sign of each stick axis the lean is sent on (`trainer transition leanframe <x> <y>`): the camera's axes as the
// game reads the stick against them. Not measured yet.
float lean_frame_x = 1.0f, lean_frame_y = 1.0f;
bool transition_stock() {
    auto &s = state();
    return s.tr_lean == 0.0f && s.tr_carry == 0.0f && s.tr_unstick == 0.0f && s.tr_lockout == 0.0f && !s.tr_spine && s.tr_plant == 0.0f &&
           std::ranges::all_of(s.tr_setting, [](std::int8_t mode) { return mode < 0; });
}
void reset_transition() {
    auto &s = state();
    s.tr_lean = s.tr_carry = s.tr_unstick = s.tr_lockout = s.tr_plant = 0.0f;
    s.tr_drop = 1.0f;
    s.tr_spine = false;
    s.tr_setting.fill(-1);
}
// How far the skater reaches for an edge from the air, and what an edge is worth as a landing. Held at 0 for a moment
// (after a coping grind, during a flight up and over a lip) the far coping of a spine or the coping just left cannot
// catch the board; the player's own numbers come back after.
constexpr std::string_view air_grind_values[]{"physicstrajectory.scoregrind", "physicsgrindsair.maxdist50_50", "physicsgrindsair.maxdist5_0",
                                              "physicsgrindsair.maxdisttipslide", "physicsgrindsair.maxdistboardslide"};
void hold_air_grinds(bool off) {
    auto &s = state();
    if (!s.ready || s.enforced) return;
    bool wrote = false;
    for (const auto key : air_grind_values) {
        const auto *e = find_entry(key);
        if (!e || e->frozen || e->class_field >= 0 || e->kind != Kind::real || static_cast<std::size_t>(e->offset) + e->size > s.want.image.size()) continue;
        const double wanted = off ? 0.0 : e->touched ? e->value : e->stock;
        if (read_field(s.want.image, *e) == static_cast<double>(static_cast<float>(wanted))) continue;
        write_field(s.want.image, *e, wanted);
        wrote = true;
    }
    if (wrote) s.write_due = true;
    s.tr.air_grinds_off = off;
}
void drive_transition(std::uintptr_t base, std::uintptr_t client, std::uint64_t now) {
    auto &s = state();
    auto &t = s.tr;
    const auto &m = s.motion;
    const bool blocked = extras_blocked();
    for (std::size_t i = 0; i < transition_settings.size(); ++i) force_transition_setting(i, blocked ? -1 : s.tr_setting[i]);
    if (now >= s.tr_option_at) {
        // The game's own options: whatever its Settings menu (or the trainer) last set.
        s.tr_option_at = now + 500;
        for (std::size_t i = 0; i < transition_options.size(); ++i) {
            const auto held = profile_runtime::local_game_option(transition_options[i].setting);
            const int value = held ? static_cast<int>(std::lround(*held)) : transition_options[i].stock;
            if (value != s.tr_option[i]) {
                s.tr_option[i] = value;
                s.view_due = true;
            }
        }
        const auto own = [&](std::string_view setting, bool stock, std::int8_t &shown) {
            const auto held = profile_runtime::local_game_option(setting);
            const auto value = static_cast<std::int8_t>(held ? *held != 0 : stock);
            if (value != shown) {
                shown = value;
                s.view_due = true;
            }
        };
        for (std::size_t i = 0; i < grind_settings.size(); ++i) own(grind_settings[i].setting, grind_settings[i].stock, s.grind_own[i]);
        for (std::size_t i = 0; i < transition_settings.size(); ++i) own(transition_settings[i].setting, transition_settings[i].stock, s.tr_own[i]);
    }
    const auto watch = watched_physics_state();
    const std::uint32_t physics = watch.valid && s.telemetry.skater && m.valid ? watch.state : 0;
    const bool riding = physics >= 100 && physics < 200, grinding = (physics >= 400 && physics < 500) || physics == 603;
    const bool flying = physics >= 200 && physics < 300;
    const auto &v = m.velocity;
    const float over = std::hypot(v[0], v[2]), speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    // Steep ground: the skater stands out from the wall, so the level part of their up axis points into the ramp. A
    // skater held upright on a wall still climbs or drops steeply: the wall is then ahead going up and behind coming down.
    if (riding && m.axes_valid) {
        float out_x = m.up[0], out_z = m.up[2], length = std::hypot(out_x, out_z);
        bool steep = m.up[1] < 0.8f && length > 0.1f;
        if (!steep && speed > 1.5f && std::abs(v[1]) > 0.6f * speed && over > 0.2f) {
            const float away = v[1] > 0 ? -1.0f : 1.0f;
            out_x = away * v[0];
            out_z = away * v[2];
            length = over;
            steep = true;
        }
        if (steep) {
            t.steep_at = now;
            t.steep_out = {out_x / length, out_z / length};
        }
    }
    const bool on_transition = t.steep_at && now - t.steep_at < 3000;
    // The speed along the lip (level, square to the way out of the wall) and across it (up the wall and over).
    const float out_speed = v[0] * t.steep_out[0] + v[2] * t.steep_out[1];
    const float lip_along = std::hypot(v[0] - out_speed * t.steep_out[0], v[2] - out_speed * t.steep_out[1]);
    if (grinding && !t.grinding) {
        // A grind that begins within a second of steep ground is on a coping.
        t.coping = t.steep_at && now - t.steep_at < 1000;
        if (t.coping) {
            t.ramp = t.steep_out;
            t.edge = {-t.ramp[1], t.ramp[0]};
            t.in_speed = speed;
            t.in_angle = std::atan2(std::hypot(out_speed, v[1]), std::max(lip_along, 0.001f)) * 180.0f / std::numbers::pi_v<float>;
        }
    }
    if (grinding && t.coping) {
        // The coping runs the way the skater travels on it (a bowl's coping curves); the ramp stays on the side it was on.
        if (over > 1.0f) t.edge = {v[0] / over, v[2] / over};
        const float lean_on_edge = t.ramp[0] * t.edge[0] + t.ramp[1] * t.edge[1];
        const float side_x = t.ramp[0] - lean_on_edge * t.edge[0], side_z = t.ramp[1] - lean_on_edge * t.edge[1];
        if (const float length = std::hypot(side_x, side_z); length > 0.2f) t.ramp = {side_x / length, side_z / length};
    } else if (!grinding) {
        if (t.coping && t.grinding && physics != 200 && physics != 300 && !(physics >= 500 && physics < 600)) t.left_at = now;
        t.coping = false;
        // For a handplant, and for a flight: the wall last ridden.
        t.ramp = t.steep_out;
        t.edge = {-t.ramp[1], t.ramp[0]};
    }
    // A flight straight up and over a lip (an air, a spine transfer): little of its speed runs along the lip. A carve
    // into a coping grind leaves the wall along the lip and keeps its reach for the coping.
    if (flying && !t.flying) t.flight_off = s.tr_spine && !blocked && on_transition && v[1] > 2.0f && lip_along < 0.5f * speed;
    if (t.flight_off && !flying) {
        if (t.flying) t.flight_off_until = now + 300;
        else if (now >= t.flight_off_until) t.flight_off = false;
    }
    const bool locked = s.tr_lockout > 0.0f && !blocked && t.left_at && now - t.left_at < static_cast<std::uint64_t>(s.tr_lockout * 1000.0f);
    const bool air_off = locked || (t.flight_off && s.tr_spine && !blocked);
    if (air_off != t.air_grinds_off || air_off) hold_air_grinds(air_off);
    // The lean: the left stick the game is shown, pushed toward the ramp, more the faster the grind runs along the
    // coping. The game reads that stick against the camera, so the ramp's side is turned into the camera's axes.
    const float along_speed = std::abs(v[0] * t.edge[0] + v[2] * t.edge[1]);
    const float lean_wanted = t.coping && !blocked ? s.tr_lean * 0.8f * std::clamp((along_speed - 1.0f) / 4.0f, 0.0f, 1.0f) : 0.0f;
    t.lean += (lean_wanted - t.lean) * 0.15f;
    if (blocked) t.lean = 0.0f;
    float lean_x = 0, lean_y = 0;
    bool leaning = false;
    if (t.coping && std::abs(t.lean) > 0.02f) {
        std::array<float, 16> camera{};
        if (read_local_camera_transform(base, client, camera)) {
            // Rows: right, up, forward; a camera looks back along its forward row.
            const float right = std::hypot(camera[0], camera[2]), ahead = std::hypot(camera[8], camera[10]);
            if (right > 0.1f && ahead > 0.1f) {
                lean_x = (t.ramp[0] * camera[0] + t.ramp[1] * camera[2]) / right * t.lean * lean_frame_x;
                lean_y = -(t.ramp[0] * camera[8] + t.ramp[1] * camera[10]) / ahead * t.lean * lean_frame_y;
                leaning = true;
            }
        }
    }
    DingoSDKOverlaySetGrindStickLean(leaning, lean_x, lean_y);
    TransitionAssist assist;
    if (!blocked) {
        assist.carry = s.tr_carry;
        assist.drop_speed = s.tr_drop;
        assist.unstick = s.tr_unstick;
        assist.plant = s.tr_plant;
    }
    assist.coping = t.coping;
    assist.transition = on_transition;
    assist.edge = t.edge;
    assist.ramp = t.ramp;
    assist.measure = s.hud_jump || s.logging;
    // <dev>
    assist.measure = assist.measure || dev_capture;
    // </dev>
    set_transition_assist(client, s.entity, assist);
    if (const auto report = transition_report(); report.sequence != t.reports) {
        t.reports = report.sequence;
        const auto off_coping = [](float along, float across, float down) {
            return std::atan2(std::hypot(across, down), std::max(std::abs(along), 0.001f)) * 180.0f / std::numbers::pi_v<float>;
        };
        if (report.kind == 1)
            say(logging::Level::info, std::format("Trainer transition: coping grind left without a pop (state {} -> {}, then {}). It began at {:.0f} deg to the coping, {:.1f} m/s. "
                                                  "Last step on it: {:.2f} m/s along, {:.2f} into the ramp. A third of a second later the game had {:.2f} along, {:.2f} into the ramp, "
                                                  "{:.2f} down = {:.0f} deg off the coping; the skater left with {:.2f} along, {:.2f} into the ramp = {:.0f} deg. Hung {} steps; "
                                                  "speed put back on {} steps, pushed on {}; {:.2f} m lower after a third of a second, {:.2f} m after one and a half.",
                                                  report.from, report.to, report.to_last, t.in_angle, t.in_speed, report.in_along, report.in_across, report.game_along,
                                                  report.game_across, report.out_down, off_coping(report.game_along, report.game_across, report.out_down), report.out_along,
                                                  report.out_across, off_coping(report.out_along, report.out_across, report.out_down), report.hung, report.carried,
                                                  report.kicked, report.drop, report.fall));
        else if (report.kind == 2)
            say(logging::Level::info, std::format("Trainer transition: handplant (state {} -> {}, then {}): {:.2f} m lost inside the plant; it let go at {:.2f} m/s into the ramp, "
                                                  "{:.2f} along the lip, {:.2f} down; a fifth of a second later {:.2f} into the ramp (the game's own {:.2f}), {:.2f} along, "
                                                  "{:.2f} down; {:.2f} m below the plant's top a second and a half after. Hung {} steps; speed changed on {} steps, pushed on {}.",
                                                  report.from, report.to, report.to_last, report.drop, report.in_across, report.in_along, report.in_down, report.out_across,
                                                  report.game_across, report.out_along, report.out_down, report.fall, report.hung, report.carried, report.kicked));
        else if (report.kind == 3)
            say(logging::Level::info, std::format("Trainer transition: a landing off a transition hung (state {} -> {}, then {}): {} steps slower than 1.1 m/s, pushed on {}; "
                                                  "the flight came in at {:.2f} m/s over the ground, {:.2f} down.",
                                                  report.from, report.to, report.to_last, report.hung, report.kicked, report.in_along, report.in_down));
    }
    // <dev>
    // What the skater and the game's lip states do around a coping, a plant or a wall, thirty times a second.
    static std::uint32_t transition_ticks = 0;
    const bool near_it = (t.steep_at && now - t.steep_at < 2000) || grinding || physics >= 600 || (t.left_at && now - t.left_at < 2000);
    if (dev_capture && near_it && physics && ++transition_ticks % 2 == 0) {
        const auto g = read_transition_state(base, static_cast<std::uintptr_t>(s.entity));
        float lx{}, ly{}, rx{}, ry{};
        (void)pad_sticks(lx, ly, rx, ry);
        say(logging::Level::info, std::format("Trainer trans: state {}, {:.1f} km/h, v {:.2f} {:.2f} {:.2f}, up {:.2f} {:.2f} {:.2f}, fwd {:.2f} {:.2f} {:.2f}, out {:.2f} {:.2f} ({} ms ago), "
                                              "coping {}, edge {:.2f} {:.2f}, ramp {:.2f} {:.2f}, lip found {} tricking {} plant {} vert {} input {:.2f} dropin {}, lipdir {:.2f} {:.2f} {:.2f}, "
                                              "nearest {:.1f} {:.1f} {:.1f}, ground {:.2f} {:.2f} {:.2f}, pos {:.1f} {:.1f} {:.1f}, L {:.2f} {:.2f} R {:.2f} {:.2f}, lean {:.2f} -> {:.2f} {:.2f} ({} polls), "
                                              "air grinds off {}, found {:x}.",
                                              physics, speed * 3.6f, v[0], v[1], v[2], m.up[0], m.up[1], m.up[2], m.forward[0], m.forward[1], m.forward[2], t.steep_out[0],
                                              t.steep_out[1], t.steep_at ? now - t.steep_at : 0, t.coping, t.edge[0], t.edge[1], t.ramp[0], t.ramp[1], g.lip_found, g.lip_tricking,
                                              g.handplanting, g.targeting_vert, g.transition_input, g.drop_in_type, g.lip_direction[0], g.lip_direction[1], g.lip_direction[2],
                                              g.lip_nearest[0], g.lip_nearest[1], g.lip_nearest[2], g.ground_normal[0], g.ground_normal[1], g.ground_normal[2], m.position[0],
                                              m.position[1], m.position[2], lx, ly, rx, ry, t.lean, lean_x, lean_y, DingoSDKOverlayGrindStickLeaned(), t.air_grinds_off, g.found));
    }
    // </dev>
    t.grinding = grinding;
    t.flying = flying;
}
bool builtin_on(std::size_t p) {
    auto &s = state();
    const auto &preset = builtin_presets()[p];
    bool any = false;
    for (const auto &[index, r] : preset_targets()[p]) {
        const auto &e = s.entries[index];
        const auto &rule = preset.rules[r];
        const double expected = rule.multiply ? e.stock * rule.amount : rule.amount;
        any = true;
        // To the two decimals a dial shows: dragging one to the preset's number switches it on.
        if (std::abs(e.value - sane(e, expected)) > 4e-3 * std::max(1e-3, std::abs(expected))) return false;
    }
    return any;
}
bool builtin_on(std::string_view name) {
    const auto &presets = builtin_presets();
    for (std::size_t p = 0; p < presets.size(); ++p)
        if (presets[p].name == name) return builtin_on(p);
    return false;
}
void build_view() {
    auto &s = state();
    s.view_due = false;
    auto next = std::make_shared<View>();
    next->revision = ++s.revision;
    next->ready = s.ready;
    next->status = s.status;
    next->last = s.last;
    next->editable = editable(&next->blocked);
    next->session_enforced = s.enforced;
    next->boosts_blocked = !s.enforced && multiplayer_session_active() && !session_boosts_allowed();
    // <dev>
    if (dev_fake_session == 1) {
        next->session_enforced = true;
        next->editable = false;
        next->blocked = "You are in a multiplayer game where the host controls physics. This menu will not work in this multiplayer game.";
    } else if (dev_fake_session == 2) {
        next->boosts_blocked = true;
    }
    // </dev>
    next->rows.reserve(s.entries.size());
    for (const auto &e : s.entries) {
        int rank{};
        std::uint8_t topic{};
        bool trial{};
        const auto friendly = essential_name(e.key, &rank, &topic, &trial);
        next->rows.push_back({e.id, e.label, e.group, e.kind, e.value, e.stock, e.touched, e.frozen, e.detail, std::string(friendly), rank, topic,
                              trial, e.used});
        next->rows.back().help = essential_help(e.key);
        if (e.touched) {
            ++next->touched;
            ++next->changed[topic];
        }
        if (e.frozen) ++next->locks;
        if (std::ranges::find(next->groups, e.group) == next->groups.end()) next->groups.push_back(e.group);
    }
    const auto is_active = [&](std::string_view name) { return std::ranges::find(s.active, name) != s.active.end(); };
    next->presets.push_back({"Stock", "Everything back to how this map loaded (frozen values stay).", true, s.active.empty() && !next->touched});
    {
        // A built-in preset is on when the values say so, whatever switched them: a slider, a
        // dial or another preset.
        const auto &presets = builtin_presets();
        const auto &targets = preset_targets();
        for (std::size_t p = 0; p < presets.size(); ++p) {
            const auto &preset = presets[p];
            PresetRow row{std::string(preset.name), std::string(preset.note), true, false};
            const Entry *headline{};
            for (const auto &[index, r] : targets[p])
                if (!headline && r == 0) headline = &s.entries[index];
            row.active = builtin_on(p);
            const auto dial = preset_dial(preset.name);
            row.title = std::string(dial.title);
            row.topic = dial.topic;
            if (!preset.rules.empty() && preset.rules[0].multiply && headline && headline->stock != 0 && !dial.title.empty()) {
                row.dial = true;
                row.amount = preset.rules[0].amount;
                row.factor = headline->value / headline->stock;
            }
            next->presets.push_back(std::move(row));
        }
    }
    for (const auto &[name, values] : s.user) next->presets.push_back({name, std::format("{} values", values.size()), false, is_active(name)});
    next->slot = s.slot;
    next->auto_return = s.auto_return;
    next->hippy_height = s.hippy_height;
    next->nocomply_height = s.nocomply_height;
    next->manual_height = s.manual_height;
    next->grind_side = s.grind_side;
    next->grind_push = s.grind_push;
    next->grind_range = s.grind_range;
    next->grind_mode = s.grind_mode;
    next->grind_setting = s.grind_setting;
    next->grind_live = s.grind_live;
    next->tr_lean = s.tr_lean;
    next->tr_carry = s.tr_carry;
    next->tr_drop = s.tr_drop;
    next->tr_unstick = s.tr_unstick;
    next->tr_lockout = s.tr_lockout;
    next->tr_spine = s.tr_spine;
    next->tr_plant = s.tr_plant;
    next->tr_setting = s.tr_setting;
    next->grind_own = s.grind_own;
    next->tr_own = s.tr_own;
    next->tr_option = s.tr_option;
    next->offboard_height = s.offboard_height;
    next->flip_speed = s.flip_speed;
    next->flip_advanced = s.flip_advanced;
    next->flip_pop = s.flip_pop;
    next->flip_flick = s.flip_flick;
    next->flip_trick = s.flip_trick;
    next->flip_advanced_blocked = s.flip_advanced && extras_blocked();
    next->flip_live = s.flip_live;
    next->boneless_height = s.boneless_height;
    next->revert_boost = s.revert_boost;
    next->pump_power = s.pump_power;
    next->pump_live = s.pump_live;
    next->pump_blocked = s.pump_power != 1.0f && extras_blocked();
    next->revert = s.revert;
    next->return_delay = s.return_delay;
    next->pad_shortcuts = s.pad_shortcuts;
    next->pad_menu = s.pad_menu;
    next->hud = s.hud;
    next->hud_jump = s.hud_jump;
    next->logging = s.logging;
    next->map = s.map;
    next->map_note = s.map_file.note;
    next->map_preset = s.map_file.preset.empty() ? std::string{} : s.map_file.preset_name.empty() ? "Map preset" : s.map_file.preset_name;
    next->spots = s.map_file.spots;
    next->waypoint_set = s.waypoint.has_value();
    if (s.waypoint) next->waypoint = *s.waypoint;
    next->open_serial = s.open_serial;
    for (const auto &one : setups())
        if (modified(one)) {
            ++next->changed[one.topic];
            ++next->settings[one.topic];
            ++next->setups_changed;
        }
    // Nothing of the trainer's is in force. (A lock on a value that is the game's own changes nothing: counted apart.)
    next->stock = !next->touched && !next->setups_changed;
    next->folds = s.folds;
    next->share_text = s.share_text;
    next->share_serial = s.share_serial;
    next->open_tab = s.open_tab;
    if (const auto found = s.maps.find(s.map); found != s.maps.end()) {
        next->markers = found->second.markers;
        next->profile_preset = found->second.preset;
    }
    publish(std::move(next));
}
// ---- sharing presets ---------------------------------------------------------------------------
// One line of text, short enough to paste in a chat: "RST1:" and the preset's JSON in base64.
constexpr std::string_view share_tag = "RST1:";
constexpr std::string_view base64_digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64(std::string_view bytes) {
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        std::uint32_t chunk{};
        const auto count = std::min<std::size_t>(3, bytes.size() - i);
        for (std::size_t k = 0; k < 3; ++k) chunk = chunk << 8 | (k < count ? static_cast<unsigned char>(bytes[i + k]) : 0u);
        for (std::size_t k = 0; k < 4; ++k) out += k <= count ? base64_digits[chunk >> (18 - 6 * k) & 63] : '=';
    }
    return out;
}
std::optional<std::string> unbase64(std::string_view text) {
    std::string out;
    std::uint32_t chunk{};
    int bits{};
    for (const char c : text) {
        if (c == '=' || std::isspace(static_cast<unsigned char>(c))) continue;
        const auto digit = base64_digits.find(c);
        if (digit == std::string_view::npos) return std::nullopt;
        chunk = chunk << 6 | static_cast<std::uint32_t>(digit);
        if ((bits += 6) >= 8) out += static_cast<char>(chunk >> (bits -= 8) & 0xff);
    }
    return out;
}
std::filesystem::path shared_directory() {
    const auto directory = data_directory();
    return directory.empty() ? directory : directory / L"shared";
}
// File names from preset names: letters, digits, space, '-' and '_' only.
std::string file_stem(std::string_view name) {
    std::string stem;
    for (const char c : name) stem += std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' ? c : '_';
    return stem.empty() ? "preset" : stem;
}
// `preset export <name>`: one of the player's presets, or "current" for what is changed right now.
std::string export_preset(const std::string &name) {
    auto &s = state();
    Json values = Json::object();
    std::string title;
    if (lower(name) == "current") {
        for (const auto &e : s.entries)
            if (e.touched) values[e.key] = e.value;
        for (const auto &one : setups())
            if (off_stock(one)) values[one.key] = one.get();
        title = "My setup";
    } else {
        for (const auto &[user_name, saved] : s.user)
            if (lower(user_name) == lower(name)) {
                for (const auto &[key, value] : saved) values[key] = value;
                title = user_name;
            }
        if (title.empty()) return "error: no preset of yours is called \"" + name + "\". Use \"current\" for what is changed right now.";
    }
    if (values.empty()) return "error: nothing is changed, so there is nothing to share.";
    const auto count = values.size();
    Json json = Json::object();
    json["reskate_trainer"] = 1;
    json["name"] = title;
    json["values"] = std::move(values);
    s.share_text = std::string(share_tag) + base64(json.dump());
    ++s.share_serial;
    std::string where;
    if (const auto directory = shared_directory(); !directory.empty()) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream file(directory / (file_stem(title) + ".json"), std::ios::binary | std::ios::trunc);
        file << json.dump(2);
        if (file) where = std::format(" Also saved as shared\\{}.json.", file_stem(title));
    }
    changed();
    return std::format("\"{}\" copied to the clipboard as one line ({} values): paste it anywhere.{}", title, count, where);
}
// `preset import [file]`: the text the menu put in shared\clipboard.txt, or a file in shared\.
// Either the one-line form or the JSON itself. The preset joins the player's own; nothing is applied.
std::string import_preset(const std::string &file_name) {
    auto &s = state();
    const auto directory = shared_directory();
    if (directory.empty()) return "error: the trainer's folder could not be found.";
    const auto path = directory / (file_name.empty() ? "clipboard.txt" : file_stem(std::filesystem::path(file_name).stem().string()) + ".json");
    std::string text;
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return file_name.empty() ? "error: there is nothing to import." : "error: shared\\" + path.filename().string() + " does not exist.";
        text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    if (text.size() > 256 * 1024) return "error: that is too long to be a preset.";
    if (const auto tag = text.find(share_tag); tag != std::string::npos) {
        const auto start = tag + share_tag.size();
        const auto end = text.find_first_of(" \t\r\n`\"'", start);
        const auto decoded = unbase64(std::string_view(text).substr(start, end == std::string::npos ? end : end - start));
        if (!decoded) return "error: that is not a whole preset line: copy all of it, from RST1: to the end.";
        text = *decoded;
    }
    try {
        const auto json = Json::parse(text);
        if (!json.is_object() || !json.contains("values") || !json.at("values").is_object()) return "error: that is not a ReSkate Trainer preset.";
        std::map<std::string, double, std::less<>> values;
        std::size_t unknown{};
        for (const auto &[key, value] : json.at("values").items()) {
            if (!value.is_number() || values.size() >= 512) continue;
            const auto id = lower(key);
            const bool setting = find_setup(id) || id == "trick.flip_gate" || id == "trick.catch_at" || id == "trick.catch_percent";
            if (!setting && !find_entry(id)) ++unknown; // kept: another build of the game may have it
            values[id] = value.get<double>();
        }
        if (values.empty()) return "error: that preset holds no values.";
        auto name = json.value("name", "Imported");
        if (name.empty() || name.size() > 40) name = "Imported";
        const auto taken = [&](const std::string &candidate) {
            if (lower(candidate) == "stock" || lower(candidate) == "map" || s.user.contains(candidate)) return true;
            return std::ranges::any_of(builtin_presets(), [&](const auto &preset) { return lower(preset.name) == lower(candidate); });
        };
        auto unique = name;
        for (int n = 2; taken(unique); ++n) unique = std::format("{} {}", name, n);
        const auto count = values.size();
        s.user[unique] = std::move(values);
        changed();
        return std::format("Imported \"{}\" ({} values{}). Turn it on under Your presets.", unique, count,
                           unknown ? std::format(", {} of them unknown to this version", unknown) : "");
    } catch (...) {
        return "error: that is not a ReSkate Trainer preset.";
    }
}
bool flag(const std::string &text, bool &out) {
    const auto value = lower(text);
    if (value == "1" || value == "on" || value == "true") { out = true; return true; }
    if (value == "0" || value == "off" || value == "false") { out = false; return true; }
    return false;
}
std::optional<int> slot_argument(const std::vector<std::string> &arguments, std::size_t index) {
    auto &s = state();
    if (arguments.size() <= index) return s.slot;
    const auto value = number(arguments[index]);
    if (!value || *value < 1 || *value > static_cast<double>(marker_slots) || *value != std::floor(*value)) return std::nullopt;
    return static_cast<int>(*value) - 1;
}
} // namespace

void tick(std::uintptr_t base, std::uintptr_t client, bool playing, const std::string &level) noexcept {
    try {
        auto &s = state();
        const auto now = GetTickCount64();
        if (!s.loaded) load_store();
        s.base = base;
        sync_session();
        s.boosts = boosts_in_force();
        // <dev>
        s.client = client;
        // </dev>
        if (const auto key = lower(playing ? level : std::string{}); key != s.map) enter_map(key);
        if (const auto key = lower(level); !key.empty() && key != s.waypoint_level) {
            s.waypoint_level = key;
            s.waypoint.reset();
            s.view_due = true;
        }
        if (take_teleport_card_shown()) s.waypoint_wanted_until = now + 1000;
        if (now < s.waypoint_wanted_until && now >= s.waypoint_next_read) {
            s.waypoint_next_read = now + 250;
            refresh_waypoint();
        }
        if (playing) {
            observe(base, client, now);
            shortcuts();
            update_landing(now);
            if (s.return_at && now >= s.return_at) {
                s.return_at = 0;
                const auto &marker = current_map().markers[static_cast<std::size_t>(s.slot)];
                if (marker.set) (void)go_to(marker.position, "your marker");
            }
        } else if (s.telemetry.skater) {
            s.telemetry.skater = false;
            s.motion.valid = false;
            s.entity = 0;
        }
        (void)start_trick_heights(base);
        (void)start_state_holds(base);
        if (take_class_list_shown()) s.class_list_wanted = true;
        // The search reads all of the game's writable memory, so it runs only for a player with
        // a use for it: one of the classes' values or the flip speed is not the game's own
        // (theirs, or a host's they skate with), or they have the list of every value open.
        if (s.class_search_at && now >= s.class_search_at && playing && s.telemetry.skater && !finding_classes() &&
            (s.class_list_wanted || classes_wanted())) {
            // A level brings fresh copies, so only a search finished since it loaded counts. The
            // skater's own classes arrive a little after the skater does: a search that came up
            // empty is repeated, twice.
            const bool have = class_searches() > s.class_search_seen && class_copies(0) != 0;
            if (have || s.class_search_tries >= 3) {
                s.class_search_at = 0;
            } else {
                ++s.class_search_tries;
                s.class_search_at = now + 15000;
                find_classes();
            }
        }
        flip_speed_in_use(s.boosts.flip != 1.0f || s.flip_advanced || s.flip_flick);
        // The speed itself goes straight to the trick in the air (drive_flip_speed); the flip curves
        // are only scaled where that number cannot be reached.
        want_flip_speed(s.flip_live ? 1.0f : s.boosts.flip);
        // The player's own choice, and only where their own trick settings count: not under a
        // host's enforced physics or in a session that has turned boosts off.
        // A flip asked to be slow is slow from the start: the game's rule would hurry it round before
        // the landing, so it is held off for as long as the speed in force is under the game's own.
        const bool slow = (s.flip_drive.active ? s.flip_drive.factor : s.boosts.flip) < 1.0f;
        want_flip_gate(extras_blocked() || !slow ? flip_gate_stock : flip_gate_off);
        if (s.boosts.flip != s.flip_applied) {
            // The catch times follow the flip speed (want_class).
            s.flip_applied = s.boosts.flip;
            for (const auto &e : s.entries)
                if (e.class_field >= 0 && slowed_catch_time(e.key)) want_class(e);
        }
        (void)apply_classes();
        // The game builds its tuning objects again with a new skater (a respawn, a teleport, a
        // session change) and they start at its own numbers: look again, but not in a flurry.
        {
            const bool lost = refresh_classes(now);
            const auto entity = static_cast<std::uint64_t>(s.entity);
            bool new_skater = entity && s.class_entity && entity != s.class_entity;
            if (entity) s.class_entity = entity;
            // A respawn can keep the entity and still fill new copies (the push speeds' native
            // one among them): the old ones go on holding what was written, so nothing looks lost.
            if (skater_creations() != s.class_creations) s.grind_stock.fill(-1); // a new skater starts from the game's own switches
            if (const auto creations = skater_creations(); creations != s.class_creations) {
                new_skater = new_skater || s.class_creations != 0;
                s.class_creations = creations;
            }
            // A bail builds a new skater too, and most of those bring nothing new: a search that a
            // new skater asked for and that found only what was known doubles the wait before the
            // next one (a search reads all of the game's writable memory, for seconds).
            if (!s.class_skater_judged && !s.class_search_at && !finding_classes()) {
                s.class_skater_judged = true;
                s.class_skater_wait = class_search_found() == s.class_found ? std::min(s.class_skater_wait * 2, class_skater_wait_high) : class_skater_wait_low;
                s.class_skater_after = now + s.class_skater_wait;
            }
            s.class_research_due = s.class_research_due || lost;
            s.class_skater_due = s.class_skater_due || new_skater;
            if (!classes_wanted()) s.class_research_due = s.class_skater_due = false;
            const bool for_lost = s.class_research_due && now >= s.class_research_after;
            const bool for_skater = s.class_skater_due && now >= s.class_skater_after && now >= s.class_research_after;
            if ((for_lost || for_skater) && !s.class_search_at && !finding_classes()) {
                s.class_research_due = s.class_skater_due = false;
                s.class_search_seen = class_searches();
                s.class_search_tries = 0;
                s.class_search_at = now + 3000;
                s.class_research_after = now + 20000;
                if (!for_lost) {
                    s.class_skater_judged = false;
                    s.class_found = class_search_found();
                    s.class_skater_after = now + s.class_skater_wait;
                }
            }
        }
        set_trick_heights(s.boosts.nocomply, s.boosts.boneless);
        // The pop out of a manual: the ollie's launch, scaled while Enum.Intent.Manual says the skater is in one.
        // "Slow flips pop higher": the launch of each flip trick, by the speed it will turn at (the game has the trick in
        // mind when it launches: measured, a kickflip reads 2 and an ollie 1 at that call).
        for (std::uint32_t trick = 0; trick < 64; ++trick) {
            float up = 0;
            if (s.flip_pop && !extras_blocked() && (!plain_pop(trick) || ollie_pop(trick))) {
                float factor = s.boosts.flip;
                if (s.flip_advanced)
                    if (const auto index = flip_trick_index(trick); index >= 0) factor *= s.flip_trick[static_cast<std::size_t>(index)];
                up = slow_flip_up(trick, factor);
            }
            set_flip_launch(trick, up);
        }
        set_manual_pop(s.enforced || (multiplayer_session_active() && !session_boosts_allowed()) ? 1.0f : s.manual_height,
                       read_manual_intent(s.base, static_cast<std::uintptr_t>(s.entity)));
        {
            // The sideways part of a pop out of a grind, against the way the skater was travelling on it.
            const auto watch = watched_physics_state();
            const bool on_grind = watch.valid && watch.state >= 400 && watch.state < 500;
            // The controller is only asked while it matters: on a grind, with the stick push in use or being logged.
            const float stick = on_grind && (s.grind_push != 0.0f || s.hud_jump || s.logging) ? left_stick_x() : 0.0f;
            // <dev>
            // What the stick does to the board on a grind, six times a second: for matching Skate 3's response.
            // Thirty times a second while the stick is held: the turn's speed is read off these lines.
            static std::uint32_t grind_ticks = 0;
            float raw_x{}, raw_y{};
            if (dev_capture && on_grind) (void)left_stick_raw(raw_x, raw_y);
            if (dev_capture && on_grind && ++grind_ticks % (std::abs(raw_x) > 0.2f ? 2 : 10) == 0) {
                const auto mirrored = DingoSDKOverlayGrindStickMirrored();
                const auto c = read_grind_control(s.base, static_cast<std::uintptr_t>(s.entity));
                const float travel = std::atan2(s.motion.velocity[0], s.motion.velocity[2]) * 180.0f / std::numbers::pi_v<float>;
                say(logging::Level::info, std::format("Trainer grindctl: state {}, type {} / {}, stick {:.2f} {:.2f}, LR {:.3f} UD {:.3f}, heading {:.3f} x {:.2f} base {:.3f}, "
                                                      "pitch {:.3f} x {:.2f} base {:.3f}, active {}, body {:.0f} deg, travel {:.0f} deg, {:.1f} km/h, found {:x}, lean polls {}.",
                                                      watch.state, c.grind_type, c.skate_grind_type, raw_x, raw_y, c.lr, c.ud, c.heading, c.heading_multiplier, c.base_heading,
                                                      c.pitch, c.pitch_multiplier, c.base_pitch, c.active_input, s.telemetry.heading, travel, s.telemetry.speed * 3.6f, c.found, mirrored) + std::format(" Trainer heading {:.3f}, stick x {:.2f}, y scale {:.2f}.", s.grind_heading, s.grind_frame_cos, s.grind_frame_sin));
            }
            // </dev>
            set_grind_side(extras_blocked() ? 1.0f : s.grind_side, s.motion.velocity[0], s.motion.velocity[2], watch.valid && watch.state >= 400 && watch.state < 500,
                           extras_blocked() ? 0.0f : s.grind_push, stick);
            float along{}, side{}, nudge{};
            if (take_grind_pop(along, side, nudge) && (s.hud_jump || s.logging || s.grind_side != 1.0f || s.grind_push != 0.0f))
                say(logging::Level::info, std::format("Trainer grind pop: the game's launch {:.2f} m/s along the grind, {:.2f} m/s sideways (side force x {:.2f}); "
                                                      "stick {:.2f}, stick push {:.2f} m/s.", along, side, extras_blocked() ? 1.0f : s.grind_side, nudge,
                                                      extras_blocked() ? 0.0f : nudge * s.grind_push));
        }
        // The speeds pushes aim for are the push class's (value_links ties them to the tuning's top
        // pushing speed, which only gates whether a push may start).
        const auto *top_speed = find_entry("physicspush.maxpushablespeed");
        // Past the game's own ceiling the push is carried by the trainer (see trainer_push_speed):
        // the player's own setting only, like the other speed the trainer adds.
        const float push_factor = top_speed && top_speed->touched && top_speed->stock > 0 && !extras_blocked()
                                      ? std::max(1.0f, static_cast<float>(top_speed->value / top_speed->stock)) : 1.0f;
        set_push_speed(client, s.entity, push_factor, top_speed ? static_cast<float>(top_speed->stock) : 0.0f, s.boosts.cruise);
        // Pumping: the game says when (its pumping state), the trainer sizes the gain on the physics
        // step (trainer_pump_power). The player's own setting only, like the push speed.
        {
            const auto pump = read_pump_state(base, static_cast<std::uintptr_t>(s.entity));
            if (pump.found != s.pump_live) {
                s.pump_live = pump.found;
                s.view_due = true;
            }
            bool measure = false, searching = false;
            // <dev>
            measure = s.pump_log;
            searching = dev_capture; // the capture reads the off-board flip states
            // </dev>
            const bool grind_held = std::ranges::any_of(s.grind_mode, [](std::int8_t mode) { return mode >= 0; });
            keep_holds_alive();
            for (std::size_t i = 0; i < grind_settings.size(); ++i) force_grind_setting(i, extras_blocked() ? -1 : s.grind_setting[i]);
            states_in_use(s.pump_power != 1.0f || s.manual_height != 1.0f || grind_held || s.grind_watch || s.grind_push != 0.0f || s.grind_range != 0.0f || measure || searching);
            {
                const auto watch = watched_physics_state();
                const bool grinding = watch.valid && watch.state >= 400 && watch.state < 500;
                // Skate 3's turn (retail sub_82D89F58): nothing inside a quarter of the stick, the rest stretched, x 0.85,
                // cubed, eased 0.91 / 0.09 a sixtieth of a second; the end of the stick is `grind_range` degrees.
                const bool turning = grinding && s.grind_range != 0.0f && !extras_blocked();
                const auto turn_now = GetTickCount64();
                if (turning) {
                    float x{}, y{};
                    (void)left_stick_raw(x, y);
                    const float past = std::abs(x) <= 0.25f ? 0.0f : (std::abs(x) - 0.25f) * (4.0f / 3.0f) * (x < 0 ? -1.0f : 1.0f);
                    const float wanted = std::pow(std::clamp(past * 0.85f, -0.85f, 0.85f), 3.0f);
                    const float steps = s.grind_yaw_at ? std::clamp(static_cast<float>(turn_now - s.grind_yaw_at) * 0.06f, 0.0f, 6.0f) : 1.0f;
                    s.grind_yaw += (wanted - s.grind_yaw) * (1.0f - std::pow(0.91f, steps));
                    if (std::abs(s.grind_yaw) < 0.0001f && wanted == 0.0f) s.grind_yaw = 0.0f;
                    s.grind_heading = s.grind_yaw / 0.614125f * s.grind_range * std::numbers::pi_v<float> / 180.0f;
                    // The game's scripts cannot be answered (they do not go through the lookups), so its own grind control
                    // is steered: it turns the board to about atan2(LR, |UD|) of the right stick it sees, after turning
                    // that stick by an angle of its own. The stick it is shown: the turn, turned back by that angle; the
                    // player's own up and down (the tip), shortened so the turn and the tip together stay on the rim.
                    // (The sideways sign the game flips while the stick is down is undone where the stick is made up.)
                    // Nothing is ever added to the player's up and down: a made-up move there reads as a pop or a flip.
                    // It is only shortened so the turn can pass 45 degrees: freely while the stick is up (nose grind),
                    // to no less than 0.6 while it is down (5-0), where letting go of the stick is the pop.
                    float sent[3]{};
                    DingoSDKOverlayGrindStickSent(sent);
                    const float own_y = sent[2];
                    float stick_x = std::clamp(s.grind_heading / 1.66f, -1.0f, 1.0f), y_scale = 1.0f; // flat: about 95 degrees at the end
                    if (std::abs(own_y) > 0.3f) {
                        const float heading = std::clamp(s.grind_heading, -1.4f, 1.4f);
                        y_scale = std::max(own_y > 0 ? 0.17f : 0.6f, std::cos(heading));
                        stick_x = std::clamp(std::tan(heading) * y_scale, -1.0f, 1.0f);
                    }
                    DingoSDKOverlaySetGrindStickTurn(true, stick_x, 0.0f, y_scale);
                    s.grind_frame_cos = stick_x; // for the log: the stick's sideways part as asked for
                    s.grind_frame_sin = y_scale;
                } else {
                    s.grind_yaw = s.grind_heading = 0.0f;
                    DingoSDKOverlaySetGrindStickTurn(false, 0.0f, 0.0f, 1.0f);
                }
                s.grind_yaw_at = turning ? turn_now : 0;
            }
            {
                auto modes = s.grind_mode, live = s.grind_live;
                if (extras_blocked()) modes.fill(-1);
                std::array<bool, grind_switches.size()> giving_back{};
                for (std::size_t i = 0; i < modes.size(); ++i)
                    if (modes[i] < 0 && s.grind_stock[i] >= 0) {
                        modes[i] = s.grind_stock[i]; // released: the game's own value back, once
                        giving_back[i] = true;
                    }
                hold_grind_switches(base, static_cast<std::uintptr_t>(s.entity), modes.data(), live.data());
                // Given back once the skater's value was there to write (no page yet: tried again next tick).
                for (std::size_t i = 0; i < modes.size(); ++i)
                    if (giving_back[i] && live[i] >= 0) s.grind_stock[i] = -1;
                for (std::size_t i = 0; i < modes.size(); ++i)
                    if (s.grind_mode[i] >= 0 && !extras_blocked() && s.grind_stock[i] < 0 && live[i] >= 0) s.grind_stock[i] = live[i]; // read before the first write
                if (live != s.grind_live) {
                    // <dev>
                    for (std::size_t i = 0; i < live.size(); ++i)
                        if (dev_capture && live[i] != s.grind_live[i])
                            say(logging::Level::info, std::format("Trainer capture: {} {} -> {}, physics state {}.", grind_switches[i].state, s.grind_live[i], live[i],
                                                                  watched_physics_state().state));
                    // </dev>
                    s.grind_live = live;
                    s.view_due = true;
                }
            }
            set_pump_power(client, s.entity, extras_blocked() ? 1.0f : s.pump_power, pump.found && pump.pumping, measure);
            // <dev>
            if (s.pump_log) {
                // One line a pump, when it ends: what the game's pump gained and what the trainer added.
                const auto report = pump_power_report();
                const bool on = pump.found && pump.pumping;
                if (on) {
                    s.pump_strength = std::max(s.pump_was ? s.pump_strength : 0.0f, pump.strength);
                    s.pump_gained = report.gained;
                    s.pump_given = report.given;
                    s.pump_speed = report.speed;
                    s.pump_steps = report.steps;
                } else if (s.pump_was) {
                    const float speed = std::max(s.pump_speed, 1.0f);
                    say(logging::Level::info, std::format("Trainer pumped #{}: the game's pump gained {:+.2f} J/kg ({:+.2f} m/s at {:.1f} m/s), the trainer added {:+.2f} J/kg "
                                                          "({:+.2f} m/s) over {} steps; power x{:.2f}.",
                                                          report.pumps, s.pump_gained, s.pump_gained / speed, speed, s.pump_given, s.pump_given / speed, s.pump_steps,
                                                          s.pump_power));
                }
                s.pump_was = on;
                if (!pump.found && report.pumps == s.pump_seen) {
                    static std::uint64_t told{};
                    if (now >= told) {
                        told = now + 10000;
                        say(logging::Level::info, "Trainer pump log: the game's pumping states are not found yet (they come with the memory search).");
                    }
                }
            }
            // </dev>
        }
        drive_transition(base, client, now);
        // The revert boost adds speed, so it keeps to the session's rule for boosts and to a host's physics.
        set_revert_boost(client, s.entity, s.enforced || (multiplayer_session_active() && !session_boosts_allowed()) ? 0.0f : s.revert_boost, s.revert);
        if (const auto report = revert_boost_report(); report.sequence != s.revert_boosts_seen) {
            s.revert_boosts_seen = report.sequence;
            // Every landing with some spin in it, boosted or not, so a report can say why not.
            if ((s.hud_jump || s.logging) && report.spin >= 30.0f)
                say(logging::Level::info,
                    std::format("Trainer revert: landed after {} ms with {:.0f} deg of spin, board {} off the body and {:.0f} deg off the travel, board turned {:.0f} deg past the body, state {}, "
                                "{:.1f} m/s: {}{}.",
                                report.air_ms, report.spin, report.board_read ? std::format("{:.0f} deg", report.board_offset) : std::string("unread"), report.slip,
                                report.board_rotation, report.landed_state, report.speed, report.outcome,
                                report.added > 0 ? std::format(" +{:.1f} m/s", report.added) : std::string()));
        }
        for (TrickLaunch launch; take_trick_launch(launch);)
            if (launch.factor != 1.0f)
                say(logging::Level::info, std::format("Trainer trick: {} launched at {:.2f} m/s up, x{:.2f}.",
                                                      launch.trick == Trick::boneless ? "boneless" : "no comply", launch.up_speed, launch.factor));
        // <dev>
        run_sweep(now);
        run_dev_commands(now);
        (void)start_push_probe(base);
        if (s.state_log) {
            static std::uint64_t seen = 0, next = 0;
            PushSample p;
            if (const auto calls = push_sample(p); calls != seen && now >= next) {
                next = now + 300;
                say(logging::Level::info,
                    std::format("Trainer push ({} calls): target {:.2f} (default {:.2f}), current {:.2f}, speed {:.2f}, cap {:.2f}, dv {:.2f}..{:.2f}, "
                                "mass {:.2f}, step {:.4f}, impulse {:.3f}, animated {}, cc={:x} dc={:x}.",
                                calls - seen, p.target, p.target_default, p.current, p.speed, p.cap, p.start, p.end, p.mass, p.step,
                                std::sqrt(p.impulse), p.animated ? 1 : 0, p.cc, p.dc));
                seen = calls;
            }
        }
        run_pad();
        // </dev>
        hold(now);
        if (s.extras_due) publish_extras();
        run_selftest(now);
        publish(s.telemetry);
        if (s.view_due) build_view();
        if (s.save_due && now >= s.next_save) save_store();
    } catch (...) {}
}

namespace {
// ---- tricklining -------------------------------------------------------------------------------
// trainer revert <field> <number> | reset: what counts as a revert and what it is worth.
std::string revert_command(const std::vector<std::string> &a) {
    auto &s = state();
    const auto name = a.empty() ? std::string{} : lower(a[0]);
    std::string why;
    if (!editable(&why)) return "error: " + why;
    if (name == "reset") {
        s.revert = {};
        changed();
        return "Bends are judged the way they shipped: 90 degrees of spin and the board 40 degrees off the body (the off-the-travel rule is off).";
    }
    const auto found = std::ranges::find(revert_fields, name, &RevertField::name);
    const auto value = a.size() > 1 ? number(a[1]) : std::nullopt;
    if (found == std::end(revert_fields) || !value) return "error: usage: trainer revert spin|slip|twist|bend|full|auto|max_speed|cooldown|min_air <number>, or revert reset";
    s.revert.*found->member = static_cast<float>(*value);
    s.revert = sane_revert(s.revert);
    changed();
    return std::format("Revert {} = {:.4g}", name, s.revert.*found->member);
}
// The values that make a trick line: one command sets them the way Skate 3 had them, or puts the
// game's own back. Skate 3's are its tuning for what this game still shares with it (the "Skate 3"
// preset) and speed back out of reverts; its heavier revert and powerslide friction is a first
// guess (x3 and x1.5), to be tuned by playing.
constexpr std::pair<std::string_view, double> trickline_friction[]{
    {"onboard_powerslide.frictionscalar_revert", 3.0}, {"onboard_powerslide.frictionscalar_autorevert", 3.0}, {"onboard_powerslide.frictionscalar_slide", 1.5}};
// trainer feel stock|easy|normal|hardcore: which game the skating plays like. The three Skate 3
// choices are its difficulty settings (the built-in presets of those names); only one is on.
constexpr std::pair<std::string_view, std::string_view> feels[]{{"easy", "Skate 3 Easy"}, {"normal", "Skate 3"}, {"hardcore", "Skate 3 Hardcore"}};
std::string feel_command(const std::vector<std::string> &a) {
    const auto which = a.empty() ? std::string{} : lower(a[0]);
    std::string why;
    if (!editable(&why)) return "error: " + why;
    const auto found = std::ranges::find(feels, which, [](const auto &feel) { return feel.first; });
    if (found == std::end(feels) && which != "stock" && which != "skate") return "error: usage: trainer feel stock|easy|normal|hardcore";
    // Only a feel that is on is taken off, and of that only the values that still hold its numbers: taking off more
    // would put back values the player set themselves that it happens to share (the push speed dial, for one).
    for (const auto &feel : feels)
        if (std::ranges::find(state().active, feel.second) != state().active.end() || builtin_on(feel.second)) (void)remove_preset(feel.second, true);
    if (found != std::end(feels)) (void)apply_preset(found->second);
    changed();
    return found == std::end(feels) ? "Plays like skate.: the game's own tuning." : std::format("Plays like {}: its tuning for everything this game shares with it.", found->second);
}
// trainer trickline extras on|off: what Skate 3's trick lines had beyond its tuning. Speed back
// out of reverts, heavier revert and powerslide friction (x3 and x1.5: a first guess), and higher
// footplants (no comply and boneless x1.5, OVM's ask: a first guess too).
constexpr float trickline_footplant = 1.5f;
std::string trickline_command(const std::vector<std::string> &a) {
    auto &s = state();
    std::string why;
    if (!editable(&why)) return "error: " + why;
    bool on{};
    if (a.size() < 2 || lower(a[0]) != "extras" || !flag(a[1], on)) return "error: usage: trainer trickline extras on|off";
    // Off puts back only what still holds the extras' own numbers: a value the player has since moved is theirs.
    for (const auto &[key, factor] : trickline_friction)
        if (auto *e = find_entry(key); e && !is_locked(*e) && (on || std::abs(e->value - sane(*e, e->stock * factor)) < 1e-6 * std::max(1.0, std::abs(e->value))))
            set_entry(*e, on ? e->stock * factor : e->stock);
    s.revert_boost = on ? std::max(s.revert_boost, 1.0f) : s.revert_boost == 1.0f ? 0.0f : s.revert_boost;
    s.nocomply_height = on ? std::max(s.nocomply_height, trickline_footplant) : s.nocomply_height == trickline_footplant ? 1.0f : s.nocomply_height;
    s.boneless_height = on ? std::max(s.boneless_height, trickline_footplant) : s.boneless_height == trickline_footplant ? 1.0f : s.boneless_height;
    changed();
    return on ? "Tricklining extras on: the board bending boost, heavier revert and powerslide friction, no comply and boneless height x 1.5."
              : "Tricklining extras off.";
}
// trainer transition | transition feel skate|skate3 | transition lean|carry|drop|unstick|lockout|plant <n> |
// transition spine on|off | transition <setting>|all game|off|on
std::string transition_command(const std::vector<std::string> &a) {
    auto &s = state();
    const auto arg = [&](std::size_t i) { return i < a.size() ? a[i] : std::string{}; };
    const auto what = lower(arg(0)), to = lower(arg(1));
    std::string why;
    if (what.empty()) {
        const auto now = GetTickCount64();
        std::string out = std::format("Lean into the ramp {:.2f}, speed kept along the coping {:.2f} (at most {:.2f} m/s across it), hang-up push {:.2f}, no catching the coping again for "
                                      "{:.2f} s, flights clear the coping {}, handplant exit {:.2f}.",
                                      s.tr_lean, s.tr_carry, s.tr_drop, s.tr_unstick, s.tr_lockout, s.tr_spine ? "on" : "off", s.tr_plant);
        out += std::format("\nsteep ground {} ms ago, way out of the wall {:.2f} {:.2f}; on a coping: {}; coping {:.2f} {:.2f}, ramp {:.2f} {:.2f}; air grind reach held off: {}; "
                           "lean polls given {}",
                           s.tr.steep_at ? now - s.tr.steep_at : 0, s.tr.steep_out[0], s.tr.steep_out[1], s.tr.coping ? "yes" : "no", s.tr.edge[0], s.tr.edge[1], s.tr.ramp[0],
                           s.tr.ramp[1], s.tr.air_grinds_off ? "yes" : "no", DingoSDKOverlayGrindStickLeaned());
        for (std::size_t i = 0; i < transition_settings.size(); ++i)
            out += std::format("\n{} (setting {}, the game's default {}): {}; asked {} times since last listed", transition_settings[i].key, transition_settings[i].setting,
                               transition_settings[i].stock ? "on" : "off", s.tr_setting[i] < 0 ? "game's own" : s.tr_setting[i] ? "forced on" : "forced off",
                               transition_setting_asked(i));
        for (std::size_t i = 0; i < transition_options.size(); ++i)
            out += std::format("\n{} (the game's own option {}, default {}): {}", transition_options[i].key, transition_options[i].setting, transition_options[i].stock, s.tr_option[i]);
        return out;
    }
    if (what == "feel") {
        const bool skate3 = to == "skate3" || to == "3";
        if (!skate3 && to != "skate" && to != "stock") return "error: usage: trainer transition feel skate|skate3";
        if (!editable(&why)) return "error: " + why;
        if (std::ranges::find(s.active, transition_preset) != s.active.end() || builtin_on(transition_preset)) (void)remove_preset(transition_preset, true);
        // Values the preset set in earlier builds of this day and no longer does: back to the game's own where they
        // still hold what it set.
        static constexpr std::pair<std::string_view, double> dropped[]{
            {"physicsreckoning.vertjumpalignmaxgroundnormaly", 0.2}, {"physicsairstates.naturalairmaxdist", 0.12}, {"physicstrajectory.scoremiddlebonus", 40.0},
            {"physicstrajectory.mintimefortrajectorytobevalid", 0.25}, {"physicsmode.pumpeffectfactor", 18.0}, {"physicsmode.pumpmaxacceleration", 10.0},
            {"physicsmode.unintentionalpumpscalar", 0.7}, {"physicspumping.pumpangle", 50.0}, {"physicspumping.pumpminfactor", 0.01}};
        for (const auto &[key, value] : dropped)
            if (auto *e = find_entry(key); e && e->touched && !is_locked(*e) && std::abs(e->value - value) < 1e-4) set_entry(*e, e->stock);
        reset_transition();
        if (skate3) {
            s.tr_lean = skate3_tr_lean;
            s.tr_carry = skate3_tr_carry;
            s.tr_drop = skate3_tr_drop;
            s.tr_unstick = skate3_tr_unstick;
            s.tr_lockout = skate3_tr_lockout;
            // "Airs clear the coping" is not part of it: the one session it was on in, every popped air landed on the deck.
            s.tr_plant = skate3_tr_plant;
            for (std::size_t i = 0; i < transition_settings.size(); ++i) s.tr_setting[i] = transition_settings[i].skate3;
            (void)apply_preset(transition_preset);
        }
        changed();
        return std::string{skate3 ? "Transition like Skate 3: lean into the ramp, roll off the coping at the angle you came in, no hang-ups, Skate 3's handplant exit, its "
                                    "coping exit timers. Airs and pumping stay skate.'s own."
                                  : "Transition like skate.: everything is the game's own."};
    }
    struct Number {
        std::string_view key;
        float *value;
        float low, high;
        std::string_view says;
    };
    const Number numbers[]{
        {"lean", &s.tr_lean, -1.0f, 1.0f, "Lean into the ramp on a coping grind"},
        {"carry", &s.tr_carry, 0.0f, 1.0f, "Speed kept along the coping rolling off it"},
        {"drop", &s.tr_drop, 0.0f, 100.0f, "Most speed across the coping while it is kept (m/s)"},
        {"unstick", &s.tr_unstick, 0.0f, 100.0f, "Push out of a hang-up"},
        {"lockout", &s.tr_lockout, 0.0f, 60.0f, "Seconds the coping cannot be caught again"},
        {"plant", &s.tr_plant, 0.0f, 1.0f, "Handplant exit like Skate 3's"},
    };
    for (const auto &one : numbers) {
        if (what != one.key) continue;
        const auto value = number(arg(1));
        if (!value) return std::format("error: usage: trainer transition {} <number> ({} to {}; 0 = off)", one.key, one.low, one.high);
        if (*value != 0 && !editable(&why)) return "error: " + why;
        *one.value = std::clamp(static_cast<float>(*value), one.low, one.high);
        changed();
        return std::format("{}: {:.2f}.", one.says, *one.value);
    }
    for (std::size_t i = 0; i < transition_options.size(); ++i) {
        // The game's own options: the same value its Settings menu shows and sets.
        const auto &one = transition_options[i];
        if (what != one.key) continue;
        bool on{};
        const auto value = one.boolean ? (flag(arg(1), on) ? std::optional<double>(on ? 1.0 : 0.0) : std::nullopt) : number(arg(1));
        if (!value) return std::format("{} (the game's own option {}): {}. Set it with: trainer transition {} {}", one.label, one.setting, s.tr_option[i], one.key,
                                       one.boolean ? "on|off" : "<number>");
        if (!profile_runtime::set_local_game_option(one.setting, one.boolean ? *value : std::round(*value), one.boolean))
            return "error: the game's settings could not be written (no local profile is active).";
        s.tr_option_at = 0;
        return std::format("{} = {} (the game's own option {}: its Settings menu shows the same).", one.label, one.boolean ? (*value != 0 ? "on" : "off") : std::format("{:.0f}", std::round(*value)),
                           one.setting);
    }
    if (what == "spine") {
        bool on{};
        if (!flag(arg(1), on)) return "error: usage: trainer transition spine on|off";
        if (on && !editable(&why)) return "error: " + why;
        s.tr_spine = on;
        changed();
        return std::string{on ? "Flights up and over a lip no longer reach for the coping." : "Flights reach for the coping as the game has it."};
    }
    // <dev>
    if (what == "leanframe") {
        const auto x = number(arg(1)), y = number(arg(2));
        if (!x || !y) return "error: usage: trainer transition leanframe <x sign> <y sign>";
        lean_frame_x = static_cast<float>(*x);
        lean_frame_y = static_cast<float>(*y);
        return std::format("Lean stick axes x {:.1f}, y {:.1f}.", lean_frame_x, lean_frame_y);
    }
    // </dev>
    const int mode = to == "on" || to == "1" ? 1 : to == "off" || to == "0" ? 0 : to == "game" || to == "-1" ? -1 : -2;
    if (mode == -2) return "error: usage: trainer transition (lists everything) | feel skate|skate3 | lean|carry|drop|unstick|lockout|plant <n> | spine on|off | <setting>|all game|off|on";
    if (mode >= 0 && !editable(&why)) return "error: " + why;
    bool any = false;
    for (std::size_t i = 0; i < transition_settings.size(); ++i)
        if (what == "all" || what == transition_settings[i].key) {
            s.tr_setting[i] = static_cast<std::int8_t>(mode);
            any = true;
        }
    if (!any) return "error: no transition setting is called \"" + what + "\". `trainer transition` lists them.";
    changed();
    return what + ": " + (mode < 0 ? "the game's own" : mode ? "forced on" : "forced off") + ".";
}
std::string run(std::string_view verb, const std::vector<std::string> &a) {
    auto &s = state();
    const auto v = lower(verb);
    const auto arg = [&](std::size_t i) { return i < a.size() ? a[i] : std::string{}; };
    if (v == "status") {
        std::size_t touched{}, frozen{};
        for (const auto &e : s.entries) { touched += e.touched; frozen += e.frozen; }
        std::string own;
        for (const auto &one : setups())
            if (off_stock(one)) own += std::format(" {}={:.4g}{}", one.key, one.get(), modified(one) ? "" : "(not in force)");
        return std::format("{} {} values ({} changed, {} frozen). Map: {}. Presets: {}. The trainer's own settings off the game's:{}.", s.status, s.entries.size(), touched,
                           frozen, s.map.empty() ? "none" : s.map, s.active.empty() ? "stock" : std::to_string(s.active.size()) + " applied", own.empty() ? " none" : own);
    }
    if (v == "set" || v == "freeze" || v == "reset") {
        if (v == "reset" && lower(arg(0)) == "all") {
            const auto count = reset_all();
            std::size_t locked{};
            for (const auto &e : s.entries) locked += e.touched;
            changed();
            return locked ? std::format("{} values put back; {} locked values kept (Reset everything clears those too).", count, locked)
                          : std::format("{} values put back.", count);
        }
        if (v == "reset" && (lower(arg(0)) == "tricks" || lower(arg(0)).starts_with("section"))) {
            // reset tricks: the sliders and switches of POP AND TRICKS. reset section <name>: everything one section of
            // the Tune tab shows, its tuning values too (locked ones stay).
            // (The console hands `reset` its words as one: "section grinds".)
            const auto words = lower(arg(0)) + (a.size() > 1 ? " " + lower(arg(1)) : std::string{});
            const bool section = words.starts_with("section");
            const auto named = section && words.size() > 8 ? words.substr(8) : std::string{};
            const auto topic = section ? topic_named(named) : topic_pop;
            if (!topic) return "error: usage: trainer reset section pop|speed|bails|grinds|tricklining|transition|pumping|foot";
            std::size_t count = reset_setups(topic);
            if (section)
                for (auto &e : s.entries) {
                    if (!e.touched || is_locked(e)) continue;
                    std::uint8_t of{};
                    (void)essential_name(e.key, nullptr, &of);
                    if (of != topic) continue;
                    set_entry(e, e.stock);
                    ++count;
                }
            changed();
            return std::format("{}: {} put back to the game's own.", section ? std::string(topics[topic].title) : std::string("Tricks"), count);
        }
        if (v == "reset" && lower(arg(0)) == "presets") {
            const auto on = s.active;
            for (const auto &name : on) (void)remove_preset(name);
            s.active.clear();
            changed();
            return std::format("{} presets switched off.", on.size());
        }
        if (v == "reset" && lower(arg(0)) == "everything") {
            // The game as it shipped: no lock survives, no preset stays on, nothing of the trainer's stays set.
            // (Under a host's physics the player's own setup is kept for when they leave.)
            if (s.enforced) return "error: the host controls physics in this multiplayer game; your own setup is kept for when you leave.";
            std::size_t count{};
            for (auto &e : s.entries) e.frozen = false;
            for (auto &e : s.entries) {
                if (e.touched) {
                    set_entry(e, e.stock);
                    ++count;
                } else {
                    remember(e);
                }
            }
            s.saved.clear();
            s.active.clear();
            count += reset_setups();
            // A preset this map applies by itself would be back at its next load.
            if (!s.map.empty()) current_map().preset.clear();
            changed();
            return std::format("Everything is the game's own again: {} values and settings put back, locks cleared, presets off.", count);
        }
        auto *e = find_entry(arg(0));
        if (!e) return "error: no tuning value is called \"" + arg(0) + "\". Try: trainer find <text>";
        std::string why;
        if (v != "freeze" && !editable(&why)) return "error: " + why;
        if (v == "set") {
            const auto value = number(arg(1));
            if (!value) return "error: " + e->id + " needs a number.";
            set_entry(*e, *value);
        } else if (v == "freeze") {
            if (!flag(arg(1), e->frozen)) return "error: freeze needs 0 or 1.";
            remember(*e);
        } else {
            set_entry(*e, e->stock);
        }
        changed();
        return std::format("{} = {:.6g}{}{}", e->id, e->value, e->touched ? std::format(" (stock {:.6g})", e->stock) : "", e->frozen ? ", frozen" : "");
    }
    if (v == "find") {
        const auto pattern = lower(arg(0));
        std::string result;
        std::size_t count{};
        for (const auto &e : s.entries)
            if (matches(e.key, pattern) && ++count <= 12) result += std::format("{}{} = {:.6g}", result.empty() ? "" : "; ", e.id, e.value);
        return count ? std::format("{} match: {}{}", count, result, count > 12 ? "; ..." : "") : "Nothing matches.";
    }
    if (v == "dial") {
        // trainer dial <multiplier> <built-in preset>
        const auto factor = number(arg(0));
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (!factor || name.empty()) return "error: usage: trainer dial <multiplier> <preset name>";
        std::string why;
        if (!editable(&why)) return "error: " + why;
        return apply_dial(name, *factor);
    }
    if (v == "preset") {
        const auto action = lower(arg(0));
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (action == "import") return import_preset(name);
        if (name.empty()) return "error: usage: trainer preset apply|remove|save|delete|export <name>, or preset import [file]";
        if (action == "export") return export_preset(name);
        if (action == "remove") {
            std::string why;
            if (!editable(&why)) return "error: " + why;
            return remove_preset(name);
        }
        if (action == "apply") {
            std::string why;
            if (!editable(&why)) return "error: " + why;
            return apply_preset(name);
        }
        if (action == "save") {
            if (name.size() > 48) return "error: that name is too long.";
            for (const auto &preset : builtin_presets())
                if (lower(preset.name) == lower(name)) return "error: that name belongs to a built-in preset.";
            if (lower(name) == "stock" || lower(name) == "map") return "error: that name is reserved.";
            std::map<std::string, double, std::less<>> values;
            for (const auto &e : s.entries)
                if (e.touched) values[e.key] = e.value;
            for (const auto &one : setups())
                if (off_stock(one)) values[one.key] = one.get();
            if (values.empty()) return "error: nothing is changed, so there is nothing to save.";
            const auto count = values.size();
            s.user[name] = std::move(values);
            changed();
            return std::format("Saved \"{}\" ({} values).", name, count);
        }
        if (action == "delete") {
            if (!s.user.erase(name)) return "error: no preset of yours is called \"" + name + "\".";
            std::erase(s.active, name);
            changed();
            return "Deleted \"" + name + "\".";
        }
        return "error: usage: trainer preset apply|save|delete <name>";
    }
    if (v == "revert") return revert_command(a);
    if (v == "trickline") return trickline_command(a);
    if (v == "feel") return feel_command(a);
    if (v == "slot") {
        const auto slot = slot_argument(a, 0);
        if (!slot) return std::format("error: slots are 1 to {}.", marker_slots);
        s.slot = *slot;
        changed();
        return std::format("Marker slot {}{}.", s.slot + 1, current_map().markers[static_cast<std::size_t>(s.slot)].set ? "" : " (empty)");
    }
    if (v == "marker") {
        const auto action = lower(arg(0));
        const auto slot = slot_argument(a, 1);
        if (!slot) return std::format("error: slots are 1 to {}.", marker_slots);
        if (s.map.empty()) return "error: load a level first.";
        auto &marker = current_map().markers[static_cast<std::size_t>(*slot)];
        if (action == "save") {
            if (!s.telemetry.skater) return "error: no skater to mark.";
            marker = {true, s.telemetry.position};
            changed();
            return std::format("Marker {} saved at ({:.1f}, {:.1f}, {:.1f}).", *slot + 1, marker.position[0], marker.position[1], marker.position[2]);
        }
        if (action == "go") {
            if (!marker.set) return std::format("error: marker {} is empty.", *slot + 1);
            return go_to(marker.position, std::format("marker {}", *slot + 1));
        }
        if (action == "clear") {
            marker = {};
            changed();
            return std::format("Marker {} cleared.", *slot + 1);
        }
        return "error: usage: trainer marker save|go|clear [slot]";
    }
    if (v == "ground") {
        const auto x = number(arg(0)), z = number(arg(1));
        if (!x || !z) return "error: usage: trainer ground <x> <z>  (teleport to the topmost surface there)";
        return land_at(static_cast<float>(*x), static_cast<float>(*z), s.telemetry.position[1], "the ground there");
    }
    if (v == "waypoint") {
        refresh_waypoint();
        if (lower(arg(0)) == "info")
            return "Map waypoint: " + (s.waypoint ? std::format("{:.1f}, {:.1f}, {:.1f}", (*s.waypoint)[0], (*s.waypoint)[1], (*s.waypoint)[2]) : std::string("none")) +
                   " (" + s.waypoint_detail + ").";
        if (!s.waypoint) return "error: no waypoint on the map. Open the pause map and place one first.";
        return land_at((*s.waypoint)[0], (*s.waypoint)[2], (*s.waypoint)[1], "your map waypoint");
    }
    if (v == "transition") return transition_command(a);
    if (v == "grind") {
        // trainer grind | grind watch 0|1 | grind side <x> | grind <key>|all game|off|on
        const auto what = lower(arg(0)), to = lower(arg(1));
        const auto word = [](std::int8_t value) { return value < 0 ? "-" : value ? "on" : "off"; };
        if (what.empty()) {
            std::string out = std::format("Side force off grinds x {:.2f}.", s.grind_side);
            for (std::size_t i = 0; i < grind_settings.size(); ++i)
                out += std::format("\n{} (setting {}, the game's default {}): {}; asked {} times since last listed", grind_settings[i].key, grind_settings[i].setting,
                                   grind_settings[i].stock ? "on" : "off",
                                   s.grind_setting[i] < 0 ? "game's own" : s.grind_setting[i] ? "forced on" : "forced off", grind_setting_asked(i));
            for (std::size_t i = 0; i < grind_switches.size(); ++i)
            {
                std::uint32_t handle{};
                const int status = grind_switch_status(s.base, static_cast<std::uintptr_t>(s.entity), i, handle);
                std::uint32_t asked{}, answered{};
                grind_switch_asked(i, asked, answered);
                out += std::format("\n{} ({}): {}, the game reads {} [{}, handle {:x}; asked {} times since last listed, {} answered held]", grind_switches[i].key,
                                   grind_switches[i].state, s.grind_mode[i] < 0 ? "game's own" : s.grind_mode[i] ? "held on" : "held off", word(s.grind_live[i]),
                                   status == 0 ? "state not found" : status == 1 ? "no handle" : status == 2 ? "no value for this skater" : "ok", handle, asked, answered);
            }
            return out;
        }
        if (what == "feel") {
            // trainer grind feel skate|skate3: the game's own grinds, or without what skate. added to leaving a grind.
            std::string why;
            const bool skate3 = to == "skate3" || to == "3";
            if (!skate3 && to != "skate" && to != "stock") return "error: usage: trainer grind feel skate|skate3";
            if (skate3 && !editable(&why)) return "error: " + why;
            for (std::size_t i = 0; i < grind_switches.size(); ++i) s.grind_mode[i] = skate3 ? skate3_grind_mode(i) : std::int8_t{-1};
            for (std::size_t i = 0; i < grind_settings.size(); ++i) s.grind_setting[i] = skate3 ? grind_settings[i].skate3 : std::int8_t{-1};
            s.grind_push = skate3 ? 1.8f : 0.0f; // Skate 3's pop goes sideways with the left stick, 1.8 m/s at full stick
            // Skate 3's left stick turns the board; the right stick only tips it (5-0, nose grind) and pops.
            s.grind_range = skate3 ? skate3_grind_range : 0.0f;
            changed();
            return std::string{skate3 ? "Grinds like Skate 3: lean exits and skate.'s own ways off a grind off, the pop goes sideways with the stick."
                                      : "Grinds like skate.: every setting and switch is the game's own."};
        }
        if (what == "watch") {
            s.grind_watch = to == "1" || to == "on";
            return std::string{s.grind_watch ? "Reading the grind switches." : "No longer reading the grind switches."};
        }
        std::string why;
        if (what == "turn" || what == "tip" || what == "lean") return "That lever is gone: it did nothing. `trainer grind range <degrees>` is the left stick's turn.";
        if (what == "range") {
            const auto value = number(arg(1));
            if (!value) return "error: usage: trainer grind range <degrees>, 0 = off (the game's own turn, by the right stick); negative turns the other way.";
            if (*value != 0 && !editable(&why)) return "error: " + why;
            s.grind_range = std::clamp(static_cast<float>(*value), -360.0f, 360.0f);
            changed();
            return std::format("Left stick turns the board on a grind: up to {:.1f} degrees{}.", s.grind_range, s.grind_range == 0 ? " (off)" : "");
        }
        if (what == "push") {
            const auto value = number(arg(1));
            if (!value) return "error: usage: trainer grind push <m/s at full stick>, 0 = none, 1.8 = Skate 3's; a negative number pushes the other way.";
            if (*value != 0 && !editable(&why)) return "error: " + why;
            s.grind_push = std::clamp(static_cast<float>(*value), -50.0f, 50.0f);
            changed();
            return std::format("Sideways pop with the stick: {:.2f} m/s at full stick.", s.grind_push);
        }
        if (what == "side") {
            const auto value = number(arg(1));
            if (!value || *value < 0) return "error: usage: trainer grind side <multiplier>, 1 = the game's own.";
            if (!editable(&why)) return "error: " + why;
            s.grind_side = std::clamp(static_cast<float>(*value), 0.0f, 1000.0f);
            changed();
            return std::format("Side force off grinds x {:.2f}.", s.grind_side);
        }
        const int mode = to == "on" || to == "1" ? 1 : to == "off" || to == "0" ? 0 : to == "game" || to == "-1" ? -1 : -2;
        if (mode == -2) return "error: usage: trainer grind (lists everything) | feel skate|skate3 | side <multiplier> | push <m/s> | range <degrees> | <setting or switch>|all game|off|on";
        if (mode >= 0 && !editable(&why)) return "error: " + why;
        bool any = false;
        for (std::size_t i = 0; i < grind_settings.size(); ++i)
            if (what == "all" || what == grind_settings[i].key) {
                s.grind_setting[i] = static_cast<std::int8_t>(mode);
                any = true;
            }
        for (std::size_t i = 0; i < grind_switches.size(); ++i)
            if (what == "all" || what == grind_switches[i].key) {
                if (grind_switches[i].read_only && mode >= 0) {
                    if (what != "all") return "error: " + what + " can only be watched: held, it leaves the skater in the grind state on the ground.";
                    continue;
                }
                s.grind_mode[i] = static_cast<std::int8_t>(mode);
                any = true;
            }
        if (!any) return "error: no grind switch is called \"" + what + "\". `trainer grind` lists them.";
        changed();
        return what + ": " + (mode < 0 ? "the game's own" : mode ? "held on" : "held off") + ".";
    }
    if (v == "tp") {
        const auto x = number(arg(0)), y = number(arg(1)), z = number(arg(2));
        if (!x || !y || !z) return "error: usage: trainer tp <x> <y> <z>";
        return go_to({static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z)}, "that point");
    }
    if (v == "spot") {
        const auto index = number(arg(0));
        if (!index || *index < 1 || *index > static_cast<double>(s.map_file.spots.size())) return "error: this map has no such spot.";
        const auto &spot = s.map_file.spots[static_cast<std::size_t>(*index) - 1];
        return go_to(spot.position, spot.name);
    }
    if (v == "option") {
        const auto name = lower(arg(0));
        bool on{};
        std::string why;
        if (name == "flip_speed" || name.starts_with("flip.")) {
            const auto value = number(arg(1));
            auto *option = trick_option(name);
            if (!option) return "error: no such flip trick. flip_speed is all of them; flip.kickflip, flip.360flip ... are one each.";
            if (!value) return "error: " + name + " needs a multiplier, 1 = the game's own speed.";
            if (!editable(&why)) return "error: " + why;
            *option = std::clamp(static_cast<float>(*value), flip_low, flip_high);
        } else if (name == "hippy_height" || name == "nocomply_height" || name == "boneless_height" || name == "offboard_height" || name == "manual_height") {
            const auto value = number(arg(1));
            if (!value) return "error: " + name + " needs a multiplier, 1 = the game's own height.";
            if (!editable(&why)) return "error: " + why;
            (name == "hippy_height" ? s.hippy_height : name == "nocomply_height" ? s.nocomply_height : name == "offboard_height" ? s.offboard_height
             : name == "manual_height" ? s.manual_height : s.boneless_height) =
                std::clamp(static_cast<float>(*value), height_low, height_high);
        } else if (name == "pump_power") {
            const auto value = number(arg(1));
            if (!value) return "error: pump_power needs a multiplier, 1 = what the game's own pump gains.";
            if (!editable(&why)) return "error: " + why;
            s.pump_power = std::clamp(static_cast<float>(*value), 0.0f, pump_high);
        } else if (name == "revert_boost") {
            const auto value = number(arg(1));
            if (!value) return "error: revert_boost needs a strength: 0 is off, 1 gives back about 2 m/s on an auto revert.";
            if (!editable(&why)) return "error: " + why;
            s.revert_boost = std::clamp(static_cast<float>(*value), 0.0f, revert_boost_high);
        } else if (name == "catch_percent") {
            return "That setting is gone with the switch it belonged to.";
        } else if (name == "return_delay") {
            const auto value = number(arg(1));
            if (!value) return "error: return_delay needs seconds.";
            s.return_delay = std::clamp(static_cast<float>(*value), 0.0f, 10.0f);
        } else if (!flag(arg(1), on)) {
            return "error: usage: trainer option hud|hud_jump|auto_return|pad|pad_menu|flip_advanced|flip_pop|flip_flick|log 0|1";
        } else if (name == "hud") s.hud = on;
        else if (name == "hud_jump") s.hud_jump = on;
        else if (name == "auto_return") s.auto_return = on;
        else if (name == "pad") s.pad_shortcuts = on;
        else if (name == "pad_menu") s.pad_menu = on;
        else if (name == "flip_gate") return "That switch is gone: a flip set slower than the game's own stays slow by itself.";
        else if (name == "catch_at") return "That switch is gone: it did not work reliably. Use the flip trick speed.";
        else if (name == "flip_pop") {
            if (on && !editable(&why)) return "error: " + why;
            s.flip_pop = on;
        } else if (name == "flip_flick") {
            if (on && !editable(&why)) return "error: " + why;
            s.flip_flick = on;
        } else if (name == "flip_advanced") {
            if (on && !editable(&why)) return "error: " + why;
            s.flip_advanced = on;
        }
        // <dev>
        else if (name == "statelog") s.state_log = on;
        else if (name == "fliptrace") s.flip_trace = on;
        else if (name == "footlog") dev_foot = {.on = on};
        else if (name == "capture") dev_capture = on;
        else if (name == "pumplog") {
            s.pump_log = on;
            s.pump_started = s.pump_rising = false;
            s.pump_serial = 0;
        }
        // </dev>
        else if (name == "log") {
            set_logging(on);
            if (on && !s.logging) return "error: the telemetry file could not be created.";
        } else return "error: no option is called \"" + name + "\".";
        changed();
        return name + " = " + arg(1);
    }
    if (v == "profile") {
        const auto action = lower(arg(0));
        if (s.map.empty()) return "error: load a level first.";
        if (action == "clear") {
            current_map().preset.clear();
            changed();
            return "This map no longer applies a preset on load.";
        }
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (action != "set" || name.empty()) return "error: usage: trainer profile set <preset> | clear";
        current_map().preset = name;
        changed();
        return "\"" + name + "\" will be applied whenever this map loads.";
    }
    if (v == "fold") {
        bool open{};
        if (arg(0).empty() || arg(0).size() > 64 || !flag(arg(1), open)) return "error: usage: trainer fold <section or card> 0|1";
        s.folds[arg(0)] = open;
        s.save_due = true;
        s.view_due = true;
        return std::string{};
    }
    if (v == "open") {
        const auto tab = lower(arg(0));
        // After "movement": a section of the Tune tab, opened and scrolled to (realistic, fun: lists of earlier versions).
        const std::array<std::string_view, 19> tabs{"tune", "presets", "practice", "map", "realistic", "fun", "everything", "movement", "trickline", "transition",
                                                    "pop", "speed", "bails", "grinds", "pumping", "foot", "tricklining", "tricks", "game"};
        const auto found = std::ranges::find(tabs, tab);
        if (!tab.empty() && found == tabs.end())
            return "error: usage: trainer open [tune|practice|map|movement|presets|game|pop|speed|bails|grinds|tricklining|transition|pumping|foot|everything]";
        s.open_tab = tab.empty() ? 0 : static_cast<int>(found - tabs.begin());
        ++s.open_serial;
        s.view_due = true;
        return "Opening the trainer.";
    }
    if (v == "jumps") {
        const auto &j = s.telemetry.last;
        if (!j.serial) return "No jump measured yet.";
        return std::format("Jump {}: takeoff {:.1f} km/h at {:.1f} deg, air {:.2f} s, height {:.2f} m, distance {:.2f} m, drop {:.2f} m, landing {:.1f} km/h.",
                           j.serial, j.takeoff_speed * 3.6f, j.takeoff_angle, j.air_time, j.height, j.distance, j.drop, j.landing_speed * 3.6f);
    }
    if (v == "where") {
        if (!s.telemetry.skater) return "error: no skater.";
        const auto &p = s.telemetry.position;
        return std::format("({:.2f}, {:.2f}, {:.2f}) heading {:.0f}, {:.1f} km/h. Blender: ({:.2f}, {:.2f}, {:.2f})", p[0], p[1], p[2],
                           s.telemetry.heading, s.telemetry.speed * 3.6f, p[0], -p[2], p[1]);
    }
    // <dev>
    if (v == "watchw") {
        // trainer watchw <hex offset in the skater context> | off: which code writes that field?
        if (lower(arg(0)) == "spin") {
            // The board's angular velocity (body 0, +0x90): who turns the board in a flip trick?
            try {
                const auto bodies = client_source::detail::debug_noclip_bodies(s.base, s.client, s.entity);
                if (bodies.parts.empty()) return "error: no bodies.";
                std::uintptr_t axis{};
                const auto text = arg(1);
                if (!text.empty()) (void)std::from_chars(text.data(), text.data() + text.size(), axis, 16);
                return std::format("Watching writes of the board's turn rate (+0x{:x}) in {} threads.", 0x90 + axis,
                                   watch_writes(bodies.parts[0] + 0x90 + axis, s.base));
            } catch (...) { return "error: the skater's bodies could not be read."; }
        }
        if (lower(arg(0)) == "body") {
            try {
                const auto bodies = client_source::detail::debug_noclip_bodies(s.base, s.client, s.entity);
                if (bodies.parts.empty()) return "error: no bodies.";
                return std::format("Watching writes of body velocity z ({} parts, off board {}) in {} threads.", bodies.parts.size(),
                                   bodies.offboard ? 1 : 0, watch_writes(bodies.parts[0] + 0x78, s.base));
            } catch (...) { return "error: the skater's bodies could not be read."; }
        }
        if (lower(arg(0)) != "off") {
            std::uintptr_t context{}, offset{};
            try {
                context = client_source::detail::debug_noclip_bodies(s.base, s.client, s.entity).context;
            } catch (...) {}
            const auto text = arg(0);
            if (!context || std::from_chars(text.data(), text.data() + text.size(), offset, 16).ec != std::errc{}) return "error: no skater, or not a hex offset.";
            return std::format("Watching writes of context+0x{:x} in {} threads.", offset, watch_writes((context + offset) | (arg(1) == "r" ? 1u : 0u), s.base));
        }
        (void)watch_writes(0, s.base);
        std::array<Writer, 32> found{};
        const auto count = writers(found.data(), found.size());
        std::string result = std::format("{} writers:", count);
        for (std::size_t i = 0; i < count; ++i) {
            result += std::format(" {:x} x{} ({:.3f})", found[i].rva, found[i].count, found[i].last);
            for (std::size_t which = 0; which < 3; ++which) {
                std::array<std::uintptr_t, 10> frames{};
                const auto depth = writer_stack(i, which, frames.data(), frames.size());
                if (!depth) break;
                result += " [";
                for (std::size_t f = 0; f < depth; ++f) result += std::format("{}{:x}", f ? " < " : "", frames[f]);
                result += "]";
            }
        }
        return result;
    }
    if (v == "flipext") {
        bool on{};
        if (!flag(arg(0), on)) return "error: usage: trainer flipext 0|1";
        dev_slow_extensions = on;
        s.flip_applied = -1; // the catch times are set again on the next tick
        return std::format("Catch extension times {} the flip speed.", on ? "follow" : "ignore");
    }
    if (v == "fakesession") {
        // trainer fakesession enforced|boosts|off: what the menu says in a session, shown without one.
        const auto what = lower(arg(0));
        dev_fake_session = what == "enforced" ? 1 : what == "boosts" ? 2 : 0;
        changed(false);
        return std::format("The menu shows session state {}.", dev_fake_session);
    }
    if (v == "flipgate") {
        // trainer flipgate [seconds]: the copies of the flick warp limit's number, and a new value for them.
        std::array<std::uintptr_t, 32> at{};
        const auto count = flick_warp_limits(at.data(), at.size());
        const auto value = number(arg(0));
        std::string result = std::format("{} copies:", count);
        for (std::size_t i = 0; i < std::min(count, at.size()); ++i) {
            float now{};
            (void)memory::peek(at[i], now);
            MEMORY_BASIC_INFORMATION info{};
            (void)VirtualQuery(reinterpret_cast<void *>(at[i]), &info, sizeof(info));
            result += std::format(" {:x}={:.4f}{}", at[i], now, info.Protect == PAGE_READONLY ? "(ro)" : "");
            if (value) result += poke_float(at[i], static_cast<float>(*value)) ? std::format("->{:.4f}", *value) : std::string("->failed");
        }
        return result;
    }
    if (v == "scanb" || v == "scans") {
        // trainer scanb <align> <hex bytes>... | scans <text>: where those bytes sit in memory; then the
        // words around the first hits (scanb <align> ... ctx <words before> <words>).
        std::vector<std::uint8_t> pattern;
        std::size_t align = 1, before = 0, words = 0, first = 0;
        if (v == "scans") {
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (i) pattern.push_back(' ');
                for (const char c : a[i]) pattern.push_back(static_cast<std::uint8_t>(c));
            }
        } else {
            if (a.size() < 2) return "error: usage: trainer scanb <align> <hex byte>... [ctx <words before> <words> [first hit]]";
            (void)std::from_chars(a[0].data(), a[0].data() + a[0].size(), align, 10);
            std::size_t i = 1;
            for (; i < a.size() && lower(a[i]) != "ctx"; ++i) {
                unsigned byte{};
                if (std::from_chars(a[i].data(), a[i].data() + a[i].size(), byte, 16).ec != std::errc{} || byte > 255) return "error: hex bytes.";
                pattern.push_back(static_cast<std::uint8_t>(byte));
            }
            if (i + 2 < a.size()) {
                (void)std::from_chars(a[i + 1].data(), a[i + 1].data() + a[i + 1].size(), before, 10);
                (void)std::from_chars(a[i + 2].data(), a[i + 2].data() + a[i + 2].size(), words, 10);
                if (i + 3 < a.size()) (void)std::from_chars(a[i + 3].data(), a[i + 3].data() + a[i + 3].size(), first, 10);
            }
        }
        if (pattern.empty()) return "error: nothing to look for.";
        std::vector<std::uintptr_t> at(4096);
        const auto count = scan_bytes(pattern.data(), pattern.size(), align, at.data(), at.size());
        std::string result = std::format("{} hits:", count);
        const auto shown = std::min(count, at.size());
        for (std::size_t i = first; i < shown && i < first + (words ? 12u : 40u); ++i) {
            MEMORY_BASIC_INFORMATION info{};
            (void)VirtualQuery(reinterpret_cast<void *>(at[i]), &info, sizeof(info));
            result += std::format(" [{:x}{}", at[i], info.Protect == PAGE_READWRITE ? "" : std::format("(p{:x})", info.Protect));
            for (std::size_t k = 0; k < std::min<std::size_t>(words, 40); ++k) {
                std::uint32_t word{};
                if (!memory::peek(at[i] - before * 4 + k * 4, word)) break;
                result += std::format(" {:x}", word);
            }
            result += "]";
        }
        return result;
    }
    if (v == "curvex") {
        // trainer curvex <curve 0..7> <factor>: that curve of the flip trick tuning at the game's outputs times the factor.
        const auto index = number(arg(0)), factor = number(arg(1));
        if (!index || !factor) return "error: usage: trainer curvex <curve 0..7> <factor>";
        return dev_scale_curve(static_cast<std::size_t>(*index), static_cast<float>(*factor));
    }
    if (v == "flipcurves") {
        // trainer flipcurves <hex mask>: which flip trick curves the flip speed scales (bit n = curve n in memory order).
        std::uint32_t mask{};
        const auto text = arg(0);
        if (text.empty() || std::from_chars(text.data(), text.data() + text.size(), mask, 16).ec != std::errc{}) return "error: usage: trainer flipcurves <hex mask>";
        dev_set_flip_curve_mask(mask);
        find_classes();
        return std::format("Flip speed now scales the curves of mask {:x}; searching again.", mask);
    }
    if (v == "setters") {
        std::array<Writer, 32> found{};
        const auto count = velocity_setters(found.data(), found.size());
        std::string result = "velocity setters:";
        for (std::size_t i = 0; i < count; ++i)
            if (found[i].count) result += std::format(" {:x} x{} ({:.2f})", found[i].rva, found[i].count, found[i].last);
        return result;
    }
    if (v == "ctx") {
        std::string result;
        try {
            const auto context = client_source::detail::debug_noclip_bodies(s.base, s.client, s.entity).context;
            for (const auto &text : a) {
                std::uintptr_t offset{};
                if (std::from_chars(text.data(), text.data() + text.size(), offset, 16).ec != std::errc{} || offset > 0x4000) return "error: hex offsets.";
                std::uint32_t bits{};
                float real{};
                std::memcpy(&bits, reinterpret_cast<const void *>(context + offset), 4);
                std::memcpy(&real, &bits, 4);
                result += std::format("{}+{:x} = {:08x} ({:.4g})", result.empty() ? "ctx: " : ", ", offset, bits, real);
            }
        } catch (...) { return "error: the skater's context could not be read."; }
        return result;
    }
    if (v == "scanf") {
        // trainer scanf <window hex> <number> ...: the first number is the anchor.
        std::uintptr_t window{};
        std::array<float, 24> wanted{};
        const bool again = lower(arg(0)) == "hits" || lower(arg(0)) == "deref" || lower(arg(0)) == "pokeall";
        if (!again && (a.size() < 2 || a.size() > wanted.size() + 1 || std::from_chars(a[0].data(), a[0].data() + a[0].size(), window, 16).ec != std::errc{}))
            return "error: usage: trainer scanf <window hex> <number> ...";
        for (std::size_t i = 1; i < a.size() && !again; ++i) {
            const auto value = number(a[i]);
            if (!value) return "error: \"" + a[i] + "\" is not a number.";
            wanted[i - 1] = static_cast<float>(*value);
        }
        static std::array<FloatHit, 12> hits{};
        static std::size_t last_found{};
        if (lower(arg(0)) == "deref") {
            const auto hit = number(arg(1)), offset = number(arg(2)), count = number(arg(3));
            if (!hit || !offset || !count || *hit >= static_cast<double>(std::min(last_found, hits.size()))) return "error: usage: trainer scanf deref <hit> <byte offset> <count>";
            std::uintptr_t target{};
            const auto slot = hits[static_cast<std::size_t>(*hit)].address + static_cast<std::intptr_t>(*offset);
            if (!memory::peek(slot, target)) return "error: unreadable slot.";
            std::string result = std::format("[{:x}] -> {:x}:", slot, target);
            for (std::size_t i = 0; i < static_cast<std::size_t>(std::min(*count, 96.0)); ++i) {
                std::uint32_t bits{};
                if (!memory::peek(target + i * 4, bits)) { result += " ?"; break; }
                float real;
                std::memcpy(&real, &bits, 4);
                const float size = std::abs(real);
                result += bits == 0 ? " 0" : size > 1.0e-4f && size < 1.0e6f ? std::format(" {:.4g}", real) : std::format(" #{:x}", bits);
            }
            return result;
        }
        if (lower(arg(0)) == "pokeall") {
            // trainer scanf pokeall <byte offset> <number> [first hit] [hits]: write one float beside each hit of the last search.
            const auto offset = number(arg(1)), value = number(arg(2)), first = number(arg(3)), many = number(arg(4));
            if (!offset || !value) return "error: usage: trainer scanf pokeall <byte offset> <number> [first hit] [hits]";
            std::size_t written{};
            const auto begin = first ? static_cast<std::size_t>(*first) : 0, end = many ? begin + static_cast<std::size_t>(*many) : hits.size();
            for (std::size_t i = begin; i < std::min({last_found, hits.size(), end}); ++i) {
                __try_poke(hits[i].address + static_cast<std::intptr_t>(*offset), static_cast<float>(*value));
                ++written;
            }
            return std::format("{} hits written.", written);
        }
        if (lower(arg(0)) == "hits") {
            // trainer scanf hits <words before> <count> [skip]: memory around each hit of the last search.
            const auto before = number(arg(1)), count = number(arg(2)), skip = number(arg(3));
            if (!before || !count) return "error: usage: trainer scanf hits <words before> <count> [skip]";
            std::string result;
            for (std::size_t h = static_cast<std::size_t>(skip ? *skip : 0); h < std::min(last_found, hits.size()); ++h) {
                result += std::format(" [{:x}:", hits[h].address);
                for (std::size_t i = 0; i < static_cast<std::size_t>(std::min(*count, 64.0)); ++i) {
                    std::uint32_t bits{};
                    if (!memory::peek(hits[h].address - static_cast<std::size_t>(*before) * 4 + i * 4, bits)) { result += " ?"; break; }
                    float real;
                    std::memcpy(&real, &bits, 4);
                    const float size = std::abs(real);
                    result += bits == 0 ? " 0" : size > 1.0e-4f && size < 1.0e6f ? std::format(" {:.4g}", real) : std::format(" #{:x}", bits);
                }
                result += "]";
            }
            return result;
        }
        std::size_t megabytes{};
        const auto started = GetTickCount64();
        const auto found = scan_floats(wanted.data(), a.size() - 1, window, hits.data(), hits.size(), megabytes);
        last_found = found;
        std::string result = std::format("{} hits in {} MB, {} ms:", found, megabytes, GetTickCount64() - started);
        for (std::size_t h = 0; h < std::min(found, hits.size()); ++h) {
            result += std::format(" [{:x}:", hits[h].address);
            for (std::size_t i = 1; i + 1 < a.size(); ++i) result += std::format(" {:+x}", hits[h].offsets[i]);
            result += "]";
        }
        return result;
    }
    if (v == "peek") {
        // trainer peek <address hex> <count>: that many four-byte values as numbers.
        std::uintptr_t address{}, count{16};
        if (a.empty() || std::from_chars(a[0].data(), a[0].data() + a[0].size(), address, 16).ec != std::errc{}) return "error: usage: trainer peek <address hex> [count]";
        if (a.size() > 1) (void)std::from_chars(a[1].data(), a[1].data() + a[1].size(), count, 10);
        std::string result;
        for (std::size_t i = 0; i < std::min<std::size_t>(count, 96); ++i) {
            std::uint32_t bits{};
            if (!memory::peek(address + i * 4, bits)) return result + " (unreadable)";
            float real;
            std::memcpy(&real, &bits, 4);
            const float size = std::abs(real);
            result += size > 1.0e-4f && size < 1.0e6f ? std::format(" {:.4g}", real) : std::format(" #{:x}", bits);
        }
        return result;
    }
    if (v == "undo") return std::format("{} numbers put back to the game's own behind the trainer's back.", dev_undo_classes());
    if (v == "poke") {
        // trainer poke <address hex> <number>: write one float (diagnostic).
        std::uintptr_t address{};
        const auto value = number(arg(1));
        if (!value || std::from_chars(a[0].data(), a[0].data() + a[0].size(), address, 16).ec != std::errc{}) return "error: usage: trainer poke <address hex> <number>";
        std::uint32_t before{};
        if (!memory::peek(address, before)) return "error: unreadable.";
        const auto real = static_cast<float>(*value);
        __try_poke(address, real);
        return std::format("{:x} = {}", address, real);
    }
    if (v == "sweep") {
        auto &w = sweep();
        if (!a.empty() && lower(a[0]) == "stop") {
            if (!w.running) return "No sweep is running.";
            std::array<std::uintptr_t, 4> none{};
            (void)watch_four(none.data(), s.base, w.image_size);
            w.running = false;
            w.file.close();
            return w.live ? "Sweep stopped: " + write_live_sweep() : "Sweep stopped.";
        }
        if (!a.empty() && lower(a[0]) == "status") return w.running && w.live ? write_live_sweep() : "No live sweep is running.";
        if (w.running) return "The sweep is already running.";
        if (a.empty()) return "error: usage: trainer sweep <tag> [live] [unseen] [ms] [class key ...] | stop | status";
        w = {};
        w.tag = a[0];
        std::vector<std::string> earlier; // after:<tag>: what that sweep's file shows as read is not asked for again
        for (std::size_t i = 1; i < a.size(); ++i) {
            if (lower(a[i]).starts_with("after:")) {
                earlier.push_back(a[i].substr(6));
                continue;
            }
            if (const auto ms = number(a[i]); ms && *ms >= 50 && *ms <= 10000) w.dwell = static_cast<int>(*ms); // a number: milliseconds per round
            else if (lower(a[i]) == "live") w.live = true;
            else if (lower(a[i]) == "unseen") w.unseen = true;
            else w.only.push_back(lower(a[i]));
        }
        if (w.live) {
            w.game.assign(class_field_count, 0);
            w.other.assign(class_field_count, 0);
            w.reader.assign(class_field_count, 0);
            w.found.assign(class_field_count, 0);
            w.live_copy.assign(class_count, -1);
            for (const auto &tag : earlier) {
                std::ifstream file(data_directory() / ("sweep-" + tag + ".tsv"));
                for (std::string line; std::getline(file, line);) {
                    const auto tab = line.find('\t');
                    if (tab == std::string::npos || line.compare(tab + 1, 2, "c:") != 0) continue;
                    const auto plus = line.find('+', tab);
                    if (plus == std::string::npos) continue;
                    const auto reads = std::strtoull(line.c_str() + tab + 3, nullptr, 10) + std::strtoull(line.c_str() + plus + 1, nullptr, 10);
                    if (!reads) continue;
                    for (std::size_t f = 0; f < class_field_count; ++f)
                        if (line.compare(0, tab, std::format("{}.{}", class_specs[sweep_class(f)].key, class_fields[f].name)) == 0) w.other[f] += reads;
                }
            }
        } else {
            w.file.open(data_directory() / ("sweep-" + a[0] + ".tsv"), std::ios::binary | std::ios::trunc);
            if (!w.file) return "error: the sweep file could not be created.";
        }
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(s.base);
        w.image_size = reinterpret_cast<const IMAGE_NT_HEADERS *>(s.base + static_cast<std::uintptr_t>(dos->e_lfanew))->OptionalHeader.SizeOfImage;
        w.end = class_field_count;
        w.running = true;
        return "Sweeping the game's reads of the class values.";
    }
    if (v == "curves") {
        // trainer curves [run length]: the type word comes from the push class's first curve.
        std::array<FieldCopy, 16> copies{};
        std::size_t push_first{};
        for (std::size_t c = 0; c < class_count; ++c)
            if (std::string_view(class_specs[c].key) == "push") push_first = class_specs[c].first;
        const auto count = class_field_copies(push_first, copies.data(), copies.size());
        std::uintptr_t type{};
        for (std::size_t i = 0; i < count && !type; ++i) {
            if (copies[i].kind != 'c') continue;
            std::uintptr_t curve{};
            // MaxPushSpeedLight is the class's first field; PushingAngleCurve sits 0x10 after it.
            if (memory::peek(copies[i].address + 0x10, curve) && curve) (void)memory::peek(curve, type);
        }
        if (!type) return "error: no curve to take the type from (is the push class found?).";
        static std::array<PointerRun, 24> runs{};
        static std::size_t found{};
        std::array<std::uint32_t, 24> histogram{};
        const auto wanted = number(arg(0));
        const auto started = GetTickCount64();
        const auto objects = pointer_runs(type, wanted ? static_cast<std::uint32_t>(*wanted) : 0, runs.data(), runs.size(), found, histogram);
        std::string result = std::format("type {:x}: {} curves, {} runs in {} ms; by length:", type - s.base, objects, found, GetTickCount64() - started);
        for (std::size_t n = 2; n < histogram.size(); ++n)
            if (histogram[n]) result += std::format(" {}x{}", histogram[n], n);
        for (std::size_t r = 0; r < std::min(found, runs.size()) && r < 6; ++r) {
            result += std::format(" | run at {:x}:", runs[r].address);
            for (std::uint32_t k = 0; k < runs[r].length && k < 10; ++k) {
                std::uintptr_t curve{}, points{};
                float low{}, high{};
                std::uint32_t n{};
                if (!memory::peek(runs[r].address + k * 8, curve) || !memory::peek(curve + 0x18, points) || !memory::peek(curve + 0x20, low) ||
                    !memory::peek(curve + 0x24, high)) break;
                (void)memory::peek(points - 4, n);
                n &= 0x7fffffffu;
                float x0{}, y0{}, x1{}, y1{};
                if (n && n < 64) {
                    (void)memory::peek(points + 0xc, x0);
                    (void)memory::peek(points + 0x14, y0);
                    (void)memory::peek(points + (n - 1) * 0x1c + 0xc, x1);
                    (void)memory::peek(points + (n - 1) * 0x1c + 0x14, y1);
                }
                result += std::format(" [{} pts {:.3g},{:.3g}..{:.3g},{:.3g}]", n, x0, y0, x1, y1);
            }
        }
        return result;
    }
    if (v == "watchx") {
        std::uintptr_t rva{};
        const auto text = arg(0);
        if (std::from_chars(text.data(), text.data() + text.size(), rva, 16).ec != std::errc{} || (rva & 3)) return "error: usage: trainer watchx <rva hex, 4-aligned>";
        return std::format("Watching calls of {:x} in {} threads.", rva, watch_writes((s.base + rva) | 2u, s.base));
    }
    if (v == "topspeed") {
        const auto top = s.telemetry.top_speed;
        s.telemetry.top_speed = 0;
        return std::format("Top speed since last asked: {:.2f} m/s ({:.1f} km/h); now {:.2f} m/s.", top, top * 3.6f, s.telemetry.speed);
    }
    if (v == "bodyflip") {
        // trainer bodyflip [speed <x>|rate <x>|off]...: read the off-board flip states; hold one at a number every tick.
        auto &b = dev_body_flip;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const auto word = lower(a[i]);
            if (word == "off") b.hold_speed = b.hold_rate = false;
            else if (word == "speed" && i + 1 < a.size()) { b.speed = std::stof(a[++i]); b.hold_speed = true; }
            else if (word == "rate" && i + 1 < a.size()) { b.rate = std::stof(a[++i]); b.hold_rate = true; }
        }
        auto text = std::format("speed {} now {:.4g} (seen {:.4g}..{:.4g}){}, rate {} now {:.4g} (seen {:.4g}..{:.4g}){}.",
            b.last.speed_at ? "found" : "NOT found", b.last.speed, b.low_speed, b.high_speed, b.hold_speed ? std::format(", held at {:.4g}", b.speed) : "",
            b.last.rate_at ? "found" : "NOT found", b.last.rate, b.low_rate, b.high_rate, b.hold_rate ? std::format(", held at {:.4g}", b.rate) : "");
        b.low_speed = b.low_rate = 1e9f;
        b.high_speed = b.high_rate = -1e9f;
        return text;
    }
    if (v == "flipland") {
        if (const auto seconds = number(arg(0))) flip_land_window = std::clamp(static_cast<float>(*seconds), 0.0f, 2.0f);
        return std::format("A slowed flip returns to the game's speed {:.3f} s before the landing.", flip_land_window);
    }
    if (v == "launches") return trajectory_calls();
    if (v == "launchdump") return trajectory_parameters();
    if (v == "launchpoke") {
        const auto slot = number(arg(0)), value = number(arg(1));
        return slot && value ? trajectory_poke(static_cast<int>(*slot), static_cast<float>(*value)) : std::string{"launchpoke <slot> <value>"};
    }
    if (v == "flightdump") return flight_dump();
    if (v == "nofloor") {
        trajectory_no_floor(number(arg(0)) && *number(arg(0)) != 0);
        return std::string{"Launch floor test switch set."};
    }
    if (v == "flipturn") {
        flip_turn_test = number(arg(0)) ? static_cast<float>(*number(arg(0))) : 0.0f;
        return std::format("Flip turn time for the pop: {:.2f} s (0: by trick).", flip_turn_test);
    }
    if (v == "flipleast") {
        flip_least_game = number(arg(0)) ? static_cast<float>(*number(arg(0))) : 1.0f;
        return std::format("With 'Flick tricks at full speed' the game's flip speed counts as at least {:.2f} (0: as flicked).", flip_least_game);
    }
    if (v == "launchtest") {
        // trainer launchtest <ollie launch scale> [<float slot> <value>]
        const auto scale = number(arg(0)), slot = number(arg(1)), value = number(arg(2));
        trajectory_test(scale ? static_cast<float>(*scale) : 1.0f, slot && value ? static_cast<int>(*slot) : -1, value ? static_cast<float>(*value) : 0.0f);
        return std::format("Ollie launch x{:.2f}, slot {}.", scale ? *scale : 1.0, slot && value ? static_cast<int>(*slot) : -1);
    }
    if (v == "dp") {
        // trainer dp [on|off] [release <s>] [extra <m/s>] [probe on|off]: Dark Pop's switches, for scripted tests.
        auto &debug = dingosdk::client_source::detail::source_state().trial.debug;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const auto word = lower(a[i]);
            if (word == "on" || word == "off") debug.dark_pop = word == "on";
            else if (word == "release" && i + 1 < a.size()) debug.dark_pop_release = std::clamp(std::stof(a[++i]), 0.3f, 1.5f);
            else if (word == "extra" && i + 1 < a.size()) debug.dark_pop_strength = std::clamp(std::stof(a[++i]), 0.0f, 25.0f);
            else if (word == "probe" && i + 1 < a.size()) debug.probe = lower(a[++i]) == "on";
        }
        return std::format("Dark Pop {}, release {:.3f} s, extra {:.1f} m/s, {} pops, probe {}.", debug.dark_pop ? "on" : "off",
            debug.dark_pop_release, debug.dark_pop_strength, debug.dark_pop_count, debug.probe ? "on" : "off");
    }
    if (v == "pad") {
        // trainer pad <controls:ms> ... | off: a scripted controller, for tests nobody has to play.
        auto &p = s.pad;
        if (lower(arg(0)) == "off" || a.empty()) {
            p = {};
            DingoSDKOverlayScriptController(nullptr);
            return "The controller is the player's again.";
        }
        std::vector<PadStep> steps;
        const bool mount = lower(arg(0)) == "mount", loop = lower(arg(0)) == "loop";
        for (const auto &word : mount ? std::vector<std::string>{"b:150", "n:1200", "y:200", "n:2500"}
                                : loop ? std::vector<std::string>(a.begin() + 1, a.end()) : a) {
            const auto step = pad_step(word);
            if (!step) return "error: \"" + word + "\" is not a step. Example: trainer pad ry=-1:300 ry=1:60 n:500 (buttons a b x y lb rb "
                                                   "ls rs up down left right start back, lt rt, sticks lx ly rx ry from -1 to 1)";
            steps.push_back(*step);
        }
        // Appended to a script still running, so one command line need not hold a whole run.
        if (p.running && !p.loop && !loop) {
            p.steps.insert(p.steps.end(), steps.begin(), steps.end());
            return std::format("{} steps added.", steps.size());
        }
        p = {};
        p.steps = std::move(steps);
        p.step_start = clock_now();
        p.running = true;
        p.mount_tries = mount ? 8 : 0;
        p.loop = loop;
        DingoSDKOverlayScriptControllerPolls(nullptr, &p.served_before);
        return std::format("Playing {} controller steps.", p.steps.size());
    }
    // </dev>
    if (v == "classes") {
        if (lower(arg(0)) == "find") {
            find_classes();
            return "Looking for the game's tuning classes.";
        }
        return "Copies found: " + classes_summary();
    }
    if (v == "refresh") return tuning::refresh_skater(s.base, s.entity) ? "Skater copy refreshed." : "error: the skater has no cached copy right now.";
    // <dev>
    if (v == "ant") {
        // trainer ant: the skater's components (address:vtable rva).
        const auto entity = static_cast<std::uintptr_t>(s.entity);
        std::uintptr_t collection{}, owner{};
        if (!entity || !memory::peek(entity + 0x70, collection) || !memory::peek(collection, owner) || owner != entity) return "error: no component collection.";
        std::uint8_t count{};
        (void)memory::peek(collection + 8, count);
        std::string result = std::format("entity {:x}, {} components:", entity, count);
        for (unsigned i = 0; i < count && i < 128; ++i) {
            std::uintptr_t component{}, type{};
            if (!memory::peek(collection + 0x20 + i * 0x20, component) || !memory::peek(component, type)) continue;
            result += std::format(" {:x}:{:x}", component, type - s.base);
        }
        return result;
    }
    if (v == "airdump") {
        // trainer airdump <name> <tick> ...: the game state blocks as <name>_t<tick>_<block>.bin on those ticks of the next flight.
        dev_airdump_ticks.clear();
        dev_airdump_name = arg(0);
        for (std::size_t i = 1; i < a.size(); ++i)
            if (const auto tick = number(a[i])) dev_airdump_ticks.push_back(static_cast<int>(*tick));
        return std::format("Dumping on {} ticks of the next flight.", dev_airdump_ticks.size());
    }
    if (v == "hold") {
        // trainer hold <number> <address hex> ... | hold off: that number at those addresses on every tick in the air.
        dev_hold_at.clear();
        const auto value = number(arg(0));
        if (!value) return "Holding nothing.";
        dev_hold_value = static_cast<float>(*value);
        for (std::size_t i = 1; i < a.size(); ++i) {
            std::uintptr_t address{};
            if (std::from_chars(a[i].data(), a[i].data() + a[i].size(), address, 16).ec != std::errc{}) return "error: hex addresses.";
            dev_hold_at.push_back(address);
        }
        return std::format("Holding {} at {} addresses in the air.", dev_hold_value, dev_hold_at.size());
    }
    if (v == "narrow") {
        // trainer narrow start|keep <low> <high> [tick] | now <low> <high> | list: sample on that tick of the next flight
        // (or at once), keeping the floats inside the range.
        const auto what = lower(arg(0));
        const auto low = number(arg(1)), high = number(arg(2)), tick = number(arg(3));
        if (what == "list") {
            std::string result = std::format("{} kept:", dev_narrow.at.size());
            for (std::size_t i = 0; i < std::min<std::size_t>(dev_narrow.at.size(), 60); ++i) {
                float value{};
                (void)memory::peek(dev_narrow.at[i], value);
                result += std::format(" {:x}={:.5g}", dev_narrow.at[i], value);
            }
            return result;
        }
        if (what.empty()) return dev_narrow.last;
        if (!low || !high || (what != "start" && what != "keep" && what != "now")) return "error: usage: trainer narrow start|keep|now <low> <high> [tick]";
        dev_narrow.low = static_cast<float>(*low);
        dev_narrow.high = static_cast<float>(*high);
        if (what == "now") {
            dev_narrow.pending = dev_narrow.at.empty() ? 1 : 2;
            return dev_narrow.last = narrow_run(dev_narrow);
        }
        if (tick) dev_narrow.tick = static_cast<int>(*tick);
        dev_narrow.pending = what == "start" ? 1 : 2;
        return std::format("Sampling on tick {} of the next flight.", dev_narrow.tick);
    }
    if (v == "watchr") {
        // trainer watchr <address hex> ... (up to four) | watchr: which code reads or writes those four bytes; then the counts.
        static std::array<std::uintptr_t, 4> watched{};
        if (!a.empty()) {
            watched = {};
            for (std::size_t i = 0; i < std::min<std::size_t>(a.size(), 4); ++i)
                if (std::from_chars(a[i].data(), a[i].data() + a[i].size(), watched[i], 16).ec != std::errc{}) return "error: hex addresses.";
            auto &w = sweep();
            if (!w.image_size) {
                const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(s.base);
                w.image_size = reinterpret_cast<const IMAGE_NT_HEADERS *>(s.base + static_cast<std::uintptr_t>(dos->e_lfanew))->OptionalHeader.SizeOfImage;
            }
            return std::format("Watching in {} threads.", watch_four(watched.data(), s.base, w.image_size));
        }
        std::array<std::uint64_t, 4> counts{}, others{};
        std::array<std::uintptr_t, 4> readers{};
        watch_four_counts(counts.data(), others.data());
        watch_four_readers(readers.data());
        std::string result = "watch:";
        for (std::size_t i = 0; i < 4; ++i)
            if (watched[i]) result += std::format(" {:x}: {}+{} first {}", watched[i], counts[i], others[i], sweep_reader(readers[i], s.base, sweep().image_size));
        return result;
    }
    if (v == "dumpwalk") {
        // trainer dumpwalk <file> <root hex> <block bytes hex> <depth> [max blocks]: the block at the root and, to that
        // depth, the blocks every pointer in them leads to (private read-write memory only), as records
        // [u64 address][u32 depth][u32 size][bytes] in the trainer folder.
        std::uintptr_t root{}, block{};
        const auto depth = number(arg(3)), most = number(arg(4));
        if (a.size() < 4 || !depth || std::from_chars(a[1].data(), a[1].data() + a[1].size(), root, 16).ec != std::errc{} ||
            std::from_chars(a[2].data(), a[2].data() + a[2].size(), block, 16).ec != std::errc{} || block < 8 || block > 0x10000)
            return "error: usage: trainer dumpwalk <file> <root hex> <block bytes hex> <depth> [max blocks]";
        const std::size_t limit = most ? static_cast<std::size_t>(*most) : 20000;
        std::ofstream file(data_directory() / a[0], std::ios::binary | std::ios::trunc);
        if (!file) return "error: could not write the file.";
        const auto heap = [](std::uintptr_t address, std::size_t size) {
            MEMORY_BASIC_INFORMATION info{};
            if (address < 0x10000 || (address & 7) || !VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof(info))) return false;
            const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            return info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && info.Protect == PAGE_READWRITE && address + size <= end;
        };
        std::vector<std::pair<std::uintptr_t, std::uint32_t>> queue{{root, 0}};
        std::map<std::uintptr_t, bool> seen{{root, true}};
        std::vector<std::uint8_t> bytes(block);
        std::size_t written{};
        for (std::size_t i = 0; i < queue.size() && written < limit; ++i) {
            const auto [address, level] = queue[i];
            if (!heap(address, block)) continue;
            std::memcpy(bytes.data(), reinterpret_cast<const void *>(address), block);
            const std::uint64_t where = address;
            const std::uint32_t head[2]{level, static_cast<std::uint32_t>(block)};
            file.write(reinterpret_cast<const char *>(&where), 8).write(reinterpret_cast<const char *>(head), 8);
            file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(block));
            ++written;
            if (level >= static_cast<std::uint32_t>(*depth)) continue;
            for (std::size_t at = 0; at + 8 <= block; at += 8) {
                std::uintptr_t target{};
                std::memcpy(&target, bytes.data() + at, 8);
                if (target > 0x10000 && target < 0x7fffffffffff && !(target & 7) && seen.emplace(target, true).second) queue.emplace_back(target, level + 1);
            }
        }
        return std::format("{} blocks of {:x} bytes written.", written, block);
    }
    // </dev>
    if (v == "states") {
        std::string result;
        for (const auto &[id, ticks] : s.states) result += std::format("{}{}: {}", result.empty() ? "" : ", ", id, ticks);
        return result.empty() ? "No physics state seen yet." : "Physics states seen (id: ticks): " + result;
    }
    // <dev>
    if (v == "levers") return std::format("Forced settings and held switches in force: {}. Lean stick on: {}.", holds_in_force(), [] {
        float raw[2]{};
        return DingoSDKOverlayGrindStickLeanRaw(raw);
    }() ? "yes" : "no");
    if (v == "audit") {
        // The release audit, run on a solo level: stock is stock, everything that goes on comes off again, a saved
        // preset brings back exactly what was set, a section's reset stays in its section. One log line each.
        int failures{};
        const auto snapshot = [&] {
            std::vector<double> all;
            for (const auto &e : s.entries) all.push_back(e.touched ? e.value : e.stock);
            for (const auto &one : setups()) all.push_back(one.get());
            return all;
        };
        const auto differences = [&](const std::vector<double> &a, const std::vector<double> &b) {
            std::string out;
            std::size_t count{};
            for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
                if (std::abs(a[i] - b[i]) > 1e-9 * std::max(1.0, std::abs(a[i])) && ++count <= 4)
                    out += std::format(" {} {:.6g}->{:.6g}", i < s.entries.size() ? s.entries[i].key : setups()[i - s.entries.size()].key, a[i], b[i]);
            return count ? std::format("{} differ:{}", count, out) : std::string{};
        };
        const auto check = [&](bool ok, const std::string &what, const std::string &detail = {}) {
            failures += !ok;
            say(ok ? logging::Level::info : logging::Level::warning, std::format("trainer audit: {} - {}{}{}", ok ? "ok" : "FAILED", what, detail.empty() ? "" : ": ", detail));
        };
        const auto words = [](std::string_view text) {
            std::vector<std::string> out;
            for (std::size_t start = 0; start < text.size();) {
                auto end = text.find(' ', start);
                if (end == std::string_view::npos) end = text.size();
                if (end > start) out.emplace_back(text.substr(start, end - start));
                start = end + 1;
            }
            return out;
        };
        const auto does = [&](std::string_view verb_, std::string_view arguments) {
            const auto answer = run(verb_, words(arguments));
            if (answer.starts_with("error")) check(false, std::format("{} {}", verb_, arguments), answer);
        };
        const auto is_stock = [&] {
            for (const auto &e : s.entries)
                if (e.touched || e.frozen) return false;
            return std::ranges::none_of(setups(), off_stock) && s.active.empty();
        };
        does("reset", "everything");
        const auto stock = snapshot();
        check(is_stock(), "reset everything leaves nothing set");
        for (const auto &preset : builtin_presets()) {
            does("preset", "apply " + std::string(preset.name));
            const bool moved = !differences(stock, snapshot()).empty();
            does("preset", "remove " + std::string(preset.name));
            check(moved && is_stock(), std::format("built-in preset {} on and off", preset.name), moved ? differences(stock, snapshot()) : "it set nothing");
        }
        static constexpr std::pair<std::string_view, std::string_view> pairs[][2]{
            {{"feel", "easy"}, {"feel", "stock"}},       {{"feel", "normal"}, {"feel", "stock"}},
            {{"feel", "hardcore"}, {"feel", "stock"}},   {{"grind", "feel skate3"}, {"grind", "feel skate"}},
            {{"transition", "feel skate3"}, {"transition", "feel skate"}}, {{"trickline", "extras on"}, {"trickline", "extras off"}},
        };
        for (const auto &pair : pairs) {
            does(pair[0].first, pair[0].second);
            const bool moved = !differences(stock, snapshot()).empty();
            does(pair[1].first, pair[1].second);
            check(moved && is_stock(), std::format("{} {} then {}", pair[0].first, pair[0].second, pair[1].second), moved ? differences(stock, snapshot()) : "it set nothing");
        }
        // One feel straight to another: only the last one's numbers may be left.
        does("feel", "easy");
        does("feel", "hardcore");
        const auto hardcore = snapshot();
        does("feel", "stock");
        does("feel", "hardcore");
        check(differences(hardcore, snapshot()).empty(), "feel easy then hardcore equals hardcore alone", differences(hardcore, snapshot()));
        does("feel", "stock");
        check(is_stock(), "feel stock after that");
        // A whole setup: saved, wiped, loaded, taken off.
        does("feel", "normal");
        does("grind", "feel skate3");
        does("transition", "feel skate3");
        does("option", "flip_speed 0.5");
        does("option", "flip_pop 1");
        does("option", "flip_flick 1");
        does("option", "pump_power 3");
        does("option", "offboard_height 2");
        does("revert", "spin 60");
        does("grind", "side 2");
        const auto mine = snapshot();
        does("preset", "save audit setup");
        does("reset", "section grinds");
        {
            const auto after = snapshot();
            bool grinds_stock = true, others_kept = true;
            for (std::size_t i = 0; i < setups().size(); ++i) {
                const auto &one = setups()[i];
                if (one.topic == topic_grinds && off_stock(one)) grinds_stock = false;
                if (one.topic != topic_grinds && after[s.entries.size() + i] != mine[s.entries.size() + i]) others_kept = false;
            }
            check(grinds_stock && others_kept, "reset section grinds puts back the grind settings and no other setting");
        }
        does("reset", "everything");
        check(is_stock(), "reset everything after a whole setup", differences(stock, snapshot()));
        does("preset", "apply audit setup");
        check(differences(mine, snapshot()).empty(), "a saved preset brings the whole setup back", differences(mine, snapshot()));
        const auto shared = export_preset("audit setup");
        check(!shared.starts_with("error"), "preset export", shared);
        does("preset", "remove audit setup");
        check(is_stock(), "the saved preset turned off", differences(stock, snapshot()));
        does("preset", "delete audit setup");
        does("reset", "everything");
        say(failures ? logging::Level::warning : logging::Level::info, std::format("trainer audit: finished, {} failures.", failures));
        return std::format("Audit finished: {} failures (see the log).", failures);
    }
    // </dev>
    if (v == "dump") {
        const auto directory = data_directory();
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream file(directory / L"tuning-values.txt", std::ios::binary | std::ios::trunc);
        if (!file) return "error: could not write the list.";
        for (const auto &e : s.entries)
            file << std::format("{}\t0x{:x}\t{}\t{:.6g}\t{:.6g}\n", e.id, e.offset,
                                e.kind == Kind::real ? "real" : e.kind == Kind::flag ? "flag" : e.kind == Kind::curve ? "curve" :
                                e.kind == Kind::graph ? "graph" : "int", e.stock, e.value);
        return std::format("Wrote {} values to {}.", s.entries.size(), path_utf8(directory / L"tuning-values.txt"));
    }
    if (v == "selftest") {
        if (s.test.step) return "The self test is already running.";
        s.test = {};
        s.test.step = 1;
        s.test.until = GetTickCount64() + 500;
        return "Self test started; see the log for \"trainer selftest:\" lines.";
    }
    return "error: trainer status|open|set|freeze|reset|find|preset|slot|marker|tp|ground|waypoint|spot|option|profile|jumps|where|states|dump|selftest";
}
} // namespace

bool stage_import(std::string_view text) noexcept {
    try {
        const auto directory = shared_directory();
        if (directory.empty() || text.empty() || text.size() > 256 * 1024) return false;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream file(directory / "clipboard.txt", std::ios::binary | std::ios::trunc);
        file << text;
        return static_cast<bool>(file);
    } catch (...) {
        return false;
    }
}
std::string command(std::string_view verb, const std::vector<std::string> &arguments) {
    auto result = run(verb, arguments);
    // The menu shows the last answer, except for the ones a dragged slider sends every frame.
    if (verb != "set" || result.starts_with("error")) {
        auto &s = state();
        s.last = result;
        s.view_due = true;
    }
    return result;
}
} // namespace dingosdk::trainer
