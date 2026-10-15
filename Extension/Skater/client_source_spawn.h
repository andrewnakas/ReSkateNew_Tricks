#pragma once
#include <cstdint>
#include <array>
#include <string>
#include "Extension/UI/Overlay/overlay.h"

namespace dingosdk {
// Binds the native camera functions the debug controls use. Installs no hooks.
void initialize_client_source_spawn(std::uintptr_t base);
// Installs the exact camera-reset handler guard; forwarding is unchanged until a Noclip request is queued.
bool start_client_noclip_velocity(std::uintptr_t base) noexcept;
// Trainer: at the local skater's next physics update, multiply its upward velocity by
// `factor` if it is off the board and rising (a hippy jump that has just started). One request
// at a time; it expires after 150 ms.
bool queue_jump_scale(std::uintptr_t client, std::uintptr_t entity, float factor) noexcept;
// What the last request did: 0 pending or none, 2 scaled, -1 the skater was not rising yet,
// -2 nothing to scale (on the board, or a state that keeps no velocity), -3 it failed.
struct JumpScaleResult {
    int outcome{};
    float up_speed{}; // before scaling
};
JumpScaleResult take_jump_scale_result() noexcept;
// Trainer: the local skater's push speed as a multiple of the game's own (1 leaves the skater
// alone); `stock` is the game's top pushing speed in m/s. `cruise` above 0 is auto push: a
// rolling skater that is not braking gains speed up to it (m/s). Publish every client tick: it
// expires after 500 ms.
void set_push_speed(std::uintptr_t client, std::uintptr_t entity, float factor, float stock, float cruise) noexcept;
// ReSkate Tricks: while the skater holds a casper or primo (physics state 103), take `deceleration` m/s off its
// speed over the ground every second. Publish every client tick: it expires after 500 ms.
void set_freestyle_slide(std::uintptr_t client, std::uintptr_t entity, float deceleration) noexcept;
// Whether the slide also keeps the board from pitching up under the ground's friction (on by default).
void set_freestyle_slide_level(bool level) noexcept;
// ReSkate Tricks: for this long from a slide's start no part moves up or down (a pose landed from the air).
void set_freestyle_slide_soft(std::uint32_t milliseconds) noexcept;
// ReSkate Tricks: the slide holds the mean speed of the parts, not the first part's (a board turning under the feet).
void set_freestyle_slide_mean(bool mean) noexcept;
// ReSkate Tricks: called at the end of every physics step (physics thread), nullptr for none.
void set_tricks_step(void (*step)() noexcept) noexcept;
// ReSkate Tricks: the pose is in and held: the board's tipping (every turn but about the vertical) is taken out each step.
void set_freestyle_slide_hold(bool held) noexcept;
void set_freestyle_slide_stable(bool stable) noexcept;
// The last slide's rocking, and reset: the peak turn rate about the way it travels (rad/s) and the widest swing (degrees).
void take_freestyle_slide_roll(float& peak_rate, float& swing) noexcept;
void set_freestyle_slide_upright(float strength) noexcept;
// The skater's root frame while a pose slides: 0 the game's, 1 watched, 2 held upright on the heading it stood with.
void set_freestyle_root_upright(int mode) noexcept;
// The rider's five drives to the deck while a pose is held: mode 0 the game's, 1 watched, 2 given an upright deck (the
// deck's own place and heading, standing up). `feet`: 0 the feet's rows left alone, 1 their orientation turned too,
// 2 their points as well.
void set_freestyle_rider_drives(int mode, int feet) noexcept;
// <dev> The rows as sampled over the last poses, and how many rows were written / found set again by the game.
std::string take_freestyle_rider_drives(std::uint32_t& writes, std::uint32_t& regame) noexcept;
// Called before the game's root update of a rig (any skater's): the frame the local skater's root is built from is
// set upright if asked. How many times it was, since the last call.
void freestyle_root_updating(std::uintptr_t rig) noexcept;
// <dev> also: the lowest height its up row had before it was set, the mean of (1 - that height), and the calls seen.
std::uint32_t take_freestyle_root_source(float& lowest, float& mean_tip, std::uint32_t& calls) noexcept;
// The local skater's root turned half round about the vertical (the anti-casper, with the game's mirror); `rig` is the
// local skater's. How many root builds it turned since the last call.
void set_freestyle_root_turn(std::uintptr_t rig, bool on) noexcept;
// The local skater's root held upright in any physics state (a pose's way out, through its air), with the mode set.
void set_freestyle_root_hold(std::uintptr_t rig, bool on) noexcept;
// <dev> Logs every physics part's place in the skater's root frame at the next held-pose step.
void request_parts_log() noexcept;
// The local skater's heading held as it was when the lock began, while the mod spins the board under the feet.
void set_freestyle_root_heading_lock(std::uintptr_t rig, bool on) noexcept;
// The local skater (client, entity) whose board a released board spin turns back.
void set_board_turn_owner(std::uintptr_t client, std::uintptr_t entity) noexcept;
// At the next physics step: the board's nine parts turned about the vertical through the deck so the deck heads the way
// the skater's root does (a board spun half round under the feet ends as the clip has it, with no swing back: a deck
// looks the same either way round), when they are more than 60 degrees apart; then `after` is called (in the same
// step, or once the request times out). False when no owner is known (then `after` is not called).
bool request_board_turn_back(void (*after)() noexcept) noexcept;
// The local skater's feet (skater core `core`) let go of the deck for the next `milliseconds`: their rider-to-deck
// drives' force limits times `scale`; put back after. 0 ms puts them back at the next step.
void set_feet_free(std::uintptr_t core, std::uint32_t milliseconds, float scale) noexcept;
// <dev> Rows written since the last call.
std::uint32_t take_feet_free_writes() noexcept;
// <dev> How many turn-backs were done since the last call, and the last angle turned (degrees).
std::uint32_t take_board_turn_backs(float& last) noexcept;
// <dev> The deck's physics heading against the root's (degrees) and the deck's up row height, measured each step while
// this is called (false on the first call).
bool watch_board_heading(float& angle, float& up) noexcept;
// <dev> The deck's turn rate at the last watched step, degrees a second.
float watched_board_spin() noexcept;
std::uint32_t take_freestyle_root_turned() noexcept;
// Called after the game's root update of a rig (any skater's): the local skater's root is set upright if asked.
void freestyle_root_updated(std::uintptr_t rig) noexcept;
// <dev> Samples of a held pose's first seconds on the ground: ms, root up row height, board height, board speed up,
// root side and forward row heights.
std::uint32_t take_freestyle_root_trace(std::array<std::array<float, 6>, 64>& samples) noexcept;
// The board's target frame as the game gave it (before the mod turned it): the height of its up and right rows.
void set_freestyle_target_heights(float up, float right) noexcept;
// </dev>
// Steps seen since the last call (and of them how many were set upright), and the range of the root's up row height.
std::uint32_t take_freestyle_root_upright(float& low, float& high, std::uint32_t& fixed) noexcept;
// Engine-thread-only interactive controls. Presentation callbacks only queue requests.
overlay::DebugModel on_client_debug_tick(std::uintptr_t base, std::uintptr_t client,
    bool can_control, bool camera_phase_observed, const overlay::DebugRequest* request = nullptr,
    const overlay::FlightInput* flight_input = nullptr);
bool restore_client_debug(std::uintptr_t base, std::uintptr_t client, bool camera_phase_observed);
bool client_free_camera_active() noexcept;
// Native camera lease for the party's Spectate action. Null restores only the
// camera acquired by this function; it never changes another debug camera. A positive
// `fov` is applied to the spectate camera while it is held; its own comes back after.
bool update_party_camera(std::uintptr_t base, std::uintptr_t client, bool ready, bool phase,
    const std::array<float, 16>* transform, std::string& detail, float fov = 0) noexcept;
bool read_local_camera_transform(std::uintptr_t base, std::uintptr_t client, std::array<float, 16>& transform) noexcept;
// Publishes the local view's camera (Engine/Game/UI/game_view.h) for things drawn over the
// world. Client thread, every tick while something needs it.
bool publish_local_camera_view(std::uintptr_t base, std::uintptr_t client) noexcept;
}
