#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dingosdk::trainer {
// The animation system's game states the trainer reads and writes for the local skater.
//
// A flip trick's clip plays at Float.Anim.TrickFlipSpeed, a number the game sets once when the
// board is flicked. The trainer multiplies that number while the trick is in the air: nothing
// that sets the pop reads it, and it works for every trick (EnumGS.Anim.CurrentFlipTrick says
// which one is turning).
struct FlipState {
    std::uintptr_t speed_at{}; // where the live flip speed is; 0: not found
    std::uint32_t trick{};     // CurrentFlipTrick
    std::uintptr_t trick_at{}; // where that is
    float speed{};
    // The impossible's clips (and only they) play at Float.Anim.TrickFlipSpeedClamped instead: 0 when not found.
    std::uintptr_t clamped_at{};
    float clamped{};
    explicit operator bool() const noexcept { return speed_at != 0; }
};
FlipState read_flip_state(std::uintptr_t base, std::uintptr_t entity) noexcept;
// The state objects are loaded with a level: the memory search (trainer_classes.cpp) hands
// every region it reads to this, which keeps the ones it knows. True once both are known.
void find_state_objects(std::uintptr_t start, std::size_t size) noexcept;
bool flip_states_found() noexcept;
// The skater's pages of game states (0 where there is none).
std::array<std::uintptr_t, 6> state_pages(std::uintptr_t base, std::uintptr_t entity) noexcept;
bool write_flip_speed(const FlipState &state, float speed) noexcept;

// Pumping a transition, as the game's trick scripts see it: whether the skater counts as pumping
// this tick and how hard (Bool.Intent.Pumping, Float.Intent.UserTriggeredPumpStrength,
// Float.Anim.PumpingAssistAmount).
struct PumpState {
    bool found{};   // the states are known and the skater has them
    bool pumping{};
    float strength{}, assist{};
};
PumpState read_pump_state(std::uintptr_t base, std::uintptr_t entity) noexcept;

// Off the board, a flip in the air (jump, then RB and the left stick): Float.Intent.BodyFlipSpeedMod and
// Float.Intent.BodyFlipAccelerationRate. An address is 0 while that state is not known or the skater has none.
struct BodyFlipState {
    std::uintptr_t speed_at{}, rate_at{};
    float speed{}, rate{};
};
BodyFlipState read_body_flip_state(std::uintptr_t base, std::uintptr_t entity) noexcept;
bool write_state_number(std::uintptr_t at, float value) noexcept;
// Float.Intent.GrindStabilityNudge: the left stick's sideways push on a grind as the game's scripts hand it to the
// grind code (Skate 3's PhysGrindStabilityNudge is left stick X). False while the state is not known or has no value.
bool read_grind_nudge(std::uintptr_t base, std::uintptr_t entity, float &nudge) noexcept;
// How the game's scripts steer the board on a grind, as they hand it to the grind code: the stick as they see it
// (GrindControlLR / UD), the board's turn and tip they ask for (GrindControlHeading / Pitch, with their multipliers and
// base values), the grind's type and whether the stick counts as active. `found` has bit n set where value n was read,
// in the order of the members.
struct GrindControl {
    float lr{}, ud{}, heading{}, pitch{}, heading_multiplier{}, pitch_multiplier{}, base_heading{}, base_pitch{};
    std::uint32_t grind_type{}, skate_grind_type{}, active_input{};
    std::uint32_t found{};
};
GrindControl read_grind_control(std::uintptr_t base, std::uintptr_t entity) noexcept;
// Enum.Intent.Manual for the skater: 0 none, above 0 a manual; -1 while the state is not known.
std::int32_t read_manual_intent(std::uintptr_t base, std::uintptr_t entity) noexcept;

// The tricks that have a speed of their own under "Advanced trick speed". A nollie turns at
// its regular trick's speed, but for the impossibles, which are different tricks. Underflips, held flips, hospital and
// flower flips have none: their clips play at no speed state.
struct FlipTrick {
    std::string_view key;   // the option and preset name
    std::string_view label; // in the menu
};
inline constexpr std::array<FlipTrick, 20> flip_tricks{{
    {"flip.kickflip", "Kickflip"},
    {"flip.heelflip", "Heelflip"},
    {"flip.popshuvit", "Pop shuvit"},
    {"flip.fspopshuvit", "FS pop shuvit"},
    {"flip.360popshuvit", "360 pop shuvit"},
    {"flip.fs360popshuvit", "FS 360 pop shuvit"},
    {"flip.varialkickflip", "Varial kickflip"},
    {"flip.varialheelflip", "Varial heelflip"},
    {"flip.360flip", "360 flip"},
    {"flip.laserflip", "Laser flip"},
    {"flip.hardflip", "Hardflip"},
    {"flip.inwardheelflip", "Inward heelflip"},
    {"flip.360hardflip", "360 hardflip"},
    {"flip.360inwardheelflip", "360 inward heelflip"},
    {"flip.impossible", "Impossible"},
    {"flip.frontfootimpossible", "Front foot impossible"},
    {"flip.ollie", "Ollie, nollie, boned ollie"},
    {"flip.nollieimpossible", "Nollie impossible"},
    {"flip.nolliebackfootimpossible", "Nollie back foot impossible"},
    {"flip.lateflip", "Late flip"},
}};
// Which of those a CurrentFlipTrick value belongs to; -1: none (an ollie, a wallie, an underflip).
int flip_trick_index(std::uint32_t trick) noexcept;
// The name the game gives a CurrentFlipTrick value ("Kickflip", "N_360Flip"); empty when unknown.
std::string_view flip_trick_name(std::uint32_t trick) noexcept;

// The grind exits and transfers: Bool.Intent states the game's own scripts and settings set, and its grind code reads
// (found from the game's data, 2026-10-11). Each can be left to the game or held off / on. What each one does in play
// is the game's name for it: none is measured yet.
struct GrindSwitch {
    std::string_view key;   // `trainer grind <key>`, saved as grind.<key>
    std::string_view label; // in the menu
    std::string_view help;
    std::string_view state; // the game state's name
    bool skate3_off{};      // "Grinds like Skate 3" holds it off: skate. added it
    // Shown, never held. The exit without a pop is the game's word that the skater IS leaving the grind: held off, the
    // skater came off the rail and stayed in the grind state on the ground (playtest of 2026-10-11, 8 s in state 403).
    bool read_only{};
};
inline constexpr std::array<GrindSwitch, 11> grind_switches{{
    {"nopop", "Leaving a grind without a pop", "The game sets this while the skater comes off a grind without popping (the nudge off). It can only be watched: "
     "holding it off left the skater in the grind state on the ground.", "Bool.Intent.NoPopGrindExit", false, true},
    {"togrind", "Exit a grind onto another grind", "One of the game's experimental settings: a grind exit may go straight into another grind.", "Bool.Intent.EnableGrindExitToGrind", true},
    {"double", "Double grinds out of a grind exit", "One of the game's experimental settings: a grind exit may end in a double grind.", "Bool.Intent.EnableGrindExitDoubleGrinds", true},
    {"stronger", "Stronger grind exits", "One of the game's experimental settings: exits push off the grind harder.", "Bool.Intent.EnableStrongerGrindExits", true},
    {"camera", "Grind exits follow the camera", "The stick direction of an exit is read against the camera, not the board.", "Bool.Intent.EnableCameraRelativeGrindExits", true},
    {"direct", "Direct grind control", "One of the game's experimental settings: the left stick turns and tips the board on the grind directly.", "Bool.Intent.EnableGrindDirectControl"},
    {"linear", "Grind linear drive", "One of the game's experimental settings for how the grind is driven along the edge.", "Bool.Intent.EnableGrindLinearDrive", true},
    {"eat", "Scripted grind control", "A feature flag of the game's grind control scripts.", "Bool.Intent.EnableEATGrindControl"},
    {"toslide", "Turn a grind into a boardslide", "The game's scripts set this when the stick turns the board from a grind into a boardslide.", "Bool.Intent.GrindToBoardslide"},
    {"coping", "No legacy coping grinds", "Turns off the older way of locking onto coping.", "Bool.Intent.DisableLegacyCopingGrinds"},
    {"airtogrind", "Grinds from the air", "The game's scripts set this in the air when a grind may be landed on.", "Bool.Intent.AllowAirToGrind"},
}};
// What "Grinds like Skate 3" holds switch `index` at: its own ways off a grind off, and the stick's control of the board
// ON (the game turns EnableGrindDirectControl off with its legacy grinds and only sets it again at the next mount).
constexpr std::int8_t skate3_grind_mode(std::size_t index) noexcept {
    return grind_switches[index].skate3_off ? std::int8_t{0} : grind_switches[index].key == "direct" ? std::int8_t{1} : std::int8_t{-1};
}
// The game's OWN gameplay settings for grinds (DelMarBoolSettingAsset; its scripts ask for them several times a frame
// through the settings getter ReSkate already hooks, Extension/Settings/local_gameplay_settings.cpp). Forced, the game's
// own Skate 3 style grind code runs: this is what "Grinds feel like Skate 3" is built on.
struct GrindSetting {
    std::string_view key;     // `trainer grind <key>`, saved as grind.set.<key>
    std::string_view label;   // in the menu
    std::string_view help;
    std::string_view setting; // the game's key for it
    bool stock{};             // the game's default
    std::int8_t skate3{-1};   // what "Grinds like Skate 3" forces it to (-1: left alone)
};
inline constexpr std::array<GrindSetting, 6> grind_settings{{
    {"legacy", "Legacy grinds", "The game's own switch for its older, Skate 3 style grind code: the left stick snaps the board from one grind to the next.", "egf_legacygrinds", false, -1},
    {"leanexit", "Lean off a grind", "The game's own setting: leaning with the left stick takes the skater off the grind without a pop.", "grind_leanexit", true, 0},
    {"lateralpop", "Sideways ollie", "The game's own setting: an ollie can be pushed sideways with the stick.", "lateralpop", true, 0},
    {"transitions", "Experimental grind transitions", "The game's own experimental switch for changing grinds on the edge.", "exp_grind_transitions", false, 0},
    {"stances", "Grind stances", "The game's own setting for the stances a grind can be held in.", "grind_stances", true, -1},
    {"prewind", "Wind up a spin on a grind", "The game's own setting: a spin can be wound up while still on the grind.", "egf_prewindfromgrind", false, 0},
}};
// Each tick of the trainer: without it for half a second, every forced setting and held switch below is the game's own
// again, whatever the trainer last asked for.
void keep_holds_alive() noexcept;
// How many settings are forced and switches held right now (0 once the lease has run out).
int holds_in_force() noexcept;
// mode: -1 the game's own, 0 forced off, 1 forced on.
void force_grind_setting(std::size_t index, int mode) noexcept;
// For the settings getter hook: the forced value when `reference` (the script's setting reference) is one of those.
std::optional<bool> forced_gameplay_bool(const void *reference) noexcept;
// How often the game asked for that setting since last asked here.
std::uint32_t grind_setting_asked(std::size_t index) noexcept;

// Transition skating (bowls, quarter pipes, vert, spines): the game's own gameplay settings that decide which of its
// lip trick, vert landing, handplant and pumping code runs. Forced the same way as the grind settings. What each one
// changes in play is the game's name for it: none is measured yet.
struct TransitionSetting {
    std::string_view key;     // `trainer transition <key>`, saved as transition.set.<key>
    std::string_view label;   // in the menu
    std::string_view help;
    std::string_view setting; // the game's key for it
    bool stock{};             // the game's default
    std::int8_t skate3{-1};   // what "Transition feels like Skate 3" forces it to (-1: left alone)
    std::uint8_t card{};      // which card of the section shows it (transition_card_*)
};
inline constexpr std::uint8_t transition_card_coping = 0, transition_card_plant = 1, transition_card_lip = 2, transition_card_vert = 3, transition_card_pump = 4;
inline constexpr std::array<TransitionSetting, 12> transition_settings{{
    {"airtogrind", "Air to grind (trial)", "The game's own experimental switch for locking onto an edge straight out of the air.", "egf_airtogrind", false, -1, transition_card_coping},
    {"airgrindintents", "Aim for grinds in the air", "The game's own setting: in the air the skater aims for an edge it could grind. Off, a flight over a coping should stop reaching for it.", "egf_airtogrindintents", true, -1, transition_card_coping},
    {"spinetransfer", "Handplant spine transfer", "The game's own experimental switch: a handplant on a spine may come down the other side.", "egf_handplantspinetransfer", false, -1, transition_card_plant},
    {"legacylip", "Legacy lip tricks", "The game's own switch for its older lip trick code (stalls and lip tricks on coping).", "egf_experimentallegacyliptricks", false, -1, transition_card_lip},
    {"rollin", "Roll in from the ground", "The game's own setting: riding over a lip from the deck rolls the skater into the ramp.", "egf_groundroll_in", true, -1, transition_card_lip},
    {"legacyvert", "Legacy vert landings", "The game's own switch for its older way of landing on vert: off, the newer landing corrections run.", "egf_exp_legacyvertlandings", false, -1, transition_card_vert},
    {"enhancedsearch", "Enhanced landing search", "The game's own setting for how it looks for a landing while the skater is in the air.", "enh_traj", true, -1, transition_card_vert},
    {"middle", "Force the middle landing", "The game's own experimental switch: of the landings it finds, the middle one is taken.", "egf_forcemiddletrajectory", false, -1, transition_card_vert},
    {"legacypump", "Legacy pumping", "The game's own switch for its older, Skate 3 style pumping code. A trial: with it on, skate.'s own pumping and Pump power "
     "stop, and in the first playtest (2026-10-11) nothing pumped at all. The strength and limit rows below belong to it.", "egf_enablelegacypumping", false, -1, transition_card_pump},
    {"pumptuning", "Experimental pump tuning", "The game's own experimental switch for a second set of pumping numbers.", "exp_pumptuning_on", false, -1, transition_card_pump},
    {"semiauto", "Semi-automatic pumping", "The game's own setting: pumps are helped along.", "enableSemiAutoPumping", false, -1, transition_card_pump},
    {"antipump", "Anti-pumping", "The game's own setting: pushing down at the wrong moment on a transition takes speed away.", "egf_exp_antipumping", true, -1, transition_card_pump},
}};
// mode: -1 the game's own, 0 forced off, 1 forced on.
void force_transition_setting(std::size_t index, int mode) noexcept;
std::uint32_t transition_setting_asked(std::size_t index) noexcept;
// Options of the game's OWN Settings menu that belong with transition skating. They are not the trainer's: it shows the
// value the game holds and changing it here is the same as changing it there (and the other way round), so they are
// not part of the trainer's "modified" state, its presets or its resets.
struct TransitionOption {
    std::string_view key;     // `trainer transition <key> <value>`
    std::string_view label;   // in the menu
    std::string_view help;
    std::string_view setting; // the game's key for it
    bool boolean{};
    int stock{};              // the game's default
    int low{}, high{};
};
inline constexpr std::array<TransitionOption, 3> transition_options{{
    {"pumpassist", "Pump assist", "The game's own Pump Assist: how much of a pump you get without pressing for it. The game takes the larger of your left stick and this, "
     "so at its default of 85 the stick only adds the last 15 percent. 0 is fully manual: no press, no pump. Skate 3 gave about 70 percent of a pump by "
     "itself.", "egf_intentpumpingassistamount", false, 85, 0, 100},
    {"pumpstrength", "Pump strength (game)", "The game's own Pumping Strength option.", "egf_pumpingstrength", false, 100, 0, 100},
    {"forwardpush", "Forward to push", "The game's own control option: the left stick pushed forward pushes. With it on the stick's up and down is not passed on as "
     "the transition input, so forward cannot pump or ask to fly out of a ramp.", "egf_pushforwardtopush", true, 0, 0, 1},
}};
// What the game's physics says about the lip the skater is at, and the stick that asks to fly out of a transition.
// `found` has bit n set where value n was read, in the order of the members.
struct TransitionState {
    bool lip_found{}, lip_tricking{}, handplanting{}, targeting_vert{};
    float transition_input{};
    std::uint32_t drop_in_type{};
    std::array<float, 3> lip_direction{}, lip_nearest{}, ground_normal{};
    std::uint32_t found{};
};
TransitionState read_transition_state(std::uintptr_t base, std::uintptr_t entity) noexcept;

// The game finds every state's value through one lookup (0x11dcdd0). Hooked, a held switch is answered with the
// trainer's own value whoever asks and whenever: writing the value alone lost to the game's scripts, which set the
// switches again on every mount and, for the exit without a pop, every tick of a grind (playtest of 2026-10-11).
// Safe to call every tick; false while the game's code is not the known one.
bool start_state_holds(std::uintptr_t base) noexcept;
// How often the game asked for that switch since last asked here, and how often it was answered with a held value.
void grind_switch_asked(std::size_t index, std::uint32_t &asked, std::uint32_t &answered) noexcept;
// modes: -1 the game's own, 0 held off, 1 held on. live: what each read before the write (-1: not found).
// Why a switch reads nothing: 0 its state object is not found, 1 found but its handle is not valid, 2 found but the
// skater has no page or no value for it, 3 it has a value. `handle` is the object's handle word.
int grind_switch_status(std::uintptr_t base, std::uintptr_t entity, std::size_t index, std::uint32_t &handle) noexcept;
void hold_grind_switches(std::uintptr_t base, std::uintptr_t entity, const std::int8_t *modes, std::int8_t *live) noexcept;
}
