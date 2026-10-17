#include "tricks.h"
#include "tricks_states.h"
#include "tricks_board.h"
#include "tricks_view.h"
#include "tricks_score_hud.h"
#include "tricks_scan.h"
#include "Engine/Core/Json/json.h"
#include <shlobj.h>
#include "Extension/Trainer/trainer.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <algorithm>
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
#include "Extension/Settings/named_settings.h"

// The first controller as the game polls it, and the mask that keeps freestyle's inputs from the game
// (Extension/UI/Overlay/input_capture.cpp).
extern "C" void DingoSDKOverlayTricksPad(void *gamepad);
extern "C" void DingoSDKOverlayTricksMask();
extern "C" std::uint32_t DingoSDKOverlayTricksMaskedPolls();
extern "C" void DingoSDKOverlayTricksAutoMask();
extern "C" void DingoSDKOverlayTricksMaskLeft();
extern "C" void DingoSDKOverlayTricksMaskRight();
extern "C" void DingoSDKOverlayTricksMaskTriggers();
extern "C" void DingoSDKOverlayTricksStroke(const float *xy, const std::uint32_t *ms, std::uint32_t count);
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
    std::uintptr_t base{}, entity{}, rig{};
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
    // Watched states: logged with the physics state whenever their value changes (`tricks state watch <name> b|i|f`).
    struct Watched {
        std::string name;
        int kind{};
        std::uintptr_t at{};
        double last{-12345.0};
        bool missing_logged{};
    };
    std::vector<Watched> watched;
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
    float gs_hop{0.05f}; // metres the board comes off the ground at the middle of the turn
    bool gs_scoop{true};
    bool nc_hide{true};
    std::uint64_t watch_until{}, watch_next{}; // <dev> root vs board heading log after a board-turning trick
    std::uintptr_t s_riding_fakie{};
    std::uintptr_t s_watch_revert{};
    bool watch_air{}, watch_fine{};
    float flip_dip{0.2f};
    float feet_free{0.05f};
    bool kinematic{true}; // the mod's air flips played on the board itself (smooth), not through the board's drive
    int legs_flip{0}; // off: its clip's board kept turning after the mod's flip and the board followed it (2026-10-14) // the game's flip clip played for the legs of a dolphin or dragon (11: 360 flip; 0: none) // the feet's hold on the deck through a flip of the mod's (1: the game's own) // metres the board drops under the feet at the middle of an air flip of the mod's
    // <dev> tricks spinshot: the game nearly stopped at a point of a spin (or of any air) and a still asked for
    float shot_progress{-1}; std::uint32_t shot_air_ms{}, shot_hold_ms{6000}, shot_count{};
    bool shot_taken{}; std::uint64_t shot_until{}, shot_air_since{};
    std::string shot_tag{"sp"}; // <dev> tricks watchair: the watch log after every air
    bool big_spin{}; // out for now (the owner, 2026-10-14: it turned pop shuvits 180 on landing) // the right stick hidden from the game through a no comply (its flick is the mod's)
    // <dev> Grabs while grinding (experiment, off): the game has no grab in its grind node, but its grab clips reach the
    // hands to the board through reach IK states (ReachLockIKTag: floatchooser / boolchooser mirrored wrist states).
    // Those held while grinding with a trigger: does a hand go to the board?
    bool grind_grab{true};
    std::array<std::uintptr_t, 4> s_reach{};
    bool grind_grab_on{};
    // <dev> A grab during a flip trick (experiment, off): the flip clips tag Bool.Anim.CannotGrab; held false when a
    // trigger is pressed in a flip's air, to see what the game's own grab does from there.
    bool flip_grab{true}, flip_grab_on{};
    bool grab_hand_back{};
    std::uint64_t flip_grab_started{};
    std::uint32_t flip_grab_ms{900}; // slower: the owner could not see the catch at 520; one fluid motion at 900
    bool flip_grab_endflip{true};
    // The game's own grab states (measured 2026-10-15, a stock RT grab: CanGrab, IsGrabbing, GrabType 2; a stock ground
    // grab: CanGroundGrab, State.Is.GroundGrab, IsGrabbing, GrabType 2, then IsGroundGrabLoop).
    std::uintptr_t s_can_grab{}, s_can_ground_grab{}, s_is_ground_grab{}, s_is_grabbing{}, s_grab_type{}, s_ground_grab_loop{};
    std::uint64_t grind_grab_since{};
    int grind_grab_kind{};             // the grab grind's grab, taken at the press: 1 melon (LT), 2 indy (RT), 3 double (both)
    int grind_grab_scored_grind{-1};   // the grind (Enum.GS.SkateGrindType) the grab grind was scored on
    std::uintptr_t s_skate_grind_type{};
    std::uintptr_t s_grab_left_hand{}, s_grab_right_hand{};
    bool fandango{true}, fandango_on{}, fandango_any{}, fandango_north{};
    int fandango_family{0}; // <dev> the graph held at this family as well (1: riding, the push's kick; 2: air, Ollie South)
    // The back foot's drives moved off the tail (m, the deck's frame) and the back leg's motors eased to this share.
    float fandango_up{0.15f}, fandango_back{0.3f}, fandango_side{}, fandango_leg{0.15f};
    int fandango_which{-1};     // the leg lifted: -1 the foot nearer the back truck, 0 right, 1 left
    bool fandango_drives{};     // <dev> the physics drives moved as well (they never moved the drawn foot: run fd1)
    std::string fandango_error; // why the leg pose could not be hooked
    std::uint64_t fandango_log{};
    // The pre-grab (the owner, 2026-10-17): riding, a trigger with the left stick up grabs the nose, down the tail (the
    // trigger alone stays the game's own ground grab, mid-board). The trigger's hand: RT right, LT left. A flick from it
    // pops a small ollie or nollie with the hand still on ("playcub style": into the game's air grab, written nose or
    // tail), or throws the board in a flip with the hand (a fingerflip).
    //   stage 0 off, 1 held on the ground, 2 the pop asked (the trigger hidden so the game's ground grab lets go), 3 the air
    bool pregrab{true};
    int pregrab_stage{}, pregrab_hand{}; // the hand: 0 right (RT), 1 left (LT)
    float pregrab_end{1.0f};             // +1 the front truck's end (the nose), -1 the tail
    std::uint64_t pregrab_since{}, pregrab_stick_since{}, pregrab_air_at{}, pregrab_log{};
    int pregrab_flick{-1};               // the flick taken from it: 1 ollie, 16 nollie, 2 kickflip, 3 heelflip, 4 / 7 shuvits
    int pregrab_flick_stage{};           // the player's flick read: 0 none, 1 down first, 2 up first
    std::uint64_t pregrab_flick_at{};
    bool pregrab_scored{}, pregrab_thrown{};
    bool pregrab_reach{true};            // the hand drawn on the nose or tail (the pose editor)
    float pregrab_over{0.12f}, pregrab_up{0.05f}; // the hand's place: metres past the truck, and up from it
    float pregrab_pop{0.0f};             // the stroke's strength (0..1: 40 to 500 ms down first; the smallest pop)
    float pregrab_crouch{0.25f};         // metres the pelvis drive is lowered while pre-grabbing (the hand reaches the end)
    std::uint32_t pregrab_gap{400};      // ms the stick is neutral (and the trigger hidden) before the stroke (150 never
                                         // popped: the game's ground grab was still letting go, runs pp/pq 2026-10-17)
    int pregrab_ground_type{-1};         // <dev> EnumGS.GrabType on the ground (-1 the trigger's: 2 RT stands tall, 1 LT crouches)
    std::uint32_t pregrab_entry_ms{120}; // the left stick held this long with the trigger
    bool pregrab_write_grab{true};       // the game's air grab written as the nose / tail grab type
    int pregrab_nose_grab{21}, pregrab_tail_grab{11}; // Enum.DetailedGrabType: Nose, Tail
    float pregrab_jump{-1};              // <dev> Float.Intent.JumpStrength held through the pop (-1 off)
    // The fingerflips: the game's own (Bool.Intent.Fingerflip with Enum.GS.Intent.FliptrickTypeForFingerflip; the values
    // are to be read: -1 the mod's throw instead) for the kickflip, heelflip, BS and FS shuvit flicks.
    std::array<int, 4> ff_type{-1, -1, -1, -1};
    std::uint32_t ff_delay{80}, ff_ms{450};
    std::uintptr_t s_ff_intent{}, s_ff_type{}, s_jump_strength{};
    std::string pregrab_error;
    bool tweak_fall{true}, was_on_rail{};
    int air_grab_type{};
    std::uint64_t air_grab_seen{}, air_tweak_seen{};
    std::uintptr_t s_detailed_grab{};  // the graph held at riding (1: the push's back foot off) or air (2: Ollie South)
    std::uintptr_t s_ollie_south{};
    std::uint32_t flip_grab_spin_ms{650}; // out, flipped, and back into the game's grab
    float flip_grab_rise{0.35f};  // metres the board rises at the top of a flip grab (from the feet to the waist)
    float flip_grab_reach{0.25f};  // and out in front of the skater (toward the toes, the skater's side row)
    float grab_rigid{40.0f};
    float flip_grab_leash{1.4f};  // the board this far from the pelvis in a flip grab: let go (it was flying off)
    bool flip_grab_holding{};      // the board's drive strength through a flip grab's path
    float grab_side{0.11f};      // the board's edge the back hand takes, in the deck's side axis (the front hand the other)
    float grab_gain{0.6f};      // the share of the way to the board a hand is pulled each step
    float grab_crouch{0.3f};     // metres the pelvis drive is lowered toward the board in a grind grab
    float grab_tuck{0.45f};      // and in a flip grab: the knees come up to the chest, as vert skaters tuck a kickflip indy
    int grab_arm_mask{0};       // <dev> 0: the trigger's arm; otherwise this limb mask
    float grab_limbs{0.1f};      // the share of their motors the grabbing arm and the legs keep through a grab
    std::uintptr_t s_cannot_grab{};
    std::uint64_t flip_grab_until{};  // the legs' scoop (the game's trick-setup crouch) while it turns
    // The hand on the board through the game's own hand grab IK target (set_hand_ik): in a grind grab, and in a flip
    // grab from the catch on. The back hand is the IK's right hand for this skater unless `hand_ik_back_right` is off.
    bool hand_ik{true}, hand_ik_back_right{true};
    float hand_ik_up{0.02f};        // metres over the deck's centre
    float hand_ik_catch{0.4f};      // share of the flip grab's turn where the hand takes it
    // The casper's flick-out exit clip: FreeStyleFlipTrick 9 (exit_flip1) or 16 (exit_flip2). Its pop is the clip's own
    // (holding the jump strength at 0.35 through physics 103 left the 0.83 s air unchanged, run cd3).
    std::uint32_t casper_exit{9};
    float primo_air_need{0.25f}; // s of air the game must foresee before a primo's flip out turns (a bounce foresees less)
    float out_air_best{};
    int grind_family{1};            // the graph's physics family held through a grind grab (1 riding; 3 is the grind's; -1 off)
    std::uint64_t hand_ik_logged{}, hand_ik_watch_until{}; // <dev> `tricks handik hold`: gaps and the context logged
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
    // A casper's flick-out turned from the pose (the owner, 2026-10-14): the roll back over is the flip. out_dir is the
    // way it rolls back (1 kickflip, -1 heelflip, 0 the shorter), out_roll then only what comes on top of it.
    bool out_pose{};
    int out_dir{};
    int flickout_flip{0}; // kick / heel: 0 a half flip, 1 bigger (one and a half)
    int flickout_up{0};   // stick up: 0 tre half flip, 1 tre with the bigger flip, 2 a straight half flip (no shuv)
    // The retail flip's legs for a flick-out (the owner, 2026-10-14: the feet as in the game's own flips): the game's
    // animation flip trick set for its air, the board still the mod's. 0: no legs (the straight half flip).
    bool out_legs{true};
    int out_legs_flip{};
    std::uint64_t out_until{}; // a flick out of the casper: its turn starts when the exit is in the air
    bool out_spinning{};
    bool exit_air{}; // the flip out of a casper has left the ground
    std::uint64_t fall_until{};
    std::uintptr_t s_late_flip{};
    std::uint64_t mod_since{}, landed_at{}, air_last{};
    bool catch_armed{true}, edge_brief{};
    std::uint64_t air_start{}, last_centre{1}, last_pop{};
    // Off (owner, 2026-10-14: "if you touch the rim you instantly fall"): only the stick back at the middle drops a casper.
    bool edge_fall{false};
    bool centre_fall{true}; // a casper with the stick back at the middle is lost
    bool fall_slam{};
    bool left_mask{true};
    bool left_mask_air{true};
    bool air_pose{true}, air_pose_on{};
    // (Holding the plain in-air states true to cut a flip short made a casper bounce again and cost most of the speed:
    // measured 2026-10-13. None by default.)
    // One term of the graph's generic landing (EBool.Anim.Is.GenericLanding), held so the flip is left for a generic
    // landing clip, whose early exit to riding opens at its tick 4: the pose starts 60 ms after the catch (measured
    // 2026-10-13; without it the landing node carries the flip clip on and the pose starts at touchdown).
    std::vector<std::string> air_names{"Animation/Dingo/Bool.GP.DidInAirTuck"};
    std::vector<int> air_values{0}; // 0: a bool held true; otherwise an enum held at this value
    // The trick node's rows to the landing node have no branch window but want the family before ground to be air
    // (EnumGS.LastFilteredPhysicsState == 2); the game leaves it at ground when only the current one is written.
    std::uint32_t primo_air_delay{250}; // ms from takeoff before a caught primo's way in starts (when the time to land is not known)
    float primo_air_left{5.0f};        // a caught primo's way in starts with this much time left to land
    std::uintptr_t s_time_to_land{};
    bool air_last_fps{true};
    std::uintptr_t s_last_filtered{};
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
    // The anti-casper (the nose-side casper; the owner, 2026-10-13): the game has no clips for it, so it is the casper
    // played mirrored (Animation/Dingo/Bool.Anim.Mirrored held true: the front foot on top at the nose end, the back
    // foot hooked under the raised tail). Caught with the stick resting up (between the primos' places). The mirror
    // goes on at the catch, before the way in starts, and stays on through the way out until it has been in the air
    // or the skater rides again (released earlier it snapped to the plain casper at both ends: owner, 2026-10-13).
    bool anti_on{true}, anti{}, zone_anti{}, mirror_on{};
    // The casper's height (Float.Intent.Balance: its loop's low / medium / high clips, 0 to 1) on the right stick.
    bool height_on{true};
    float height_at_edge{0.0f}, height_at_middle{1.0f}, height_inner{0.45f}, height_now{0.5f};
    std::uint64_t height_last{};
    bool anti_turn{true};
    // A primo's flick-out as the game's own ollie + flip trick (Bool.Intent.OllieNorth, EnumGS.Intent.FlipTrick held
    // for a moment once the skater rides): the flip trick wanted (0 none), until when, and since when it is asked.
    bool primo_game_pop{true};
    std::uint32_t primo_pop_hold_ms{300}; // how long the primo's way out is held (CurrentGroundTrick 7) before it is let go
    // The primo's flick-out (the owner's pick, 2026-10-15): the primo's own way out let go after `primo_pop_hold_ms`
    // pops (its FS shuvit clip lifts the board: about 0.45 m, run n5), and the flick's turn plays on the board in that hop.
    // The game has no flip out of a primo, and its ollie is only read once the way out is over (about 2.5 s: runs n3, n5).
    bool primo_hop_flip{true};
    // The primo's flip out turns the way the board rolls out of the primo (the owner, 2026-10-15: the board "spins one
    // direction to get out of primo and then reverses" for the flip; the flip should go on the same way). Its roll way
    // is read at the start of the hop: that picks kickflip or heelflip, and the varial's shuvit goes with it (kick + BS,
    // heel + FS). The flick picks the kind: 0 the flip, 1 the varial, 2 the 360 (tre / laser).
    bool out_follow{};
    bool out_from_primo{};
    std::uint32_t primo_flip_ms{430};
    int out_kind{};
    // The anti casper as a nose manual (the owner, 2026-10-14: the mirrored casper "totally twists you and spins you
    // around"; "it should just be a nose manual casper, front foot on the nose, rear foot under the board"): the game's
    // own nose manual held (EnumGS.Intent.Manual at the nose manual's value, learned from the player's nose manuals),
    // the board following its clip (103) turned upside down and further nose down, so the raised tail is over the back foot.
    bool anti_nose{true}, anti_spun{};
    int nose_manual_value{-1};
    float anti_pitch{15.0f};
    std::uintptr_t s_manual_intent{};
    std::uint32_t pop_flip{};
    std::uint64_t pop_until{}, pop_started{};
    std::uintptr_t s_ollie{}, s_jump{}, s_flip_intent{}, s_flip_speed_intent{};
    std::uint64_t exit_guard_until{};
    bool exit_guard_air{};
    std::uint32_t exit_guard_ms{2000};
    // The sticks over the last half second (a sample a tick): the flick that pops a trick is read back from it.
    struct StickSample {
        std::uint64_t at{};
        float rx{}, ry{}, lx{};
    };
    std::array<StickSample, 48> sticks{};
    std::size_t stick_next{};
    // A manual's brake and the click: a pose from a manual wants the click pressed after the brake has held a moment
    // (the ground shuvit holds the click first, then pushes the stick down: the same stick as a manual brake).
    std::uint64_t brake_since{}, click_down_at{};
    bool click_was{};
    // Flick-it extras read from the pop (dolphin, dragon, big spin), once per air.
    bool pop_checked{}, pop_spinning{};
    std::uint64_t nc_takeoff_at{};
    std::uint64_t takeoff_at{};
    bool gs_logged{}; // <dev>
    int air_flip_seen{}; // <dev> the game's flip seen in this air (the stock check at the landing)
    std::uint32_t flickit_spin_ms{450};
    std::uint32_t out_spin_ms{320}; // a flick-out's flip
    std::uint64_t out_spin_at{};
    std::uint32_t out_spin_len{};
    // The Tricks page (Insert menu): each trick and each family switched on or off, kept in tricks.json.
    std::array<bool, trick_count> trick_on = [] { std::array<bool, trick_count> a{}; a.fill(true); return a; }();
    std::array<bool, family_count> family_on = [] { std::array<bool, family_count> a{}; a.fill(true); return a; }();
    bool score_feed{true};
    bool scan_auto{};
    float scan_last{-1};
    std::uint64_t scan_next_read{};
    bool store_loaded{}, view_due{true}, save_due{};
    std::uint64_t next_save{}; // the skater (root) turned half round while mirrored: `tricks anti turn on|off`
    std::uint32_t anti_steady_ms{150}; // the stick passes the top during a flick: it has to rest there this long
    std::uint64_t mirror_until{};
    std::uintptr_t s_mirrored{};
    // Poses from a manual (the owner, 2026-10-13): the manual pushed all the way down (the game's manual brake, the
    // tail or nose on the ground), then the click and the stick to a pose's place: from a tail manual the casper or
    // the primo, from a nose manual the anti-casper.
    bool manual_entry{true};
    std::uintptr_t s_tail_manual{}, s_nose_manual{}, s_manual_brake{};
    std::uint64_t tail_down_at{}, nose_down_at{};
    int manual_seen{-1}; // <dev> the manual states as last logged
    int manual_kind{};   // 1 tail, 2 nose: the manual being ridden (kept through its brake)
    bool manual_seen_brake{};
    std::uint64_t zone_since{}, shuv_down_at{}, shuv_side_at{};
    bool gs_rim{};           // the ground shuvit's stroke went out to the rim (the game's pop shuvit, not this)
    float gs_peak{}, gs_rim_at{0.88f};
    std::uint64_t pop_after{};     // the stroke not before this (a ground shuv's neutral gap)
    std::uint32_t gs_gap{300};     // ms of a neutral stick between the player's flick and the ground shuv's stroke
    float gs_lift{0.08f}; // metres the body rises for a ground shuvit's turn (the pelvis drive raised)
    float gs_jump{0.4f}, gs_speed{1.6f}; // the ground shuvit as the game's pop shuvit: its pop strength and turn speed
    bool gs_game{true}; // the ground shuvit as the game's own pop shuvit with a small pop (off: the mod turns the board)
    float pop_jump{1.0f}, pop_speed{1.0f};
    // The mod's pops are the game's own: a right-stick stroke played to the game (its gesture reader then pops the
    // ollie with the flip trick). Writing the ollie intent never popped (runs r11-r14): the pop is the trick clip's
    // jump tag (request bit 13c4.9) in physics state 103, and the clip comes from the gesture. The stick side of a
    // kickflip and of a backside shuvit for this skater, put right by the game's answer (flip 2 vs 3, 4 vs 7).
    float kick_side{1.0f}, bs_shuv_side{1.0f};
    std::uint32_t pop_asked{};
    std::uint64_t pop_stroke_at{};
    std::uintptr_t s_riding_node{};
    bool pop_wait_riding{}; // a primo's pop waits for the riding node (its way out is a clip of its own)
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
    bool keep{false}; // a held pose is not left for the game's own dismount
    // The posed board held rigidly to its clip on the ground (the drive's strength; the game's own is 1), and the
    // game's wipeout and run-out checks told to stand down while a pose is held (Bool.Anim.ForceDisableAllWipeouts):
    // at 25 km/h the owner's poses rocked 60 to 90 degrees and the game ran the skater off a second in (2026-10-13).
    float rigid{30.0f};
    bool level{true};
    float balance_fixed{-0.1f}; // <dev> the casper's Balance: 0 to 1 held there, -0.1 the stick's, -1 not written
    // The skater's root in a held pose: 0 the game's, 1 watched, 2 held upright. Held upright both before the root
    // update (the frame it is built from, [rig + 0x59b0] + 0x5b0, which rolled 90 degrees and back in a casper) and
    // after it. The casper flop (butt on the ground 0.6 s in) is gone with it: rider parts 0.88 -> 1.17 m, never
    // below (measured 2026-10-13 23:14; owner: "that looked good"). Upright after the update alone did nothing.
    int rootup{2};
    // <dev> The rider's drives to the deck (pelvis and feet) given an upright deck in a held pose: 0 the game's, 1
    // watched, 2 upright. Not the flop's cause (no change with it on, 2026-10-13): off.
    int riderframe{0}, rider_feet{1};
    std::uint32_t level_delay{0};
    bool no_runout{true}, no_runout_on{};
    std::uintptr_t s_no_wipeouts{};
    // The poses' way-in clips started part of the way in, so a catch snaps to the pose (casper_into has its board
    // within 10 degrees of the pose from tick 34 of 70; primo_into is on its edge from tick 38 of 95).
    float snap_casper{22.0f}, snap_primo{38.0f};
    bool snap_dirty{}, snap_applied{};
    // A pose from a manual starts its way in at the beginning: the board turns over on the ground from where the
    // manual has it (snapped part of the way in, the nose-manual anti-casper bounced and flipped round: owner,
    // 2026-10-13). An air catch puts the snap back.
    bool snap_from_start{};
    std::uint64_t next_snap{};
    bool stomp{true}; // a caught pose's board touching down goes straight to the ground animation state (no bounce)
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

// ---- the Tricks page: switches, settings, tricks.json --------------------------------------------------------
// A trick is on when it and its family are both on.
// <dev> The skater's root frame and the board's (part 8, as the root update copies it): their headings' angle, degrees.
bool copy_rows(std::uintptr_t at, float *out, std::size_t floats) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void *>(at), floats * sizeof(float));
        return true;
    } __except (1) { return false; }
}
bool root_board_angle(std::uintptr_t rig, float &angle, float &board_up) noexcept {
    float root[12]{}, board[12]{};
    if (!rig || !copy_rows(rig + 0x4720, root, 12) || !copy_rows(rig + 0x48a0, board, 12)) return false;
    const float rf[2]{root[8], root[10]}, bf[2]{board[8], board[10]};
    const float a = std::atan2(rf[0] * bf[1] - rf[1] * bf[0], rf[0] * bf[0] + rf[1] * bf[1]);
    angle = a * 57.2958f;
    board_up = board[5];
    return true;
}
// <dev> The board-backwards flag (rig+0x59a5) and the offset frame's heading (rig+0x4920, forward row), degrees.
bool board_backwards_state(std::uintptr_t rig, int &flag, float &offset) noexcept {
    float rows[12]{};
    if (!rig || !copy_rows(rig + 0x4920, rows, 12)) return false;
    __try { flag = *reinterpret_cast<const std::uint8_t *>(rig + 0x59a5); } __except (1) { return false; }
    offset = std::atan2(rows[8], rows[10]) * 57.2958f;
    return true;
}
// A flip in the air: the board drops under the feet as it turns and comes back up to them for the catch (the stock
// flips' board turns well below the lifted feet; the mod's turned at the feet and went through the shoes: stills
// 2026-10-14). A flat turn (a shuvit) drops half as far.
// `pose` (not -2): turned from the board's pose as it is (a flick-out), rolled back over the way `pose` says (1, -1, 0
// the shorter) with `roll` on top.
void start_air_spin(const Runtime &r, float roll, float yaw, float pitch, std::uint32_t ms, bool near_ground = false, bool follow_clip = false, int pose = -2) noexcept {
    std::uintptr_t kin_core{};
    if (r.kinematic && r.rig) (void)copy_rows(r.rig + 0x4630, reinterpret_cast<float *>(&kin_core), 2);
    if (kin_core) {
        const float kdip = near_ground ? 0.0f : pitch != 0 ? r.flip_dip * 1.5f : roll != 0 || pose != -2 ? r.flip_dip : r.flip_dip * 0.5f;
        start_board_spin_kinematic(kin_core, roll, yaw, pitch, ms, 1.0f, kdip);
        set_kinematic_spin_follow(follow_clip);
        if (pose != -2) set_kinematic_spin_pose(true, pose);
        set_kinematic_spin_to_feet(pose != -2); // a flick-out ends under the feet, where the legs are
        if (r.feet_free < 1.0f) set_feet_free(kin_core, ms + 60, r.feet_free);
        return;
    }
    start_board_spin(roll, yaw, pitch, ms);
    // An end-over-end turn stands the board up: half a board's length more to clear (stills 2026-10-14: a dolphin's
    // upper end reached the soles at 0.2 m).
    // A no comply's air is a step off the ground: no room under the board (a dipped no comply shuvit hit the ground
    // and landed on its edge, 2026-10-14).
    const float dip = near_ground ? 0.0f : pitch != 0 ? r.flip_dip * 1.5f : roll != 0 ? r.flip_dip : r.flip_dip * 0.5f;
    if (dip > 0) set_board_hop(-dip);
    // The feet let go of the deck while it turns (the core is the rig's owner, rig + 0x4630).
    std::uintptr_t core{};
    if (r.feet_free < 1.0f && r.rig && copy_rows(r.rig + 0x4630, reinterpret_cast<float *>(&core), 2) && core)
        set_feet_free(core, ms + 60, r.feet_free);
}
// The legs of a flip trick for a flip of the mod's (the owner, 2026-10-14: the feet should get out of the board's way
// as in a 360 flip): the game's animation flip trick set (EnumGS.Anim.CurrentFlipTrick; the intent stays none, so
// the game neither scores nor turns the board for it) and the board turned from the skater's frame, not the clip's.
void flip_legs(Runtime &r, std::uintptr_t base) noexcept {
    if (r.legs_flip <= 0) return;
    if (!r.s_current_flip) r.s_current_flip = find_state(base, "Animation/Dingo/EnumGS.Anim.CurrentFlipTrick");
    (void)write_state_at(base, r.s_current_flip, 1, r.legs_flip);
    set_board_spin_base(r.rig, true);
}
// A pre-grab's nose or tail grab in the air is held, not tweaked: it grinds like the plain grabs.
bool pregrab_plain(const Runtime &r, std::uint64_t now) { return r.pregrab_air_at && now - r.pregrab_air_at < 3000; }
bool plain_grab(const Runtime &r, std::uint64_t now, int type) {
    return type == 1 || type == 6 || type == 33 || (pregrab_plain(r, now) && (type == r.pregrab_nose_grab || type == r.pregrab_tail_grab));
}
bool trick_enabled(const Runtime &r, std::string_view id) {
    const int i = trick_index(id);
    return i < 0 || (r.trick_on[i] && r.family_on[static_cast<std::size_t>(trick_catalog[i].family)]);
}
// ---- the sticks' recent past: what a pop's flick asked for ------------------------------------------------------
void record_sticks(Runtime &r, std::uint64_t now, float rx, float ry, float lx) {
    r.sticks[r.stick_next] = {now, rx, ry, lx};
    r.stick_next = (r.stick_next + 1) % r.sticks.size();
}
std::vector<Runtime::StickSample> recent_sticks(const Runtime &r, std::uint64_t now, std::uint64_t window) {
    std::vector<Runtime::StickSample> out;
    for (std::size_t i = 0; i < r.sticks.size(); ++i) {
        const auto &s = r.sticks[(r.stick_next + i) % r.sticks.size()]; // oldest first
        if (s.at && s.at + window >= now) out.push_back(s);
    }
    return out;
}
float stick_angle(float x, float y) {
    float a = std::atan2(y, x) * 180.0f / 3.14159265f;
    return a < 0 ? a + 360.0f : a;
}
// The flip a flick-it asked for, read from the right stick's way over `window` ms: the stick pulled down, then where it
// went from there. The game's flip ids: 2 kickflip (up-left), 3 heelflip (up-right), 4 pop shuvit (rolled to the left
// rim), 7 FS pop shuvit (to the right rim); 0 a plain pop (straight up); -1 no flick.
int popped_flip(const Runtime &r, std::uint64_t now, std::uint64_t window) {
    const auto v = recent_sticks(r, now, window);
    std::size_t down = v.size();
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].ry < -0.6f) { down = i; break; }
    if (down == v.size()) return -1;
    for (std::size_t i = down + 1; i < v.size(); ++i) {
        const float len = std::sqrt(v[i].rx * v[i].rx + v[i].ry * v[i].ry);
        if (len < 0.75f) continue;
        const float a = stick_angle(v[i].rx, v[i].ry);
        if (a >= 240.0f && a <= 300.0f) continue; // still down
        if (a >= 100.0f && a <= 165.0f) return 2;
        if (a >= 15.0f && a < 80.0f) return 3;
        if (a >= 80.0f && a < 100.0f) return 0;
        if (a > 165.0f && a < 240.0f) return 4;
        return 7; // 300..360, 0..15
    }
    return -1;
}
// The dolphin's stroke (stick down, up, and straight back down): 1; ending on a lower diagonal (the dragon): 2, with the
// side it ended on in `side` (-1 left, 1 right). 0: none.
// From the nose (`nollie`): the same stroke the other way up, as the game's nollie is its ollie's flick reversed
// (stick up, straight down, and pushed back up; ending on an upper diagonal: the nollie dragon).
int forward_flick(const Runtime &r, std::uint64_t now, std::uint64_t window, int &side, bool nollie = false) {
    const auto v = recent_sticks(r, now, window);
    const float sense = nollie ? -1.0f : 1.0f;
    int stage = 0; // 0 looking for down, 1 for up, 2 for the second down
    for (const auto &sample : v) {
        const Runtime::StickSample s{sample.at, sample.rx, sample.ry * sense, sample.lx};
        if (stage == 0 && s.ry < -0.6f) stage = 1;
        else if (stage == 1 && s.ry > 0.75f && std::abs(s.rx) < 0.4f) stage = 2; // straight up: a kickflip's flick is diagonal
        // Pushed down to the rim, not the spring-back: straight down a dolphin, on a lower diagonal a dragon (a diagonal's
        // stick sits near y -0.7, which the old y < -0.8 never let through: the owner "couldn't do a dragon", 2026-10-15).
        else if (stage == 2 && s.ry < -0.5f && std::sqrt(s.rx * s.rx + s.ry * s.ry) > 0.85f) {
            side = s.rx < 0 ? -1 : 1;
            return std::abs(s.rx) > 0.4f ? 2 : 1;
        }
    }
    return 0;
}
int setting_index(std::string_view key) {
    for (std::size_t i = 0; i < setting_count; ++i)
        if (key == setting_catalog[i].key) return static_cast<int>(i);
    return -1;
}
double setting_value(const Runtime &r, std::string_view key) {
    if (key == "zone_left") return r.zone_left;
    if (key == "zone_right") return r.zone_right;
    if (key == "anti_steady") return r.anti_steady_ms;
    if (key == "height_edge") return r.height_at_edge;
    if (key == "height_middle") return r.height_at_middle;
    if (key == "snap_casper") return r.snap_casper;
    if (key == "snap_primo") return r.snap_primo;
    if (key == "slide") return r.slide;
    if (key == "rigid") return r.rigid;
    if (key == "out_spin_ms") return r.out_spin_ms;
    if (key == "exit_guard_ms") return r.exit_guard_ms;
    if (key == "groundshuv_ms") return r.gs_ms;
    if (key == "groundshuv_hop") return r.gs_hop;
    if (key == "nc_spin_ms") return r.nc_spin_ms;
    if (key == "air_spin_ms") return r.air_spin_ms;
    if (key == "flickit_spin_ms") return r.flickit_spin_ms;
    if (key == "pregrab_pop") return r.pregrab_pop;
    if (key == "pregrab_over") return r.pregrab_over;
    if (key == "fingerflip_ms") return r.ff_ms;
    if (key == "flickout_flip") return r.flickout_flip;
    if (key == "flickout_up") return r.flickout_up;
    return 0;
}
// Clamped to the setting's range; false for a key that is not a setting.
bool set_setting(Runtime &r, std::string_view key, double value) {
    const int i = setting_index(key);
    if (i < 0 || !std::isfinite(value)) return false;
    const auto &info = setting_catalog[i];
    const float v = std::clamp(static_cast<float>(value), info.low, info.high);
    const auto ms = static_cast<std::uint32_t>(std::lround(v));
    if (key == "zone_left") r.zone_left = v;
    else if (key == "zone_right") r.zone_right = v;
    else if (key == "anti_steady") r.anti_steady_ms = ms;
    else if (key == "height_edge") r.height_at_edge = v;
    else if (key == "height_middle") r.height_at_middle = v;
    else if (key == "snap_casper" || key == "snap_primo") {
        (key == "snap_casper" ? r.snap_casper : r.snap_primo) = v;
        r.snap_applied = false; // written into the transitions again on the next tick
        r.snap_from_start = false;
    } else if (key == "slide") r.slide = v;
    else if (key == "rigid") r.rigid = v;
    else if (key == "out_spin_ms") r.out_spin_ms = ms;
    else if (key == "exit_guard_ms") r.exit_guard_ms = ms;
    else if (key == "groundshuv_ms") r.gs_ms = ms;
    else if (key == "groundshuv_hop") r.gs_hop = v;
    else if (key == "nc_spin_ms") r.nc_spin_ms = ms;
    else if (key == "air_spin_ms") r.air_spin_ms = ms;
    else if (key == "flickit_spin_ms") r.flickit_spin_ms = ms;
    else if (key == "pregrab_pop") r.pregrab_pop = v;
    else if (key == "pregrab_over") r.pregrab_over = v;
    else if (key == "fingerflip_ms") r.ff_ms = ms;
    else if (key == "flickout_flip") r.flickout_flip = static_cast<int>(ms);
    else if (key == "flickout_up") r.flickout_up = static_cast<int>(ms);
    else return false;
    return true;
}
std::filesystem::path store_path() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw) result = std::filesystem::path(raw) / L"ReSkate" / L"tricks";
    if (raw) CoTaskMemFree(raw);
    return result;
}
void changed(Runtime &r, bool save = true) {
    r.view_due = true;
    if (save) r.save_due = true;
}
// The name of a mod trick, shown as a ReSkate notice (`tricks names`).
bool announce_tricks = true;
void load_store(Runtime &r) {
    r.store_loaded = true;
    try {
        const auto directory = store_path();
        if (directory.empty()) return;
        std::ifstream file(directory / L"tricks.json", std::ios::binary);
        if (!file) return;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (text.empty() || text.size() > 256 * 1024) return;
        const auto json = Json::parse(text);
        if (!json.is_object()) return;
        if (json.contains("tricks") && json.at("tricks").is_object())
            for (std::size_t i = 0; i < trick_count; ++i) r.trick_on[i] = json.at("tricks").value(trick_catalog[i].id, true);
        if (json.contains("families") && json.at("families").is_object())
            for (std::size_t i = 0; i < family_count; ++i) r.family_on[i] = json.at("families").value(family_catalog[i].id, true);
        if (json.contains("settings") && json.at("settings").is_object())
            for (std::size_t i = 0; i < setting_count; ++i) {
                const auto &settings = json.at("settings");
                if (settings.contains(setting_catalog[i].key) && settings.at(setting_catalog[i].key).is_number())
                    (void)set_setting(r, setting_catalog[i].key, settings.at(setting_catalog[i].key).get<double>());
            }
        if (json.contains("options") && json.at("options").is_object()) {
            const auto &o = json.at("options");
            announce_tricks = o.value("names", announce_tricks);
            r.score_feed = o.value("score", r.score_feed);
            r.height_on = o.value("height", r.height_on);
            r.anti_turn = o.value("anti_turn", r.anti_turn);
            r.two_primos = o.value("two_primos", r.two_primos);
        }
        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: settings loaded from tricks.json.");
    } catch (...) {
        logging::log(logging::Level::warning, logging::Channel::skater, "Tricks: tricks.json could not be read; defaults kept.");
    }
}
void save_store(Runtime &r) {
    r.save_due = false;
    try {
        Json json = Json::object();
        json["schema"] = 1;
        Json tricks = Json::object();
        for (std::size_t i = 0; i < trick_count; ++i) tricks[trick_catalog[i].id] = r.trick_on[i];
        json["tricks"] = std::move(tricks);
        Json families = Json::object();
        for (std::size_t i = 0; i < family_count; ++i) families[family_catalog[i].id] = r.family_on[i];
        json["families"] = std::move(families);
        Json settings = Json::object();
        for (std::size_t i = 0; i < setting_count; ++i) settings[setting_catalog[i].key] = setting_value(r, setting_catalog[i].key);
        json["settings"] = std::move(settings);
        Json options = Json::object();
        options["names"] = announce_tricks;
        options["score"] = r.score_feed;
        options["height"] = r.height_on;
        options["anti_turn"] = r.anti_turn;
        options["two_primos"] = r.two_primos;
        json["options"] = std::move(options);
        const auto directory = store_path();
        if (directory.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const auto target = directory / L"tricks.json", temporary = directory / L"tricks.json.tmp";
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
        logging::log(logging::Level::warning, logging::Channel::skater, "Tricks: could not save tricks.json.");
    }
}
void publish_view(Runtime &r) {
    r.view_due = false;
    auto next = std::make_shared<View>();
    next->ready = true;
    next->trick = r.trick_on;
    next->family = r.family_on;
    for (std::size_t i = 0; i < setting_count; ++i) next->setting[i] = static_cast<float>(setting_value(r, setting_catalog[i].key));
    next->names = announce_tricks;
    next->score = r.score_feed;
    next->height = r.height_on;
    next->anti_turn = r.anti_turn;
    next->two_primos = r.two_primos;
    publish(std::move(next));
}
// The pose in the air: the graph's copy of the physics state family, written in place every physics step (the game
// writes it back each step; the animation reads it after).
struct AirPoke {
    std::atomic<std::uintptr_t> base{}, entity{}, state{}, last{};
    std::atomic<std::uint64_t> until{};
    std::atomic<std::uint32_t> pokes{}, put_back{};
    std::atomic<std::uint32_t> value{1}; // the family held (1 the pose in the air; a grind grab holds the ground's)
};
AirPoke air_poke;
void air_poke_step() noexcept {
    if (GetTickCount64() >= air_poke.until.load(std::memory_order_relaxed)) return;
    std::uint32_t before{};
    if (poke_state_value(air_poke.base.load(std::memory_order_relaxed), air_poke.entity.load(std::memory_order_relaxed), air_poke.state.load(std::memory_order_relaxed), air_poke.value.load(std::memory_order_relaxed), &before)) {
        air_poke.pokes.fetch_add(1, std::memory_order_relaxed);
        if (before != air_poke.value.load(std::memory_order_relaxed)) air_poke.put_back.fetch_add(1, std::memory_order_relaxed);
    }
    // The family before this one, as the graph's landing rows want it: air (2).
    if (const auto last = air_poke.last.load(std::memory_order_relaxed)) (void)poke_state_value(air_poke.base.load(std::memory_order_relaxed), air_poke.entity.load(std::memory_order_relaxed), last, 2, nullptr);
}
void announce(std::string_view name);
void score_trick(std::string_view id, std::string_view name);
// The stroke that makes the game pop with flip trick `flip` (0 or 1 the ollie, 2 kickflip, 3 heelflip, 4 shuvit,
// 7 frontside shuvit, 11 the 360 flip): the stick pulled down for a setup time, then flicked, quickly. `strength`
// (0..1) is the setup time, 40 to 500 ms (measured 2026-10-15: how quick the flick is changes nothing).
// The 360 flip is the game's pattern (Gesture_FlipTrick_360Flip, its y is the stick's down): from the side away from
// the kick, rolled down through the bottom and flicked up to the kick's diagonal.
void play_pop_stroke(Runtime &r, std::uint32_t flip, float strength) {
    std::array<float, 16> xy{};
    std::array<std::uint32_t, 8> ms{};
    std::uint32_t n = 0;
    const auto step = [&](float x, float y, std::uint32_t t) { xy[2 * n] = x; xy[2 * n + 1] = y; ms[n++] = t; };
    const auto setup = static_cast<std::uint32_t>(std::lround(40.0f + 460.0f * std::clamp(strength, 0.0f, 1.0f)));
    const float kick = r.kick_side, shuv = r.bs_shuv_side;
    switch (flip) {
    case 2: case 3:
        step(0, -1, setup);
        step(flip == 2 ? 0.7f * kick : -0.7f * kick, 0.7f, 150);
        break;
    case 4: case 7: {
        const float x = flip == 4 ? shuv : -shuv;
        step(0, -1, setup);
        step(0.7f * x, -0.7f, 40);
        step(x, 0, 120);
        break;
    }
    case 11:
        step(-kick, -0.2f, setup);
        step(-0.6f * kick, -0.7f, 40);
        step(0, -0.8f, 40);
        step(0.6f * kick, 0.45f, 150);
        break;
    case 16: // the nollie: up first, then down
        step(0, 1, setup);
        step(0, -1, 150);
        break;
    default:
        step(0, -1, setup);
        step(0, 1, 150);
        break;
    }
    step(0, 0, 60);
    DingoSDKOverlayTricksStroke(xy.data(), ms.data(), n);
    r.pop_asked = flip;
    r.pop_stroke_at = GetTickCount64();
}
// The hand of a grab held on the board's edge through the game's own hand IK for the next 120 ms (renewed each tick
// while the grab lasts): the back hand on the toe edge (+grab_side on the deck's side row), the front on the heel edge.
// The hands' distances to the target are logged every 100 ms.
void hold_hand_ik(Runtime &r, std::uintptr_t core, std::uint64_t now, bool back_hand) {
    const bool ik_right = back_hand == r.hand_ik_back_right;
    set_hand_ik(core, 120, ik_right, back_hand ? r.grab_side : -r.grab_side, r.hand_ik_up, 0);
    if (now - r.hand_ik_logged >= 100) {
        r.hand_ik_logged = now;
        float back{}, front{};
        const auto steps = take_hand_ik(back, front);
        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: hand ik: {} hand, back hand {:.3f} m, front hand {:.3f} m from the deck; {} ({} steps).", back_hand ? "back" : "front",
                     back, front, hand_ik_places(), steps);
    }
}
void start_ground_shuv(Runtime &r, std::uint64_t now, bool frontside) {
    // The game's own pop shuvit with a small pop (the owner, 2026-10-15: the board turned under feet still on it, "the feet
    // need a clear flick"): the game's legs scoop and lift off and the board turns under them, barely off the ground.
    if (r.gs_game) {
        r.pop_flip = frontside ? 7u : 4u;
        r.pop_until = now + 600;
        r.pop_wait_riding = false;
        r.pop_started = 0;
        // The player's own flick still in the game's reading of the stick merged with the stroke (the owner's log: asked
        // 7, the game played 13, the stroke played as the player's let go): the stick neutral a moment first.
        r.pop_after = now + r.gs_gap;
        r.pop_until = now + r.gs_gap + 600;
        r.pop_jump = r.gs_jump;
        r.pop_speed = r.gs_speed;
        score_trick("groundshuv", frontside ? "FS Ground Shuvit" : "Ground Shuvit");
        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: ground {} shuvit (the game's, pop {:.2f}).", frontside ? "frontside" : "backside", r.gs_jump);
        return;
    }
    std::uint32_t seen{}, turned{};
    board_hook_counts(seen, turned); // counted afresh over the turn
    // Played on the board itself (game time, the clip's board as the base, lifted by the hop): it ends exactly on the
    // board the game wants, so nothing is left turned half round under the feet (the hook's held half turn ended in a
    // step-off, physics 504, on both sides: 2026-10-15).
    std::uintptr_t core{};
    if (r.kinematic && r.rig) (void)copy_rows(r.rig + 0x4630, reinterpret_cast<float *>(&core), 2);
    if (core) {
        start_board_spin_kinematic(core, 0, frontside ? -180.0f : 180.0f, 0, r.gs_ms, 1.0f, -r.gs_hop);
        set_kinematic_spin_follow(true);
        // The feet off the deck for the turn: their drives held them to it and the board twisted them round, then they
        // jumped back (the owner, 2026-10-15: "the feet need a clear flick"). Freed, and the body lifted a little (the
        // pelvis drive raised) so the board turns under them.
        set_feet_free(core, r.gs_ms + 60, r.feet_free);
        set_limb_free(core, r.gs_ms, 0, 1.0f, -r.gs_lift);
    } else {
        start_board_spin(0, frontside ? -180.0f : 180.0f, 0, r.gs_ms);
        set_board_hop(r.gs_hop);
    }
    r.gs_phase = 1;
    r.gs_until = now + r.gs_ms + 60;
    score_trick("groundshuv", frontside ? "FS Ground Shuvit" : "Ground Shuvit");
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
void announce(std::string_view name) {
    if (announce_tricks && !name.empty()) overlay::notify(overlay::NoticeLevel::info, "TRICK", std::string(name));
}
// A mod trick done: its name and points (tricks_view.h) shown, and added to the game's score line when it can be.
void score_trick(std::string_view id, std::string_view name) {
    const int i = trick_index(id);
    const int points = i >= 0 ? trick_catalog[i].points : 0;
    announce(points > 0 && runtime().score_feed ? std::format("{}  +{}", name, points) : std::string(name));
    if (runtime().score_feed) queue_trick_entry(std::string(name), points);
    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: scored {} ({}, {} points).", name, id, points);
}
// The game's grind names (Enum.SkateSlappyGrindType, the values of Enum.GS.SkateGrindType).
std::string_view grind_name(int grind) {
    static constexpr std::string_view names[]{"",          "FS Lipslide", "FS Boardslide", "FS 50-50",    "FS Noseslide", "FS Tailslide", "FS Nosegrind", "FS 5-0",
                                              "FS Salad",  "FS Smith",    "FS Feeble",     "FS Willy",    "FS Overwilly", "FS Noseblunt", "FS Bluntslide",
                                              "FS Crooked", "FS Overcrook", "FS Darkslide", "BS Lipslide", "BS Boardslide", "BS 50-50", "BS Noseslide",
                                              "BS Tailslide", "BS Nosegrind", "BS 5-0", "BS Salad", "BS Smith", "BS Feeble", "BS Willy", "BS Overwilly",
                                              "BS Noseblunt", "BS Bluntslide", "BS Crooked", "BS Overcrook", "BS Darkslide"};
    if (grind >= 0 && grind < static_cast<int>(std::size(names))) return names[grind];
    // 35.. the bigflip ones (BF_), the curb hops: their base name.
    static constexpr std::string_view bf[]{"FS 50-50", "FS Nosegrind", "FS 5-0", "FS Salad", "FS Smith", "FS Feeble", "FS Willy", "FS Overwilly", "FS Crooked", "FS Overcrook",
                                           "BS 50-50", "BS Nosegrind", "BS 5-0", "BS Salad", "BS Smith", "BS Feeble", "BS Willy", "BS Overwilly", "BS Crooked", "BS Overcrook",
                                           "BS Boardslide", "FS Boardslide", "BS Noseslide", "FS Noseslide", "BS Tailslide", "FS Tailslide", "BS Lipslide", "FS Lipslide",
                                           "BS Bluntslide", "FS Bluntslide", "BS Noseblunt", "FS Noseblunt"};
    if (grind >= 35 && grind < 35 + static_cast<int>(std::size(bf))) return bf[grind - 35];
    return {};
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
    if (!r.store_loaded) {
        load_store(r);
        r.view_due = true;
    }
    if (r.view_due) publish_view(r);
    update_trick_entries(base);
    // <dev> `tricks scan auto`: every change of the HUD's line score narrows the search; changes logged throughout.
    if (r.scan_auto && now >= r.scan_next_read) {
        r.scan_next_read = now + 50;
        float line{};
        if (read_hud_line_score(base, line) && line != r.scan_last) {
            r.scan_last = line;
            std::string step;
            if (line > 0 && !scan_busy()) {
                const auto left = scan_left();
                if (left >= 1000000) step = scan_command("new", line);
                else if (left > 8) step = scan_command("next", line);
                else step = scan_command("list", 0);
            }
            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: scan auto: line score {} at {} ms. {}", line, now % 100000, step);
        }
    }
    if (r.save_due && now >= r.next_save) {
        r.next_save = now + 500;
        save_store(r);
    }
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
            set_key_skater(base, 0);
        }
        return;
    }
    if (now >= r.next_skater) {
        try {
            overlay::DebugModel skater;
            (void)client_source::detail::debug_skater(base, client, skater);
            r.entity = skater.skater_position_valid ? skater.skater_identity : 0;
            // Where the rig writes the board's target frame: the procedural board turns that one only.
            const auto bodies = r.entity ? client_source::detail::debug_noclip_bodies(base, client, r.entity) : decltype(client_source::detail::debug_noclip_bodies(base, client, r.entity)){};
            set_board_context(r.entity ? bodies.context : 0);
            r.rig = r.entity ? bodies.rig_wrapper : 0;
            set_key_skater(base, r.entity);
        } catch (...) {
            r.entity = 0;
            r.rig = 0;
            set_board_context(0);
        }
        r.next_skater = now + (r.entity ? 1000 : 250);
    }
    // <dev> The physics state's changes around a pose, for the test runs' logs.
    {
        static int last_state = -1;
        static std::uint64_t pose_seen{};
        const auto physics = watched_physics_state();
        if (r.freestyle != Freestyle::off || r.leaving || r.gs_phase) pose_seen = now;
        if (physics.valid && static_cast<int>(physics.state) != last_state) {
            if (now - pose_seen < 4000 || now - r.pop_stroke_at < 3000)
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: physics {} -> {} ({}, {:.1f} m/s) at {} ms.", last_state, static_cast<int>(physics.state),
                             freestyle_name(r.freestyle), trainer::telemetry().speed, now % 100000);
            std::uint32_t contact{};
            if (const auto stomps = take_pose_stomps(contact))
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: bounce stopped {} times (wheels {}, truck {}, deck {}, body {}, trajectory {}).", stomps, contact & 0xf, contact >> 4 & 1,
                             contact >> 5 & 1, contact >> 6 & 1, contact >> 7 & 1);
            {
                float first{};
                if (const auto writes = take_board_rigid(first))
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: board held rigid for {} steps (the drive's own strength was {:.2f}).", writes, first);
            }
            if (last_state == 103) {
                float lean[6]{};
                if (take_board_lean(lean))
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: board target lean (height of right, up, forward rows): {:.2f}..{:.2f}, {:.2f}..{:.2f}, {:.2f}..{:.2f}.", lean[0], lean[1],
                                 lean[2], lean[3], lean[4], lean[5]);
            }
            {
                float low{}, high{};
                std::uint32_t fixed{};
                if (last_state == 103 && r.rootup >= 1) { // <dev> only while watched or held
                    std::array<std::array<float, 6>, 64> samples{};
                    const auto count = take_freestyle_root_trace(samples);
                    std::string text;
                    for (std::uint32_t i = 0; i < count && i < samples.size(); ++i)
                        text += std::format(" {:.0f}:{:.2f}/{:.2f}/{:+.1f}/{:.2f}/{:.2f}", samples[i][0], samples[i][1], samples[i][2], samples[i][3], samples[i][4], samples[i][5]);
                    if (count) logging::log(logging::Level::info, logging::Channel::skater, "Tricks: skater root trace (ms:root up row/rider parts' highest over the board/board part nearest the root's position/the root's source frame up row/its side row):{}", text);
                }
                if (last_state == 103 && r.riderframe) { // <dev>
                    std::uint32_t writes{}, regame{};
                    const auto rows = take_freestyle_rider_drives(writes, regame);
                    if (!rows.empty() || writes)
                        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: rider drives (mode {}, feet {}): {} row writes, {} set again by the game{}", r.riderframe, r.rider_feet, writes, regame, rows);
                }
                {
                    float lowest{}, mean_tip{};
                    std::uint32_t calls{};
                    if (const auto sourced = take_freestyle_root_source(lowest, mean_tip, calls))
                        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: skater root's source frame set upright {} of {} times (its up row before: lowest {:.4f}, mean tip {:.5f}); second root update called {} times.", sourced, calls, lowest, mean_tip, take_root_update2_calls());
                }
                if (const auto calls = take_freestyle_root_upright(low, high, fixed))
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: skater root seen for {} steps, its up row's height {:.2f}..{:.2f}; set upright {} times.", calls, low, high, fixed);
            }
            if (const auto levelled = take_board_level())
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: board target held level for {} steps.", levelled);
            if (const auto kept = take_pose_kept())
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: bounce: a dismount was refused {} times.", kept);
            if (last_state == 103) {
                float peak{}, swing{};
                take_freestyle_slide_roll(peak, swing);
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: slide rocking: peak {:.2f} rad/s, swing {:.1f} degrees.", peak, swing);
            }
            last_state = static_cast<int>(physics.state);
        }
        // The animation graph's node, from the states its nodes hold true while active.
        static const char *const node_names[]{"Animation/Dingo/Bool.State.Is.TrickInAir", "Animation/Dingo/Bool.State.Is.Landing", "Animation/Dingo/Bool.State.Onboard.Riding"};
        static std::uintptr_t node_states[3]{};
        static int last_nodes = -1;
        if (now - pose_seen < 4000 && state_objects_known()) {
            int nodes = 0;
            for (int i = 0; i < 3; ++i) {
                if (!node_states[i]) node_states[i] = find_state(base, node_names[i]);
                double value{};
                if (node_states[i] && read_state_at(base, node_states[i], 0, value) && value != 0) nodes |= 1 << i;
            }
            if (nodes != last_nodes) {
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: nodes trick {} land {} riding {} (physics {}, {}) at {} ms.", nodes & 1, nodes >> 1 & 1, nodes >> 2 & 1,
                             physics.valid ? static_cast<int>(physics.state) : -1, freestyle_name(r.freestyle), now % 100000);
                last_nodes = nodes;
            }
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
    // The way-in clips' start ticks, written into the two transitions once per level (their objects come with it).
    if (!playing && r.snap_applied) {
        r.snap_applied = false;
        r.snap_from_start = false;
        forget_transitions();
    }
    if (playing && skater_key_known() && state_objects_known() && !r.snap_applied && now >= r.next_snap) {
        r.next_snap = now + 2000;
        const int casper = set_transition_start(base, "animation/dingo/transition_onboard.freestyle.conduit_to_onboard.freestyle.casper.into", r.snap_casper);
        const int primo = set_transition_start(base, "animation/dingo/transition_onboard.freestyle.conduit_to_onboard.freestyle.primo.into", r.snap_primo);
        if (casper >= 0 && primo >= 0) {
            r.snap_applied = true;
            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: way-in clips start at tick {:.0f} (casper) and {:.0f} (primo).", r.snap_casper, r.snap_primo);
        }
    }
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
        {
            std::int16_t lx{};
            std::memcpy(&lx, pad.data() + 4, 2);
            record_sticks(r, now, held_x, held_y, lx / 32767.0f);
            if (click && !r.click_was) r.click_down_at = now;
            r.click_was = click;
        }
        if (r.ground_shuvs && trick_enabled(r, "groundshuv") && !r.hold_scheme && click_pressed && physics.valid && physics.state == 100 && r.freestyle == Freestyle::off && !r.gs_phase && !r.nc_trick &&
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
            // An LT pre-grab's trigger is the hand on the board, not the catch.
            const bool lt_grab = r.pregrab_stage && r.pregrab_hand == 1;
            const bool modifier = click || (pad[2] > 128 && !lt_grab);
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
            if (!(on_ground && now - r.last_pop >= 700) && !lt_grab) DingoSDKOverlayTricksAutoMask();
            float angle = std::atan2(held_y, held_x) * 180.0f / 3.14159265f;
            if (angle < 0) angle += 360.0f;
            Freestyle zone = Freestyle::off;
            bool zone_anti = false;
            if (held > 0.3f) {
                if (angle > r.zone_left && angle < r.zone_right) zone = Freestyle::casper;
                else if (angle >= 150.0f && angle <= r.zone_left) zone = Freestyle::primo;
                else if (angle >= r.zone_right || angle <= 40.0f) zone = Freestyle::primo2;
                else if (r.anti_on) { zone = Freestyle::casper; zone_anti = true; }
            }
            // One primo for both sides: a flick out of primo2 never popped in the runs (its way over to the casper is
            // 50 ticks against the primo's 30), the primo's does.
            if (!r.two_primos && zone == Freestyle::primo2) zone = Freestyle::primo;
            // A pose switched off on the Tricks page has no place on the stick.
            if (zone == Freestyle::casper && !trick_enabled(r, zone_anti ? "anticasper" : "casper")) zone = Freestyle::off, zone_anti = false;
            if ((zone == Freestyle::primo || zone == Freestyle::primo2) && !trick_enabled(r, "primo")) zone = Freestyle::off;
            if (zone != r.zone || zone_anti != r.zone_anti) {
                r.zone = zone;
                r.zone_anti = zone_anti;
                r.zone_since = now;
            }
            const bool steady = zone != Freestyle::off && now - r.zone_since >= (zone_anti ? r.anti_steady_ms : 60) && (!zone_anti || now - r.air_start >= 120);
            const auto pose_name = [](Freestyle f, bool anti) { return anti ? "Anti Casper" : f == Freestyle::casper ? "Casper" : f == Freestyle::primo ? "Primo" : "Primo 2"; };
            // From a manual pushed all the way down: the last 0.4 s of it still counts (the click takes the stick from
            // the game, and the manual lets go).
            bool from_manual = false;
            if (r.manual_entry && trick_enabled(r, "manual_entry") && on_ground && state_objects_known() && r.freestyle == Freestyle::off && !r.leaving) {
                if (!r.s_tail_manual) r.s_tail_manual = find_state(base, "Animation/Dingo/Bool.Anim.InTailManual");
                if (!r.s_nose_manual) r.s_nose_manual = find_state(base, "Animation/Dingo/Bool.Anim.InNoseManual");
                if (!r.s_manual_brake) r.s_manual_brake = find_state(base, "Animation/Dingo/Bool.Anim.ManualBrake");
                double tail{}, nose{}, brake{};
                (void)read_state_at(base, r.s_tail_manual, 0, tail);
                (void)read_state_at(base, r.s_nose_manual, 0, nose);
                (void)read_state_at(base, r.s_manual_brake, 0, brake);
                const int seen = (tail != 0) | (nose != 0) << 1 | (brake != 0) << 2;
                if (seen != r.manual_seen) {
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: manual tail {} nose {} brake {} (stick {:.2f},{:.2f}) at {} ms.", tail != 0, nose != 0, brake != 0, held_x, held_y, now % 100000);
                    r.manual_seen = seen;
                }
                // The brake is a node of its own: the manual's tail / nose state is off while it plays (measured
                // 2026-10-13), so the brake belongs to the manual seen before it.
                if (tail != 0) r.manual_kind = 1;
                else if (nose != 0) r.manual_kind = 2;
                else if (brake == 0) r.manual_kind = 0;
                if (nose != 0) { // the nose manual's intent value, for the anti casper
                    if (!r.s_manual_intent) r.s_manual_intent = find_state(base, "Animation/Dingo/EnumGS.Intent.Manual");
                    double value{};
                    if (r.s_manual_intent && read_state_at(base, r.s_manual_intent, 1, value) && value > 0 && static_cast<int>(value) != r.nose_manual_value) {
                        r.nose_manual_value = static_cast<int>(value);
                        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: the nose manual's intent value is {}.", r.nose_manual_value);
                    }
                }
                if (brake != 0 && r.manual_kind == 1) r.tail_down_at = now;
                if (brake != 0 && r.manual_kind == 2) r.nose_down_at = now;
                if (brake != 0 && !(r.manual_seen_brake)) r.brake_since = now; // a new brake
                r.manual_seen_brake = brake != 0;
                const bool tail_ready = r.tail_down_at && now - r.tail_down_at < 400, nose_ready = r.nose_down_at && now - r.nose_down_at < 400;
                // The click pressed after the brake has held a moment: a click held first, then the stick down, is the
                // ground shuvit (the owner could not get it: the casper from a manual took it, 2026-10-14).
                const bool clicked_after = r.brake_since && r.click_down_at >= r.brake_since + 150;
                from_manual = zone != Freestyle::off && clicked_after && (zone_anti ? nose_ready : tail_ready);
            }
            const bool catching = in_air || (on_ground && now - r.air_last < 350) || from_manual; // the landing's first moment still counts
            if (catching) {
                if (modifier && held > 0.3f) DingoSDKOverlayTricksMask(); // not a grab, a manual or another flip
                if (r.freestyle == Freestyle::off && !r.nc_trick && !r.leaving && modifier && steady && state_objects_known()) {
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: caught in {}{} (stick at {:.0f}){}.", zone_anti ? "anti-" : "", freestyle_name(zone), angle, from_manual && !in_air ? " from a manual" : "");
                    score_trick(zone_anti ? "anticasper" : zone == Freestyle::casper ? "casper" : "primo", pose_name(zone, zone_anti));
                    if (const bool start = from_manual && !in_air; start != r.snap_from_start && r.snap_applied) {
                        (void)set_transition_start(base, "animation/dingo/transition_onboard.freestyle.conduit_to_onboard.freestyle.casper.into", start ? 0.0f : r.snap_casper);
                        (void)set_transition_start(base, "animation/dingo/transition_onboard.freestyle.conduit_to_onboard.freestyle.primo.into", start ? 0.0f : r.snap_primo);
                        r.snap_from_start = start;
                    }
                    r.freestyle = zone;
                    r.anti = zone_anti;
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
                    } else if (steady && (zone != wanted || zone_anti != r.anti)) {
                        // The stick moved on: what it rests on at the landing is the pose.
                        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: catch changed to {}{} (stick at {:.0f}).", zone_anti ? "anti-" : "", freestyle_name(zone), angle);
                        r.freestyle = zone;
                        r.anti = zone_anti;
                        r.after_primo = Freestyle::off;
                        r.shown = Freestyle::off;
                    }
                }
                if (r.after_primo != Freestyle::off) r.after_primo_at = now + 700; // the way in through the primo starts at the landing
            }
            // The ground shuvit, as the owner plays it: the click held, the stick held down at its edge, rolled to a
            // lower diagonal and let go. (Rolled on to the side, it is the game's pop shuvit: not this one.)
            // <dev> Why a ground shuvit gesture is not taken: the gates as they are when the click and the stick are held.
            if (modifier && on_ground && held > 0.8f && !r.gs_logged) {
                r.gs_logged = true;
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: ground shuvit gesture: physics {}, pose {}, leaving {}, phase {}, {} ms since the air, stick at {:.0f}.", physics.valid ? static_cast<int>(physics.state) : -1,
                             static_cast<int>(r.freestyle), r.leaving, r.gs_phase, now - r.air_last, angle);
            } else if (!modifier) r.gs_logged = false;
            // The owner's pick (2026-10-15): a small flick with no click. The stick goes part way down and rolls over to
            // a side, never out to its rim, and is let go: the click + stick down was the casper's, which took it first
            // (physics 103 in all of the owner's tries). A stroke that reaches the rim is the game's pop shuvit
            // (a part-way stroke never popped: run q1).
            if (r.ground_shuvs && trick_enabled(r, "groundshuv") && !modifier && physics.valid && physics.state == 100 && r.freestyle == Freestyle::off && !r.leaving && !r.gs_phase && now - r.air_last > 500) {
                if (held < 0.2f) {
                    if (r.shuv_side != 0 && !r.gs_rim && now - r.shuv_side_at < 350 && state_objects_known()) {
                        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: ground shuvit flick ({}, peak {:.2f}).", r.shuv_side > 0 ? "FS" : "BS", r.gs_peak);
                        start_ground_shuv(r, now, r.shuv_side > 0);
                    }
                    r.shuv_side = 0;
                    r.shuv_down_at = 0;
                    r.gs_rim = false;
                    r.gs_peak = 0;
                } else {
                    r.gs_peak = std::max(r.gs_peak, held);
                    if (held > r.gs_rim_at) r.gs_rim = true; // out to the rim: the game's own trick
                    else if (held >= 0.3f && angle >= 240.0f && angle <= 300.0f) r.shuv_down_at = now;
                    else if (held >= 0.3f && r.shuv_down_at && now - r.shuv_down_at < 500 && ((angle > 300.0f || angle <= 20.0f) || (angle >= 160.0f && angle < 240.0f))) {
                        r.shuv_side = angle > 270.0f || angle <= 20.0f ? 1 : -1;
                        r.shuv_side_at = now;
                    }
                }
            } else {
                r.shuv_side = 0;
                r.shuv_down_at = 0;
                r.gs_rim = false;
                r.gs_peak = 0;
            }
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
                // Level and on one heading from the moment the pose is in (not while it changes to another or leaves).
                set_board_level(r.level && in_pose && !(r.anti && r.anti_nose) && now - r.pose_since >= r.level_delay && now >= r.unhold_until && !r.primo_out_at
                                    ? (current == Freestyle::casper ? 1 : 2)
                                    : 0);
            }
            // No wipeout while a pose is being caught, landed or held: only the stick loses it.
            // The way out of a pose (a flick or the click let go) is guarded until its flip has been in the air and is
            // down again: at speed the game stepped the skater off as soon as the pose's own holds stopped (the owner's
            // session 2026-10-14: 103 -> 504 on most exits at 6 to 7 m/s).
            bool exit_guard = now < r.exit_guard_until;
            if (exit_guard) {
                if (in_air) r.exit_guard_air = true;
                else if (r.exit_guard_air && on_ground) r.exit_guard_until = std::min(r.exit_guard_until, now + 300);
                if (r.by_hold && r.freestyle != Freestyle::off) r.exit_guard_until = 0; // caught again: the pose's own holds
                exit_guard = now < r.exit_guard_until;
            }
            if (r.protect && ((r.by_hold && r.freestyle != Freestyle::off) || exit_guard)) protect_pose(300);
            if (r.stomp && r.by_hold && r.freestyle != Freestyle::off) stomp_pose(300);
            if (exit_guard) keep_pose_on_board(200);
            {
                const bool posed = r.by_hold && r.freestyle != Freestyle::off;
                const bool held_103 = (posed || exit_guard) && physics.valid && physics.state == 103;
                // A flip grab's board follows its path closely (it lagged and swung out on the drive's own strength).
                set_board_rigid(held_103 ? r.rigid : r.flip_grab_on && r.flip_grab_holding ? r.grab_rigid : 0.0f);
                // Through the way out's air too: there the game's own frame follows the board, still upside down when
                // the flip out pops, and the skater swung over head first (owner, 2026-10-14).
                set_freestyle_root_upright(held_103 || exit_guard ? r.rootup : 0);
                set_freestyle_root_hold(r.rig, exit_guard && r.rootup == 2);
                set_freestyle_rider_drives(posed && physics.valid && physics.state == 103 ? r.riderframe : 0, r.rider_feet);
                // <dev> Watched once the pose has been in for a second (its way in turns the board on purpose).
                set_board_lean_watch(posed && physics.valid && physics.state == 103 && r.landed_at && now - r.landed_at > 1000);
                // </dev>
                const bool want = r.no_runout && (posed || exit_guard);
                if (want || r.no_runout_on) {
                    if (!r.s_no_wipeouts) r.s_no_wipeouts = find_state(base, "Animation/Dingo/Bool.Anim.ForceDisableAllWipeouts");
                    (void)write_state_at(base, r.s_no_wipeouts, 0, want ? 1 : 0);
                    r.no_runout_on = want;
                }
            }
            // Not stepped off the board either, unless the player asks for it (Y).
            if (r.keep && r.by_hold && r.freestyle != Freestyle::off && !(buttons & 0x8000)) keep_pose_on_board(200);
            else if (buttons & 0x8000) keep_pose_on_board(0);
            if (r.by_hold && r.freestyle != Freestyle::off) DingoSDKOverlayTricksMask();
            // On the ground in a pose the left stick is hidden as well. The owner's recording: with the click held his
            // left stick wanders over the whole range (x -0.9 to 1.0), and the game steers and leans the skater with it:
            // the rocking from side to side.
            // In the air with a pose caught, too: the same wandering stick turned the skater through the landing, and a
            // casper then came back off the ground once (measured 2026-10-13). `tricks leftmask air off` gives the left
            // stick back in the air (to spin into a pose).
            if (r.left_mask && r.by_hold && r.freestyle != Freestyle::off && (!in_air || r.left_mask_air)) DingoSDKOverlayTricksMaskLeft();
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
                // A primo's way in is only landed from its first ticks (later the board is on its edge in the air, is
                // knocked flat by the touchdown and the game steps the skater off): its start is held back.
                // (Measured 2026-10-13: started at takeoff it is at its tick 31 on touchdown and fails every time; started
                // 0.42 s before the touchdown it fails too; 0.35 s and 0.27 s before, it lands on its wheels and holds.) The time left comes from the game's own
                // Float.Physics.TimeToLand; the time since takeoff stands in when that cannot be read.
                bool primo_due = now - r.air_start >= r.primo_air_delay;
                if (r.air_pose && in_air && r.by_hold && r.freestyle != Freestyle::off && r.freestyle != Freestyle::casper && !r.air_pose_on) {
                    if (!r.s_time_to_land) r.s_time_to_land = find_state(base, "Animation/Dingo/Float.Physics.TimeToLand");
                    std::uint32_t bits{};
                    float left{};
                    if (r.primo_air_left > 0 && peek_state_value(base, r.entity, r.s_time_to_land, bits)) {
                        std::memcpy(&left, &bits, sizeof(left));
                        if (std::isfinite(left) && left > 0.0f && left < 20.0f) {
                            primo_due = left <= r.primo_air_left;
                            if (primo_due) logging::log(logging::Level::info, logging::Channel::skater, "Tricks: primo started in the air with {:.2f} s to land.", left);
                        }
                    }
                }
                const bool in_pose_air = r.air_pose && in_air && r.by_hold && r.freestyle != Freestyle::off && !(r.anti && r.anti_nose) &&
                                         (r.freestyle == Freestyle::casper || r.air_pose_on || primo_due);
                if (in_pose_air || r.air_pose_on) {
                    if (!r.s_filtered) r.s_filtered = find_state(base, "Animation/Dingo/EnumGS.FilteredPhysicsState");
                    // Answered at the lookup (the game's own write of it wins over a write made here), and written too.
                    // A flip trick's clip carries its own landing and is only left at its branch windows (0.5 s in). The
                    // graph's other way down, the landing node, has no window but wants EBool.Anim.Is.GenericLanding, an
                    // expression over the plain in-air states among others: those are held true, so the flip is cut short.
                    for (std::size_t i = 0; i < r.air_names.size(); ++i) {
                        if (i >= r.s_air.size()) r.s_air.push_back(0);
                        if (!r.s_air[i]) r.s_air[i] = find_state(base, r.air_names[i].c_str());
                        const int air_value = i < r.air_values.size() ? r.air_values[i] : 0;
                        (void)write_state_at(base, r.s_air[i], air_value ? 1 : 0, in_pose_air ? (air_value ? air_value : 1) : 0);
                    }
                    if (in_pose_air) {
                        if (!r.hooks) r.hooks = start_holds(base);
                        hold_object(r.s_filtered, 1, 120);
                        (void)write_state_at(base, r.s_filtered, 1, 1);
                        air_poke.base.store(base, std::memory_order_relaxed);
                        air_poke.entity.store(r.entity, std::memory_order_relaxed);
                        air_poke.state.store(r.s_filtered, std::memory_order_relaxed);
                        air_poke.value.store(1, std::memory_order_relaxed);
                        if (r.air_last_fps && !r.s_last_filtered) r.s_last_filtered = find_state(base, "Animation/Dingo/EnumGS.LastFilteredPhysicsState");
                        if (r.air_last_fps && r.s_last_filtered) (void)write_state_at(base, r.s_last_filtered, 1, 2);
                        air_poke.last.store(r.air_last_fps ? r.s_last_filtered : 0, std::memory_order_relaxed);
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
                    r.exit_guard_until = now + r.exit_guard_ms;
                    r.exit_guard_air = false;
                };
                const auto fall = [&](const char *why) {
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: the {} was lost ({}).", freestyle_name(r.freestyle), why);
                    leave(0, 0);
                    r.exit_guard_until = 0;
                    protect_pose(0); // the protection would keep the skater on the board through the hop
                    // The owner's pick: the skater hops off and stands (2026-10-12). The other way is a real slam.
                    if (r.fall_slam) request_wipeout();
                    else r.fall_until = now + 400;
                };
                // An anti-casper is held with the stick up: its rules are the casper's turned upside down (a swipe down
                // from where it is held flicks out, held at the lower edge it is lost).
                const float hy = r.anti ? -held_y : held_y;
                const bool settled = now - r.landed_at > 600; // the landing's first moment is forgiven
                // The stick in its upper half at the edge: a flick if it is brief, a fall if it stays (the owner holds his
                // poses with the stick right at the lower edge, so the edge itself is no fall).
                // A quick swipe to the left or right rim from a casper's place is a flick too (a pop shuvit out); resting
                // there is still the change over to the primo.
                const bool side = casper && held > 0.8f && std::abs(hy) <= 0.3f && now - r.low_last < 160;
                const bool upper = (held > 0.8f && hy > 0.3f) || side;
                if (held > 0.4f && hy < 0.1f) r.low_last = now;
                if (r.primo_out_at) {
                    if (now >= r.primo_out_at) {
                        r.out_until = now + 1500;
                        leave(9, 2500);
                    }
                } else if (upper && !r.swiped && !r.edge_since && settled && now - r.low_last < 160) {
                    // Swiped up from where the pose is held: a flick, taken the moment the stick is up.
                    r.edge_x = held_x;
                    r.edge_y = hy;
                    r.edge_last = now;
                    r.edge_brief = true;
                    r.swiped = true;
                } else if (upper && !r.swiped) {
                    if (!r.edge_since) r.edge_since = now;
                    r.edge_last = now;
                    r.edge_x = held_x;
                    r.edge_y = hy;
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
                        r.out_pose = true;
                        r.out_dir = 0;
                        r.out_legs_flip = 0;
                        const float bigger = r.flickout_flip ? 360.0f : 0.0f;
                        const char *id = "flick_treflip";
                        if (flick >= 150.0f || flick <= -150.0f) { r.out_yaw = 180; r.out_legs_flip = 4; turn = "Pop Shuvit"; id = "flick_shuvit"; }
                        else if (flick <= 30.0f) { r.out_yaw = -180; r.out_legs_flip = 7; turn = "FS Pop Shuvit"; id = "flick_fsshuvit"; }
                        else if (flick >= 100.0f) { r.out_dir = 1; r.out_roll = bigger; r.out_legs_flip = 2; turn = "Kickflip"; id = "flick_kickflip"; }
                        else if (flick < 80.0f) { r.out_dir = -1; r.out_roll = -bigger; r.out_legs_flip = 3; turn = "Heelflip"; id = "flick_heelflip"; }
                        else if (r.flickout_up == 2) { turn = "Half Flip"; } // straight back over, no shuv
                        else { r.out_dir = 1; r.out_roll = r.flickout_up == 1 ? 360.0f : 0.0f; r.out_yaw = 360; r.out_legs_flip = 11; }
                        r.edge_brief = false;
                        // A flick-out switched off on the Tricks page: the swipe does nothing and the pose stays.
                        if (!trick_enabled(r, id)) {
                            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: {} out is switched off.", turn);
                        } else {
                        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: flicked out of the {} ({}).", freestyle_name(r.freestyle), turn);
                        // Named as the flip it is (the owner, 2026-10-14: "Kickflip", not "Casper Kickflip").
                        const bool follow = !casper && r.primo_hop_flip && !(r.anti && r.anti_nose);
                        if (!follow) score_trick(id, turn); // a primo's is named once its way round is known
                        r.out_follow = follow;
                        r.out_kind = flick >= 100.0f ? 0 : flick < 80.0f ? 1 : 2;
                        if (casper && !(r.anti && r.anti_nose)) {
                            r.out_until = now + 1500;
                            leave(r.casper_exit, 2500);
                        } else if (r.primo_hop_flip && !(r.anti && r.anti_nose)) {
                            r.out_until = now + 1500;
                            r.out_air_best = 0;
                            leave(0, r.primo_pop_hold_ms);
                        } else if (r.primo_game_pop || (r.anti && r.anti_nose)) {
                            // The primo has no pop of its own. It puts the board down (its own way out) and, the moment the
                            // skater rides, the game is asked for an ollie with this flip trick: the game's own flip, which
                            // the catch can take into a pose again (the owner, 2026-10-14: not through the casper).
                            r.pop_flip = id == std::string_view("flick_kickflip") ? 2u : id == std::string_view("flick_heelflip") ? 3u : id == std::string_view("flick_shuvit") ? 4u : id == std::string_view("flick_fsshuvit") ? 7u : 11u;
                            r.pop_until = now + 3500;
                            r.pop_wait_riding = true;
                            r.pop_started = 0;
                            r.pop_jump = 0.2f;
                            r.pop_speed = 1.0f;
                            r.out_roll = r.out_yaw = 0; // the game's own flip: no turn of ours
                            r.out_pose = false;
                            r.out_until = 0;
                            leave(0, 1500);
                        } else {
                            // Through the casper, whose flip out carries the turn (the older way: `tricks primopop off`).
                            r.freestyle = Freestyle::casper;
                            r.after_primo = Freestyle::off;
                            r.primo_out_at = now + r.primo_out_ms;
                        }
                        }
                    } else if (!modifier) {
                        if (!r.centre_since) r.centre_since = now;
                        if (now - r.centre_since >= 250) {
                            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: let go of the {}.", freestyle_name(r.freestyle));
                            if (casper) { score_trick("casperflip", r.anti ? "Anti Casper Flip" : "Casper Flip"); leave(9, 2500); }
                            else leave(0, 1500);
                        }
                    } else if (held < 0.2f) {
                        if (!r.centre_since) r.centre_since = now;
                        if (now - r.centre_since >= 250 && settled) {
                            if (casper && r.centre_fall) fall("stick back at the middle");
                            else {
                                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: the {} was put down.", freestyle_name(r.freestyle));
                                if (casper) { score_trick("casperflip", r.anti ? "Anti Casper Flip" : "Casper Flip"); leave(9, 2500); }
                                else leave(0, 1500);
                            }
                        }
                    } else {
                        r.centre_since = 0;
                        // The stick moved to another pose's place and rests there: over to that pose.
                        const Freestyle current = r.after_primo != Freestyle::off ? r.after_primo : r.freestyle;
                        if (settled && zone != Freestyle::off && (zone != current || zone_anti != r.anti) && now - r.zone_since >= 250) {
                            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: {}{} to {}{}.", r.anti ? "anti-" : "", freestyle_name(current), zone_anti ? "anti-" : "", freestyle_name(zone));
                            score_trick(zone_anti ? "anticasper" : zone == Freestyle::casper ? "casper" : "primo", pose_name(zone, zone_anti));
                            r.freestyle = zone;
                            r.anti = zone_anti;
                            r.after_primo = Freestyle::off;
                            r.unhold_until = now + 900;
                        }
                    }
                }
            }
            // The flick's turn, once the casper's flip out is in the air; dropped again on the ground.
            // A primo's flip out waits for the hop's real air (physics 200/201): the way out's clip lifts the board a
            // moment before it, and a turn started there touched down 125 ms in (run n9).
            const auto out_physics = watched_physics_state();
            // And only once the game predicts the air long enough for the turn: a bounce before the hop (201 for under
            // 100 ms) started it, and it touched down 93 and 203 ms into its 430 ms turn (run pf, 2 of 4).
            const auto hop_air_ok = [&] {
                if (!r.out_follow) return true;
                if (!r.s_time_to_land) r.s_time_to_land = find_state(base, "Animation/Dingo/Float.Physics.TimeToLand");
                std::uint32_t bits{};
                float left{};
                if (!r.s_time_to_land || !peek_state_value(base, r.entity, r.s_time_to_land, bits)) return true;
                std::memcpy(&left, &bits, sizeof(left));
                if (std::isfinite(left) && left > r.out_air_best) r.out_air_best = left;
                return !std::isfinite(left) || left >= r.primo_air_need;
            };
            if (r.out_until && in_air && (!r.out_follow || (out_physics.valid && out_physics.state >= 200 && out_physics.state < 300)) && hop_air_ok()) {
                r.out_from_primo = r.out_follow;
                if (r.out_follow) {
                    r.out_follow = false;
                    float b[12]{};
                    const float back = r.rig && copy_rows(r.rig + 0x48a0, b, 12) ? std::atan2(-b[1], b[5]) * 57.2958f : 0.0f;
                    const bool kick = back >= 0.0f; // the roll's positive way is the kickflip's (as the casper's flip outs)
                    r.out_dir = kick ? 1 : -1;
                    r.out_roll = kick ? 360.0f : -360.0f;
                    const char *name{}, *id{};
                    if (r.out_kind == 1) {
                        r.out_yaw = kick ? 180.0f : -180.0f;
                        name = kick ? "Varial Kickflip" : "Varial Heelflip";
                        id = kick ? "flick_kickflip" : "flick_heelflip";
                        r.out_legs_flip = kick ? 2 : 3;
                    } else if (r.out_kind == 2) {
                        r.out_yaw = kick ? 360.0f : -360.0f;
                        name = kick ? "Tre Flip" : "Laser Flip";
                        id = "flick_treflip";
                        r.out_legs_flip = kick ? 11 : 3;
                    } else {
                        r.out_yaw = 0;
                        name = kick ? "Kickflip" : "Heelflip";
                        id = kick ? "flick_kickflip" : "flick_heelflip";
                        r.out_legs_flip = kick ? 2 : 3;
                    }
                    score_trick(id, name);
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: primo flip out the way it rolls out: {} (roll back {:.0f} deg at the hop, {:.2f} s of air foreseen).", name, back, r.out_air_best);
                }
                if (r.out_pose) {
                    // Timed by the whole turn (about 180 for the roll back over), so a half flip is not slowed to a
                    // big flip's time nor a big one rushed (the owner: tre from a casper "does way too much").
                    const float turn = std::max(180.0f + std::abs(r.out_roll), std::abs(r.out_yaw));
                    // Done before the touchdown (the casper's hop is short: a landing mid-turn finished the rest in 90 ms
                    // and threw the skater, the owner 2026-10-15), with no dip (the hop is a few centimetres off the
                    // ground: a dipped board went into it, "fall through the map").
                    auto ms = static_cast<std::uint32_t>((180.0f + 0.5f * turn) * static_cast<float>(r.out_spin_ms) / 320.0f);
                    // A primo's hop is down 560-700 ms after it starts (runs n7, n9): its turn ends well before. The game's
                    // hop is not always the same (0.50 s of air foreseen, or 0.27-0.39 s: runs ph, pk), so the turn takes
                    // 85% of the air the game foresees.
                    if (r.out_from_primo) {
                        ms = std::min<std::uint32_t>(ms, r.primo_flip_ms);
                        if (r.out_air_best > 0) ms = std::min<std::uint32_t>(ms, static_cast<std::uint32_t>(std::max(200.0f, r.out_air_best * 850.0f)));
                    }
                    start_air_spin(r, r.out_roll, r.out_yaw, 0, ms, true, false, r.out_dir);
                    r.out_spinning = true;
                    r.out_spin_at = now;
                    r.out_spin_len = ms;
                    if (r.out_legs && r.out_legs_flip) {
                        if (!r.s_current_flip) r.s_current_flip = find_state(base, "Animation/Dingo/EnumGS.Anim.CurrentFlipTrick");
                        (void)write_state_at(base, r.s_current_flip, 1, r.out_legs_flip);
                        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: flick-out legs: the game's flip {} over {} ms of board play.", r.out_legs_flip, ms);
                    }
                } else if (r.out_roll != 0 || r.out_yaw != 0) {
                    start_air_spin(r, r.out_roll, r.out_yaw, 0, r.out_spin_ms);
                    r.out_spinning = true;
                }
                r.out_until = 0;
            } else if (r.out_until && now >= r.out_until) {
                if (r.out_follow)
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: primo flip out never started: the most air the game foresaw was {:.2f} s (needs {:.2f}).", r.out_air_best, r.primo_air_need);
                r.out_until = 0;
            }
            if (r.out_spinning && on_ground) {
                // <dev> How far the turn was at the touchdown (wall clock: a guide).
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: flick-out down {} ms into its {} ms turn (still turning: {}; exit clip {}).", now - r.out_spin_at, r.out_spin_len,
                             kinematic_spin_running(), r.casper_exit);
                stop_board_spin();
                r.out_spinning = false;
            }
            // A flip out has been in the air and is down again: the exit is over, so the next trick (and the next
            // catch) can start now, not when the exit's own time runs out.
            if (r.leaving && r.exit_flip && r.freestyle == Freestyle::off) {
                if (in_air) r.exit_air = true;
                else if (r.exit_air && on_ground) {
                    r.exit_air = false;
                    if (r.leaving_until > now + 150) r.leaving_until = now + 150;
                }
            } else r.exit_air = false;
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
        // The primo's flick-out: the game's ollie with the flip trick, asked for once the skater rides again.
        if (r.pop_flip) {
            if (now >= r.pop_until || (physics.valid && physics.state >= 300)) {
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: primo flick-out {} gave up (physics {}).", r.pop_flip, physics.valid ? static_cast<int>(physics.state) : -1);
                r.pop_flip = 0;
            } else if (physics.valid && physics.state == 100 && !r.pop_started && now >= r.pop_after && [&] {
                           if (!r.pop_wait_riding) return true;
                           // The game reads a flick only once its graph is in the riding node: the primo's way out (a
                           // ground clip, physics 103 and 100 by turns) took every stroke played before it (run n2).
                           if (!r.s_riding_node) r.s_riding_node = find_state(base, "Animation/Dingo/Bool.State.Onboard.Riding");
                           double riding{};
                           return r.s_riding_node && read_state_at(base, r.s_riding_node, 0, riding) && riding != 0;
                       }()) {
                r.pop_started = now;
                play_pop_stroke(r, r.pop_flip, r.pop_jump);
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: stroke played for flip {} (strength {:.2f}).", r.pop_flip, r.pop_jump);
            }
            if (in_air && r.pop_started) {
                // The game's answer puts the stroke's sides right for the next one (kickflip and heelflip, the shuvits).
                if (!r.s_current_flip) r.s_current_flip = find_state(base, "Animation/Dingo/EnumGS.Anim.CurrentFlipTrick");
                double got{};
                (void)read_state_at(base, r.s_current_flip, 1, got);
                const auto answer = static_cast<std::uint32_t>(got);
                if ((r.pop_flip == 2 && answer == 3) || (r.pop_flip == 3 && answer == 2)) r.kick_side = -r.kick_side;
                if ((r.pop_flip == 4 && answer == 7) || (r.pop_flip == 7 && answer == 4)) r.bs_shuv_side = -r.bs_shuv_side;
                if (answer != r.pop_flip) {
                    // The owner's first backside ground shuvit of a session played flip 5 (2026-10-15; three fresh starts
                    // played 4 headlessly, 2026-10-16): what the player's own sticks and stance were, for the next log.
                    if (!r.s_riding_fakie) r.s_riding_fakie = find_state(base, "Animation/Dingo/Bool.Physics.RidingFakie");
                    double fakie{};
                    (void)read_state_at(base, r.s_riding_fakie, 0, fakie);
                    std::string seen;
                    for (const auto &s : recent_sticks(r, now, 700)) seen += std::format(" {}:({:.1f},{:.1f})", now - s.at, s.rx, s.ry);
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: mod pop answered otherwise: asked {}, got {}; sides kick {:+.0f} shuv {:+.0f}, fakie {}, physics {}, the player's right stick over 700 ms:{}",
                                 r.pop_flip, answer, r.kick_side, r.bs_shuv_side, fakie != 0, physics.valid ? static_cast<int>(physics.state) : -1, seen);
                }
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: mod pop: asked flip {}, the game played {} ({} ms after the stroke, {} ms after the ask).", r.pop_flip, answer, now - r.pop_started, now + (r.pop_flip == 4 || r.pop_flip == 7 ? 600 : 3500) - r.pop_until);
                r.pop_flip = 0; // the game's own trick from here: the catch is as after any flip
                r.leaving_until = std::min(r.leaving_until, now);
            }
        }
        // Grab during a flip (the owner, 2026-10-14: "a clean kickflip animation that goes up to like waist where you grab
        // it and catch it with your grabbing hand and bring it back to your feet in the grab position"). A trigger in a
        // flip's air: the board rises to the waist and comes back down to the feet, the hand of that trigger meets it at
        // the top and stays on it to the end (RT the back hand on the toe edge, LT the front hand on the heel edge).
        if (r.flip_grab || r.flip_grab_on) {
            const bool left = pad[2] > 100, right = pad[3] > 100;
            std::uintptr_t core{};
            if (r.rig) (void)copy_rows(r.rig + 0x4630, reinterpret_cast<float *>(&core), 2);
            // Not from a pre-grab: its trigger is the hand already on the board (a nollie from it played flip 32 and turned
            // into a flip grab, run pr).
            if (r.flip_grab && trick_enabled(r, "flipgrab") && in_air && (left || right) && !r.flip_grab_on && core && !r.pregrab_stage) {
                if (!r.s_current_flip) r.s_current_flip = find_state(base, "Animation/Dingo/EnumGS.Anim.CurrentFlipTrick");
                double flip{};
                // A flip, not the game's ollie (1) or nollie (16): those with a trigger are the game's own grab.
                if (read_state_at(base, r.s_current_flip, 1, flip) && flip != 0 && flip != 1 && flip != 16) {
                    r.flip_grab_on = true;
                    r.flip_grab_until = now + r.flip_grab_ms;
                    r.flip_grab_started = now;
                    r.flip_grab_holding = true;
                    r.grab_hand_back = right;
                    // The flip is the mod's from here (the clip's board would turn it twice), played on the game's own board
                    // for its grab (the drive's target): the game's grab clip brings the hand to that board, so the hand
                    // and the board meet where the game's grab has them, and nothing has to be put back under the feet
                    // (the owner, 2026-10-15, on a path of the mod's: "the board goes through the legs and the hand does
                    // not grab it and put it back"; the mod's writes never moved the hand). The flip turns while the
                    // hand comes in, dipped under the tucked feet, and is done when the hand has it.
                    const int f = static_cast<int>(flip);
                    const float roll = f == 3 ? -360.0f : f == 4 || f == 7 ? 0.0f : 360.0f, yaw = f == 4 ? 180.0f : f == 7 ? -180.0f : f == 11 ? 360.0f : 0.0f;
                    // Out and up in front (the owner, 2026-10-15: under the feet it "didn't flip"), turned by the top, and
                    // back down on to the game's grab board, where its hand is.
                    start_board_spin_kinematic(core, roll, yaw, 0, r.flip_grab_spin_ms, 0.5f, 0.0f);
                    set_kinematic_spin_follow(true);
                    set_kinematic_spin_lift(r.flip_grab_rise, r.flip_grab_reach);
                    if (!r.s_flip_intent) r.s_flip_intent = find_state(base, "Animation/Dingo/EnumGS.Intent.FlipTrick");
                    (void)write_state_at(base, r.s_flip_intent, 1, 0);
                    // The game's flip ended too, so its grab can start (with the flip still current the hand never came:
                    // stills r10).
                    if (r.flip_grab_endflip) (void)write_state_at(base, r.s_current_flip, 1, 0);
                    set_feet_free(core, r.flip_grab_spin_ms + 60, r.feet_free);
                    // The vert tuck: the pelvis drive lowered toward the board, the knees come up (the arms stay the
                    // game's grab).
                    set_limb_free(core, r.flip_grab_ms, 0, 1.0f, r.grab_tuck);
                    score_trick(right ? "flipgrab" : "flipgrab", std::format("{} {}", flip_name(static_cast<std::uint32_t>(flip)), right ? "Indy" : "Melon"));
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: flip grab (flip {}, {} hand).", flip, right ? "back" : "front");
                }
            }
            if (r.flip_grab_on) {
                // The tuck is held while the trigger is (eased out over 0.15 s when let go); the board is the game's grab
                // once the flip has turned.
                const bool holding = r.grab_hand_back ? right : left;
                r.flip_grab_holding = holding;
                // The board flown away from the rider (the owner, 2026-10-16: "can glitch out and launch you across the
                // map"; the log had the deck 1.7 m, then 11 m from the pelvis, held to the drive's target at 40x the game's
                // own strength): let go at once, the flip ended and the board the game's again.
                if (in_air && holding && r.hand_ik && now - r.flip_grab_started > 80) {
                    float deck_up{}, deck_out{};
                    hand_ik_deck(deck_up, deck_out);
                    if (std::hypot(deck_up, deck_out) > r.flip_grab_leash) {
                        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: flip grab let go: the board {:.2f} m up, {:.2f} m out from the pelvis at {} ms.", deck_up, deck_out, now - r.flip_grab_started);
                        r.flip_grab_on = false;
                        r.flip_grab_holding = false;
                        stop_board_spin();
                    }
                }
                if (in_air && holding && core) set_limb_free(core, 80, 0, 1.0f, r.grab_tuck);
                // The hand on the board's toe edge (the back hand) or heel edge (the front) from the catch on, through
                // the game's own hand IK: it follows the board down to the feet.
                if (r.hand_ik && in_air && holding && core)
                    hold_hand_ik(r, core, now, r.grab_hand_back);
                // The game's grab gate held open (it is shut through a flip: the trigger never brought its grab, and with
                // the gate open the game's own grab plays, its hand to the board: run r13).
                if (in_air && holding) {
                    if (!r.s_can_grab) r.s_can_grab = find_state(base, "Animation/Dingo/Bool.GP.CanGrab");
                    (void)write_state_at(base, r.s_can_grab, 0, 1);
                }
                if (!in_air) {
                    float move{}, turn{};
                    take_kinematic_spin_smoothness(move, turn);
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: flip grab landed {} ms after the catch (the flip {}; largest step {:.3f} m past its travel, {:.1f} deg; {}).", now - r.flip_grab_started,
                                 kinematic_spin_running() ? "still turning" : "done", move, turn, take_kinematic_spin_guard());
                    r.flip_grab_on = false;
                    stop_board_spin();
                }
            }
        }
        // The pre-grab: the nose or tail held on the ground, popped with the hand on, or thrown in a flip.
        if (r.pregrab || r.pregrab_stage) {
            const bool lt = pad[2] > 100, rt = pad[3] > 100;
            std::int16_t raw_ly{};
            std::memcpy(&raw_ly, pad.data() + 6, 2);
            const float ly = raw_ly / 32767.0f;
            std::uintptr_t core{};
            if (r.rig) (void)copy_rows(r.rig + 0x4630, reinterpret_cast<float *>(&core), 2);
            const auto put = [&](std::uintptr_t &at, const char *name, int kind, double value) {
                if (!at) at = find_state(base, name);
                (void)write_state_at(base, at, kind, value);
            };
            // The game's own ground grab, as a stock one sets it (the grind grab's states), with the trigger's hand.
            const auto ground_grab = [&](bool on) {
                put(r.s_can_ground_grab, "Animation/Dingo/Bool.GP.CanGroundGrab", 0, on);
                put(r.s_is_ground_grab, "Animation/Dingo/Bool.State.Is.GroundGrab", 0, on);
                put(r.s_is_grabbing, "Animation/Dingo/Bool.Anim.IsGrabbing", 0, on);
                put(r.s_grab_type, "Animation/Dingo/EnumGS.GrabType", 1, on ? (r.pregrab_ground_type >= 0 ? r.pregrab_ground_type : r.pregrab_hand ? 1 : 2) : 0);
                put(r.s_grab_left_hand, "Animation/Dingo/Bool.GP.Grab.LeftHand", 0, on && r.pregrab_hand == 1);
                put(r.s_grab_right_hand, "Animation/Dingo/Bool.GP.Grab.RightHand", 0, on && r.pregrab_hand == 0);
                put(r.s_ground_grab_loop, "Animation/Dingo/Bool.Anim.IsGroundGrabLoop", 0, on && now - r.pregrab_since > 600);
            };
            const auto reach = [&](std::uint32_t ms) {
                if (!r.pregrab_reach || !core) return;
                std::string detail;
                if (!set_hand_reach(base, core, ms, r.pregrab_hand, r.pregrab_end, r.pregrab_over, r.pregrab_up, detail) && detail != r.pregrab_error) {
                    r.pregrab_error = detail;
                    logging::log(logging::Level::warning, logging::Channel::skater, "Tricks: the pre-grab's hand pose is unavailable: {}", detail);
                }
            };
            const auto end_name = [&] { return r.pregrab_end >= 0 ? "Nose" : "Tail"; };
            const auto stop = [&](const char *why) {
                if (r.pregrab_stage == 1) ground_grab(false);
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: pre-grab off ({}; stage {}; {}).", why, r.pregrab_stage, take_hand_reach());
                r.pregrab_stage = 0;
                r.pregrab_flick = -1;
                r.pregrab_flick_stage = 0;
                reach(0);
            };
            const bool holding = r.pregrab_hand ? lt : rt;
            if (!r.pregrab_stage) {
                // In: the trigger with the left stick pushed up or down for a moment, riding.
                const bool asking = r.pregrab && trick_enabled(r, "pregrab") && on_ground && (lt || rt) && std::abs(ly) > 0.5f && r.freestyle == Freestyle::off &&
                                    !r.pop_flip && state_objects_known();
                if (!asking) r.pregrab_stick_since = 0;
                else if (!r.pregrab_stick_since) r.pregrab_stick_since = now;
                else if (now - r.pregrab_stick_since >= r.pregrab_entry_ms) {
                    r.pregrab_stage = 1;
                    r.pregrab_since = now;
                    r.pregrab_hand = rt ? 0 : 1;
                    // Up is the end ahead (the nose, or the tail riding fakie).
                    if (!r.s_riding_fakie) r.s_riding_fakie = find_state(base, "Animation/Dingo/Bool.Physics.RidingFakie");
                    double fakie{};
                    (void)read_state_at(base, r.s_riding_fakie, 0, fakie);
                    r.pregrab_end = (ly > 0) != (fakie != 0) ? 1.0f : -1.0f;
                    r.pregrab_flick = -1;
                    r.pregrab_flick_stage = 0;
                    r.pregrab_stick_since = 0;
                    score_trick("pregrab", std::format("{} Grab", end_name()));
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: pre-grab {} {} (left stick {:.2f}, fakie {}).", r.pregrab_end >= 0 ? "nose" : "tail", r.pregrab_hand ? "LT" : "RT", ly, fakie != 0);
                }
            } else if (r.pregrab_stage == 1) {
                if (!holding) stop("let go");
                else if (!on_ground) stop("not riding");
                else {
                    ground_grab(true);
                    reach(120);
                    if (core && r.pregrab_crouch > 0) set_limb_free(core, 120, 0, 1.0f, r.pregrab_crouch);
                    // The flick is the mod's: the game's own reading of it would pop out of the grab into its own trick.
                    DingoSDKOverlayTricksMaskRight();
                    // Down then up an ollie, up then down a nollie; down then a diagonal a kickflip or heelflip, down then
                    // to a side a shuvit (the stroke's sides, as the game answered them).
                    const float x = held_x, y = held_y;
                    if (r.pregrab_flick_stage && now - r.pregrab_flick_at > 400) r.pregrab_flick_stage = 0;
                    int flick = -1;
                    if (!r.pregrab_flick_stage) {
                        if (y < -0.6f) r.pregrab_flick_stage = 1, r.pregrab_flick_at = now;
                        else if (y > 0.6f && std::abs(x) < 0.4f) r.pregrab_flick_stage = 2, r.pregrab_flick_at = now;
                    } else if (r.pregrab_flick_stage == 1) {
                        if (y > 0.6f && std::abs(x) < 0.4f) flick = 1;
                        else if (y > 0.3f && std::abs(x) >= 0.4f) flick = (x > 0) == (r.kick_side > 0) ? 2 : 3;
                        else if (std::abs(x) > 0.8f && std::abs(y) < 0.5f) flick = (x > 0) == (r.bs_shuv_side > 0) ? 4 : 7;
                    } else if (r.pregrab_flick_stage == 2 && y < -0.6f && std::abs(x) < 0.4f) flick = 16;
                    const bool flip = flick == 2 || flick == 3 || flick == 4 || flick == 7;
                    if (flick > 0 && (!flip || trick_enabled(r, "fingerflip")) && trick_enabled(r, "pregrab_ollie")) {
                        // The game's ground grab let go, the trigger hidden from it until the board is up (a held ground
                        // grab never pops), then the game's own small ollie or nollie through the stroke.
                        ground_grab(false);
                        r.pregrab_stage = 2;
                        r.pregrab_flick = flick;
                        r.pregrab_scored = r.pregrab_thrown = false;
                        r.pop_flip = flick == 16 ? 16u : 1u;
                        r.pop_after = now + r.pregrab_gap;
                        r.pop_until = now + r.pregrab_gap + 1500;
                        r.pop_wait_riding = false;
                        r.pop_started = 0;
                        r.pop_jump = r.pregrab_pop;
                        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: pre-grab pop: flick {} from the {} ({} ms held).", flick, end_name(), now - r.pregrab_since);
                    }
                }
            } else if (r.pregrab_stage == 2) {
                const bool flip = r.pregrab_flick != 1 && r.pregrab_flick != 16;
                DingoSDKOverlayTricksMaskRight();
                DingoSDKOverlayTricksMaskTriggers();
                reach(120);
                if (r.pregrab_jump >= 0) put(r.s_jump_strength, "Animation/Dingo/Float.Intent.JumpStrength", 2, r.pregrab_jump);
                if (physics.valid && physics.state >= 300) stop("off the board");
                else if (in_air) {
                    r.pregrab_stage = 3;
                    r.pregrab_air_at = now;
                    if (!flip) {
                        score_trick("pregrab_ollie", std::format("{} Grab {}", end_name(), r.pregrab_flick == 16 ? "Nollie" : "Ollie"));
                        r.pregrab_scored = true;
                    }
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: pre-grab in the air ({} ms after the flick).", now - r.pregrab_flick_at);
                } else if (!r.pop_flip && now > r.pop_until) stop("no pop");
            } else if (r.pregrab_stage == 3) {
                const bool flip = r.pregrab_flick != 1 && r.pregrab_flick != 16;
                DingoSDKOverlayTricksMaskRight();
                if (physics.valid && physics.state >= 300) stop("off the board");
                else if (physics.valid && physics.state >= 400 && physics.state <= 406) stop("on a rail"); // the grind grab takes it from here
                else if (in_air && !flip) {
                    // The hand on: the game's own air grab (the trigger is the game's again), written as the nose or tail grab.
                    reach(120);
                    if (r.pregrab_write_grab && holding) {
                        if (!r.s_detailed_grab) r.s_detailed_grab = find_state(base, "Animation/Dingo/Enum.GS.DetailedGrabType");
                        (void)write_state_at(base, r.s_detailed_grab, 1, r.pregrab_end >= 0 ? r.pregrab_nose_grab : r.pregrab_tail_grab);
                    }
                } else if (in_air && flip) {
                    // Thrown: the trigger kept from the game (no grab of its own over the flip).
                    DingoSDKOverlayTricksMaskTriggers();
                    const int k = r.pregrab_flick == 2 ? 0 : r.pregrab_flick == 3 ? 1 : r.pregrab_flick == 4 ? 2 : 3;
                    if (!r.pregrab_thrown && now - r.pregrab_air_at >= r.ff_delay) {
                        r.pregrab_thrown = true;
                        const char *names[]{"Kickflip", "Heelflip", "Shuvit", "FS Shuvit"};
                        if (r.ff_type[k] >= 0) {
                            put(r.s_can_grab, "Animation/Dingo/Bool.GP.CanGrab", 0, 1);
                            put(r.s_ff_intent, "Animation/Dingo/Bool.Intent.Fingerflip", 0, 1);
                            put(r.s_ff_type, "Animation/Dingo/Enum.GS.Intent.FliptrickTypeForFingerflip", 1, r.ff_type[k]);
                            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: fingerflip type {} (states {:x} {:x}).", r.ff_type[k], r.s_ff_intent, r.s_ff_type);
                        } else {
                            // The mod's throw: the hand turns the board off the nose or tail and lets go a third of the way in;
                            // the board ends under the feet.
                            const float roll = k == 0 ? 360.0f * r.kick_side : k == 1 ? -360.0f * r.kick_side : 0.0f;
                            const float yaw = k == 2 ? 180.0f : k == 3 ? -180.0f : 0.0f;
                            start_air_spin(r, roll, yaw, 0, r.ff_ms, false, true, -2);
                            set_kinematic_spin_to_feet(true);
                            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: pre-grab throw: {} off the {} ({} ms).", names[k], end_name(), r.ff_ms);
                        }
                        score_trick("fingerflip", std::format("{} Finger {}", end_name(), names[k]));
                        r.pregrab_scored = true;
                    }
                    if (!r.pregrab_thrown || now - r.pregrab_air_at < r.ff_delay + r.ff_ms * 3 / 10) reach(60);
                    if (r.ff_type[k] >= 0 && r.pregrab_thrown && now - r.pregrab_air_at < r.ff_delay + 200) {
                        put(r.s_can_grab, "Animation/Dingo/Bool.GP.CanGrab", 0, 1);
                        put(r.s_ff_intent, "Animation/Dingo/Bool.Intent.Fingerflip", 0, 1);
                        put(r.s_ff_type, "Animation/Dingo/Enum.GS.Intent.FliptrickTypeForFingerflip", 1, r.ff_type[k]);
                    }
                } else if (!in_air && physics.valid && physics.state >= 100 && physics.state <= 105) {
                    // Down: still holding, back into the pre-grab at once; let go, riding.
                    float move{}, turn{};
                    if (flip) take_kinematic_spin_smoothness(move, turn);
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: pre-grab landed {} ms after the pop (grab type {} last seen; {}{}).", now - r.pregrab_air_at, r.air_grab_type,
                                 take_hand_reach(), flip ? std::format("; throw's largest step {:.3f} m / {:.1f} deg", move, turn) : std::string());
                    if (holding && !flip) {
                        r.pregrab_stage = 1;
                        r.pregrab_since = now;
                        r.pregrab_flick = -1;
                        r.pregrab_flick_stage = 0;
                    } else stop("landed");
                }
            }
            if (r.pregrab_stage && now >= r.pregrab_log) {
                r.pregrab_log = now + 250;
                double grab{};
                if (!r.s_detailed_grab) r.s_detailed_grab = find_state(base, "Animation/Dingo/Enum.GS.DetailedGrabType");
                (void)read_state_at(base, r.s_detailed_grab, 1, grab);
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: pre-grab stage {}, physics {}, grab type {}: {}.", r.pregrab_stage, physics.valid ? static_cast<int>(physics.state) : -1, grab, take_hand_reach());
            }
        }
        // <dev> A console-held hand IK target: the hands' distances to it and the context, every 100 ms.
        if (r.hand_ik_watch_until && now - r.hand_ik_logged >= 100) {
            r.hand_ik_logged = now;
            float back{}, front{};
            const auto steps = take_hand_ik(back, front);
            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: hand ik hold: back hand {:.3f} m, front hand {:.3f} m ({} steps, physics {}); {}", back, front, steps,
                         physics.valid ? static_cast<int>(physics.state) : -1, dump_hand_ik(base, r.entity));
            if (now >= r.hand_ik_watch_until) r.hand_ik_watch_until = 0;
        }
        // <dev> Grab while grinding (experiment).
        if (r.grind_grab || r.grind_grab_on) {
            const bool grinding = physics.valid && physics.state >= 400 && physics.state <= 406;
            const bool left = pad[2] > 100, right = pad[3] > 100; // LT, RT
            // Not when the rail was met in a tweaked grab: that is a fall (below), not a grab grind.
            const bool doomed = !r.grind_grab_on && r.tweak_fall &&
                                ((now - r.air_tweak_seen < 200 && !pregrab_plain(r, now)) ||
                                 (now - r.air_grab_seen < 200 && r.air_grab_type > 0 && !plain_grab(r, now, r.air_grab_type)));
            const bool want = r.grind_grab && trick_enabled(r, "grindgrab") && grinding && (left || right) && !doomed;
            // The hand of the trigger brought to the board's edge, the body crouched down to it (the game's grind clips
            // never grab: its reach states did nothing).
            std::uintptr_t core{};
            if (r.rig) (void)copy_rows(r.rig + 0x4630, reinterpret_cast<float *>(&core), 2);
            // The game's own ground grab played on the rail (the owner, 2026-10-15: it scored but "no grab happens"; the
            // mod's pulls never moved the hand). Its states, as a stock ground grab sets them, held while grinding with
            // the trigger: the game crouches and puts the hand on the board itself.
            const auto ground_grab = [&](bool on) {
                const auto put = [&](std::uintptr_t &at, const char *name, int kind, double value) {
                    if (!at) at = find_state(base, name);
                    (void)write_state_at(base, at, kind, value);
                };
                put(r.s_can_ground_grab, "Animation/Dingo/Bool.GP.CanGroundGrab", 0, on);
                put(r.s_is_ground_grab, "Animation/Dingo/Bool.State.Is.GroundGrab", 0, on);
                put(r.s_is_grabbing, "Animation/Dingo/Bool.Anim.IsGrabbing", 0, on);
                put(r.s_grab_type, "Animation/Dingo/EnumGS.GrabType", 1, on ? (r.grind_grab_kind == 3 ? 9 : r.grind_grab_kind) : 0); // the game's: 1 LT, 2 RT, 9 both
                put(r.s_grab_left_hand, "Animation/Dingo/Bool.GP.Grab.LeftHand", 0, on && (r.grind_grab_kind & 1));
                put(r.s_grab_right_hand, "Animation/Dingo/Bool.GP.Grab.RightHand", 0, on && (r.grind_grab_kind & 2));
                put(r.s_ground_grab_loop, "Animation/Dingo/Bool.Anim.IsGroundGrabLoop", 0, on && now - r.grind_grab_since > 600);
            };
            // The grab is the one pressed for (the standard grabs only: once the board is held, no tweak changes it).
            if (want && !r.grind_grab_on) {
                r.grind_grab_since = now;
                r.grind_grab_kind = left && right ? 3 : right ? 2 : 1;
                r.grind_grab_scored_grind = -1;
            } else if (want && left && right && r.grind_grab_kind != 3 && now - r.grind_grab_since < 150)
                r.grind_grab_kind = 3; // both triggers pressed a moment apart
            // Family 2 (air): the game's own in-air grab with its tweaks (the right stick), its gate held open; the
            // ground grab's states are not written.
            const bool air_grab = r.grind_family == 2;
            if (air_grab) {
                if (want || r.grind_grab_on) {
                    if (!r.s_can_grab) r.s_can_grab = find_state(base, "Animation/Dingo/Bool.GP.CanGrab");
                    (void)write_state_at(base, r.s_can_grab, 0, want);
                }
            } else if (want) ground_grab(true);
            else if (r.grind_grab_on) ground_grab(false);
            // The hand on the board through the game's own hand IK (the grind clip ignores the grab states), and the
            // body crouched to it.
            if (want && r.hand_ik && core) {
                hold_hand_ik(r, core, now, right);
                set_limb_free(core, 80, 0, 1.0f, r.grab_crouch);
            }
            // The graph kept out of its grind node (its copy of the physics state family held at the ground's), so the
            // game's own ground grab, which starts with the hand on the board, keeps playing while the board grinds
            // (the grind clip took the hand off the board 200 ms in: run gg1).
            if (want && r.grind_family >= 0) {
                if (!r.hooks) r.hooks = start_holds(base);
                if (!r.s_filtered) r.s_filtered = find_state(base, "Animation/Dingo/EnumGS.FilteredPhysicsState");
                const auto family = static_cast<std::uint32_t>(r.grind_family);
                hold_object(r.s_filtered, family, 120);
                (void)write_state_at(base, r.s_filtered, 1, family);
                air_poke.base.store(base, std::memory_order_relaxed);
                air_poke.entity.store(r.entity, std::memory_order_relaxed);
                air_poke.state.store(r.s_filtered, std::memory_order_relaxed);
                air_poke.last.store(0, std::memory_order_relaxed);
                air_poke.value.store(family, std::memory_order_relaxed);
                air_poke.until.store(now + 120, std::memory_order_relaxed);
                set_tricks_step(&air_poke_step);
            } else if (r.grind_grab_on && !want && r.grind_family >= 0) {
                hold_object(0, 0, 0);
                air_poke.until.store(0, std::memory_order_relaxed);
            }
            // Scored with the grind it is on, as the game names it (once the grind is known; again if it changes to another).
            if (want) {
                if (!r.s_skate_grind_type) r.s_skate_grind_type = find_state(base, "Animation/Dingo/Enum.GS.SkateGrindType");
                double grind{};
                if (r.s_skate_grind_type && read_state_at(base, r.s_skate_grind_type, 1, grind) && grind > 0 && static_cast<int>(grind) != r.grind_grab_scored_grind &&
                    (r.grind_grab_scored_grind < 0 || now - r.grind_grab_since > 300)) {
                    const bool first = r.grind_grab_scored_grind < 0;
                    r.grind_grab_scored_grind = static_cast<int>(grind);
                    if (first || grind_name(r.grind_grab_scored_grind) != std::string_view{})
                        score_trick("grindgrab", std::format("{} {}", r.grind_grab_kind == 3 ? "Double Grab" : r.grind_grab_kind == 2 ? "Indy" : "Melon", grind_name(r.grind_grab_scored_grind)));
                } else if (now - r.grind_grab_since > 400 && r.grind_grab_scored_grind < 0) {
                    r.grind_grab_scored_grind = 0;
                    score_trick("grindgrab", r.grind_grab_kind == 3 ? "Double Grab Grind" : r.grind_grab_kind == 2 ? "Indy Grind" : "Melon Grind");
                }
            }
            if (want != r.grind_grab_on) {
                float gap{};
                const auto steps = take_hand_pull(gap);
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: grind grab {} ({} hand; {} steps pulled, the hand {:.2f} m from the board).", want ? "on" : "off", right ? "back" : "front", steps, gap);
            }
            r.grind_grab_on = want;
        }
        // A tweaked grab landed on a rail is a fall (the owner, 2026-10-16: only the standard grabs grind; "if you land in a
        // rail in a tweaked grab you will fall"): the game's air grab type is read in the air, and a grind entered within a
        // moment of a tweaked one (not plain BS 1, FS 6 or Double 33) ends in a wipeout.
        if (r.tweak_fall) {
            const bool airborne = physics.valid && physics.state >= 200 && physics.state < 300;
            const bool on_rail = physics.valid && physics.state >= 400 && physics.state <= 406;
            if (airborne) {
                if (!r.s_detailed_grab) r.s_detailed_grab = find_state(base, "Animation/Dingo/Enum.GS.DetailedGrabType");
                double grab{};
                if (r.s_detailed_grab && read_state_at(base, r.s_detailed_grab, 1, grab)) {
                    r.air_grab_type = static_cast<int>(grab);
                    r.air_grab_seen = now;
                }
            }
            // Or held so as the rail is met: a trigger with the right stick pushed out (the tweak, before the game has
            // turned it into its grab type).
            std::int16_t trx{}, try_{};
            std::memcpy(&trx, pad.data() + 8, 2);
            std::memcpy(&try_, pad.data() + 10, 2);
            const bool tweaking = (pad[2] > 100 || pad[3] > 100) && std::hypot(trx / 32767.0f, try_ / 32767.0f) > 0.5f;
            if (airborne && tweaking) r.air_tweak_seen = now;
            if (on_rail && !r.was_on_rail && ((now - r.air_grab_seen < 150 && r.air_grab_type > 0 && !plain_grab(r, now, r.air_grab_type)) ||
                (now - r.air_tweak_seen < 150 && tweaking && !pregrab_plain(r, now)))) {
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: landed on a rail in a tweaked grab (type {}): a fall.", r.air_grab_type);
                request_wipeout();
                r.air_grab_type = 0;
            }
            r.was_on_rail = on_rail;
        }
        // The fandango: a crooked grind with the back foot off the board, held with A (the owner, 2026-10-16: "get into
        // crook and hold the proper back foot button"). The back foot's drives to the deck are moved up and back off the
        // tail and the back leg's motors eased, so the foot is held off the board while the clip keeps the crook (the
        // game's own one-foot airs never play from a grind, and its push only kicked the foot down and back on).
        if (r.fandango || r.fandango_on) {
            if (!r.s_skate_grind_type) r.s_skate_grind_type = find_state(base, "Animation/Dingo/Enum.GS.SkateGrindType");
            double grind{};
            const bool grinding = physics.valid && physics.state >= 400 && physics.state <= 406 && r.s_skate_grind_type &&
                                  read_state_at(base, r.s_skate_grind_type, 1, grind);
            const int g = static_cast<int>(grind);
            const bool crooked = g == 15 || g == 32 || g == 43 || g == 53;
            std::uint16_t face{};
            std::memcpy(&face, pad.data(), 2);
            const bool a_held = (face & 0x1000) != 0; // XINPUT_GAMEPAD_A
            const bool want = r.fandango && trick_enabled(r, "fandango") && grinding && (crooked || r.fandango_any) && a_held;
            if (!r.s_ollie_south) r.s_ollie_south = find_state(base, r.fandango_north ? "Animation/Dingo/Bool.Intent.OllieNorth" : "Animation/Dingo/Bool.Intent.OllieSouth");
            if (r.fandango_family == 2 && (want || r.fandango_on)) (void)write_state_at(base, r.s_ollie_south, 0, want);
            std::uintptr_t core{};
            if (r.rig) (void)copy_rows(r.rig + 0x4630, reinterpret_cast<float *>(&core), 2);
            // The back leg is the one whose toe the physics has behind the deck's middle (the left in a goofy stance).
            float left_toe[3]{}, right_toe[3]{};
            (void)take_foot_lift(left_toe, right_toe);
            if (want && core) {
                set_foot_lift(core, 120, r.fandango_up, r.fandango_back, r.fandango_side, r.fandango_drives);
                if (r.fandango_drives) set_limb_free(core, 120, 4, r.fandango_leg, 0.0f);
                const int which = r.fandango_which >= 0 ? r.fandango_which : left_toe[2] == right_toe[2] ? -1 : left_toe[2] < right_toe[2] ? 1 : 0;
                std::string detail;
                if (!set_leg_lift(base, core, 120, r.fandango_up, r.fandango_back, r.fandango_side, which, detail) && detail != r.fandango_error) {
                    r.fandango_error = detail;
                    logging::log(logging::Level::warning, logging::Channel::skater, "Tricks: the fandango's leg pose is unavailable: {}", detail);
                }
            }
            if ((want || r.fandango_on) && now >= r.fandango_log) {
                r.fandango_log = now + 250;
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: fandango leg: {}; the physics toes (deck frame side/up/forward) left ({:.2f},{:.2f},{:.2f}) right ({:.2f},{:.2f},{:.2f}).",
                             take_leg_lift(), left_toe[0], left_toe[1], left_toe[2], right_toe[0], right_toe[1], right_toe[2]);
            }
            if (want && r.fandango_family > 0) {
                if (!r.hooks) r.hooks = start_holds(base);
                if (!r.s_filtered) r.s_filtered = find_state(base, "Animation/Dingo/EnumGS.FilteredPhysicsState");
                hold_object(r.s_filtered, static_cast<std::uint32_t>(r.fandango_family), 120);
                (void)write_state_at(base, r.s_filtered, 1, r.fandango_family);
                air_poke.base.store(base, std::memory_order_relaxed);
                air_poke.entity.store(r.entity, std::memory_order_relaxed);
                air_poke.state.store(r.s_filtered, std::memory_order_relaxed);
                air_poke.last.store(0, std::memory_order_relaxed);
                air_poke.value.store(static_cast<std::uint32_t>(r.fandango_family), std::memory_order_relaxed);
                air_poke.until.store(now + 120, std::memory_order_relaxed);
                set_tricks_step(&air_poke_step);
            }
            if (want && !r.fandango_on) score_trick("fandango", crooked ? (g < 18 || (g >= 35 && g < 45) ? "FS Fandango" : "BS Fandango") : std::format("One Foot {}", grind_name(g)));
            if (!want && r.fandango_on && r.fandango_family > 0) {
                hold_object(0, 0, 0);
                air_poke.until.store(0, std::memory_order_relaxed);
            }
            if (want != r.fandango_on)
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: fandango {} (grind {}).", want ? "on" : "off", g);
            r.fandango_on = want;
        }
        // The anti-casper's mirror: on from the catch; after the pose, kept until the way out is in the air (the flip
        // out, or the casper's own exit flip, hides the change), the skater is off the board, or rides again.
        {
            const bool posed_anti = r.anti && !r.anti_nose && r.by_hold && r.freestyle != Freestyle::off;
            static bool exit_airborne{};
            bool want = posed_anti;
            if (posed_anti) {
                r.mirror_until = now + 3000;
                exit_airborne = false;
            } else if (r.mirror_on) {
                if (in_air) exit_airborne = true;
                const bool done = now >= r.mirror_until || (physics.valid && physics.state >= 300) || exit_airborne ||
                                  (physics.valid && physics.state == 100 && now + 2500 > r.mirror_until);
                want = !done;
            }
            if (want || r.mirror_on) {
                if (!r.s_mirrored) r.s_mirrored = find_state(base, "Animation/Dingo/Bool.Anim.Mirrored");
                (void)write_state_at(base, r.s_mirrored, 0, want ? 1 : 0);
                if (want != r.mirror_on)
                    logging::log(logging::Level::info, logging::Channel::skater, "Tricks: anti-casper mirror {} (physics {}){}.", want ? "on" : "off", physics.valid ? static_cast<int>(physics.state) : -1,
                                 want ? "" : std::format(", root turned half round in {} builds", take_freestyle_root_turned()));
                r.mirror_on = want;
            }
            // Turned half round with the mirror, at the same moments (the mirror alone is the casper seen from behind).
            set_freestyle_root_turn(r.rig, r.mirror_on && r.anti_turn);
            if (!want && !posed_anti) r.anti = false;
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
    if (r.entity) set_board_turn_owner(client, r.entity);
    if ((r.freestyle != Freestyle::off || now < r.exit_guard_until) && r.entity) set_freestyle_slide(client, r.entity, r.slide);
    set_freestyle_slide_upright(r.upright);
    // The casper's loop is a blend of three clips (low, medium, high) on Float.Intent.Balance, 0 to 1: the left
    // stick's up and down leans the casper while it is held.
    if (r.freestyle == Freestyle::casper && r.casper_balance && skater_key_known()) {
        std::array<std::uint8_t, 12> pad{};
        DingoSDKOverlayTricksPad(pad.data());
        std::int16_t ly{};
        std::memcpy(&ly, pad.data() + 6, 2);
        if (!r.s_balance) r.s_balance = find_state(base, "Animation/Dingo/Float.Intent.Balance");
        if (r.height_on && r.by_hold && r.balance_fixed < 0) {
            // The board's height on the right stick, as a manual's (the owner, 2026-10-13): how far the stick is out in
            // the pose's place. At the edge (where the owner holds it) the low clip, eased back toward the middle the
            // high one, smoothed so the thumb's own shake does not show. Above the middle's fall (0.2) all the way.
            std::int16_t hx{}, hy{};
            std::memcpy(&hx, pad.data() + 8, 2);
            std::memcpy(&hy, pad.data() + 10, 2);
            const float out = std::sqrt(static_cast<float>(hx) * hx + static_cast<float>(hy) * hy) / 32767.0f;
            const float t = std::clamp((out - r.height_inner) / (1.0f - r.height_inner), 0.0f, 1.0f);
            const float target = r.height_at_middle + (r.height_at_edge - r.height_at_middle) * t;
            const float step = r.height_last ? std::min(1.0f, static_cast<float>(now - r.height_last) / 150.0f) : 1.0f;
            r.height_now += (target - r.height_now) * step;
            r.height_last = now;
            (void)write_state_at(base, r.s_balance, 2, r.height_now);
        } else if (r.balance_fixed >= -0.5f) {
            r.height_last = 0;
            r.height_now = 0.5f;
            (void)write_state_at(base, r.s_balance, 2, r.balance_fixed >= 0 ? r.balance_fixed : r.left_mask ? 0.5 : 0.5 + 0.5 * (ly / 32767.0));
        }
    }
    // Ground shuvit: 7 held while the board turns, then let go; the turn is only dropped once the board has stopped
    // following a frame (out of 103), or it would be turned back.
    if (r.gs_phase && skater_key_known()) {
        const auto physics = watched_physics_state();
        DingoSDKOverlayTricksMask();
        // Not stepped off while the board turns under the feet (103 -> 504 at the start, both sides, 2026-10-15).
        protect_pose(300);
        keep_pose_on_board(200);
        if (r.gs_phase == 1) {
            r.writes += write_state(base, State::ground_trick, 7);
            // A slight scoop with the legs (the owner, 2026-10-14: "it looks like a revert"): the game's own trick-setup
            // crouch held while the board turns under the feet.
            if (r.gs_scoop) {
                if (!r.s_early) r.s_early = find_state(base, r.early_name.c_str());
                (void)write_state_at(base, r.s_early, 0, now < r.gs_until ? 1 : 0);
            }
            // Riding keeps the board on its physics (ForcePhysicsSkateboard), so the ground trick value alone never
            // reaches the ground animation state: the state selector is told to go there for the length of the turn.
            if (now < r.gs_until) hold_ground_animation(120);
            else hold_ground_animation(0);
            if (r.entity) set_freestyle_slide(client, r.entity, 0.5f); // keep the speed: 103's own friction stops the board
            set_freestyle_slide_mean(true);
            if (now >= r.gs_until) {
                r.writes += write_state(base, State::ground_trick, 0);
                r.gs_phase = 2;
                r.gs_until = now + 600;
            }
        } else if ((physics.valid && physics.state != 103) || now >= r.gs_until) {
            stop_board_spin();
            set_freestyle_slide_mean(false);
            r.gs_phase = 0;
            std::uint32_t seen{}, turned{};
            board_hook_counts(seen, turned);
            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: ground shuvit over: the board's frame was turned {} times of {} (physics {}).", turned, seen,
                         physics.valid ? static_cast<int>(physics.state) : -1);
        }
    }
    // No comply flips.
    if (r.nc_flips && r.family_on[static_cast<std::size_t>(Family::nocomply)] && skater_key_known()) {
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
            if (now - r.nc_seen_at > 1000) // <dev>
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: no comply started (physics {}, plant {}).", physics.valid ? static_cast<int>(physics.state) : -1, planting);
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
        // The flip is the one the flick that popped the no comply asked for (the owner: "press A fast and then do
        // kickflip or shuv version instead of ollie", 2026-10-14): read back from the right stick's way over the half
        // second before the no comply leaves the ground. A plain flick (straight up) is the game's own no comply.
        // (The earlier pick, a second flick once the no comply had started, also took the popping flick's down for a
        // hardflip whenever A came before the flick.)
        (void)x; (void)y; (void)length;
        // The no comply leaves the ground on the flick's "down" (this game's no comply is a push, then the stick down),
        // so the flick's direction comes in its first moments of air: read until it shows, for 0.35 s.
        if (air && !r.nc_trick && !r.nc_air && now - r.nc_seen_at < 1000 && r.controls) {
            if (!r.nc_takeoff_at) r.nc_takeoff_at = now;
            const int trick = popped_flip(r, now, 900);
            const char *id = trick == 2 ? "nc_kickflip" : trick == 3 ? "nc_heelflip" : trick == 4 ? "nc_shuvit" : trick == 7 ? "nc_fsshuvit" : nullptr;
            if (id && trick_enabled(r, id)) {
                r.nc_trick = static_cast<std::uint32_t>(trick);
                r.nc_until = now + 2000;
                r.nc_air = false;
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: no comply flip {} read from the pop's flick.", trick);
            } else if (id || now - r.nc_takeoff_at > 350) r.nc_air = true; // this air is the game's own (plain, or the flip switched off)
        }
        if (!air && !r.nc_trick) {
            r.nc_air = false;
            r.nc_takeoff_at = 0;
        }
        // Once the no comply has started, the rest of the flick is the mod's pick only: the game read it as a spin and
        // turned the body 180 (the owner, 2026-10-14: "only if you spin your left stick").
        if (r.nc_hide && now - r.nc_seen_at < 900 && r.controls) DingoSDKOverlayTricksMask();
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
                start_air_spin(r, roll, yaw, 0, r.nc_spin_ms, true, true);
                const char *nc_id = r.nc_trick == 2 ? "nc_kickflip" : r.nc_trick == 3 ? "nc_heelflip" : r.nc_trick == 4 ? "nc_shuvit" : r.nc_trick == 7 ? "nc_fsshuvit" : "nc_kickflip";
                score_trick(nc_id, std::format("{} {}", r.plant_flip ? "Boneless" : "No Comply", flip_name(r.nc_trick)));
            }
            if (now >= r.nc_until || (r.nc_air && ground)) {
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: no comply flip {} ended ({}).", r.nc_trick, r.nc_air ? "landed" : "timed out");
                stop_board_spin();
                r.nc_trick = 0;
            }
        }
    }
    // <dev> Spin stills: the game slowed nearly to a stop where asked, a still logged, then the speed put back.
    if (r.shot_until && now >= r.shot_until) {
        (void)change_named_setting("SimulationTime.TimeScale", "1", false);
        r.shot_until = 0;
    }
    if ((r.shot_progress >= 0 || r.shot_air_ms) && !r.shot_until) {
        const auto ph = watched_physics_state();
        const bool in_air = ph.valid && ph.state >= 200 && ph.state < 300;
        if (!in_air) r.shot_air_since = 0;
        else if (!r.shot_air_since) r.shot_air_since = now;
        const float progress = board_spin_progress();
        bool due = false;
        if (r.shot_progress >= 0) {
            if (progress < 0) r.shot_taken = false;
            else if (!r.shot_taken && progress >= r.shot_progress) due = true;
        } else if (r.shot_air_ms) {
            if (!in_air) r.shot_taken = false;
            else if (!r.shot_taken && now - r.shot_air_since >= r.shot_air_ms) due = true;
        }
        if (due) {
            r.shot_taken = true;
            (void)change_named_setting("SimulationTime.TimeScale", "0.02", false);
            r.shot_until = now + r.shot_hold_ms;
            logging::log(logging::Level::info, logging::Channel::skater, "shot-{}{} (Tricks: still at spin {:.2f})", r.shot_tag, ++r.shot_count, progress);
        }
    }
    // <dev> After a board-turning trick: does the body face the board, and does the game think it rides fakie?
    if ((r.nc_trick && r.nc_air) || r.gs_phase == 1 || r.pop_spinning || r.out_spinning || r.leaving) r.watch_until = now + 3500;
    if (r.watch_air) {
        const auto ph = watched_physics_state();
        if (ph.valid && ph.state >= 200 && ph.state < 300) r.watch_until = now + 2500;
    }
    if (now < r.watch_until && now >= r.watch_next) {
        r.watch_next = now + (r.watch_fine ? 15 : 100);
        float angle{}, up{};
        double fakie{};
        if (!r.s_riding_fakie) r.s_riding_fakie = find_state(base, "Animation/Dingo/Bool.Physics.RidingFakie");
        (void)read_state_at(base, r.s_riding_fakie, 0, fakie);
        const auto ph = watched_physics_state();
        float turned_by = 0;
        if (!kinematic_spin_running())
            if (const auto check = take_kinematic_spin_check(); !check.empty())
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: watch: board play {}.", check);
        std::uint32_t kin_failures{}, kin_skipped{};
        if (const auto kin = take_kinematic_spin_steps(kin_failures, kin_skipped); kin || kin_failures || kin_skipped)
            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: watch: board played {} steps ({} failed, {} skipped).", kin, kin_failures, kin_skipped);
        if (const auto feet = take_feet_free_writes())
            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: watch: feet rows loosened {} times.", feet);
        if (const auto turns = take_board_turn_backs(turned_by))
            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: watch: board turned back {:.0f} deg ({}).", turned_by, turns);
        int backwards = -1;
        float offset = 0;
        (void)board_backwards_state(r.rig, backwards, offset);
        float deck_angle{}, deck_up{};
        if (watch_board_heading(deck_angle, deck_up)) {
            angle = deck_angle;
            up = deck_up;
            double flip_now{}, flip_wanted{}, revert{};
            if (!r.s_current_flip) r.s_current_flip = find_state(base, "Animation/Dingo/EnumGS.Anim.CurrentFlipTrick");
            if (!r.s_flip_intent) r.s_flip_intent = find_state(base, "Animation/Dingo/EnumGS.Intent.FlipTrick");
            if (!r.s_watch_revert) r.s_watch_revert = find_state(base, "Animation/Dingo/Bool.Intent.Revert");
            (void)read_state_at(base, r.s_current_flip, 1, flip_now);
            (void)read_state_at(base, r.s_flip_intent, 1, flip_wanted);
            (void)read_state_at(base, r.s_watch_revert, 0, revert);
            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: watch: {} ms deck {:.0f} deg, up {:.2f}, spin {:.0f}/s, fakie {}, physics {}, offset {:.0f}, flip {} intent {} revert {}.", now % 100000, angle, up,
                         watched_board_spin(), fakie != 0, ph.valid ? static_cast<int>(ph.state) : -1, offset, flip_now, flip_wanted, revert != 0);
        } else if (root_board_angle(r.rig, angle, up))
            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: watch: body vs board {:.0f} deg, board up {:.2f}, fakie {}, physics {}, flag {}, offset {:.0f}.", angle, up, fakie != 0, ph.valid ? static_cast<int>(ph.state) : -1, backwards, offset);
    }
    // The skater's heading is held while the mod spins the board under the feet: a no comply flip, a ground shuvit. The
    // body is built from the board's frame and was dragged round with it (the owner, 2026-10-14: "the pop shuv and
    // kickflip no complys still 180 your body"; the ground shuvit "looks like a revert").
    set_freestyle_root_heading_lock(r.rig, (r.nc_trick != 0 && r.nc_air) || r.gs_phase == 1);
    // Flick-it flips (the owner, 2026-10-14): read from the flick that popped the skater, in the first moments of its
    // air, and turned on top of the game's own trick: the dolphin flip (stick down, up and straight back down: the
    // board flips end over end forward), the dragon flip (the same ending on a lower diagonal: with a 360 shuvit) and
    // the big spin (the game's pop shuvit while the left stick spins the skater 180: the board turns 360).
    if (r.controls && r.family_on[static_cast<std::size_t>(Family::flickit)] && skater_key_known()) {
        const auto physics = watched_physics_state();
        const bool air = physics.valid && physics.state >= 200 && physics.state < 300;
        const bool ground = physics.valid && physics.state >= 100 && physics.state <= 105;
        if (ground) {
            // <dev> Stock check (the owner: plain kickflips and pop shuvs stay fully stock): what of the mod's touched
            // this air. A stock flip must show 0 masked polls and 0 hook turns (board play and feet are in the watch).
            if (r.takeoff_at) {
                std::uint32_t seen{}, turned{};
                board_hook_counts(seen, turned);
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: stock check: flip {}, {} masked polls, {} hook turns, mod trick {}.", r.air_flip_seen,
                             DingoSDKOverlayTricksMaskedPolls(), turned, r.pop_spinning || r.out_spinning || r.flip_grab_on || r.nc_trick ? "yes" : "no");
                r.air_flip_seen = 0;
            }
            if (r.pop_spinning) stop_board_spin();
            r.pop_spinning = false;
            r.pop_checked = false;
            r.takeoff_at = 0;
        } else if (air && !r.takeoff_at) {
            r.takeoff_at = now;
        }
        if (air && r.s_current_flip) {
            double f{};
            if (read_state_at(base, r.s_current_flip, 1, f) && f != 0) r.air_flip_seen = static_cast<int>(f);
        }
        if (air && !r.pop_checked && r.takeoff_at && r.freestyle == Freestyle::off && !r.by_hold && !r.nc_trick && !r.leaving && !r.out_spinning) {
            // The dolphin's second stroke (the stick back down after the pop) is not the game's: it read it as a flip
            // and played a 360 flip first (the owner, 2026-10-14). Hidden from the game while it could still come.
            // Only on a plain pop (the game's flip none or its ollie): a kickflip's flick passes through the straight-up cone too,
            // and the mask then hid the stick in its air (the owner: plain kickflips and pop shuvs stay fully stock).
            if (!r.s_current_flip) r.s_current_flip = find_state(base, "Animation/Dingo/EnumGS.Anim.CurrentFlipTrick");
            double game_flip{};
            // (1 is the game's ollie, 16 its nollie: the owner's plain pops read 1, and the dolphin never came, 2026-10-14.)
            const bool plain_pop = r.s_current_flip && read_state_at(base, r.s_current_flip, 1, game_flip) && (game_flip == 0 || game_flip == 1 || game_flip == 16);
            if (plain_pop && (trick_enabled(r, "dolphin") || trick_enabled(r, "dragon"))) {
                bool straight_seen = false; // the middle stroke: straight up (from the tail) or straight down (from the nose)
                for (const auto &sample : recent_sticks(r, now, 500)) straight_seen = straight_seen || (std::abs(sample.ry) > 0.75f && std::abs(sample.rx) < 0.4f);
                if (straight_seen) DingoSDKOverlayTricksMask();
            }
            int side = 0;
            int forward = plain_pop ? forward_flick(r, now, 700, side) : 0;
            bool nollie = false;
            if (!forward && plain_pop) {
                forward = forward_flick(r, now, 700, side, true);
                nollie = forward != 0;
            }
            // Riding fakie, or popped from the nose, the board flips end over end backwards (the owner, 2026-10-14);
            // the turn keeps the stick's side. Both at once (switch nollie) is forward again.
            double fakie_value{};
            if (forward && !r.s_riding_fakie) r.s_riding_fakie = find_state(base, "Animation/Dingo/Bool.Physics.RidingFakie");
            bool fakie = forward && r.s_riding_fakie && read_state_at(base, r.s_riding_fakie, 0, fakie_value) && fakie_value != 0;
            // The flag stays off after a 180 ollie (measured 2026-10-15); the board's offset frame turns round instead.
            if (forward && !fakie && r.rig) {
                int backwards{};
                float offset{};
                if (board_backwards_state(r.rig, backwards, offset) && std::abs(offset) > 90.0f) fakie = true;
            }
            const float pitch = fakie != nollie ? -360.0f : 360.0f;
            const char *stance = fakie && nollie ? "Switch Nollie " : fakie ? "Fakie " : nollie ? "Nollie " : "";
            const char *id = nullptr;
            std::string name;
            const auto plain_ollie = [&] { // the game's own flip trick set back to none: the board's turn is the mod's
                if (!r.s_current_flip) r.s_current_flip = find_state(base, "Animation/Dingo/EnumGS.Anim.CurrentFlipTrick");
                (void)write_state_at(base, r.s_current_flip, 1, 0);
                if (!r.s_flip_intent) r.s_flip_intent = find_state(base, "Animation/Dingo/EnumGS.Intent.FlipTrick");
                (void)write_state_at(base, r.s_flip_intent, 1, 0);
            };
            const bool forward_flip = (forward == 1 && trick_enabled(r, "dolphin")) || (forward == 2 && trick_enabled(r, "dragon"));
            if (forward_flip) plain_ollie();
            // The owner's turns (2026-10-14): the dolphin is a forward varial (one front flip and a half turn of the board),
            // the dragon a forward 360 (one front flip and a full turn of the board: the nose goes all the way round in
            // both).
            if (forward == 1 && trick_enabled(r, "dolphin")) {
                start_air_spin(r, 0, side > 0 ? -180.0f : 180.0f, pitch, r.flickit_spin_ms, false, true);
                flip_legs(r, base);
                id = "dolphin";
                name = std::format("{}Dolphin Flip", stance);
            } else if (forward == 2 && trick_enabled(r, "dragon")) {
                start_air_spin(r, 0, side > 0 ? -360.0f : 360.0f, pitch, r.flickit_spin_ms, false, true);
                flip_legs(r, base);
                id = "dragon";
                name = std::format("{}Dragon Flip", stance);
            } else if (r.big_spin && trick_enabled(r, "bigspin")) {
                if (!r.s_current_flip) r.s_current_flip = find_state(base, "Animation/Dingo/EnumGS.Anim.CurrentFlipTrick");
                double current{};
                (void)read_state_at(base, r.s_current_flip, 1, current);
                const int flip = static_cast<int>(current);
                bool spun = false;
                for (const auto &sample : recent_sticks(r, now, 700)) spun = spun || std::abs(sample.lx) > 0.6f;
                if ((flip == 4 || flip == 7) && spun) {
                    start_air_spin(r, 0, flip == 4 ? 180.0f : -180.0f, 0, r.flickit_spin_ms);
                    id = "bigspin";
                    name = flip == 4 ? "Big Spin" : "FS Big Spin";
                }
            }
            if (id) {
                r.pop_checked = true;
                r.pop_spinning = true;
                score_trick(id, name);
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: flick-it {} read from the pop.", name);
            } else if (now - r.takeoff_at > 350) {
                r.pop_checked = true; // too late in the air for a pop's flick
                // <dev> what the pop was, for the flick-it reading
                double current{};
                if (r.s_current_flip) (void)read_state_at(base, r.s_current_flip, 1, current);
                bool spun = false;
                for (const auto &sample : recent_sticks(r, now, 1000)) spun = spun || std::abs(sample.lx) > 0.6f;
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: pop read: flip {}, forward stroke {}, left stick spun {}.", static_cast<int>(current), forward, spun);
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
            // (Dolphin and dragon flips are flick-it tricks now, right stick only: the owner, 2026-10-14. A tap with the stick
            // up, down or on a diagonal does nothing.)
            bool none = false;
            if (std::abs(x) > 0.6f && std::abs(x) >= std::abs(y)) { yaw = x > 0 ? -180.0f : 180.0f; what = "late shuvit"; }
            else if (std::abs(y) > 0.45f) none = true;
            else roll = 360.0f * way;
            const char *id = yaw != 0 ? "air_lateshuv" : "air_extraflip";
            if (!none && trick_enabled(r, id)) {
                start_air_spin(r, roll, yaw, pitch, r.air_spin_ms);
                ++r.air_used;
                score_trick(id, yaw != 0 ? std::string_view("Late Shuvit") : extra_flip_name(trick));
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: in-air {} on flip trick {} (roll {}, yaw {}, pitch {}).", what, trick, roll, yaw, pitch);
            }
        }
    }
    // <dev>
    for (auto &extra : r.extras) {
        if (extra.freestyle_only && r.freestyle == Freestyle::off) continue;
        if (!extra.at) extra.at = find_state(base, extra.name.c_str());
        if (!extra.at) extra.at = find_anywhere(base, extra.name.c_str());
        if (extra.at && write_state_at(base, extra.at, extra.kind, extra.value)) ++extra.writes;
    }
    for (auto &w : r.watched) {
        if (!w.at) w.at = find_state(base, w.name.c_str());
        if (!w.at) w.at = find_anywhere(base, w.name.c_str());
        if (!w.at) {
            if (!w.missing_logged && state_objects_known()) logging::log(logging::Level::info, logging::Channel::skater, "Tricks: state watch: {} not found.", w.name);
            w.missing_logged = state_objects_known();
            continue;
        }
        double v{};
        if (!read_state_at(base, w.at, w.kind, v) || v == w.last) continue;
        w.last = v;
        const auto ph = watched_physics_state();
        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: state {} = {} at {} ms (physics {}).", w.name, v, GetTickCount64() % 100000, ph.valid ? static_cast<int>(ph.state) : -1);
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
    // The anti casper as a nose manual: the manual intent held, the board on its clip (103), turned over and down.
    const bool anti_nose_on = r.freestyle != Freestyle::off && r.anti && r.anti_nose;
    if (anti_nose_on) {
        if (!r.s_manual_intent) r.s_manual_intent = find_state(base, "Animation/Dingo/EnumGS.Intent.Manual");
        (void)write_state_at(base, r.s_manual_intent, 1, r.nose_manual_value > 0 ? r.nose_manual_value : 2);
        r.writes += write_state(base, State::wants_freestyle, 0);
        const auto physics = watched_physics_state();
        const bool grounded = physics.valid && (physics.state == 100 || physics.state == 103);
        if (grounded) hold_ground_animation(150);
        if (physics.valid && physics.state == 103 && !r.anti_spun) {
            start_board_spin(180.0f, 0.0f, r.anti_pitch, 140);
            r.anti_spun = true;
            logging::log(logging::Level::info, logging::Channel::skater, "Tricks: anti casper (nose manual {}) board turned over.", r.nose_manual_value);
        }
        return;
    }
    if (r.anti_spun) { // the anti casper is over: the board flips back the rest of the way round
        r.anti_spun = false;
        start_board_spin(360.0f, 0.0f, 0.0f, 0);
        stop_board_spin();
        if (!r.s_manual_intent) r.s_manual_intent = find_state(base, "Animation/Dingo/EnumGS.Intent.Manual");
        (void)write_state_at(base, r.s_manual_intent, 1, 0);
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
    changed(r); // any command may have changed a kept setting: the page's view and tricks.json follow
    if (verb == "enable" || verb == "family") {
        // tricks enable <trick> on|off | tricks family <family> on|off (the Tricks page's switches)
        if (arguments.size() != 2 || (arguments[1] != "on" && arguments[1] != "off")) return std::format("usage: tricks {} <id> on|off", verb);
        const bool on = arguments[1] == "on";
        if (verb == "enable") {
            const int i = trick_index(arguments[0]);
            if (i < 0) return "error: no trick " + arguments[0] + ".";
            r.trick_on[i] = on;
            return std::format("{} {}.", trick_catalog[i].name, on ? "on" : "off");
        }
        for (std::size_t i = 0; i < family_count; ++i)
            if (arguments[0] == family_catalog[i].id) {
                r.family_on[i] = on;
                return std::format("{} {}.", family_catalog[i].name, on ? "on" : "off");
            }
        return "error: no family " + arguments[0] + ".";
    }
    if (verb == "set") {
        // tricks set <setting> <value> (the Tricks page's sliders; tricks_view.h lists them)
        if (arguments.size() != 2) return "usage: tricks set <setting> <value>";
        if (arguments[0] == "casper_exit") { // <dev> not on the page
            r.casper_exit = std::atof(arguments[1].c_str()) >= 12.0 ? 16u : 9u;
            return std::format("casper_exit = {}.", r.casper_exit);
        }
        if (!set_setting(r, arguments[0], std::atof(arguments[1].c_str()))) return "error: no setting " + arguments[0] + ".";
        return std::format("{} = {:.2f}.", arguments[0], setting_value(r, arguments[0]));
    }
    if (verb == "scan") {
        // <dev> tricks scan hud | next hud | <value> | next <value> | list | status | add <value>
        // "hud" takes the line score the score HUD shows right now.
        if (arguments.empty()) return "usage: tricks scan hud | next hud | <value> | next <value> | list | status | add <value>";
        const bool next = arguments[0] == "next";
        const std::string what = next ? "next" : arguments[0];
        if (what == "list" || what == "status") return scan_command(what, 0);
        if (what == "auto") {
            r.scan_auto = arguments.size() < 2 || arguments[1] != "off";
            r.scan_last = -1;
            return r.scan_auto ? "Scan follows the line score." : "Scan auto off.";
        }
        if (what == "add" || what == "addi" || what == "addf") return arguments.size() > 1 ? scan_command(what, std::atof(arguments[1].c_str())) : "usage: tricks scan add|addi|addf <value>";
        const std::string source = next ? (arguments.size() > 1 ? arguments[1] : "hud") : arguments[0];
        double value{};
        if (source == "hud") {
            float line{};
            if (!read_hud_line_score(r.base, line)) return "error: the score HUD is not there.";
            value = line;
        } else value = std::atof(source.c_str());
        return std::format("(line score {}) ", value) + scan_command(next ? "next" : "new", value);
    }
    if (verb == "score") {
        if (arguments.size() == 2 && arguments[0] == "lines") { // <dev>
            set_line_total_writes(arguments[1] == "on");
            return std::format("Line total writes {}.", arguments[1] == "on" ? "on" : "off");
        }
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks score on|off | lines on|off";
        r.score_feed = arguments[0] == "on";
        return r.score_feed ? "Mod tricks are added to the score line." : "Mod tricks are not scored.";
    }
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
                           r.setters ? "watched" : "off", (skater_key_known() ? std::format("{:x}", skater_key()) : std::string("none")) + " (" + key_recapture_note() + ")",
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
        if (arguments[0] == "watch" && arguments.size() >= 3) {
            Runtime::Watched w;
            w.name = full(arguments[1]);
            w.kind = arguments[2] == "b" ? 0 : arguments[2] == "i" ? 1 : 2;
            r.watched.push_back(w);
            return "Watching " + w.name + ".";
        }
        if (arguments[0] == "clear") {
            r.watched.clear();
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
            r.air_values.clear();
            r.s_air.clear();
            for (std::size_t i = 1; i < arguments.size(); ++i) {
                auto name = arguments[i];
                int value = 0;
                if (const auto equals = name.find('='); equals != std::string::npos) {
                    value = std::atoi(name.c_str() + equals + 1);
                    name.resize(equals);
                }
                r.air_names.push_back(name.find('/') == std::string::npos ? "Animation/Dingo/" + name : name);
                r.air_values.push_back(value);
            }
            return std::format("{} states held while a pose is caught in the air.", r.air_names.size());
        }
        if (arguments.size() == 2 && arguments[0] == "primoleft") {
            r.primo_air_left = std::clamp(static_cast<float>(std::atof(arguments[1].c_str())), 0.0f, 2.0f);
            return std::format("A caught primo starts with {:.2f} s left to land.", r.primo_air_left);
        }
        if (arguments.size() == 2 && arguments[0] == "primodelay" && number(arguments[1], r.primo_air_delay))
            return std::format("A caught primo starts {} ms after takeoff.", r.primo_air_delay);
        if (!arguments.empty() && arguments[0] == "last") {
            if (arguments.size() < 2 || (arguments[1] != "on" && arguments[1] != "off")) return "usage: tricks airpose last on|off";
            r.air_last_fps = arguments[1] == "on";
            return r.air_last_fps ? "A caught pose leaves the flip through the landing (the family before ground is written as air)." : "A caught pose waits for the flip's own branch window.";
        }
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks airpose on|off | last on|off | states <bool state | enum state=value>...";
        r.air_pose = arguments[0] == "on";
        return r.air_pose ? "A caught pose starts in the air (the animation is told the board is down)." : "A caught pose starts on the ground.";
    }
    if (verb == "keep") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks keep on|off";
        r.keep = arguments[0] == "on";
        return r.keep ? "A held pose is not stepped off by the game." : "The game may step the skater off a held pose.";
    }
    if (verb == "soft") {
        std::uint32_t ms{};
        if (arguments.empty() || !number(arguments[0], ms) || ms > 2000) return "usage: tricks soft <ms 0 to 2000>";
        set_freestyle_slide_soft(ms);
        return std::format("A landed pose is held level for {} ms.", ms);
    }
    if (verb == "rigid") {
        if (arguments.empty()) return std::format("The posed board's drive strength is {:.0f} (0: the game's own, 1).", r.rigid);
        r.rigid = std::clamp(static_cast<float>(std::atof(arguments[0].c_str())), 0.0f, 1000.0f);
        return std::format("A posed board is held to its pose with strength {:.0f}.", r.rigid);
    }
    if (verb == "balance") {
        // <dev> tricks balance <0 to 1> | stick | off
        if (arguments.empty()) return "usage: tricks balance <0 to 1> | stick | off";
        r.balance_fixed = arguments[0] == "stick" ? -0.1f : arguments[0] == "off" ? -1.0f : std::clamp(static_cast<float>(std::atof(arguments[0].c_str())), 0.0f, 1.0f);
        return std::format("The casper's balance: {}.", r.balance_fixed);
    }
    if (verb == "rootup") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off" && arguments[0] != "watch")) return "usage: tricks rootup on|off|watch";
        r.rootup = arguments[0] == "on" ? 2 : arguments[0] == "watch" ? 1 : 0;
        return r.rootup == 2 ? "The skater's root is held upright in a pose." : r.rootup == 1 ? "The skater's root is watched in a pose." : "The skater's root is the game's in a pose.";
    }
    if (verb == "height") {
        // tricks height on|off | <balance at the edge> <balance near the middle> [inner stick length]
        if (arguments.size() == 1 && (arguments[0] == "on" || arguments[0] == "off")) {
            r.height_on = arguments[0] == "on";
            return r.height_on ? "The casper's height follows how far the right stick is out." : "The casper's height is fixed.";
        }
        if (arguments.size() < 2) return std::format("Casper height {}: {:.2f} at the edge, {:.2f} at {:.2f} out. usage: tricks height on|off | <edge> <middle> [inner]", r.height_on ? "on" : "off",
                                                     r.height_at_edge, r.height_at_middle, r.height_inner);
        r.height_at_edge = std::clamp(static_cast<float>(std::atof(arguments[0].c_str())), 0.0f, 1.0f);
        r.height_at_middle = std::clamp(static_cast<float>(std::atof(arguments[1].c_str())), 0.0f, 1.0f);
        if (arguments.size() > 2) r.height_inner = std::clamp(static_cast<float>(std::atof(arguments[2].c_str())), 0.25f, 0.9f);
        return std::format("Casper height: {:.2f} at the edge, {:.2f} at {:.2f} out.", r.height_at_edge, r.height_at_middle, r.height_inner);
    }
    if (verb == "spinshot") { // <dev> spinshot <tag> <progress 0..1> | <tag> air <ms> | off
        if (arguments.empty() || arguments[0] == "off") {
            r.shot_progress = -1;
            r.shot_air_ms = 0;
            return "Spin stills off.";
        }
        r.shot_tag = arguments[0];
        r.shot_count = 0;
        if (arguments.size() > 2 && arguments[1] == "air") {
            r.shot_progress = -1;
            r.shot_air_ms = static_cast<std::uint32_t>(std::atoi(arguments[2].c_str()));
        } else {
            r.shot_air_ms = 0;
            r.shot_progress = arguments.size() > 1 ? static_cast<float>(std::atof(arguments[1].c_str())) : 0.5f;
        }
        return "Spin stills on.";
    }
    if (verb == "grab") { // grab side|gain|crouch|rise|ms <value> | status
        if (arguments.size() == 2) {
            const float v = static_cast<float>(std::atof(arguments[1].c_str()));
            if (arguments[0] == "side") r.grab_side = v;
            else if (arguments[0] == "gain") r.grab_gain = std::clamp(v, 0.0f, 1.0f);
            else if (arguments[0] == "crouch") r.grab_crouch = std::clamp(v, 0.0f, 0.5f);
            else if (arguments[0] == "tuck") r.grab_tuck = std::clamp(v, 0.0f, 0.5f);
            else if (arguments[0] == "armmask") r.grab_arm_mask = static_cast<int>(v);
            else if (arguments[0] == "place") set_pull_moves_place(v != 0);
            else if (arguments[0] == "limbs") r.grab_limbs = std::clamp(v, 0.0f, 1.0f);
            else if (arguments[0] == "rise") r.flip_grab_rise = std::clamp(v, 0.0f, 1.2f);
            else if (arguments[0] == "reach") r.flip_grab_reach = std::clamp(v, -0.8f, 0.8f);
            else if (arguments[0] == "rigid") r.grab_rigid = std::clamp(v, 0.0f, 2000.0f);
            else if (arguments[0] == "ms") r.flip_grab_ms = static_cast<std::uint32_t>(std::clamp(v, 150.0f, 1500.0f));
            else if (arguments[0] == "endflip") r.flip_grab_endflip = v != 0;
            else if (arguments[0] == "spin") r.flip_grab_spin_ms = static_cast<std::uint32_t>(std::clamp(v, 150.0f, 1500.0f));
            else if (arguments[0] == "tuck") r.grab_tuck = std::clamp(v, 0.0f, 0.5f);
            else if (arguments[0] == "family") r.grind_family = static_cast<int>(v);
        }
        float gap{};
        const auto steps = take_hand_pull(gap);
        return std::format("Grabs: side {:.2f}, gain {:.2f}, crouch {:.1f}, flip grab rise {:.2f} m over {} ms; {} steps pulled, the hand {:.2f} m from the board.", r.grab_side, r.grab_gain,
                           r.grab_crouch, r.flip_grab_rise, r.flip_grab_ms, steps, gap);
    }
    if (verb == "handik") { // handik on|off | dump | backright 0|1 | up <m> | catch <share> | tune <speed> <p5> <flag6> | hold <ms> <right 0|1> <x> <y> <z>
        std::string out;
        if (arguments.size() == 1 && (arguments[0] == "on" || arguments[0] == "off")) r.hand_ik = arguments[0] == "on";
        else if (arguments.size() == 1 && arguments[0] == "dump") out = dump_hand_ik(r.base, r.entity);
        else if (arguments.size() == 2 && arguments[0] == "backright") r.hand_ik_back_right = arguments[1] != "0";
        else if (arguments.size() == 2 && arguments[0] == "write") set_hand_ik_write(arguments[1] != "0");
        else if (arguments.size() == 2 && arguments[0] == "up") r.hand_ik_up = std::clamp(static_cast<float>(std::atof(arguments[1].c_str())), -0.3f, 0.5f);
        else if (arguments.size() == 2 && arguments[0] == "catch") r.hand_ik_catch = std::clamp(static_cast<float>(std::atof(arguments[1].c_str())), 0.0f, 1.0f);
        else if (arguments.size() == 4 && arguments[0] == "tune")
            set_hand_ik_tuning(static_cast<float>(std::atof(arguments[1].c_str())), static_cast<float>(std::atof(arguments[2].c_str())), arguments[3] != "0");
        else if (arguments.size() == 6 && arguments[0] == "hold") {
            std::uintptr_t core{};
            if (r.rig) (void)copy_rows(r.rig + 0x4630, reinterpret_cast<float *>(&core), 2);
            set_hand_ik(core, static_cast<std::uint32_t>(std::atoi(arguments[1].c_str())), arguments[2] != "0", static_cast<float>(std::atof(arguments[3].c_str())),
                        static_cast<float>(std::atof(arguments[4].c_str())), static_cast<float>(std::atof(arguments[5].c_str())));
            r.hand_ik_watch_until = GetTickCount64() + static_cast<std::uint64_t>(std::atoi(arguments[1].c_str()));
        }
        if (out.empty()) out = std::format("Hand IK {} (the back hand is IK {}), {:.2f} m up, the flip grab's hand from {:.0f}% of the turn.", r.hand_ik ? "on" : "off", r.hand_ik_back_right ? "right" : "left",
                                           r.hand_ik_up, r.hand_ik_catch * 100);
        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: {}", out);
        return out;
    }
    if (verb == "kinematic") {
        r.kinematic = arguments.empty() || arguments[0] != "off";
        return r.kinematic ? "The mod's flips are played on the board itself." : "The mod's flips go through the board's drive.";
    }
    if (verb == "dip") { // dip <metres> | ease <in> <slow>
        if (arguments.size() == 3 && arguments[0] == "ease") {
            set_board_flip_ease(static_cast<float>(std::atof(arguments[1].c_str())), static_cast<float>(std::atof(arguments[2].c_str())));
            return std::format("Flip turn rate: full within {} of the flip, slowing by {} to the catch.", arguments[1], arguments[2]);
        }
        if (arguments.size() == 2 && arguments[0] == "legs") {
            r.legs_flip = std::atoi(arguments[1].c_str());
            return std::format("Dolphins and dragons play the game's flip {} for the legs.", r.legs_flip);
        }
        if (arguments.size() == 2 && arguments[0] == "feet") {
            r.feet_free = std::clamp(static_cast<float>(std::atof(arguments[1].c_str())), 0.0f, 1.0f);
            return std::format("The feet hold the deck at {:.2f} of the game's strength through a flip.", r.feet_free);
        }
        if (arguments.empty()) return std::format("Flips drop the board {:.2f} m under the feet. usage: tricks dip <metres> | ease <in> <slow>", r.flip_dip);
        r.flip_dip = std::clamp(static_cast<float>(std::atof(arguments[0].c_str())), 0.0f, 0.6f);
        return std::format("Flips drop the board {:.2f} m under the feet.", r.flip_dip);
    }
    if (verb == "turnback") {
        const bool on = arguments.empty() || arguments[0] != "off";
        set_board_turn_back(on);
        return on ? "A board spun round under the feet is turned back to the skater's heading when it lands." : "Turn-back off.";
    }
    if (verb == "watchair") { // <dev>
        r.watch_air = arguments.empty() || arguments[0] != "off";
        r.watch_fine = !arguments.empty() && arguments[0] == "fine";
        return r.watch_air ? "The heading watch logs after every air." : "The heading watch logs after the mod's tricks only.";
    }
    if (verb == "parts") { // <dev>
        request_parts_log();
        return "The physics parts' places are logged at the next held-pose step.";
    }
    if (verb == "outlegs") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks outlegs on|off";
        r.out_legs = arguments[0] == "on";
        return r.out_legs ? "Flick-outs play the game's flip legs." : "Flick-outs keep the pose's exit legs.";
    }
    if (verb == "drives") { // <dev>
        std::uintptr_t core{};
        if (r.rig) (void)copy_rows(r.rig + 0x4630, reinterpret_cast<float *>(&core), 2);
        if (!core) return "No skater.";
        request_drive_dump(core);
        return "The deck world's drive rows go to the log at the next step.";
    }
    if (verb == "flipgrab") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks flipgrab on|off";
        r.flip_grab = arguments[0] == "on";
        return r.flip_grab ? "Flip grab experiment on: press a trigger during a flip." : "Flip grab experiment off.";
    }
    if (verb == "fandango") {
        if (arguments.empty()) return "usage: tricks fandango on|off | any 0|1 | north 0|1";
        if (arguments[0] == "on" || arguments[0] == "off") r.fandango = arguments[0] == "on";
        else if (arguments.size() == 2 && arguments[0] == "family") r.fandango_family = std::atoi(arguments[1].c_str()); // <dev>
        else if (arguments.size() == 2 && arguments[0] == "any") r.fandango_any = arguments[1] == "1"; // <dev> on every grind
        else if (arguments.size() == 2 && arguments[0] == "north") { r.fandango_north = arguments[1] == "1"; r.s_ollie_south = 0; } // <dev> the front foot
        else if (arguments.size() == 2 && arguments[0] == "leg") r.fandango_which = arguments[1] == "r" ? 0 : arguments[1] == "l" ? 1 : -1; // <dev> r | l | auto
        else if (arguments.size() == 2 && arguments[0] == "drives") r.fandango_drives = arguments[1] == "1"; // <dev>
        else if (arguments.size() == 5 && arguments[0] == "foot") { // <dev> up back side (m, the deck's frame) and the leg's motor share
            r.fandango_up = static_cast<float>(std::atof(arguments[1].c_str()));
            r.fandango_back = static_cast<float>(std::atof(arguments[2].c_str()));
            r.fandango_side = static_cast<float>(std::atof(arguments[3].c_str()));
            r.fandango_leg = static_cast<float>(std::atof(arguments[4].c_str()));
        }
        else return "usage: tricks fandango on|off | any 0|1 | north 0|1";
        return std::format("Fandango {} (any grind {}; the back foot {:.2f} up {:.2f} back {:.2f} out, leg {:.2f}; family {}).", r.fandango ? "on" : "off", r.fandango_any ? "yes" : "no",
                           r.fandango_up, r.fandango_back, r.fandango_side, r.fandango_leg, r.fandango_family);
    }
    if (verb == "pregrab") {
        const auto usage = "usage: tricks pregrab on|off | reach 0|1 | place <over m> <up m> | pop <0..1> | gap <ms> | entry <ms> | grab <nose> <tail> | write 0|1 | jump <v|-1> | ff <kick|heel|shuv|fsshuv> <type|-1> | ffms <ms> | status";
        if (arguments.empty()) return usage;
        const auto &a = arguments[0];
        const auto f = [&](std::size_t i) { return static_cast<float>(std::atof(arguments[i].c_str())); };
        if (a == "on" || a == "off") r.pregrab = a == "on";
        else if (a == "reach" && arguments.size() == 2) r.pregrab_reach = arguments[1] == "1";
        else if (a == "place" && arguments.size() == 3) r.pregrab_over = f(1), r.pregrab_up = f(2);
        else if (a == "pop" && arguments.size() == 2) r.pregrab_pop = std::clamp(f(1), 0.0f, 1.0f);
        else if (a == "type" && arguments.size() == 2) r.pregrab_ground_type = std::atoi(arguments[1].c_str());
        else if (a == "crouch" && arguments.size() == 2) r.pregrab_crouch = std::clamp(f(1), 0.0f, 0.6f);
        else if (a == "gap" && arguments.size() == 2) r.pregrab_gap = static_cast<std::uint32_t>(std::max(0, std::atoi(arguments[1].c_str())));
        else if (a == "entry" && arguments.size() == 2) r.pregrab_entry_ms = static_cast<std::uint32_t>(std::max(0, std::atoi(arguments[1].c_str())));
        else if (a == "grab" && arguments.size() == 3) r.pregrab_nose_grab = std::atoi(arguments[1].c_str()), r.pregrab_tail_grab = std::atoi(arguments[2].c_str());
        else if (a == "write" && arguments.size() == 2) r.pregrab_write_grab = arguments[1] == "1";
        else if (a == "jump" && arguments.size() == 2) r.pregrab_jump = f(1);
        else if (a == "ffms" && arguments.size() == 2) r.ff_ms = static_cast<std::uint32_t>(std::clamp(std::atoi(arguments[1].c_str()), 150, 1200));
        else if (a == "ff" && arguments.size() == 3) {
            const auto &w = arguments[1];
            const int k = w == "kick" ? 0 : w == "heel" ? 1 : w == "shuv" ? 2 : w == "fsshuv" ? 3 : -1;
            if (k < 0) return usage;
            r.ff_type[static_cast<std::size_t>(k)] = std::atoi(arguments[2].c_str());
        } else if (a != "status") return usage;
        return std::format("Pre-grab {} (stage {}, {} hand at the {}): reach {}, hand {:.2f} past the truck {:.2f} up, pop {:.2f}, gap {} ms, entry {} ms, air grab {} nose {} tail {}, jump {:.2f}; "
                           "fingerflip types kick {} heel {} shuv {} fsshuv {}, throw {} ms. {}",
                           r.pregrab ? "on" : "off", r.pregrab_stage, r.pregrab_hand ? "left" : "right", r.pregrab_end >= 0 ? "nose" : "tail", r.pregrab_reach ? "on" : "off", r.pregrab_over,
                           r.pregrab_up, r.pregrab_pop, r.pregrab_gap, r.pregrab_entry_ms, r.pregrab_write_grab ? "written" : "the game's", r.pregrab_nose_grab, r.pregrab_tail_grab, r.pregrab_jump,
                           r.ff_type[0], r.ff_type[1], r.ff_type[2], r.ff_type[3], r.ff_ms, take_hand_reach());
    }
    if (verb == "grindgrab") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks grindgrab on|off";
        r.grind_grab = arguments[0] == "on";
        return r.grind_grab ? "Grind grab experiment on: hold a trigger while grinding." : "Grind grab experiment off.";
    }
    if (verb == "antistyle") {
        // tricks antistyle nose|mirror | pitch <degrees>
        if (arguments.size() == 2 && arguments[0] == "pitch") {
            r.anti_pitch = std::clamp(static_cast<float>(std::atof(arguments[1].c_str())), -60.0f, 60.0f);
            return std::format("The anti casper's board goes {:.0f} degrees further nose down.", r.anti_pitch);
        }
        if (arguments.empty() || (arguments[0] != "nose" && arguments[0] != "mirror")) return "usage: tricks antistyle nose|mirror | pitch <degrees>";
        r.anti_nose = arguments[0] == "nose";
        return r.anti_nose ? "The anti casper is a nose manual with the board turned over." : "The anti casper is the casper mirrored and turned.";
    }
    if (verb == "primopop") {
        if (std::uint32_t ms{}; arguments.size() == 2 && arguments[0] == "ms" && number(arguments[1], ms) && ms <= 3000) { // <dev>
            r.primo_pop_hold_ms = ms;
            return std::format("A primo's way out is held {} ms before the pop.", ms);
        }
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off" && arguments[0] != "hop")) return "usage: tricks primopop hop|on|off | ms <n>";
        r.primo_hop_flip = arguments[0] == "hop";
        r.primo_game_pop = arguments[0] == "on";
        return r.primo_hop_flip ? "A primo's flick-out is its own way out's hop with the flick's turn on the board."
             : r.primo_game_pop ? "A primo's flick-out is the game's own ollie and flip, once riding (about 2.5 s)." : "A primo's flick-out goes through the casper.";
    }
    if (verb == "anti") {
        // tricks anti on|off | steady <ms>
        if (arguments.size() == 2 && arguments[0] == "steady") {
            std::uint32_t ms{};
            if (!number(arguments[1], ms) || ms > 2000) return "usage: tricks anti steady <ms>";
            r.anti_steady_ms = ms;
            return std::format("The anti-casper is caught after the stick rests up for {} ms.", ms);
        }
        if (arguments.size() == 2 && arguments[0] == "turn" && (arguments[1] == "on" || arguments[1] == "off")) {
            r.anti_turn = arguments[1] == "on";
            return r.anti_turn ? "The anti-casper turns the skater half round with the mirror." : "The anti-casper is the mirror alone.";
        }
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks anti on|off | steady <ms> | turn on|off";
        r.anti_on = arguments[0] == "on";
        return r.anti_on ? "Anti-casper on: the stick resting up at the catch." : "Anti-casper off.";
    }
    if (verb == "riderframe") {
        // tricks riderframe on|off|watch | feet game|turn|all
        if (arguments.size() == 2 && arguments[0] == "feet" && (arguments[1] == "game" || arguments[1] == "turn" || arguments[1] == "all")) {
            r.rider_feet = arguments[1] == "game" ? 0 : arguments[1] == "turn" ? 1 : 2;
            return std::format("The rider's feet drives in a pose: {}.", arguments[1]);
        }
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off" && arguments[0] != "watch")) return "usage: tricks riderframe on|off|watch | feet game|turn|all";
        r.riderframe = arguments[0] == "on" ? 2 : arguments[0] == "watch" ? 1 : 0;
        return r.riderframe == 2 ? "The rider's drives get an upright deck in a pose." : r.riderframe == 1 ? "The rider's drives are watched in a pose." : "The rider's drives are the game's in a pose.";
    }
    if (verb == "level2") {
        // tricks level2 on|off | delay <ms>: the held pose's target kept level (the older `level` is the slide's pitch knob).
        if (arguments.size() == 2 && arguments[0] == "tilt") {
            // tricks level2 tilt <rise>: above 0 a fixed tilt (the old hold); 0 follows the clip's (the height on the stick).
            set_board_level_tilt(static_cast<float>(std::atof(arguments[1].c_str())));
            return std::atof(arguments[1].c_str()) > 0 ? "The held casper's board keeps that fixed tilt." : "The held casper's board follows the clip's tilt.";
        }
        if (arguments.size() == 2 && arguments[0] == "delay" && number(arguments[1], r.level_delay)) return std::format("A held pose is levelled {} ms after it takes the board.", r.level_delay);
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks level2 on|off | delay <ms>";
        r.level = arguments[0] == "on";
        return r.level ? "A held pose's board target is kept level and on one heading." : "A held pose's board target follows the skater.";
    }
    if (verb == "norunout") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks norunout on|off";
        r.no_runout = arguments[0] == "on";
        return r.no_runout ? "The game's wipeout and run-out checks stand down while a pose is held." : "The game's wipeout and run-out checks run in a pose.";
    }
    if (verb == "snap") {
        // tricks snap <casper tick> <primo tick>: where the way-in clips start (0: at their beginning, as shipped).
        if (arguments.size() != 2) return std::format("The way-in clips start at tick {:.0f} (casper) and {:.0f} (primo). usage: tricks snap <casper tick> <primo tick>", r.snap_casper, r.snap_primo);
        r.snap_casper = std::clamp(static_cast<float>(std::atof(arguments[0].c_str())), 0.0f, 69.0f);
        r.snap_primo = std::clamp(static_cast<float>(std::atof(arguments[1].c_str())), 0.0f, 94.0f);
        float was_casper{}, was_primo{};
        int casper = set_transition_start(r.base, "animation/dingo/transition_onboard.freestyle.conduit_to_onboard.freestyle.casper.into", r.snap_casper, &was_casper);
        int primo = set_transition_start(r.base, "animation/dingo/transition_onboard.freestyle.conduit_to_onboard.freestyle.primo.into", r.snap_primo, &was_primo);
        if (casper == -1) casper = set_transition_start(r.base, "Animation/Dingo/Transition_onboard.freestyle.conduit_to_onboard.freestyle.casper.into", r.snap_casper, &was_casper);
        if (primo == -1) primo = set_transition_start(r.base, "Animation/Dingo/Transition_onboard.freestyle.conduit_to_onboard.freestyle.primo.into", r.snap_primo, &was_primo);
        r.snap_dirty = casper < 0 || primo < 0;
        r.snap_from_start = false;
        r.snap_applied = !r.snap_dirty;
        return std::format("Way-in clips: casper from tick {:.0f} (was mode {}, tick {:.0f}), primo from tick {:.0f} (was mode {}, tick {:.0f}). Below 0: -1 not found, -2 unexpected layout, -3 write failed.",
                           r.snap_casper, casper, was_casper, r.snap_primo, primo, was_primo);
    }
    if (verb == "stomp") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: tricks stomp on|off";
        r.stomp = arguments[0] == "on";
        return r.stomp ? "A caught pose lands straight onto the ground (no bounce)." : "A caught pose lands by the game's own landing check.";
    }
    if (verb == "leftmask") {
        if (arguments.size() == 2 && arguments[0] == "air" && (arguments[1] == "on" || arguments[1] == "off")) {
            r.left_mask_air = arguments[1] == "on";
            return r.left_mask_air ? "The left stick is hidden in the air too while a pose is caught." : "The left stick works in the air while a pose is caught.";
        }
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
    if (verb == "stroke") { // <dev> tricks stroke <flip> [strength] | side: play the stroke the mod pops with
        if (!arguments.empty() && arguments[0] == "side")
            return std::format("Stroke sides: kickflip {:+.0f}, backside shuvit {:+.0f}.", r.kick_side, r.bs_shuv_side);
        std::uint32_t flip{};
        if (arguments.empty() || !number(arguments[0], flip)) return "usage: tricks stroke <flip 0|2|3|4|7|11> [strength 0..1] | side";
        const float strength = arguments.size() > 1 ? static_cast<float>(std::atof(arguments[1].c_str())) : 1.0f;
        play_pop_stroke(r, flip, strength);
        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: stroke for flip {} played (strength {:.2f}).", flip, strength);
        return std::format("Stroke for flip {} played.", flip);
    }
    if (verb == "groundshuv") {
        // tricks groundshuv [fs|bs|on|off|ms <n>]
        if (!arguments.empty() && (arguments[0] == "on" || arguments[0] == "off")) {
            r.ground_shuvs = arguments[0] == "on";
            return r.ground_shuvs ? "Ground shuvits on: riding, the right stick part way down, rolled to a side and let go." : "Ground shuvits off.";
        }
        if (arguments.size() == 2 && (arguments[0] == "jump" || arguments[0] == "speed" || arguments[0] == "game")) { // <dev>
            const float v = static_cast<float>(std::atof(arguments[1].c_str()));
            if (arguments[0] == "jump") r.gs_jump = std::clamp(v, 0.0f, 1.0f);
            else if (arguments[0] == "speed") r.gs_speed = std::clamp(v, 0.5f, 3.0f);
            else r.gs_game = arguments[1] == "on";
            return std::format("Ground shuvit: the game's {} (pop {:.2f}, turn speed {:.2f}).", r.gs_game ? "on" : "off", r.gs_jump, r.gs_speed);
        }
        std::uint32_t ms{};
        if (arguments.size() == 2 && arguments[0] == "gap" && number(arguments[1], ms) && ms <= 1000) { // <dev>
            r.gs_gap = ms;
            return std::format("Ground shuvit: the stroke {} ms after the flick.", ms);
        }
        if (arguments.size() == 2 && arguments[0] == "ms" && number(arguments[1], ms) && ms >= 100 && ms <= 2000) {
            r.gs_ms = ms;
            return std::format("Ground shuvits take {} ms.", ms);
        }
        if (arguments.size() == 2 && arguments[0] == "hop") {
            r.gs_hop = std::clamp(static_cast<float>(std::atof(arguments[1].c_str())), 0.0f, 0.3f);
            return std::format("Ground shuvits lift the board {:.2f} m.", r.gs_hop);
        }
        if (!state_objects_known() || r.gs_phase) return "error: not ready (on the board, and not already turning).";
        // <dev> A quarter turn held for four seconds, for a still of the turned board.
        if (!arguments.empty() && arguments[0] == "quarter") {
            start_board_spin(0, 90.0f, 0, 300);
            r.gs_phase = 1;
            r.gs_until = now + 4000;
            return "Ground shuvit: a quarter turn, held.";
        }
        // </dev>
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
