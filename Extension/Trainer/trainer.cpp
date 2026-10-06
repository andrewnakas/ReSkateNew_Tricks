#include "trainer.h"
#include "trainer_classes.h"
#include "trainer_jump.h"
#include "trainer_presets.h"
#include "trainer_session.h"
#include "Extension/Multiplayer/Hud/follow_camera.h"
// <dev>
#include "trainer_probe.h"
// </dev>
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
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
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>

// <dev>
extern "C" void DingoSDKOverlayScriptController(const void *gamepad);
extern "C" void DingoSDKOverlayScriptControllerPolls(std::uint64_t *polls, std::uint64_t *served);
extern "C" std::uint64_t DingoSDKOverlayScriptControllerBlanked();
// </dev>

// Game thread only. See trainer.h for how the menu reaches this.
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
    std::array<RigSetting, camera_rigs> rigs{};
    bool camera_on{};
    CameraSet camera_board, camera_foot{1.9f, 1.6f, -8.0f, 0.4f, 0.0f, 0.06f};
    // The hippy jump's height is set by the game's trick scripts, not by tuning: the trainer
    // scales the upward velocity when it sees one start.
    float hippy_height{1};
    // An on-foot jump: the same scaling, when a skater standing off the board starts to rise.
    float offboard_height{1};
    // Board flip tricks: a multiplier on the game's flip speed curves (trainer_classes.h).
    float flip_speed{1};
    bool offboard_grounded{}; // off the board and not moving up or down at the last tick
    const char *boost_name{""};
    // The no comply and the boneless are launched by the trick scripts too (trainer_jump.cpp).
    float nocomply_height{1}, boneless_height{1};
    float revert_boost{};            // 0: off
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
// ---- the player's own camera -------------------------------------------------------------------
struct CameraPreset {
    std::string_view name, note;
    bool foot; // it is a framing for walking, not skating
    CameraSet set;
};
constexpr CameraPreset camera_presets[]{
    {"Low", "Close to the ground behind the board, looking slightly up: the Skate 1 feel.", false, {2.6f, 0.45f, 4.0f, 0.0f, 78.0f, 0.06f}},
    {"Low plus", "The low camera raised a little, so more of the skater shows.", false, {2.8f, 0.8f, 0.0f, 0.0f, 72.0f, 0.06f}},
    {"Skate 3", "Close chase camera with a wide lens.", false, {2.3f, 0.9f, -2.0f, 0.0f, 80.0f, 0.1f}},
    {"High", "Further back and above, for lines and big gaps.", false, {4.8f, 2.6f, -18.0f, 0.0f, 62.0f, 0.1f}},
    {"Shoulder", "On foot: over the right shoulder.", true, {1.7f, 1.55f, -6.0f, 0.45f, 62.0f, 0.05f}},
    {"Shoulder left", "On foot: over the left shoulder.", true, {1.7f, 1.55f, -6.0f, -0.45f, 62.0f, 0.05f}},
};
float *camera_field(CameraSet &set, std::string_view name) {
    return name == "distance" ? &set.distance : name == "height" ? &set.height : name == "pitch" ? &set.pitch
         : name == "side" ? &set.side : name == "fov" ? &set.fov : name == "lag" ? &set.lag : nullptr;
}
CameraSet sane_camera(CameraSet set) {
    const auto finite = [](float value, float low, float high, float otherwise) { return std::isfinite(value) ? std::clamp(value, low, high) : otherwise; };
    set.distance = finite(set.distance, 0.0f, 10000.0f, 3.4f);
    set.height = finite(set.height, -10000.0f, 10000.0f, 1.5f);
    set.pitch = finite(set.pitch, -89.0f, 89.0f, -9.0f);
    set.side = finite(set.side, -10000.0f, 10000.0f, 0.0f);
    set.fov = set.fov < 1.0f ? 0.0f : finite(set.fov, 1.0f, 179.0f, 0.0f);
    set.lag = finite(set.lag, 0.0f, 60.0f, 0.08f);
    return set;
}
Json write_camera(const CameraSet &set) {
    Json row = Json::object();
    row["distance"] = set.distance;
    row["height"] = set.height;
    row["pitch"] = set.pitch;
    row["side"] = set.side;
    row["fov"] = set.fov;
    row["lag"] = set.lag;
    return row;
}
CameraSet read_camera(const Json &row, CameraSet set) {
    if (!row.is_object()) return set;
    for (const auto name : {"distance", "height", "pitch", "side", "fov", "lag"})
        if (row.contains(name) && row.at(name).is_number()) *camera_field(set, name) = row.at(name).get<float>();
    return sane_camera(set);
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
RigSetting sane_rig(RigSetting rig);
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
            s.offboard_height = std::clamp(o.value("offboard_height", 1.0f), height_low, height_high);
            s.flip_speed = std::clamp(o.value("flip_speed", 1.0f), flip_low, flip_high);
            s.boneless_height = std::clamp(o.value("boneless_height", 1.0f), height_low, height_high);
            s.revert_boost = std::clamp(o.value("revert_boost", 0.0f), 0.0f, revert_boost_high);
            s.slot = std::clamp(o.value("slot", 0), 0, static_cast<int>(marker_slots) - 1);
        }
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
                if (name.is_string()) s.active.push_back(name.string());
        if (json->contains("revert") && json->at("revert").is_object()) {
            for (const auto &field : revert_fields)
                if (const auto name = std::string(field.name); json->at("revert").contains(name) && json->at("revert").at(name).is_number())
                    s.revert.*field.member = json->at("revert").at(name).get<float>();
            s.revert = sane_revert(s.revert);
        }
        if (json->contains("camera") && json->at("camera").is_object()) {
            const auto &camera = json->at("camera");
            s.camera_on = camera.value("on", false);
            if (camera.contains("board")) s.camera_board = read_camera(camera.at("board"), s.camera_board);
            if (camera.contains("foot")) s.camera_foot = read_camera(camera.at("foot"), s.camera_foot);
            if (camera.contains("rigs") && camera.at("rigs").is_object())
                for (std::size_t i = 0; i < camera_rigs; ++i) {
                    const auto *name = i == 0 ? "low" : i == 1 ? "high" : "foot";
                    if (!camera.at("rigs").contains(name) || !camera.at("rigs").at(name).is_object()) continue;
                    const auto &row = camera.at("rigs").at(name);
                    RigSetting rig;
                    rig.distance = row.value("distance", 1.0f);
                    rig.height = row.value("height", 0.0f);
                    rig.raise = row.value("raise", 0.0f);
                    rig.side = row.value("side", 1.0f);
                    const auto finite = [](float value, float low, float high, float otherwise) { return std::isfinite(value) ? std::clamp(value, low, high) : otherwise; };
                    s.rigs[i] = sane_rig(rig);
                }
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
        options["offboard_height"] = s.offboard_height;
        options["flip_speed"] = s.flip_speed;
        options["boneless_height"] = s.boneless_height;
        options["revert_boost"] = s.revert_boost;
        options["slot"] = s.slot;
        json["options"] = std::move(options);
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
        json["revert"] = std::move(revert);
        Json camera = Json::object();
        camera["on"] = s.camera_on;
        camera["board"] = write_camera(s.camera_board);
        camera["foot"] = write_camera(s.camera_foot);
        Json rigs = Json::object();
        for (std::size_t i = 0; i < camera_rigs; ++i) {
            Json row = Json::object();
            row["distance"] = s.rigs[i].distance;
            row["height"] = s.rigs[i].height;
            row["raise"] = s.rigs[i].raise;
            row["side"] = s.rigs[i].side;
            rigs[i == 0 ? "low" : i == 1 ? "high" : "foot"] = std::move(row);
        }
        camera["rigs"] = std::move(rigs);
        json["camera"] = std::move(camera);
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
bool slowed_catch_time(std::string_view key) {
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
void drive_camera(bool playing);
bool editable(std::string *why = nullptr) {
    // A guest's physics are the host's while the session enforces them: the tuning
    // (physics_tuning::enforce), and what the host shares beyond it (sync_session). Writing
    // over either would only fight the session, so the player's own stand down.
    if (session_tuning_enforced()) {
        if (why) *why = "This session's host sets everyone's physics: you skate with the host's.";
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
// A preset of the player's own also carries the trick sliders that are off 1, under these keys.
constexpr std::string_view trick_prefix = "trick.";
constexpr std::string_view trick_names[]{"flip_speed", "hippy_height", "nocomply_height", "boneless_height", "offboard_height", "revert_boost"};
// What a trick slider is when the game is left alone: the multipliers 1, the revert boost off.
constexpr float trick_stock(std::string_view name) { return name == "revert_boost" ? 0.0f : 1.0f; }
float *trick_option(std::string_view name) {
    auto &s = state();
    return name == "flip_speed" ? &s.flip_speed : name == "hippy_height" ? &s.hippy_height : name == "nocomply_height" ? &s.nocomply_height
         : name == "boneless_height" ? &s.boneless_height : name == "offboard_height" ? &s.offboard_height
         : name == "revert_boost" ? &s.revert_boost : nullptr;
}
// Sets a trick slider named by a preset key; false when the key is not one.
bool set_trick_key(std::string_view key, double value) {
    if (!key.starts_with(trick_prefix)) return false;
    const auto name = key.substr(trick_prefix.size());
    if (auto *option = trick_option(name))
        *option = name == "flip_speed" ? std::clamp(static_cast<float>(value), flip_low, flip_high)
                : name == "revert_boost" ? std::clamp(static_cast<float>(value), 0.0f, revert_boost_high)
                : std::clamp(static_cast<float>(value), height_low, height_high);
    return true;
}
std::string remove_preset(std::string_view name) {
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
            if (saved.first.starts_with(trick_prefix)) {
                (void)set_trick_key(saved.first, trick_stock(std::string_view(saved.first).substr(trick_prefix.size())));
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
                if (set_trick_key(key, value)) ++count;
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
    forget_camera_rigs();
    s.map = level;
    s.map_file = level.empty() ? MapFile{} : read_map_file(level);
    s.motion = {};
    s.telemetry.last = {};
    s.telemetry.best = {};
    s.telemetry.top_speed = 0;
    s.return_at = 0;
    s.profile_due = false;
    // A level brings fresh copies of the game's tuning classes: look for them once it has settled.
    s.class_search_at = level.empty() ? 0 : GetTickCount64() + 10000;
    s.class_search_tries = 0;
    s.class_search_seen = class_searches();
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
        if (watch.state == addr::no_bail::offboard_physics_state && s.last_state == 100 && s.boosts.hippy != 1.0f) {
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
    m.velocity = velocity;
    m.position = position;
    m.stamp = stamp;
    // How fast the board turns (flip tricks, shuvits): body 0's angular velocity (+0x90), read while a jump
    // is measured or telemetry is recorded.
    float board_turn = 0;
    bool turning = m.airborne || (s.logging && s.log);
    // <dev>
    turning = turning || s.state_log || s.flip_trace;
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
                say(logging::Level::info,
                    std::format("Trainer flip #{}: air {:.3f} s, up {:.2f} m/s, turning from {:.3f} to {:.3f} s ({:.3f} s), turned {:.0f} deg, mean {:.0f} peak {:.0f} "
                                "deg/s, air left at start {:.3f} s, {} samples.",
                                s.flip_serial, air, s.flip_takeoff_up, f[first].time, f[last].time, took, turned, took > 0 ? turned / took : 0.0f, peak,
                                air - f[first].time, f.size()));
            } else {
                say(logging::Level::info, std::format("Trainer flip #{}: air {:.3f} s, up {:.2f} m/s, no board turn (peak {:.0f} deg/s).", s.flip_serial, air,
                                                      s.flip_takeoff_up, peak));
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

void build_view() {
    auto &s = state();
    s.view_due = false;
    auto next = std::make_shared<View>();
    next->revision = ++s.revision;
    next->ready = s.ready;
    next->status = s.status;
    next->last = s.last;
    next->editable = editable(&next->blocked);
    next->rows.reserve(s.entries.size());
    for (const auto &e : s.entries) {
        int rank{};
        std::uint8_t modes{};
        const auto friendly = essential_name(e.key, &rank, &modes);
        next->rows.push_back({e.id, e.label, e.group, e.kind, e.value, e.stock, e.touched, e.frozen, e.detail, std::string(friendly), rank, modes,
                              e.used});
        next->rows.back().help = essential_help(e.key);
        if (e.touched) ++next->touched;
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
            bool any = false, on = true;
            const Entry *headline{};
            for (const auto &[index, r] : targets[p]) {
                const auto &e = s.entries[index];
                const auto &rule = preset.rules[r];
                if (!headline && r == 0) headline = &e;
                const double expected = rule.multiply ? e.stock * rule.amount : rule.amount;
                any = true;
                // To the two decimals a dial shows: dragging one to the preset's number switches it on.
                if (std::abs(e.value - sane(e, expected)) > 4e-3 * std::max(1e-3, std::abs(expected))) on = false;
            }
            row.active = any && on;
            const auto dial = preset_dial(preset.name);
            row.title = std::string(dial.title);
            row.modes = dial.modes;
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
    next->offboard_height = s.offboard_height;
    next->flip_speed = s.flip_speed;
    next->boneless_height = s.boneless_height;
    next->revert_boost = s.revert_boost;
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
    next->rigs = s.rigs;
    next->rigs_found = camera_rigs_summary();
    next->camera_on = s.camera_on;
    next->camera_board = s.camera_board;
    next->camera_foot = s.camera_foot;
    if (const auto profile = multiplayer::gameplay_camera_profile(); profile.samples >= 60)
        next->camera_game = std::format("{:.1f} m behind, {:.1f} m up, tilt {:.0f} degrees, FOV {:.0f}", profile.distance, profile.height, profile.pitch, profile.fov);
    next->open_serial = s.open_serial;
    next->stock = s.revert == RevertTuning{} && !s.camera_on && s.rigs == std::array<RigSetting, camera_rigs>{} && !next->touched && s.active.empty() && std::ranges::none_of(s.entries, &Entry::frozen) &&
                  std::ranges::all_of(trick_names, [](std::string_view name) { return *trick_option(name) == trick_stock(name); });
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
        for (const auto trick : trick_names)
            if (const auto *option = trick_option(trick); *option != trick_stock(trick)) values[std::string(trick_prefix) + std::string(trick)] = *option;
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
            const bool trick = id.starts_with(trick_prefix) && trick_option(std::string_view(id).substr(trick_prefix.size()));
            if (!trick && !find_entry(id)) ++unknown; // kept: another build of the game may have it
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
        drive_camera(playing);
        // <dev>
        s.client = client;
        // </dev>
        if (const auto key = lower(playing ? level : std::string{}); key != s.map) enter_map(key);
        if (playing) {
            observe(base, client, now);
            shortcuts();
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
        want_flip_speed(s.boosts.flip);
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
            const bool new_skater = entity && s.class_entity && entity != s.class_entity;
            if (entity) s.class_entity = entity;
            if ((lost || new_skater) && classes_wanted() && !s.class_search_at && now >= s.class_research_after && !finding_classes()) {
                s.class_search_seen = class_searches();
                s.class_search_tries = 0;
                s.class_search_at = now + 2000;
                s.class_research_after = now + 20000;
            }
        }
        set_trick_heights(s.boosts.nocomply, s.boosts.boneless);
        // The speeds pushes aim for are the push class's (value_links ties them to the tuning's top
        // pushing speed, which only gates whether a push may start).
        const auto *top_speed = find_entry("physicspush.maxpushablespeed");
        set_push_speed(client, s.entity, 1.0f, top_speed ? static_cast<float>(top_speed->stock) : 0.0f, s.boosts.cruise);
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
// Every tick: the framing for what the skater is doing, or none.
void drive_camera(bool playing) {
    auto &s = state();
    multiplayer::CustomCamera camera;
    const auto &set = s.telemetry.physics_state == addr::no_bail::offboard_physics_state ? s.camera_foot : s.camera_board;
    camera.on = s.camera_on && playing && s.telemetry.skater;
    camera.position = s.telemetry.position;
    camera.heading = s.telemetry.heading;
    camera.distance = set.distance;
    camera.height = set.height;
    camera.pitch = set.pitch;
    camera.side = set.side;
    camera.fov = set.fov;
    camera.lag = set.lag;
    multiplayer::set_custom_camera(camera);
    want_camera_rigs(s.rigs);
    if (playing && s.telemetry.skater) (void)apply_camera_rigs(GetTickCount64());
}
constexpr std::string_view rig_names[]{"low", "high", "foot"};
float *rig_field(RigSetting &rig, std::string_view name) {
    return name == "distance" ? &rig.distance : name == "height" ? &rig.height : name == "raise" ? &rig.raise : name == "side" ? &rig.side : nullptr;
}
RigSetting sane_rig(RigSetting rig) {
    const auto finite = [](float value, float low, float high, float otherwise) { return std::isfinite(value) ? std::clamp(value, low, high) : otherwise; };
    rig.distance = finite(rig.distance, -1000.0f, 1000.0f, 1.0f);
    rig.height = finite(rig.height, -1000.0f, 1000.0f, 0.0f);
    rig.raise = finite(rig.raise, -1000.0f, 1000.0f, 0.0f);
    rig.side = finite(rig.side, -1000.0f, 1000.0f, 1.0f);
    return rig;
}
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
        return "Reverts are judged the way they shipped: 90 degrees of spin, the board 12 degrees off the travel or 40 off the body.";
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
    for (const auto &feel : feels) (void)remove_preset(feel.second);
    if (found != std::end(feels)) (void)apply_preset(found->second);
    changed();
    return found == std::end(feels) ? "Plays like skate.: the game's own tuning." : std::format("Plays like {}: its tuning for everything this game shares with it.", found->second);
}
// trainer trickline extras on|off: what Skate 3's trick lines had beyond its tuning. Speed back
// out of reverts, and heavier revert and powerslide friction (x3 and x1.5: a first guess).
std::string trickline_command(const std::vector<std::string> &a) {
    auto &s = state();
    std::string why;
    if (!editable(&why)) return "error: " + why;
    bool on{};
    if (a.size() < 2 || lower(a[0]) != "extras" || !flag(a[1], on)) return "error: usage: trainer trickline extras on|off";
    for (const auto &[key, factor] : trickline_friction)
        if (auto *e = find_entry(key); e && !is_locked(*e)) set_entry(*e, on ? e->stock * factor : e->stock);
    s.revert_boost = on ? std::max(s.revert_boost, 1.0f) : 0.0f;
    changed();
    return on ? "Tricklining extras on: the board bending boost, heavier revert and powerslide friction." : "Tricklining extras off.";
}
// The game's own cameras: `trainer camera rig low|high|foot <field> <number>`, or a preset for all three.
std::string rig_command(const std::vector<std::string> &a) {
    auto &s = state();
    const auto arg = [&](std::size_t i) { return i < a.size() ? lower(a[i]) : std::string{}; };
    if (arg(1) == "stock") {
        s.rigs = {};
        changed();
        return "The game's cameras are where the game puts them.";
    }
    if (arg(1) == "sunjay") {
        // SunJay's Low Cam (thunderstore.io/c/reskate/p/SunJayTeam/SunJays_Low_Cam): both on-board
        // cameras 1.2 m back, 0.4 m lower, turning about a point 0.4 m lower.
        s.rigs[0] = {1.2f / 1.9f, -0.4f, -0.4f, 1.0f / 1.35f};
        s.rigs[1] = {1.2f / 3.0f, -0.4f, -0.4f, 1.0f};
        changed();
        return "Both on-board cameras set like SunJay's Low Cam.";
    }
    if (arg(1) == "low+") {
        s.rigs[0] = {0.8f, -0.25f, -0.15f, 0.85f};
        changed();
        return "Low camera: lower and closer, with more of the skater in view.";
    }
    const auto found = std::ranges::find(rig_names, arg(1));
    auto *field = found != std::end(rig_names) ? rig_field(s.rigs[static_cast<std::size_t>(found - std::begin(rig_names))], arg(2)) : nullptr;
    const auto value = number(arg(3));
    if (!field || !value) return "error: usage: trainer camera rig low|high|foot distance|height|raise|side <number>, or camera rig stock|sunjay|low+";
    *field = static_cast<float>(*value);
    auto &rig = s.rigs[static_cast<std::size_t>(found - std::begin(rig_names))];
    rig = sane_rig(rig);
    changed();
    return std::format("Camera {} {} = {:.3g}. {}", arg(1), arg(2), *field, camera_rigs_summary());
}
// trainer camera on|off | game | preset <name> | set board|foot <field> <number>
std::string camera_command(const std::vector<std::string> &a) {
    auto &s = state();
    const auto arg = [&](std::size_t i) { return i < a.size() ? lower(a[i]) : std::string{}; };
    const auto action = arg(0);
    if (action == "rig") return rig_command(a);
    bool on{};
    if (flag(action, on)) {
        s.camera_on = on;
        changed();
        return on ? "Your camera is on. It does not avoid walls; switch it off to get the game's back." : "The game's own camera is back.";
    }
    if (action == "game") {
        const auto profile = multiplayer::gameplay_camera_profile();
        if (profile.samples < 60) return "error: skate for a few seconds with the game's own camera first, so it can be measured.";
        s.camera_board = sane_camera({profile.distance, profile.height, profile.pitch, 0.0f, 0.0f, 0.1f});
        changed();
        return std::format("On board: the game's framing, {:.1f} m behind, {:.1f} m up, tilt {:.0f} degrees.", profile.distance, profile.height, profile.pitch);
    }
    if (action == "preset") {
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + lower(a[i]);
        for (const auto &preset : camera_presets)
            if (lower(preset.name) == name) {
                (preset.foot ? s.camera_foot : s.camera_board) = preset.set;
                s.camera_on = true;
                changed();
                return std::format("Camera: {} ({}).", preset.name, preset.foot ? "on foot" : "on board");
            }
        return "error: no camera preset is called \"" + name + "\".";
    }
    if (action == "set") {
        auto *set = arg(1) == "board" ? &s.camera_board : arg(1) == "foot" ? &s.camera_foot : nullptr;
        auto *field = set ? camera_field(*set, arg(2)) : nullptr;
        const auto value = number(arg(3));
        if (!field || !value) return "error: usage: trainer camera set board|foot distance|height|pitch|side|fov|lag <number>";
        *field = static_cast<float>(*value);
        *set = sane_camera(*set);
        changed();
        return std::format("Camera {} {} = {:.3g}", arg(1), arg(2), *field);
    }
    return "error: usage: trainer camera on|off | game | preset <name> | set board|foot <field> <number>";
}
std::string run(std::string_view verb, const std::vector<std::string> &a) {
    auto &s = state();
    const auto v = lower(verb);
    const auto arg = [&](std::size_t i) { return i < a.size() ? a[i] : std::string{}; };
    if (v == "status") {
        std::size_t touched{}, frozen{};
        for (const auto &e : s.entries) { touched += e.touched; frozen += e.frozen; }
        return std::format("{} {} values ({} changed, {} frozen). Map: {}. Presets: {}.", s.status, s.entries.size(), touched, frozen,
                           s.map.empty() ? "none" : s.map, s.active.empty() ? "stock" : std::to_string(s.active.size()) + " applied");
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
        if (v == "reset" && lower(arg(0)) == "tricks") {
            s.hippy_height = s.nocomply_height = s.boneless_height = s.offboard_height = s.flip_speed = 1.0f;
            s.revert_boost = 0.0f;
            changed();
            return "Trick heights and flip trick speed are the game's own again.";
        }
        if (v == "reset" && lower(arg(0)) == "presets") {
            const auto on = s.active;
            for (const auto &name : on) (void)remove_preset(name);
            s.active.clear();
            changed();
            return std::format("{} presets switched off.", on.size());
        }
        if (v == "reset" && lower(arg(0)) == "everything") {
            // The game as it shipped: no lock survives, no preset stays on, no trick multiplier.
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
            s.camera_on = false;
            s.rigs = {};
            s.hippy_height = s.nocomply_height = s.boneless_height = s.offboard_height = s.flip_speed = 1.0f;
            s.revert_boost = 0.0f;
            s.revert = {};
            changed();
            return std::format("Everything is the game's own again: {} values put back, locks cleared, presets off, trick sliders at 1.", count);
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
            for (const auto trick : trick_names)
                if (const auto *option = trick_option(trick); *option != trick_stock(trick)) values[std::string(trick_prefix) + std::string(trick)] = *option;
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
    if (v == "camera") return camera_command(a);
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
        if (name == "flip_speed") {
            const auto value = number(arg(1));
            if (!value) return "error: flip_speed needs a multiplier, 1 = the game's own speed.";
            if (!editable(&why)) return "error: " + why;
            s.flip_speed = std::clamp(static_cast<float>(*value), flip_low, flip_high);
        } else if (name == "hippy_height" || name == "nocomply_height" || name == "boneless_height" || name == "offboard_height") {
            const auto value = number(arg(1));
            if (!value) return "error: " + name + " needs a multiplier, 1 = the game's own height.";
            if (!editable(&why)) return "error: " + why;
            (name == "hippy_height" ? s.hippy_height : name == "nocomply_height" ? s.nocomply_height : name == "offboard_height" ? s.offboard_height : s.boneless_height) =
                std::clamp(static_cast<float>(*value), height_low, height_high);
        } else if (name == "revert_boost") {
            const auto value = number(arg(1));
            if (!value) return "error: revert_boost needs a strength: 0 is off, 1 gives back about 2 m/s on an auto revert.";
            if (!editable(&why)) return "error: " + why;
            s.revert_boost = std::clamp(static_cast<float>(*value), 0.0f, revert_boost_high);
        } else if (name == "return_delay") {
            const auto value = number(arg(1));
            if (!value) return "error: return_delay needs seconds.";
            s.return_delay = std::clamp(static_cast<float>(*value), 0.0f, 10.0f);
        } else if (!flag(arg(1), on)) {
            return "error: usage: trainer option hud|hud_jump|auto_return|pad|pad_menu|log 0|1";
        } else if (name == "hud") s.hud = on;
        else if (name == "hud_jump") s.hud_jump = on;
        else if (name == "auto_return") s.auto_return = on;
        else if (name == "pad") s.pad_shortcuts = on;
        else if (name == "pad_menu") s.pad_menu = on;
        // <dev>
        else if (name == "statelog") s.state_log = on;
        else if (name == "fliptrace") s.flip_trace = on;
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
    if (v == "open") {
        const auto tab = lower(arg(0));
        // The last three are the Tune tab on one of its lists.
        const std::array<std::string_view, 9> tabs{"tune", "presets", "practice", "map", "realistic", "fun", "everything", "camera", "trickline"};
        const auto found = std::ranges::find(tabs, tab);
        if (!tab.empty() && found == tabs.end()) return "error: usage: trainer open [tune|trickline|practice|camera|map|realistic|fun|everything]";
        s.open_tab = tab.empty() ? 1 : static_cast<int>(found - tabs.begin());
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
    if (v == "states") {
        std::string result;
        for (const auto &[id, ticks] : s.states) result += std::format("{}{}: {}", result.empty() ? "" : ", ", id, ticks);
        return result.empty() ? "No physics state seen yet." : "Physics states seen (id: ticks): " + result;
    }
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
        return std::format("Wrote {} values to {}.", s.entries.size(), (directory / L"tuning-values.txt").string());
    }
    if (v == "selftest") {
        if (s.test.step) return "The self test is already running.";
        s.test = {};
        s.test.step = 1;
        s.test.until = GetTickCount64() + 500;
        return "Self test started; see the log for \"trainer selftest:\" lines.";
    }
    return "error: trainer status|open|set|freeze|reset|find|preset|slot|marker|tp|spot|option|profile|jumps|where|states|dump|selftest";
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
