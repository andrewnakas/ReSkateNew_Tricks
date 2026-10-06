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
// The trainer's revert boost: speed given back on a landing the game auto reverts (client_noclip.cpp).
// `strength` multiplies the boost (1: about +1.5 to +2.6 m/s a landing), 0 switches it off. Call
// every tick: it lapses half a second after the last call.
// What counts as a revert and what it is worth. The defaults are AutoRevertBoost's, plus the
// landing out of line with the travel (see client_noclip.cpp).
struct RevertTuning {
    float min_spin{90.0f};   // degrees the skater must have turned in the air
    float min_slip{12.0f};   // degrees the board must land out of line with the direction of travel, or
    float min_twist{40.0f};  // degrees the board must land out of line with the body (a board bend)
    float bend_boost{2.0f};  // m/s for a landing whose board turned 40 degrees past the body, rising to
    float full_boost{4.0f};  // m/s at 360 degrees
    float auto_boost{1.5f};  // m/s once the board turned more than 140 degrees past the body (an auto revert)
    float max_speed{40.0f};  // m/s: nothing is added above this
    float cooldown{0.5f};    // seconds between boosts
    float min_air{0.25f};    // seconds a flight must last
    bool operator==(const RevertTuning &) const = default;
};
void set_revert_boost(std::uintptr_t client, std::uintptr_t entity, float strength, const RevertTuning &tuning = {}) noexcept;
// The last landing the revert boost judged, for the trainer's log: what it measured and what it did.
struct RevertBoostReport {
    std::uint64_t sequence{}; // one more per landing judged; 0: none yet
    float spin{}, board_offset{}, board_rotation{}; // degrees
    float slip{};                                   // degrees the board landed out of line with the direction of travel
    float speed{}, added{};                         // m/s
    std::uint32_t air_ms{}, landed_state{};
    bool board_read{};
    const char *outcome = ""; // "boost", or why not
};
RevertBoostReport revert_boost_report() noexcept;
// Engine-thread-only interactive controls. Presentation callbacks only queue requests.
overlay::DebugModel on_client_debug_tick(std::uintptr_t base, std::uintptr_t client,
    bool can_control, bool camera_phase_observed, const overlay::DebugRequest* request = nullptr,
    const overlay::FlightInput* flight_input = nullptr);
bool restore_client_debug(std::uintptr_t base, std::uintptr_t client, bool camera_phase_observed);
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
