#include "tricks.h"
#include "tricks_states.h"
#include "tricks_board.h"
#include "Extension/Trainer/trainer.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <thread>
#include <filesystem>
#include <fstream>

#include "Extension/Skater/no_bail.h"

// The first controller as the game polls it, and the mask that keeps freestyle's inputs from the game
// (Extension/UI/Overlay/input_capture.cpp).
extern "C" void DingoSDKOverlayTricksPad(void *gamepad);
extern "C" void DingoSDKOverlayTricksMask();
extern "C" void DingoSDKOverlayTricksAutoMask();
extern "C" void DingoSDKOverlayTricksMaskLeft();
// <dev> The scripted controller (Extension/UI/Overlay/input_capture.cpp), for test runs.
extern "C" void DingoSDKOverlayScriptController(const void *gamepad);
extern "C" void DingoSDKOverlayScriptControllerPolls(std::uint64_t *polls, std::uint64_t *served);
// </dev>

// Freestyle (caspers and primos). The game ships the whole animation side: the riding node goes
// into onboard.freestyle.node while Bool.Intent.WantsFreestyle is set, its conduit picks casper,
// primo or primo2 by EnumGS.Intent.FreestyleTrick (0, 1, 2) and moves between them when that
// changes, and every freestyle node tags CurrentGroundTrick 7, the value that keeps the physics
// following the clip. Clearing WantsFreestyle leaves: from a casper with FreeStyleFlipTrick set
// it flips out (cdb_onboard_freestylecasperfliptrick, two clips), otherwise it steps off.
// Nothing in the game's scripts sets those states any more, so the mod does.
namespace dingosdk::tricks {
namespace {
enum class Freestyle : int { off = -1, casper = 0, primo = 1, primo2 = 2 };

// <dev> A scripted controller: each step holds one controller state for a time (`tricks pad`).
struct PadStep {
    std::array<std::uint8_t, 12> pad{}; // XINPUT_GAMEPAD
    std::uint64_t ms{};
};
struct PadScript {
    std::vector<PadStep> steps;
    std::size_t index{};
    std::uint64_t step_start{};
    bool running{}, connected{};
};
// </dev>

struct Runtime {
    std::uintptr_t base{}, entity{};
    std::uint64_t next_skater{}, next_search{}, next_resolve{};
    int resolved_count{-9};
    bool hooks{};
    Freestyle freestyle{Freestyle::off};
    std::uint32_t exit_flip{};    // FreeStyleFlipTrick to leave a casper with; 0 steps off
    std::uint64_t leaving_until{}; // while leaving: WantsFreestyle held off, the exit flip held
    bool leaving{};                // states still to be put back after freestyle
    bool setters{};                // the setter watch is installed
    bool board{};                  // the procedural board hook is installed
    bool mode_hold{};              // `tricks mode hold`: answer lookups instead of writing (diagnostic)
    std::uint64_t writes{};
    PadScript pad;
    // Controls: hold the left stick's click on the board; the right stick picks the trick.
    bool controls{true};
    bool stick_armed{true}; // the right stick has been back near the centre since the last flick
    bool by_pad{};          // the current freestyle was started from the controller (its release steps out)
    // Casper and primo slides: m/s of speed scrubbed off each second while in a pose. A first guess above a grind's
    // own slowdown; tuned in play (`tricks slide`).
    float slide{2.5f};
    bool casper_balance{true};
    std::uintptr_t s_balance{};
    Freestyle shown{Freestyle::off};       // what the last tick drove
    Freestyle after_primo{Freestyle::off}; // the pose asked for while rolling: taken up from the primo
    std::uint64_t after_primo_at{};
    // <dev> `tricks state` / `tricks fstate`: any game state held at a value (fstate: only while freestyle is on).
    struct Extra {
        std::string name;
        int kind{};
        double value{};
        bool freestyle_only{};
        std::uintptr_t at{};
        std::uint64_t writes{};
    };
    std::vector<Extra> extras;
    // </dev>
    // No comply flips. A no comply is the game's own (a push, then the right stick down). Holding the flip trick the
    // scripts would ask for (EnumGS.Intent.FlipTrick, with Float.Intent.TrickFlipSpeed) through its air makes the game
    // run and score that flip after it ("No Comply, Kickflip": seen 2026-10-12). The trick is picked with a right
    // stick flick once the no comply has started, or armed beforehand with `tricks ncflip`.
    bool nc_flips{true};
    bool nc_experimental{true}; // the game's experimental no comply clips: they land clean where the stock ones are sketchy
    std::uint64_t nc_seen_at{};  // when the no comply was last active
    bool nc_armed{};             // the right stick has come back to the centre since it started the no comply
    std::uint32_t nc_trick{};    // the flip being held (0: none)
    std::uint32_t nc_next{};     // armed from the console for the next no comply
    std::uint64_t nc_until{};
    bool nc_air{};               // the held flip has been in the air
    // In-air extras.
    bool air_extras{true};
    bool air_click{};              // the click as the last tick saw it
    int air_used{};                // extras done in this air (two at most)
    float flip_sign{1.0f};         // which way a kickflip's side rolls (`tricks air sign`)
    std::uint32_t air_spin_ms{320};
    std::uintptr_t s_current_flip{};
    std::uint32_t nc_spin_ms{420}; // how long the board takes to turn
    std::uintptr_t s_nc_intent{}, s_nc_anim{}, s_flip{}, s_flip_speed{}, s_nc_exp{}, s_boneless{}, s_is_boneless{}, s_is_footplant{};
    bool plant_seen{};
    bool plant_flip{}; // the flip being held started from a boneless or footplant, not a no comply
    // Ground shuvit (no pop). The board only follows a target frame while the physics is in GROUND_ANIMATION (103),
    // so CurrentGroundTrick 7 is held for the length of the turn and the board is turned half way round under the feet.
    bool ground_shuvs{true};
    bool ground_click{};          // the click as the last tick saw it (on the ground)
    int gs_phase{};               // 0 none, 1 turning (7 held), 2 letting go (waiting for the physics to leave 103)
    std::uint64_t gs_until{};
    std::uint32_t gs_ms{380};
    // The owner's control scheme (2026-10-12): poses are landed into and held on the right stick.
    //   Land a flip trick with the right stick held part of the way left / right: primo / primo2.
    //   Land it with the left stick clicked and the right stick held part of the way out: casper.
    //   In a pose the stick stays part of the way out. To the centre: out of the pose (the casper flips out). A flick
    //   (out to the edge and back): the casper flips out with a kickflip, heelflip, varial or 360 flip turn on top.
    //   Held at the edge: the pose is lost and the skater comes off.
    bool hold_scheme{true};
    bool by_hold{}, was_air{}, air_flipped{};
    std::uint64_t centre_since{}, edge_since{}, edge_last{}, air_click_at{};
    float edge_x{}, edge_y{};
    float out_roll{}, out_yaw{};
    std::uint64_t out_until{}; // a flick out of the casper: its turn starts when the exit is in the air
    bool out_spinning{};
    std::uint64_t fall_until{};
    std::uintptr_t s_late_flip{};
    std::uint64_t mod_since{}, landed_at{}, air_last{};
    bool catch_armed{true}, edge_brief{};
    std::uint64_t air_start{}, last_centre{1}, last_pop{};
    bool edge_fall{true};   // a primo with the stick held at the outside is lost
    bool centre_fall{true}; // a casper with the stick back at the middle is lost
    bool fall_slam{};
    bool left_mask{true};
    bool air_pose{true}, air_pose_on{};
    // (Holding the plain in-air states true to cut a flip short made a casper bounce again and cost most of the speed:
    // measured 2026-10-13. None by default.)
    std::vector<std::string> air_names;
    std::uint64_t pose_since{}, unhold_until{};
    std::vector<std::uintptr_t> s_air;
    std::uintptr_t s_filtered{};
    bool two_primos{};
    bool early_exit{true}, early_on{};
    std::string early_name{"Animation/Dingo/Bool.Intent.TrickSetup"};
    std::uintptr_t s_early{};
    bool route{true}; // a rolling casper or primo2 goes in through the primo
    // Where the stick's down (the casper's place) ends and the sides (the primos') begin, from the owner's trace:
    // his caspers rest at 270 to 304, his primos at 210 to 221 and 311 to 6.
    float zone_left{245.0f}, zone_right{307.0f};
    Freestyle zone{Freestyle::off};
    std::uint64_t zone_since{}, shuv_down_at{}, shuv_side_at{};
    int shuv_side{};
    bool trace{true};
    std::uint64_t primo_out_at{};
    std::uint32_t primo_out_ms{200}; // 80 was too short for the change of pose to register: the primo then left by its own exit
    bool swiped{};
    // Off: landing on a board turned in the air is a bail every time (measured 2026-10-12).
    bool catch_turn{true}, catch_spinning{};
    Freestyle turn_for{Freestyle::off};
    // Measured 2026-10-12: a primo caught on its side lands (under the protection); a casper caught upside down, or even
    // on its side, bounces back into the air twice and loses the pose, so the casper's board is left alone in the air.
    float primo_roll{90.0f}, primo2_roll{90.0f}, casper_roll{0.0f}, casper_drop{0.0f};
    bool protect{true};
    std::uint64_t low_last{};
    float catch_drop{0.1f}; // the caught board is moved down from under the feet: on its side or upside down it would sit in them
    float upright{0.0f};
};

// The right stick's flick while the left stick is clicked in.
enum class Flick { none, up, down, left, right, up_left, up_right };
Flick read_flick(Runtime &r, float x, float y) {
    const float length = std::sqrt(x * x + y * y);
    if (length < 0.35f) r.stick_armed = true;
    if (!r.stick_armed || length < 0.75f) return Flick::none;
    r.stick_armed = false;
    const float angle = std::atan2(y, x) * 180.0f / 3.14159265f; // 0 right, 90 up
    if (angle >= 112.5f && angle < 157.5f) return Flick::up_left;
    if (angle >= 22.5f && angle < 67.5f) return Flick::up_right;
    if (angle >= 67.5f && angle < 112.5f) return Flick::up;
    if (angle >= -135.0f && angle < -45.0f) return Flick::down;
    if (angle >= -45.0f && angle < 22.5f) return Flick::right;
    if (angle >= 157.5f || angle < -135.0f) return Flick::left;
    return Flick::none;
}
Runtime &runtime() {
    static Runtime r;
    return r;
}
// The pose in the air: the graph's copy of the physics state family, written in place every physics step (the game
// writes it back each step; the animation reads it after).
struct AirPoke {
    std::atomic<std::uintptr_t> base{}, entity{}, state{};
    std::atomic<std::uint64_t> until{};
    std::atomic<std::uint32_t> pokes{}, put_back{};
};
AirPoke air_poke;
void air_poke_step() noexcept {
    if (GetTickCount64() >= air_poke.until.load(std::memory_order_relaxed)) return;
    std::uint32_t before{};
    if (poke_state_value(air_poke.base.load(std::memory_order_relaxed), air_poke.entity.load(std::memory_order_relaxed), air_poke.state.load(std::memory_order_relaxed), 1, &before)) {
        air_poke.pokes.fetch_add(1, std::memory_order_relaxed);
        if (before != 1) air_poke.put_back.fetch_add(1, std::memory_order_relaxed);
    }
}
void announce(std::string_view name);
void start_ground_shuv(Runtime &r, std::uint64_t now, bool frontside) {
    start_board_spin(0, frontside ? -180.0f : 180.0f, 0, r.gs_ms);
    r.gs_phase = 1;
    r.gs_until = now + r.gs_ms + 60;
    announce(frontside ? "FS Shuvit" : "Shuvit");
    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: ground {} shuvit.", frontside ? "frontside" : "backside");
}
std::atomic<bool> searching{};

void search_in_background() {
    if (searching.exchange(true)) return;
    std::thread([] {
        (void)search_state_objects();
        searching.store(false);
    }).detach();
}
void release_all() noexcept {
    for (std::size_t i = 0; i < state_count; ++i) hold(static_cast<State>(i), false);
}
bool any_hold(const Runtime &r, std::uint64_t now) noexcept { return r.freestyle != Freestyle::off || now < r.leaving_until; }

// The name of a mod trick, shown as a ReSkate notice (the game's own trick line does not know these tricks).
bool announce_tricks = true;
void announce(std::string_view name) {
    if (announce_tricks && !name.empty()) overlay::notify(overlay::NoticeLevel::info, "TRICK", std::string(name));
}
std::string_view flip_name(std::uint32_t trick) {
    switch (trick) {
    case 2: return "Kickflip";
    case 3: return "Heelflip";
    case 4: return "Pop Shuvit";
    case 7: return "FS Pop Shuvit";
    case 9: return "Hardflip";
    case 11: return "Tre Flip";
    default: return "Flip";
    }
}
// One more flip on the trick that is playing.
std::string_view extra_flip_name(int trick) {
    switch (trick) {
    case 0: case 1: case 16: case 31: case 32: case 42: return "Late Flip";
    case 2: case 17: return "Double Kickflip";
    case 3: case 18: return "Double Heelflip";
    case 4: case 19: return "Late Flip Shuvit";
    case 5: case 23: return "Nightmare Flip";
    case 8: case 21: return "Daydream Flip";
    case 11: case 29: return "Double Tre Flip";
    case 9: case 20: return "Double Hardflip";
    case 6: case 24: return "Double Inward Heelflip";
    case 14: case 27: return "Double Laser Flip";
    default: return "Double Flip";
    }
}
std::string_view freestyle_name(Freestyle f) {
    switch (f) {
    case Freestyle::casper: return "casper";
    case Freestyle::primo: return "primo";
    case Freestyle::primo2: return "primo2";
    default: return "off";
    }
}
bool number(std::string_view text, std::uint32_t &value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

// <dev>
// "a+ry=-1:300" -> hold A with the right stick down for 300 ms. "n:500" -> nothing held.
std::optional<PadStep> pad_step(std::string_view text) {
    const auto colon = text.rfind(':');
    std::uint32_t ms{};
    if (colon == std::string_view::npos || !number(text.substr(colon + 1), ms) || ms > 60000) return std::nullopt;
    PadStep step;
    step.ms = ms;
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
        const auto word = rest.substr(0, plus);
        rest = plus == std::string_view::npos ? std::string_view{} : rest.substr(plus + 1);
        const auto equals = word.find('=');
        const auto name = word.substr(0, equals);
        double amount = 1.0;
        if (equals != std::string_view::npos) {
            const auto text_amount = word.substr(equals + 1);
            const auto [end, error] = std::from_chars(text_amount.data(), text_amount.data() + text_amount.size(), amount);
            if (error != std::errc{} || end != text_amount.data() + text_amount.size()) return std::nullopt;
        }
        if (std::abs(amount) > 1) return std::nullopt;
        if (name == "n" || name.empty()) continue;
        bool known = false;
        for (const auto &[button, bit] : names)
            if (name == button) { buttons |= bit; known = true; }
        for (std::size_t i = 0; i < 4; ++i)
            if (name == axes[i]) { sticks[i] = static_cast<std::int16_t>(std::lround(amount * 32767.0)); known = true; }
        if (name == "lt" || name == "rt") {
            triggers[name == "rt"] = static_cast<std::uint8_t>(std::lround(std::abs(amount) * 255.0));
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
void run_pad(PadScript &p, std::uint64_t now) {
    if (!p.running) return;
    while (p.index < p.steps.size() && now - p.step_start >= p.steps[p.index].ms) {
        p.step_start += p.steps[p.index].ms;
        ++p.index;
    }
    if (p.index < p.steps.size()) {
        DingoSDKOverlayScriptController(p.steps[p.index].pad.data());
        return;
    }
    // The pad stays connected with nothing held until `tricks pad off`: a pad that comes and goes
    // drops the game to keyboard mode.
    static constexpr std::array<std::uint8_t, 12> neutral{};
    DingoSDKOverlayScriptController(neutral.data());
    p.running = false;
}
// </dev>
}

void tick(std::uintptr_t base, std::uintptr_t client, bool playing) noexcept {
    auto &r = runtime();
    r.base = base;
    const auto now = GetTickCount64();
    run_pad(r.pad, now);
    // Before the first mount: the skater's key is taken from the game's own write at the mount.
    if (!r.setters) r.setters = start_setter_watch(base);
    if (!r.board) r.board = start_board_hook(base);
    if (!playing) {
        if (r.entity) {
            r.entity = 0;
            r.freestyle = Freestyle::off;
            r.leaving_until = 0;
            r.leaving = false;
            release_all();
            forget_skater_key();
        }
        return;
    }
    if (now >= r.next_skater) {
        try {
            overlay::DebugModel skater;
            (void)client_source::detail::debug_skater(base, client, skater);
            r.entity = skater.skater_position_valid ? skater.skater_identity : 0;
            // Where the rig writes the board's target frame: the procedural board turns that one only.
            set_board_context(r.entity ? client_source::detail::debug_noclip_bodies(base, client, r.entity).context : 0);
        } catch (...) {
            r.entity = 0;
            set_board_context(0);
        }
        r.next_skater = now + (r.entity ? 1000 : 250);
    }
    // <dev> The physics state's changes around a pose, for the test runs' logs.
    {
        static int last_state = -1;
        static std::uint64_t pose_seen{};
        const auto physics = watched_physics_state();
        if (r.freestyle != Freestyle::off || r.leaving) pose_seen = now;
        if (physics.valid && static_cast<int>(physics.state) != last_state) {
            if (now - pose_seen < 4000)
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: physics {} -> {} ({}, {:.1f} m/s) at {} ms.", last_state, static_cast<int>(physics.state),
                             freestyle_name(r.freestyle), trainer::telemetry().speed, now % 100000);
            if (last_state == 103) {
                float peak{}, swing{};
                take_freestyle_slide_roll(peak, swing);
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: slide rocking: peak {:.2f} rad/s, swing {:.1f} degrees.", peak, swing);
            }
            last_state = static_cast<int>(physics.state);
        }
    }
    // </dev>
    // <dev> The trace: the controller as the player has it and what the skater is doing, one line per change, in the
    // game's logs folder (tricks-trace.csv). The owner plays the inputs he wants a trick on; the controls are built
    // from the file.
    if (r.trace && skater_key_known()) {
        static std::ofstream file;
        static std::string last;
        static std::uint64_t start{};
        static std::uintptr_t s_flip{}, s_dark{}, s_window{};
        if (!file.is_open()) {
            std::array<wchar_t, 1024> module{};
            GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
            const auto folder = std::filesystem::path(module.data()).parent_path() / L"logs";
            std::error_code error;
            std::filesystem::create_directories(folder, error);
            file.open(folder / L"tricks-trace.csv", std::ios::app);
            file << "# session " << now << "\nms,buttons,lt,rt,lx,ly,rx,ry,physics,flip,dark,window,pose,speed\n";
            start = now;
        }
        std::array<std::uint8_t, 12> pad{};
        DingoSDKOverlayTricksPad(pad.data());
        std::uint16_t buttons{};
        std::int16_t sticks[4]{};
        std::memcpy(&buttons, pad.data(), 2);
        std::memcpy(sticks, pad.data() + 4, 8);
        const auto state_value = [&](std::uintptr_t &at, const char *name, int kind) {
            if (!at) at = find_state(base, name);
            double value{};
            return at && read_state_at(base, at, kind, value) ? static_cast<int>(value) : -1;
        };
        const auto physics = watched_physics_state();
        const auto tenth = [](std::int16_t v) { return std::lround(v / 32767.0 * 10.0) / 10.0; };
        auto line = std::format("{:04x},{},{},{:.1f},{:.1f},{:.1f},{:.1f},{},{},{},{},{}", buttons, pad[2] / 26, pad[3] / 26, tenth(sticks[0]), tenth(sticks[1]),
                                tenth(sticks[2]), tenth(sticks[3]), physics.valid ? static_cast<int>(physics.state) : -1,
                                state_value(s_flip, "Animation/Dingo/EnumGS.Anim.CurrentFlipTrick", 1), state_value(s_dark, "Animation/Dingo/Bool.GP.IsDark", 0),
                                state_value(s_window, "Animation/Dingo/Bool.Anim.IsInDarkCatchWindow", 0), freestyle_name(r.freestyle));
        if (line != last && file.is_open()) {
            file << (now - start) << ',' << line << ',' << std::format("{:.1f}", trainer::telemetry().speed) << '\n';
            file.flush();
            last = std::move(line);
        }
    }
    // </dev>
    // The states the game itself uses, as soon as its EnableCaspers write has shown where they live.
    if (skater_key_known() && (!state_objects_resolved() || !state_objects_known()) && now >= r.next_resolve) {
        r.resolved_count = resolve_state_objects(base);
        r.next_resolve = now + 500;
    }
    if (!state_objects_known() && !state_objects_resolved() && now >= r.next_search) {
        search_in_background();
        r.next_search = now + 2000;
    }
    // Controls. Left stick click held on the board: the right stick's flick picks the trick, and the game sees
    // neither (no ollie or flip from the same flick).
    //   down: casper   left: primo   right: primo2
    //   up (in a casper): flip out (first clip)   up-left / up-right (in a casper): flip out (second clip)
    //   let go of the click: step out
    if (r.controls && skater_key_known()) {
        std::array<std::uint8_t, 12> pad{};
        DingoSDKOverlayTricksPad(pad.data());
        std::uint16_t buttons{};
        std::int16_t rx{}, ry{};
        std::memcpy(&buttons, pad.data(), 2);
        std::memcpy(&rx, pad.data() + 8, 2);
        std::memcpy(&ry, pad.data() + 10, 2);
        const bool click = (buttons & 0x40) != 0; // XINPUT_GAMEPAD_LEFT_THUMB
        const auto physics = watched_physics_state();
        const bool on_ground = physics.valid && physics.state >= 100 && physics.state <= 105;
        // A ground shuvit: the click pressed with the right stick already held left or right, while riding.
        const bool click_pressed = click && !r.ground_click;
        r.ground_click = click;
        const float held_x = rx / 32767.0f, held_y = ry / 32767.0f;
        if (r.ground_shuvs && !r.hold_scheme && click_pressed && physics.valid && physics.state == 100 && r.freestyle == Freestyle::off && !r.gs_phase && !r.nc_trick &&
            state_objects_known() && std::abs(held_x) > 0.6f && std::abs(held_x) >= std::abs(held_y)) {
            r.stick_armed = false; // the same stick is not also a primo pick
            start_ground_shuv(r, now, held_x > 0);
        }
        const bool in_air = physics.valid && physics.state >= 200 && physics.state < 300;
        // Off the board (bailed, stepped off): no pose is left to hold.
        if (r.by_hold && r.freestyle != Freestyle::off && physics.valid && physics.state >= 300) {
            r.freestyle = Freestyle::off;
            r.by_hold = false;
            r.exit_flip = 0;
            r.leaving_until = now;
            if (r.catch_spinning) stop_board_spin();
            r.catch_spinning = false;
        }
        const float held = std::sqrt(held_x * held_x + held_y * held_y);
        if (r.hold_scheme) {
            r.stick_armed = true; // (the click + flick scheme's: a stale false here hid the stick whenever the click was held)
            // The catch: in the air, the left stick's click or the left trigger held with the right stick out picks the
            // pose (left / right: primo / primo2; up / down: casper). The board is turned to it there and then, and the
            // game's own pose takes over on the ground.
            const bool modifier = click || pad[2] > 128;
            if (!in_air || !modifier) r.mod_since = 0;
            else if (!r.mod_since) r.mod_since = now;
            // The catch, as the owner plays it (his recorded trace, 2026-10-12): the click is held all along, the trick
            // is flicked as ever, and the stick goes straight from the flick to where the pose is wanted and rests
            // there through the landing: down for a casper, to either side for a primo. So the pick is where the stick
            // rests, read again until the board is down. Angles: 0 right, 90 up, 270 down.
            if (in_air && !r.was_air) r.air_start = now;
            if (physics.valid) r.was_air = in_air;
            if (in_air) r.air_last = now;
            {
                // The pop: the game has taken the flick (its flip trick is set on the ground), or the stick is at its
                // upper edge. From then on the click hides the stick from the game: a push seen in the pop's own states
                // lands the skater crooked.
                if (!r.s_current_flip) r.s_current_flip = find_state(base, "Animation/Dingo/EnumGS.Anim.CurrentFlipTrick");
                double current{};
                if (on_ground && r.s_current_flip && read_state_at(base, r.s_current_flip, 1, current) && current != 0) r.last_pop = now;
                if (on_ground && held > 0.8f && held_y > 0.3f) r.last_pop = now;
            }
            if (!(on_ground && now - r.last_pop >= 700)) DingoSDKOverlayTricksAutoMask();
            float angle = std::atan2(held_y, held_x) * 180.0f / 3.14159265f;
            if (angle < 0) angle += 360.0f;
            Freestyle zone = Freestyle::off;
            if (held > 0.3f) {
                if (angle > r.zone_left && angle < r.zone_right) zone = Freestyle::casper;
                else if (angle >= 150.0f && angle <= r.zone_left) zone = Freestyle::primo;
                else if (angle >= r.zone_right || angle <= 40.0f) zone = Freestyle::primo2;
            }
            // One primo for both sides: a flick out of primo2 never popped in the runs (its way over to the casper is
            // 50 ticks against the primo's 30), the primo's does.
            if (!r.two_primos && zone == Freestyle::primo2) zone = Freestyle::primo;
            if (zone != r.zone) {
                r.zone = zone;
                r.zone_since = now;
            }
            const bool steady = zone != Freestyle::off && now - r.zone_since >= 60;
            const bool catching = in_air || (on_ground && now - r.air_last < 350); // the landing's first moment still counts
            if (catching) {
                if (modifier && held > 0.3f) DingoSDKOverlayTricksMask(); // not a grab, a manual or another flip
                if (r.freestyle == Freestyle::off && !r.nc_trick && !r.leaving && modifier && steady && state_objects_known()) {
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: caught in {} (stick at {:.0f}).", freestyle_name(zone), angle);
                    announce(zone == Freestyle::casper ? "Casper" : zone == Freestyle::primo ? "Primo" : "Primo 2");
                    r.freestyle = zone;
                    r.by_hold = true;
                    r.landed_at = r.centre_since = r.edge_since = r.edge_last = 0;
                    r.edge_brief = false;
                    r.turn_for = Freestyle::off;
                } else if (in_air && r.by_hold && r.freestyle != Freestyle::off) {
                    const Freestyle wanted = r.after_primo != Freestyle::off ? r.after_primo : r.freestyle;
                    if (held < 0.25f || !modifier) {
                        // Let go before the landing: no pose.
                        r.freestyle = Freestyle::off;
                        r.by_hold = false;
                        r.exit_flip = 0;
                        r.leaving_until = now;
                    } else if (steady && zone != wanted) {
                        // The stick moved on: what it rests on at the landing is the pose.
                        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: catch changed to {} (stick at {:.0f}).", freestyle_name(zone), angle);
                        r.freestyle = zone;
                        r.after_primo = Freestyle::off;
                        r.shown = Freestyle::off;
                    }
                }
                if (r.after_primo != Freestyle::off) r.after_primo_at = now + 700; // the way in through the primo starts at the landing
            }
            // The ground shuvit, as the owner plays it: the click held, the stick held down at its edge, rolled to a
            // lower diagonal and let go. (Rolled on to the side, it is the game's pop shuvit: not this one.)
            if (r.ground_shuvs && physics.valid && physics.state == 100 && r.freestyle == Freestyle::off && !r.leaving && !r.gs_phase && now - r.air_last > 500) {
                if (held > 0.8f && angle >= 250.0f && angle <= 292.0f) {
                    r.shuv_down_at = now;
                } else if (held > 0.8f && modifier && ((angle > 296.0f && angle <= 338.0f) || (angle >= 205.0f && angle < 246.0f))) {
                    if (r.shuv_side != 0 || now - r.shuv_down_at < 500) {
                        r.shuv_side = angle > 270.0f ? 1 : -1;
                        r.shuv_side_at = now;
                    }
                } else if (held > 0.8f) {
                    r.shuv_side = 0;
                } else if (held < 0.3f && r.shuv_side != 0) {
                    if (now - r.shuv_side_at < 250 && state_objects_known()) start_ground_shuv(r, now, r.shuv_side > 0);
                    r.shuv_side = 0;
                }
            } else r.shuv_side = 0;
            // Caught in the air: the board is turned to the pose there (on its side for a primo, upside down for a
            // casper) and moved down out of the feet, and stays so until the game's own pose has it on the ground.
            if (r.catch_turn && !r.air_pose && in_air && r.by_hold && r.freestyle != Freestyle::off) {
                const Freestyle wanted = r.after_primo != Freestyle::off ? r.after_primo : r.freestyle;
                const float roll = wanted == Freestyle::casper ? r.casper_roll : wanted == Freestyle::primo ? r.primo_roll : r.primo2_roll;
                if (wanted != r.turn_for && roll == 0) {
                    if (r.catch_spinning) stop_board_spin();
                    r.catch_spinning = false;
                    r.turn_for = wanted;
                } else if (wanted != r.turn_for) {
                    start_board_spin(wanted == Freestyle::casper ? r.casper_roll : wanted == Freestyle::primo ? r.primo_roll : r.primo2_roll, 0, 0, 140);
                    set_board_drop(wanted == Freestyle::casper ? r.casper_drop : r.catch_drop);
                    r.turn_for = wanted;
                    r.catch_spinning = true;
                }
            }
            if (r.catch_spinning && (!r.by_hold || r.freestyle == Freestyle::off || (physics.valid && physics.state == 103) || (on_ground && now - r.air_last > 400))) {
                stop_board_spin();
                r.catch_spinning = false;
                r.turn_for = Freestyle::off;
            }
            // The pose is in and held (0.45 s after it took the board, not while it changes or leaves): the slide keeps
            // the board from tipping.
            {
                const bool in_pose = r.by_hold && r.freestyle != Freestyle::off && physics.valid && physics.state == 103;
                if (!in_pose) r.pose_since = 0;
                else if (!r.pose_since) r.pose_since = now;
                const Freestyle current = r.after_primo != Freestyle::off ? r.after_primo : r.freestyle;
                if (in_pose && (r.primo_out_at || r.after_primo != Freestyle::off || (zone != Freestyle::off && zone != current))) r.unhold_until = now + 900;
                set_freestyle_slide_hold(in_pose && now - r.pose_since > 450 && now >= r.unhold_until);
            }
            // No wipeout while a pose is being caught, landed or held: only the stick loses it.
            if (r.protect && r.by_hold && r.freestyle != Freestyle::off) protect_pose(300);
            if (r.by_hold && r.freestyle != Freestyle::off) DingoSDKOverlayTricksMask();
            // On the ground in a pose the left stick is hidden as well. The owner's recording: with the click held his
            // left stick wanders over the whole range (x -0.9 to 1.0), and the game steers and leans the skater with it:
            // the rocking from side to side.
            if (r.left_mask && r.by_hold && r.freestyle != Freestyle::off && !in_air) DingoSDKOverlayTricksMaskLeft();
            // The landing's clip only hands over to riding (and riding to the pose) at its end, 1.1 to 1.3 s after the
            // board is down (measured), unless the player is asking for an action: the landing node leaves at its early
            // window while EBool.Intent.Action is true, and a trick setup is one of the intents behind it. So that
            // intent is held from the landing until the pose has the board.
            {
                // The pose in the air: the animation graph only reaches the poses from riding, and riding from the landing,
                // which it enters when its copy of the physics state family (EnumGS.FilteredPhysicsState: 1 ground, 2 air,
                // 3 grind) says ground. Held at ground while a pose is caught in the air, the graph lands, leaves the
                // landing early and goes into the pose while the board is still up, and in the air the board follows
                // the animation: feet and board take the pose together.
                const bool in_pose_air = r.air_pose && in_air && r.by_hold && r.freestyle != Freestyle::off;
                if (in_pose_air || r.air_pose_on) {
                    if (!r.s_filtered) r.s_filtered = find_state(base, "Animation/Dingo/EnumGS.FilteredPhysicsState");
                    // Answered at the lookup (the game's own write of it wins over a write made here), and written too.
                    // A flip trick's clip carries its own landing and is only left at its branch windows (0.5 s in). The
                    // graph's other way down, the landing node, has no window but wants EBool.Anim.Is.GenericLanding, an
                    // expression over the plain in-air states among others: those are held true, so the flip is cut short.
                    for (std::size_t i = 0; i < r.air_names.size(); ++i) {
                        if (i >= r.s_air.size()) r.s_air.push_back(0);
                        if (!r.s_air[i]) r.s_air[i] = find_state(base, r.air_names[i].c_str());
                        if (in_pose_air || r.air_pose_on) (void)write_state_at(base, r.s_air[i], 0, in_pose_air ? 1 : 0);
                    }
                    if (in_pose_air) {
                        if (!r.hooks) r.hooks = start_holds(base);
                        hold_object(r.s_filtered, 1, 120);
                        (void)write_state_at(base, r.s_filtered, 1, 1);
                        air_poke.base.store(base, std::memory_order_relaxed);
                        air_poke.entity.store(r.entity, std::memory_order_relaxed);
                        air_poke.state.store(r.s_filtered, std::memory_order_relaxed);
                        air_poke.until.store(now + 120, std::memory_order_relaxed);
                        set_tricks_step(&air_poke_step);
                        air_poke_step();
                    } else {
                        hold_object(0, 0, 0);
                        air_poke.until.store(0, std::memory_order_relaxed);
                        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: pose in the air: asked {} times at the lookup; written in place {} times, {} of them after the game had put it back.",
                                     held_object_asked(), air_poke.pokes.exchange(0), air_poke.put_back.exchange(0));
                    }
                    r.air_pose_on = in_pose_air;
                }
                const bool waiting = r.early_exit && r.by_hold && r.freestyle != Freestyle::off && physics.valid && (physics.state == 100 || in_pose_air);
                if (waiting || r.early_on) {
                    if (!r.s_early) r.s_early = find_state(base, r.early_name.c_str());
                    (void)write_state_at(base, r.s_early, 0, waiting ? 1 : 0);
                    r.early_on = waiting;
                }
            }
            // Holding a pose (the owner's rules, 2026-10-12): the click or trigger AND the right stick off its centre.
            //   Casper: the stick back at the middle is a fall; a flick flips out (with a kickflip, heelflip, varial or
            //   360 flip turn on top).
            //   Primo: the stick held at the outside is a fall; the middle flips the board down and rides away; a flick
            //   goes through the casper's flip out with the same turns.
            //   Letting go of the click or trigger is the clean way out of either.
            if (r.by_hold && r.freestyle != Freestyle::off && on_ground) {
                if (!r.landed_at) r.landed_at = now;
                const bool casper = r.freestyle == Freestyle::casper || r.after_primo == Freestyle::casper;
                const auto leave = [&](std::uint32_t flip, std::uint64_t ms) {
                    r.exit_flip = flip;
                    r.freestyle = Freestyle::off;
                    r.leaving_until = now + ms;
                    r.by_hold = false;
                    r.primo_out_at = 0;
                };
                const auto fall = [&](const char *why) {
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: the {} was lost ({}).", freestyle_name(r.freestyle), why);
                    leave(0, 0);
                    protect_pose(0); // the protection would keep the skater on the board through the hop
                    // The owner's pick: the skater hops off and stands (2026-10-12). The other way is a real slam.
                    if (r.fall_slam) request_wipeout();
                    else r.fall_until = now + 400;
                };
                const bool settled = now - r.landed_at > 600; // the landing's first moment is forgiven
                // The stick in its upper half at the edge: a flick if it is brief, a fall if it stays (the owner holds his
                // poses with the stick right at the lower edge, so the edge itself is no fall).
                const bool upper = held > 0.8f && held_y > 0.3f;
                if (held > 0.4f && held_y < 0.1f) r.low_last = now;
                if (r.primo_out_at) {
                    if (now >= r.primo_out_at) {
                        r.out_until = now + 1500;
                        leave(9, 2500);
                    }
                } else if (upper && !r.swiped && !r.edge_since && settled && now - r.low_last < 160) {
                    // Swiped up from where the pose is held: a flick, taken the moment the stick is up.
                    r.edge_x = held_x;
                    r.edge_y = held_y;
                    r.edge_last = now;
                    r.edge_brief = true;
                    r.swiped = true;
                } else if (upper && !r.swiped) {
                    if (!r.edge_since) r.edge_since = now;
                    r.edge_last = now;
                    r.edge_x = held_x;
                    r.edge_y = held_y;
                    r.centre_since = 0;
                    if (r.edge_fall && settled && now - r.edge_since >= 450) fall("stick held up");
                } else {
                    if (r.edge_since) r.edge_brief = r.edge_last - r.edge_since < 260 && settled;
                    r.edge_since = 0;
                    const bool flicked = (r.swiped || held < 0.25f) && r.edge_brief && r.edge_last && now - r.edge_last < 300;
                    r.swiped = false;
                    if (flicked) {
                        const float flick = std::atan2(r.edge_y, r.edge_x) * 180.0f / 3.14159265f;
                        r.out_roll = r.out_yaw = 0;
                        const char *turn = "Tre Flip";
                        if (flick >= 100.0f) { r.out_roll = 360; turn = "Kickflip"; }
                        else if (flick < 80.0f) { r.out_roll = -360; turn = "Heelflip"; }
                        else { r.out_roll = 360; r.out_yaw = 360; }
                        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: flicked out of the {} ({}).", freestyle_name(r.freestyle), turn);
                        announce(std::format("{} {}", casper ? "Casper" : "Primo", turn));
                        r.edge_brief = false;
                        if (casper) {
                            r.out_until = now + 1500;
                            leave(9, 2500);
                        } else {
                            // The primo has no pop of its own: over to the casper, whose flip out carries the turn.
                            r.freestyle = Freestyle::casper;
                            r.after_primo = Freestyle::off;
                            r.primo_out_at = now + r.primo_out_ms;
                        }
                    } else if (!modifier) {
                        if (!r.centre_since) r.centre_since = now;
                        if (now - r.centre_since >= 250) {
                            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: let go of the {}.", freestyle_name(r.freestyle));
                            if (casper) { announce("Casper Flip"); leave(9, 2500); }
                            else leave(0, 1500);
                        }
                    } else if (held < 0.2f) {
                        if (!r.centre_since) r.centre_since = now;
                        if (now - r.centre_since >= 250 && settled) {
                            if (casper && r.centre_fall) fall("stick back at the middle");
                            else {
                                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: the {} was put down.", freestyle_name(r.freestyle));
                                if (casper) { announce("Casper Flip"); leave(9, 2500); }
                                else leave(0, 1500);
                            }
                        }
                    } else {
                        r.centre_since = 0;
                        // The stick moved to another pose's place and rests there: over to that pose.
                        const Freestyle current = r.after_primo != Freestyle::off ? r.after_primo : r.freestyle;
                        if (settled && zone != Freestyle::off && zone != current && now - r.zone_since >= 250) {
                            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: {} to {}.", freestyle_name(current), freestyle_name(zone));
                            announce(zone == Freestyle::casper ? "Casper" : zone == Freestyle::primo ? "Primo" : "Primo 2");
                            r.freestyle = zone;
                            r.after_primo = Freestyle::off;
                            r.unhold_until = now + 900;
                        }
                    }
                }
            }
            // The flick's turn, once the casper's flip out is in the air; dropped again on the ground.
            if (r.out_until && in_air) {
                if (r.out_roll != 0 || r.out_yaw != 0) {
                    start_board_spin(r.out_roll, r.out_yaw, 0, r.air_spin_ms);
                    r.out_spinning = true;
                }
                r.out_until = 0;
            } else if (r.out_until && now >= r.out_until) r.out_until = 0;
            if (r.out_spinning && on_ground) {
                stop_board_spin();
                r.out_spinning = false;
            }
            if (r.fall_until) {
                if (!r.s_late_flip) r.s_late_flip = find_state(base, "Animation/Dingo/Bool.Intent.LateFlip");
                const bool done = now >= r.fall_until;
                (void)write_state_at(base, r.s_late_flip, 0, done ? 0 : 1);
                if (done) r.fall_until = 0;
            }
        } else if (click && (on_ground || r.freestyle != Freestyle::off)) {
            DingoSDKOverlayTricksMask();
            const auto flick = read_flick(r, rx / 32767.0f, ry / 32767.0f);
            Freestyle pick = r.freestyle;
            if (flick == Flick::down) pick = Freestyle::casper;
            else if (flick == Flick::left) pick = Freestyle::primo;
            else if (flick == Flick::right) pick = Freestyle::primo2;
            if (pick != r.freestyle && state_objects_known()) {
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: controls picked {}.", freestyle_name(pick));
                announce(pick == Freestyle::casper ? "Casper" : pick == Freestyle::primo ? "Primo" : "Primo 2");
                r.freestyle = pick;
                r.by_pad = true;
            } else if (r.freestyle == Freestyle::casper && flick != Flick::none && flick != Flick::down) {
                // Flip out of the casper: FreeStyleFlipTrick 9 plays exit_flip1, 16 exit_flip2 (cdb keys {0, 9} and {16}).
                r.exit_flip = flick == Flick::up ? 9u : 16u;
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: controls flipped out of the casper ({}).", r.exit_flip);
                announce("Casper Flip");
                r.freestyle = Freestyle::off;
                r.leaving_until = now + 2500;
                r.by_pad = false;
            }
        } else {
            r.stick_armed = true;
            if (r.by_pad && r.freestyle != Freestyle::off) {
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: controls stepped out of the {}.", freestyle_name(r.freestyle));
                r.exit_flip = 0;
                r.freestyle = Freestyle::off;
                r.leaving_until = now + 1500;
            }
            r.by_pad = false;
        }
        // Keep the flick that ends a trick from reaching the game while the stick comes back.
        if (r.freestyle != Freestyle::off || (click && !r.stick_armed)) DingoSDKOverlayTricksMask();
    }
    // At speed the casper's and primo2's own way in goes wrong (the casper lies back for a second, primo2 slams:
    // measured at 16 km/h), the primo's does not, and from a primo both are a clean change. So a rolling skater
    // goes in through the primo.
    const bool entering = r.freestyle != Freestyle::off && r.shown == Freestyle::off;
    // (Not for a pose caught in the air: there is no ground under the board to drag it over.)
    if (r.route && !(r.by_hold && r.air_pose) && entering && r.freestyle != Freestyle::primo && trainer::telemetry().speed > 1.5f) {
        r.after_primo = r.freestyle;
        r.after_primo_at = now + 700;
        r.freestyle = Freestyle::primo;
    }
    // The primo's 0.7 s count from when the pose has the board (103): a pose caught in the air waits out the landing first.
    if (const auto physics = watched_physics_state(); r.after_primo != Freestyle::off && !(physics.valid && physics.state == 103)) r.after_primo_at = now + 700;
    if (r.freestyle == Freestyle::off) r.after_primo = Freestyle::off;
    else if (r.after_primo != Freestyle::off && now >= r.after_primo_at) {
        if (r.freestyle == Freestyle::primo) r.freestyle = r.after_primo;
        r.after_primo = Freestyle::off;
    }
    r.shown = r.freestyle;
    if (r.freestyle != Freestyle::off && r.entity) set_freestyle_slide(client, r.entity, r.slide);
    set_freestyle_slide_upright(r.upright);
    // The casper's loop is a blend of three clips (low, medium, high) on Float.Intent.Balance, 0 to 1: the left
    // stick's up and down leans the casper while it is held.
    if (r.freestyle == Freestyle::casper && r.casper_balance && skater_key_known()) {
        std::array<std::uint8_t, 12> pad{};
        DingoSDKOverlayTricksPad(pad.data());
        std::int16_t ly{};
        std::memcpy(&ly, pad.data() + 6, 2);
        if (!r.s_balance) r.s_balance = find_state(base, "Animation/Dingo/Float.Intent.Balance");
        (void)write_state_at(base, r.s_balance, 2, r.left_mask ? 0.5 : 0.5 + 0.5 * (ly / 32767.0));
    }
    // Ground shuvit: 7 held while the board turns, then let go; the turn is only dropped once the board has stopped
    // following a frame (out of 103), or it would be turned back.
    if (r.gs_phase && skater_key_known()) {
        const auto physics = watched_physics_state();
        DingoSDKOverlayTricksMask();
        if (r.gs_phase == 1) {
            r.writes += write_state(base, State::ground_trick, 7);
            if (r.entity) set_freestyle_slide(client, r.entity, 0.5f); // keep the speed: 103's own friction stops the board
            if (now >= r.gs_until) {
                r.writes += write_state(base, State::ground_trick, 0);
                r.gs_phase = 2;
                r.gs_until = now + 600;
            }
        } else if ((physics.valid && physics.state != 103) || now >= r.gs_until) {
            stop_board_spin();
            r.gs_phase = 0;
        }
    }
    // No comply flips.
    if (r.nc_flips && skater_key_known()) {
        const auto named = [&](std::uintptr_t &at, const char *name) {
            if (!at) at = find_state(base, name);
            return at;
        };
        const auto flag = [&](std::uintptr_t at) {
            double value{};
            return at && read_state_at(base, at, 0, value) && value != 0;
        };
        const auto physics = watched_physics_state();
        const bool ground = physics.valid && physics.state >= 100 && physics.state <= 105;
        const bool air = physics.valid && physics.state >= 200 && physics.state < 300;
        if (r.nc_experimental && ground) (void)write_state_at(base, named(r.s_nc_exp, "Animation/Dingo/Bool.Anim.EnableExperimentalNoComply"), 0, 1);
        // A boneless or a footplant counts the same way: the plant starts, a flick picks the flip, the board turns in its air.
        const bool planting = flag(named(r.s_boneless, "Animation/Dingo/Bool.Intent.Boneless")) || flag(named(r.s_is_boneless, "Animation/Dingo/Bool.State.Is.Boneless")) ||
                              flag(named(r.s_is_footplant, "Animation/Dingo/Bool.State.Is.Footplant"));
        if (planting && !r.plant_seen) logging::log(logging::Level::info, logging::Channel::skater, "Tricks: plant started (physics {}).", physics.state);
        r.plant_seen = planting;
        if (planting) r.plant_flip = true;
        else if (!r.nc_trick && now - r.nc_seen_at > 1000) r.plant_flip = false;
        const bool nocomply = planting || flag(named(r.s_nc_intent, "Animation/Dingo/Bool.Intent.NoComply")) || flag(named(r.s_nc_anim, "Animation/Dingo/Bool.Anim.Is.NoComply"));
        std::array<std::uint8_t, 12> pad{};
        DingoSDKOverlayTricksPad(pad.data());
        std::int16_t rx{}, ry{};
        std::memcpy(&rx, pad.data() + 8, 2);
        std::memcpy(&ry, pad.data() + 10, 2);
        const float x = rx / 32767.0f, y = ry / 32767.0f, length = std::sqrt(x * x + y * y);
        if (nocomply) {
            if (now - r.nc_seen_at > 1000) { // a new no comply: its own stick (down) is not a pick
                r.nc_armed = false;
                if (r.nc_next) {
                    r.nc_trick = r.nc_next;
                    r.nc_next = 0;
                    r.nc_until = now + 2000;
                    r.nc_air = false;
                }
            }
            r.nc_seen_at = now;
        }
        const bool window = now - r.nc_seen_at < 700;
        const bool pose_held = (pad[0] & 0x40) != 0 || pad[2] > 128; // the left stick's click, the left trigger
        if (window && !r.nc_trick && r.controls && !(r.hold_scheme && pose_held)) {
            if (length < 0.35f) r.nc_armed = true;
            else if (r.nc_armed && length > 0.75f) {
                const float angle = std::atan2(y, x) * 180.0f / 3.14159265f; // 0 right, 90 up
                // CurrentFlipTrick values (the game's): kickflip 2, heelflip 3, pop shuvit 4, FS pop shuvit 7, 360 flip 11, hardflip 9.
                std::uint32_t trick = 9; // down
                if (angle >= 112.5f && angle < 157.5f) trick = 2;       // up-left
                else if (angle >= 22.5f && angle < 67.5f) trick = 3;    // up-right
                else if (angle >= 67.5f && angle < 112.5f) trick = 11;  // up
                else if (angle >= 157.5f || angle < -135.0f) trick = 4; // left
                else if (angle >= -45.0f && angle < 22.5f) trick = 7;   // right
                r.nc_trick = trick;
                r.nc_until = now + 2000;
                r.nc_air = false;
                r.nc_armed = false;
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: no comply flip {} picked.", trick);
            }
            if (r.nc_armed || r.nc_trick) DingoSDKOverlayTricksMask();
        }
        if (r.nc_trick) {
            // The no comply's own clip plays to its end whatever the scripts ask for (its node only leaves at the
            // clip's end), so the flip is the mod's: the board is turned under the clip once it is in the air.
            DingoSDKOverlayTricksMask();
            if (air && !r.nc_air) {
                r.nc_air = true;
                // Degrees about the board's long axis, its up axis and its sideways axis; the no comply's air is 0.6 s.
                float roll = 0, yaw = 0;
                switch (r.nc_trick) {
                case 2: roll = 360; break;               // kickflip
                case 3: roll = -360; break;              // heelflip
                case 4: yaw = 180; break;                // pop shuvit
                case 7: yaw = -180; break;               // FS pop shuvit
                case 11: roll = 360; yaw = 360; break;   // 360 flip
                case 9: roll = 360; yaw = -180; break;   // hardflip (as a turn: a flip with a front side half turn)
                default: roll = 360; break;
                }
                start_board_spin(roll, yaw, 0, r.nc_spin_ms);
                announce(std::format("{} {}", r.plant_flip ? "Boneless" : "No Comply", flip_name(r.nc_trick)));
            }
            if (now >= r.nc_until || (r.nc_air && ground)) {
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: no comply flip {} ended ({}).", r.nc_trick, r.nc_air ? "landed" : "timed out");
                stop_board_spin();
                r.nc_trick = 0;
            }
        }
    }
    // In-air extras: a tap of the left stick's click in the air turns the board once more under whatever trick is
    // playing. Right stick at rest: one more flip the way the trick flips (a double flip; on a varial kickflip a
    // nightmare flip, on a varial heelflip a daydream flip). Right stick left / right: a late shuvit. Right stick
    // up / down: the board goes end over end and comes down turned around (a dolphin flip's turn).
    if (r.air_extras && r.controls && skater_key_known()) {
        const auto physics = watched_physics_state();
        const bool air = physics.valid && physics.state >= 200 && physics.state < 300;
        std::array<std::uint8_t, 12> pad{};
        DingoSDKOverlayTricksPad(pad.data());
        std::uint16_t buttons{};
        std::int16_t rx{}, ry{};
        std::memcpy(&buttons, pad.data(), 2);
        std::memcpy(&rx, pad.data() + 8, 2);
        std::memcpy(&ry, pad.data() + 10, 2);
        const bool click = (buttons & 0x40) != 0;
        // The owner's scheme lands into a casper with the click held, so there the extra is a tap: it fires on the release.
        const bool released = !click && r.air_click;
        if (click && !r.air_click) r.air_click_at = now;
        const bool pressed = r.hold_scheme ? released && now - r.air_click_at < 200 : click && !r.air_click;
        r.air_click = click;
        if (!air) {
            // Landed (or bailed): a half turn is still being asked of the board, and the next time it follows a clip
            // (a mount, a crouch) it would be turned around.
            if (r.air_used) stop_board_spin();
            r.air_used = 0;
        }
        if (air && pressed && r.by_hold && r.freestyle != Freestyle::off) {
            r.freestyle = Freestyle::off;
            r.by_hold = false;
            r.exit_flip = 0;
            r.leaving_until = now;
        }
        if (air && pressed && r.air_used < 2 && r.freestyle == Freestyle::off && !r.nc_trick) {
            if (!r.s_current_flip) r.s_current_flip = find_state(base, "Animation/Dingo/EnumGS.Anim.CurrentFlipTrick");
            double current{};
            (void)read_state_at(base, r.s_current_flip, 1, current);
            const auto trick = static_cast<int>(current);
            // The heelflip's side of the game's flip tricks (the rest flip the kickflip's way).
            static constexpr int heel_side[]{3, 6, 8, 12, 14, 18, 21, 24, 27, 30, 44, 48};
            float way = r.flip_sign;
            for (const int t : heel_side) if (t == trick) way = -way;
            const float x = rx / 32767.0f, y = ry / 32767.0f;
            float roll = 0, yaw = 0, pitch = 0;
            const char *what = "extra flip";
            if (y > 0.45f && std::abs(x) > 0.45f) { pitch = 180.0f; roll = 180.0f; yaw = x > 0 ? -360.0f : 360.0f; what = "dragon flip"; }
            else if (std::abs(x) > 0.6f && std::abs(x) >= std::abs(y)) { yaw = x > 0 ? -180.0f : 180.0f; what = "late shuvit"; }
            else if (std::abs(y) > 0.6f) { pitch = y > 0 ? 180.0f : -180.0f; roll = 180.0f; what = "dolphin turn"; }
            else roll = 360.0f * way;
            start_board_spin(roll, yaw, pitch, r.air_spin_ms);
            ++r.air_used;
            announce(yaw != 0 && pitch != 0 ? std::string_view("Dragon Flip") : yaw != 0 ? std::string_view("Late Shuvit") : pitch != 0 ? std::string_view("Dolphin Flip") : extra_flip_name(trick));
            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: in-air {} on flip trick {} (roll {}, yaw {}, pitch {}).", what, trick, roll, yaw, pitch);
        }
    }
    // <dev>
    for (auto &extra : r.extras) {
        if (extra.freestyle_only && r.freestyle == Freestyle::off) continue;
        if (!extra.at) extra.at = find_state(base, extra.name.c_str());
        if (extra.at && write_state_at(base, extra.at, extra.kind, extra.value)) ++extra.writes;
    }
    // </dev>
    if (r.mode_hold) {
        if (!any_hold(r, now)) {
            release_all();
            return;
        }
        if (!r.hooks) r.hooks = start_holds(base);
        if (r.freestyle != Freestyle::off) {
            hold(State::wants_freestyle, true, 1);
            hold(State::freestyle_trick, true, static_cast<std::uint32_t>(r.freestyle));
            hold(State::freestyle_flip_trick, true, 0);
        } else {
            // Leaving: WantsFreestyle off, the exit flip (or none) held until the node has left.
            hold(State::wants_freestyle, true, 0);
            hold(State::freestyle_flip_trick, true, r.exit_flip);
        }
        keep_holds_alive();
        return;
    }
    // Written through the game's setters every tick while freestyle is on (nothing else writes these states).
    if (r.freestyle != Freestyle::off) {
        r.writes += write_state(base, State::freestyle_trick, static_cast<std::uint32_t>(r.freestyle));
        r.writes += write_state(base, State::freestyle_flip_trick, 0);
        r.writes += write_state(base, State::wants_freestyle, 1);
        // The freestyle nodes tag CurrentGroundTrick 7, but the intent tree knows nothing of freestyle and puts its own
        // back: without 7 the physics leaves GROUND_ANIMATION for the air as soon as the clip lifts the board.
        r.writes += write_state(base, State::ground_trick, 7);
        r.leaving = true;
    } else if (r.leaving) {
        // Leaving: WantsFreestyle off, the exit flip (or none) kept until the node has left, then cleared.
        const bool done = now >= r.leaving_until;
        r.writes += write_state(base, State::freestyle_flip_trick, done ? 0 : r.exit_flip);
        r.writes += write_state(base, State::wants_freestyle, 0);
        r.writes += write_state(base, State::ground_trick, done ? 0 : 7);
        if (done) r.leaving = false;
    }
}

std::string command(std::string_view verb, const std::vector<std::string> &arguments) {
    auto &r = runtime();
    const auto now = GetTickCount64();
    if (verb == "status") {
        std::string live;
        for (std::size_t i = 0; i < state_count; ++i) {
            std::uint32_t value{};
            std::uint32_t handle{};
            const int reason = state_reason(r.base, r.entity, static_cast<State>(i), handle);
            live += read_state(r.base, r.entity, static_cast<State>(i), value) ? std::format(" {}", value) : std::format(" -(r{} h{:08x})", reason, handle);
            live += std::format("/{}", held_asked(static_cast<State>(i)));
        }
        std::string game;
        for (std::size_t i = 0; i < state_count; ++i) {
            std::int32_t value{};
            game += game_read_state(r.base, static_cast<State>(i), value) ? std::format(" {}", value) : std::string(" -");
        }
        std::array<std::uint8_t, 12> pad{};
        DingoSDKOverlayTricksPad(pad.data());
        std::uint16_t buttons{};
        std::int16_t rx{}, ry{};
        std::memcpy(&buttons, pad.data(), 2);
        std::memcpy(&rx, pad.data() + 8, 2);
        std::memcpy(&ry, pad.data() + 10, 2);
        const auto physics = watched_physics_state();
        const auto jump = trainer::telemetry().last;
        game += std::format(". Last jump {}: air {:.2f} s, height {:.2f} m, board turn {:.0f} deg/s, spin {:.0f} deg", jump.serial, jump.air_time, jump.height,
                            jump.board_turn, jump.spin);
        game += std::format(". Pad buttons {:04x} rs ({}, {}), physics {} {}, controls {}, resolved {} ({})", buttons, rx, ry,
                            physics.valid ? "valid" : "invalid", physics.state, r.controls ? "on" : "off", state_objects_resolved() ? "yes" : "no",
                            r.resolved_count);
        return std::format("Tricks: states {}, mode {}, setters {}, key {}, holds {}, skater {}, freestyle {}, exit flip {}, writes {}. "
                           "Store value/asked (wants, trick, flip, caspers, groundtrick):{}. Game reads:{}",
                           state_objects_known() ? "found" : (searching.load() ? "searching" : "not found"), r.mode_hold ? "hold" : "set",
                           r.setters ? "watched" : "off", skater_key_known() ? std::format("{:x}", skater_key()) : std::string("none"),
                           r.hooks ? "hooked" : "off", r.entity ? "yes" : "no", freestyle_name(r.freestyle), r.exit_flip, r.writes, live, game);
    }
    if (verb == "freestyle") {
        if (arguments.empty()) return "usage: tricks freestyle casper|primo|primo2|off";
        const auto &what = arguments[0];
        Freestyle f;
        if (what == "casper") f = Freestyle::casper;
        else if (what == "primo") f = Freestyle::primo;
        else if (what == "primo2") f = Freestyle::primo2;
        else if (what == "off") f = Freestyle::off;
        else return "error: casper, primo, primo2 or off.";
        if (!state_objects_known()) return "error: the freestyle states are not found yet (load into a map and try again).";
        if (f == Freestyle::off && r.freestyle != Freestyle::off) {
            r.exit_flip = 0;
            r.leaving_until = now + 1500;
        }
        r.freestyle = f;
        return std::format("Freestyle: {}.", freestyle_name(f));
    }
    if (verb == "casperflip") {
        // Leave a casper with a flip: FreeStyleFlipTrick picks the clip (the database's keys read
        // {0, 9} and {16}; which gesture values those are is still to be confirmed in game).
        std::uint32_t value = 9;
        if (!arguments.empty() && !number(arguments[0], value)) return "usage: tricks casperflip [FreeStyleFlipTrick value]";
        if (r.freestyle != Freestyle::casper) return "error: not in a casper (tricks freestyle casper first).";
        r.exit_flip = value;
        r.freestyle = Freestyle::off;
        r.leaving_until = now + 2500;
        return std::format("Casper flip out with FreeStyleFlipTrick {}.", value);
    }
    if (verb == "pad") {
        // <dev> tricks pad <steps> | mount | off: a scripted controller for test runs.
        auto &p = r.pad;
        if (arguments.empty() || arguments[0] == "off") {
            p = {};
            DingoSDKOverlayScriptController(nullptr);
            return "The controller is the player's again.";
        }
        const std::vector<std::string> mount{"b:150", "n:1200", "y:200", "n:2500"};
        const auto &words = arguments[0] == "mount" ? mount : arguments;
        std::vector<PadStep> steps;
        for (const auto &word : words) {
            const auto step = pad_step(word);
            if (!step) return "error: \"" + word + "\" is not a step. Example: tricks pad b:150 n:1200 y:200 (buttons a b x y lb rb ls rs up down "
                                                   "left right start back, lt rt, sticks lx ly rx ry from -1 to 1)";
            steps.push_back(*step);
        }
        p.steps = std::move(steps);
        p.index = 0;
        p.step_start = now;
        p.running = true;
        std::uint64_t polls{}, served{};
        DingoSDKOverlayScriptControllerPolls(&polls, &served);
        return std::format("Pad script of {} steps started (the game has polled the controller {} times, {} scripted).", p.steps.size(), polls, served);
        // </dev>
    }
    if (verb == "controls") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks controls on|off";
        r.controls = arguments[0] == "on";
        return r.controls ? "Controls on: hold the left stick's click on the board; right stick down casper, left primo, right primo2, up (in a casper) flip out."
                          : "Controls off.";
    }
    if (verb == "state" || verb == "fstate") {
        // <dev> tricks state <name> b|i|f <value> | read <name> b|i | clear | list
        const auto full = [](const std::string &name) { return name.find('/') == std::string::npos ? "Animation/Dingo/" + name : name; };
        if (arguments.empty() || arguments[0] == "list") {
            std::string text = "Held states:";
            for (const auto &extra : r.extras)
                text += std::format(" {}={} ({}{}, {} writes)", extra.name, extra.value, extra.at ? "found" : "not found", extra.freestyle_only ? ", freestyle only" : "", extra.writes);
            return r.extras.empty() ? "No states held." : text;
        }
        if (arguments[0] == "clear") {
            r.extras.clear();
            return "Held states cleared (their last values stay until the game writes them).";
        }
        if (arguments[0] == "read" && arguments.size() >= 3) {
            const auto name = full(arguments[1]);
            const auto at = find_state(r.base, name.c_str());
            double value{};
            if (!at) return "error: " + name + " is not found (is the skater on the board?).";
            if (!read_state_at(r.base, at, arguments[2] == "i", value)) return "error: it could not be read.";
            return std::format("{} = {}", name, value);
        }
        if (arguments.size() < 3) return "usage: tricks state <name> b|i|f <value> | read <name> b|i | clear | list";
        Runtime::Extra extra;
        extra.name = full(arguments[0]);
        extra.kind = arguments[1] == "b" ? 0 : arguments[1] == "i" ? 1 : 2;
        extra.freestyle_only = verb == "fstate";
        const auto &text = arguments[2];
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), extra.value);
        if (error != std::errc{}) return "error: the value is not a number.";
        std::erase_if(r.extras, [&](const Runtime::Extra &e) { return e.name == extra.name; });
        r.extras.push_back(extra);
        return std::format("Holding {} at {}{}.", extra.name, extra.value, extra.freestyle_only ? " while freestyle is on" : "");
        // </dev>
    }
    if (verb == "names") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks names on|off";
        announce_tricks = arguments[0] == "on";
        return announce_tricks ? "Trick names are shown." : "Trick names are not shown.";
    }
    if (verb == "air") {
        // tricks air on|off | sign 1|-1 | ms <length>
        if (arguments.empty()) return std::format("In-air extras {}, sign {}, {} ms.", r.air_extras ? "on" : "off", r.flip_sign, r.air_spin_ms);
        if (arguments[0] == "on" || arguments[0] == "off") {
            r.air_extras = arguments[0] == "on";
            return r.air_extras ? "In-air extras on: tap the left stick's click in the air (right stick at rest: one more flip; left / right: late shuvit; up / down: dolphin turn)."
                                : "In-air extras off.";
        }
        if (arguments.size() == 2 && arguments[0] == "sign") {
            r.flip_sign = arguments[1] == "-1" ? -1.0f : 1.0f;
            return std::format("Extra flips roll with sign {}.", r.flip_sign);
        }
        std::uint32_t ms{};
        if (arguments.size() == 2 && arguments[0] == "ms" && number(arguments[1], ms) && ms >= 50 && ms <= 3000) {
            r.air_spin_ms = ms;
            return std::format("In-air extras take {} ms.", ms);
        }
        return "usage: tricks air on|off | sign 1|-1 | ms <50-3000>";
    }
    if (verb == "spin") {
        // <dev> tricks spin <roll> <yaw> <pitch> <ms> | off | ms <no comply flip length>
        if (!arguments.empty() && arguments[0] == "off") { stop_board_spin(); return "Spin stopped."; }
        std::uint32_t seen{}, turned{};
        board_hook_counts(seen, turned);
        if (arguments.size() == 2 && arguments[0] == "ms") {
            std::uint32_t ms{};
            if (!number(arguments[1], ms) || ms < 50 || ms > 5000) return "usage: tricks spin ms <50-5000>";
            r.nc_spin_ms = ms;
            return std::format("No comply flips turn the board over {} ms.", ms);
        }
        if (arguments.size() < 4) return std::format("Board hook {}: saw the skater's frame {} times, turned it {} (usage: tricks spin <roll> <yaw> <pitch> <ms>).", r.board ? "on" : "off", seen, turned);
        float angles[3]{};
        for (int i = 0; i < 3; ++i) {
            const auto &text = arguments[static_cast<std::size_t>(i)];
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), angles[i]);
            if (error != std::errc{}) return "error: the angles are numbers (degrees).";
        }
        std::uint32_t ms{};
        if (!number(arguments[3], ms) || ms < 50 || ms > 10000) return "error: the time is 50 to 10000 ms.";
        start_board_spin(angles[0], angles[1], angles[2], ms);
        return std::format("Board spin: roll {}, yaw {}, pitch {} over {} ms.", angles[0], angles[1], angles[2], ms);
        // </dev>
    }
    if (verb == "ncflip") {
        // tricks ncflip <CurrentFlipTrick value> | on | off | stock | experimental
        if (arguments.empty()) return std::format("No comply flips {}; clips: {}.", r.nc_flips ? "on" : "off", r.nc_experimental ? "experimental" : "stock");
        if (arguments[0] == "on" || arguments[0] == "off") {
            r.nc_flips = arguments[0] == "on";
            return r.nc_flips ? "No comply flips on: after a no comply starts, flick the right stick (up-left kickflip, up-right heelflip, left pop shuvit, "
                                "right FS pop shuvit, up 360 flip, down hardflip)."
                              : "No comply flips off.";
        }
        if (arguments[0] == "stock" || arguments[0] == "experimental") {
            r.nc_experimental = arguments[0] == "experimental";
            if (!r.nc_experimental) (void)write_state_at(r.base, r.s_nc_exp, 0, 0);
            return std::format("No comply clips: {}.", arguments[0]);
        }
        std::uint32_t trick{};
        if (!number(arguments[0], trick) || trick > 58) return "usage: tricks ncflip <flip trick value 1-58> | on | off | stock | experimental";
        r.nc_next = trick;
        return std::format("The next no comply flips with trick {}.", trick);
    }
    if (verb == "catch") {
        if (arguments.size() == 2 && (arguments[0] == "casper" || arguments[0] == "primo" || arguments[0] == "primo2" || arguments[0] == "casperdrop" || arguments[0] == "out")) {
            float value{};
            const auto &text = arguments[1];
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{}) return "error: a number.";
            if (arguments[0] == "casper") r.casper_roll = value;
            else if (arguments[0] == "primo") r.primo_roll = value;
            else if (arguments[0] == "primo2") r.primo2_roll = value;
            else if (arguments[0] == "casperdrop") r.casper_drop = value;
            else r.primo_out_ms = static_cast<std::uint32_t>(value);
            return std::format("Catch: casper roll {}, drop {}; primo roll {}, primo2 roll {}, drop {}; primo flick out after {} ms.", r.casper_roll, r.casper_drop, r.primo_roll, r.primo2_roll, r.catch_drop, r.primo_out_ms);
        }
        if (arguments.size() == 2 && arguments[0] == "drop") {
            float value{};
            const auto &text = arguments[1];
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || value < -0.5f || value > 0.5f) return "usage: tricks catch drop <metres, -0.5 to 0.5>";
            r.catch_drop = value;
            return std::format("A caught board sits {:.2f} m lower.", value);
        }
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks catch on|off | drop <metres>";
        r.catch_turn = arguments[0] == "on";
        return r.catch_turn ? "The board is turned to the pose in the air at the catch." : "The board is left alone in the air; the pose starts on the ground.";
    }
    if (verb == "upright") {
        float value{};
        if (arguments.empty()) return std::format("Slides hold the skater upright at {:.2f} (0 not at all, 1 rigid).", r.upright);
        const auto &text = arguments[0];
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || value < 0 || value > 1) return "usage: tricks upright <0 to 1>";
        r.upright = value;
        return std::format("Slides hold the skater upright at {:.2f}.", value);
    }
    if (verb == "airpose") {
        if (!arguments.empty() && arguments[0] == "states") {
            r.air_names.clear();
            r.s_air.clear();
            for (std::size_t i = 1; i < arguments.size(); ++i) r.air_names.push_back(arguments[i].find('/') == std::string::npos ? "Animation/Dingo/" + arguments[i] : arguments[i]);
            return std::format("{} states held true while a pose is caught in the air.", r.air_names.size());
        }
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks airpose on|off | states <bool state>...";
        r.air_pose = arguments[0] == "on";
        return r.air_pose ? "A caught pose starts in the air (the animation is told the board is down)." : "A caught pose starts on the ground.";
    }
    if (verb == "leftmask") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks leftmask on|off";
        r.left_mask = arguments[0] == "on";
        return r.left_mask ? "In a pose the left stick does not steer or lean the skater." : "In a pose the left stick steers, and leans a casper.";
    }
    if (verb == "stable") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks stable on|off";
        set_freestyle_slide_stable(arguments[0] == "on");
        return arguments[0] == "on" ? "A held pose does not tip." : "A held pose is left to the game's physics.";
    }
    if (verb == "primos") {
        if (arguments.empty() || (arguments[0] != "one" && arguments[0] != "two")) return "usage: tricks primos one|two";
        r.two_primos = arguments[0] == "two";
        return r.two_primos ? "The stick's left is the primo, its right the primo2." : "Either side is the primo.";
    }
    if (verb == "protect") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks protect on|off";
        r.protect = arguments[0] == "on";
        return r.protect ? "No wipeout while a pose is caught, landed or held." : "Poses can wipe out.";
    }
    if (verb == "early") {
        // tricks early on|off|<bool state name>: the intent held to leave the landing early
        if (arguments.empty()) return std::format("Early landing exit {} ({}).", r.early_exit ? "on" : "off", r.early_name);
        if (arguments[0] == "on" || arguments[0] == "off") r.early_exit = arguments[0] == "on";
        else {
            r.early_name = arguments[0].find('/') == std::string::npos ? "Animation/Dingo/" + arguments[0] : arguments[0];
            r.s_early = 0;
            r.early_exit = true;
        }
        return std::format("Early landing exit {} ({}).", r.early_exit ? "on" : "off", r.early_name);
    }
    if (verb == "route") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks route on|off";
        r.route = arguments[0] == "on";
        return r.route ? "Rolling, a casper or primo2 goes in through the primo." : "Poses go in directly.";
    }
    if (verb == "zones") {
        float left{}, right{};
        if (arguments.size() != 2) return std::format("Casper between {:.0f} and {:.0f} degrees of the right stick (270 is down); primos to either side. usage: tricks zones <left> <right>", r.zone_left, r.zone_right);
        const auto parse = [](const std::string &text, float &value) {
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{};
        };
        if (!parse(arguments[0], left) || !parse(arguments[1], right) || left < 180 || left > 269 || right < 271 || right > 359) return "usage: tricks zones <left 180-269> <right 271-359>";
        r.zone_left = left;
        r.zone_right = right;
        return std::format("Casper between {:.0f} and {:.0f} degrees.", left, right);
    }
    if (verb == "trace") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks trace on|off";
        r.trace = arguments[0] == "on";
        return r.trace ? "Tracing the controller and the skater to logs/tricks-trace.csv." : "Trace off.";
    }
    if (verb == "fall") {
        if (arguments.empty() || (arguments[0] != "hop" && arguments[0] != "slam")) return "usage: tricks fall hop|slam";
        r.fall_slam = arguments[0] == "slam";
        return r.fall_slam ? "A lost pose is a slam." : "A lost pose is a hop off the board.";
    }
    if (verb == "edgefall") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks edgefall on|off";
        r.edge_fall = arguments[0] == "on";
        r.centre_fall = r.edge_fall;
        return r.edge_fall ? "Falls on: a primo with the stick held at the outside, a casper with the stick back at the middle." : "Falls off: the middle and the click's release are clean ways out.";
    }
    if (verb == "scheme") {
        if (arguments.empty() || (arguments[0] != "hold" && arguments[0] != "flick")) return std::format("Scheme: {} (usage: tricks scheme hold|flick).", r.hold_scheme ? "hold" : "flick");
        r.hold_scheme = arguments[0] == "hold";
        return r.hold_scheme ? "Scheme hold: land a flip with the right stick part of the way left / right (primo), or with the left stick clicked (casper); "
                               "centre to leave, flick to flip out, held at the edge the pose is lost."
                             : "Scheme flick: hold the left stick's click on the board and flick the right stick.";
    }
    if (verb == "groundshuv") {
        // tricks groundshuv [fs|bs|on|off|ms <n>]
        if (!arguments.empty() && (arguments[0] == "on" || arguments[0] == "off")) {
            r.ground_shuvs = arguments[0] == "on";
            return r.ground_shuvs ? "Ground shuvits on: riding, hold the right stick left or right and click the left stick." : "Ground shuvits off.";
        }
        std::uint32_t ms{};
        if (arguments.size() == 2 && arguments[0] == "ms" && number(arguments[1], ms) && ms >= 100 && ms <= 2000) {
            r.gs_ms = ms;
            return std::format("Ground shuvits take {} ms.", ms);
        }
        if (!state_objects_known() || r.gs_phase) return "error: not ready (on the board, and not already turning).";
        start_ground_shuv(r, now, !arguments.empty() && arguments[0] == "fs");
        return "Ground shuvit.";
    }
    if (verb == "level") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks level on|off";
        set_freestyle_slide_level(arguments[0] == "on");
        return arguments[0] == "on" ? "Slides keep the board from pitching up." : "Slides leave the board's pitch to the game.";
    }
    if (verb == "slide") {
        if (arguments.empty()) return std::format("Casper and primo slides scrub {:.1f} m/s off each second.", r.slide);
        float value{};
        const auto &text = arguments[0];
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value < 0 || value > 50) return "usage: tricks slide <m/s each second, 0 to 50>";
        r.slide = value;
        return std::format("Casper and primo slides scrub {:.1f} m/s off each second{}.", value, value == 0 ? " (off: the game's own)" : "");
    }
    if (verb == "mode") {
        if (arguments.empty() || (arguments[0] != "set" && arguments[0] != "hold")) return "usage: tricks mode set|hold";
        r.mode_hold = arguments[0] == "hold";
        if (!r.mode_hold) release_all();
        return std::format("States are {} now.", r.mode_hold ? "answered at the lookup (hold)" : "written through the game's setters (set)");
    }
    if (verb == "search") {
        forget_state_objects();
        search_in_background();
        return "Searching memory for the freestyle states.";
    }
    return "error: unknown tricks command.";
}
} // namespace dingosdk::tricks
