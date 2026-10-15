#include "tricks_board.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Extension/Skater/client_source_spawn.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace dingosdk::tricks {
void release_board_spin() noexcept;
namespace {
// Game build 20260929. (board, out: 4x4 matrix, flag): copies the target frame at *(board + 0x178) to `out`, optionally
// transforms it, and gives it to the board's drive. The frame's rows are right, up, forward and position.
constexpr std::uintptr_t drive_target = 0x47c3320, board_frame = 0x178;
constexpr unsigned char drive_target_bytes[]{0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x60, 0x48, 0x8b, 0x81, 0x78, 0x01, 0x00, 0x00};
using DriveTarget = void *(*)(std::uintptr_t board, float *out, std::uint8_t flag);
std::atomic<DriveTarget> original{};
// (rig, step length): builds the rig's root frame (rig + 0x4720) from the board. Called by the rig-follow functions.
constexpr std::uintptr_t root_update = 0x47784b0;
constexpr unsigned char root_update_bytes[]{0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x57, 0x48, 0x81, 0xec, 0x40, 0x01, 0x00, 0x00};
using RootUpdate = void (*)(std::uintptr_t rig, float step);
std::atomic<RootUpdate> root_original{};
void root_update_hook(std::uintptr_t rig, float step) {
    freestyle_root_updating(rig);
    if (const auto next = root_original.load(std::memory_order_acquire)) next(rig, step);
    freestyle_root_updated(rig);
}
// The other root update (rig, pointer): writes the same root (rig + 0x4720) on another path. With only the first held
// upright, steps built by this one used the tipped frame and the posed body rocked from one frame to the next (seen
// in slow motion 2026-10-14; the owner's "bounces").
constexpr std::uintptr_t root_update2 = 0x4779330;
constexpr unsigned char root_update2_bytes[]{0x48, 0x8b, 0xc4, 0x48, 0x81, 0xec, 0x28, 0x01, 0x00, 0x00, 0x80, 0xb9, 0xa5, 0x59, 0x00, 0x00, 0x00};
using RootUpdate2 = void (*)(std::uintptr_t rig, void *frame);
std::atomic<RootUpdate2> root2_original{};
std::atomic<std::uint32_t> root2_calls{};
void root_update2_hook(std::uintptr_t rig, void *frame) {
    root2_calls.fetch_add(1, std::memory_order_relaxed);
    freestyle_root_updating(rig);
    if (const auto next = root2_original.load(std::memory_order_acquire)) next(rig, frame);
    freestyle_root_updated(rig);
}
std::atomic<bool> attempted{}, ready{};
std::atomic<std::uintptr_t> local_context{};
std::atomic<std::uint32_t> seen_count{}, turned_count{};

struct Spin {
    std::atomic<bool> on{};
    std::atomic<float> roll{}, yaw{}, pitch{};
    std::atomic<float> drop{}; // metres the board is moved down its own up axis (a pose caught in the air: under the feet)
    std::atomic<float> hop{};  // metres the board is lifted at the middle of the turn (a ground shuvit: just off the ground)
    std::atomic<std::uint64_t> start{}; // QueryPerformanceCounter
    std::atomic<std::uint64_t> length{};
    std::atomic<bool> turned_back{}; // the turn back was asked for (once per spin)
};
Spin spin;
std::atomic<bool> turn_back_on{true};
// The spin's own board: the clip's turn of the board is replaced by the skater's root frame (level, the skater's
// heading), so the game can play a flip clip for the legs while the mod alone turns the board.
std::atomic<std::uintptr_t> base_rig{};
std::atomic<bool> base_root{};
// A flip grab's path (the owner, 2026-10-14: the board flips "in a smooth line from the flick to the waist about a foot in
// front of the waist", is grabbed there and brought back down to the feet): up the skater's up row by `path_rise` and
// out along its side row by `path_reach` (toward the toes), rising until `path_top` of the time, held there to
// `path_hold`, down again by the end; the board's roll and yaw are done by the top.
std::atomic<float> path_rise{}, path_reach{}, path_top{1.0f}, path_hold{1.0f}; // top 1: the turn takes the whole spin
float smooth(float x) noexcept {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
// The board's angular drive to its animated pose: three strengths in the board's drive request (board + 0x7a0, + 0x7b0,
// + 0x794), each multiplied into the deck's AngularDriveLimit (600). Every state enters with 1; the game's own
// boneless uses 1000 and its slappy 30. A posed board on the ground at speed is pushed past the limit of 1 (it rocks,
// and a primo's is knocked flat): while `rigid` is above 0 the three are held at it, and put back to 1 after.
constexpr std::uintptr_t drive_strengths[]{0x7a0, 0x7b0, 0x794};
std::atomic<float> rigid{}, rigid_seen{-1.0f};
std::atomic<bool> rigid_restore{};
std::atomic<std::uint32_t> rigid_writes{};
// <dev> How far the board's target frame (the animated pose on the skater's root) leans while a pose is held: the
// range of the height of its up and right rows. A steady pose keeps both still; a range means the root itself rocks.
std::atomic<bool> lean_on{};
std::atomic<float> lean_up_min{2.0f}, lean_up_max{-2.0f}, lean_right_min{2.0f}, lean_right_max{-2.0f}, lean_forward_min{2.0f}, lean_forward_max{-2.0f};
void track(std::atomic<float> &low, std::atomic<float> &high, float value) noexcept {
    if (value < low.load(std::memory_order_relaxed)) low.store(value, std::memory_order_relaxed);
    if (value > high.load(std::memory_order_relaxed)) high.store(value, std::memory_order_relaxed);
}
// </dev>
// A held pose kept level. The board's target is the animated pose on the skater's root, and the root follows the
// board: on the ground at speed the two tumble together (measured 2026-10-13: a casper's target pitched 50 degrees
// nose up and the skater spun). Once the pose is in, the row of the frame that the pose stands on end (a casper's
// up row points down, a primo's right row points up or down) is set exactly vertical, and the heading is the one the
// pose had when it was taken: the target no longer follows the tumble, and the rigid drive holds the board to it.
std::atomic<float> level_tilt{0.30f};
std::atomic<int> level_mode{}; // 0 off, 1 casper (up row down), 2 primo (right row on end)
int level_axis{-1};            // hook thread only: the latched row
float level_sign{}, level_rise_sign{1.0f}, level_heading[3]{}, level_rise_now{};
std::atomic<bool> level_follow{true};
std::atomic<std::uint32_t> level_steps{};
void level_frame(float *frame) noexcept {
    const int mode = level_mode.load(std::memory_order_relaxed);
    if (!mode) {
        level_axis = -1;
        return;
    }
    float *rows[3]{frame, frame + 4, frame + 8};
    if (level_axis < 0) {
        const int axis = mode == 1 ? 1 : 0;
        const float height = rows[axis][1];
        // Not there yet (the way in is still turning the board): left alone until it is.
        if (mode == 1 ? height > -0.8f : std::abs(height) < 0.8f) return;
        const float *next = rows[(axis + 1) % 3];
        const float length = std::sqrt(next[0] * next[0] + next[2] * next[2]);
        if (!(length > 0.2f)) return;
        level_axis = axis;
        level_rise_now = 0.0f;
        level_sign = height < 0 ? -1.0f : 1.0f;
        // Which end of a casper's board is down, as the clip has it (the mirrored one, the anti-casper, has the other).
        level_rise_sign = rows[2][1] < 0 ? -1.0f : 1.0f;
        level_heading[0] = next[0] / length;
        level_heading[1] = 0.0f;
        level_heading[2] = next[2] / length;
    }
    float *a = rows[level_axis], *b = rows[(level_axis + 1) % 3], *c = rows[(level_axis + 2) % 3];
    // Which way round the frame's rows go (kept as the game has it).
    const float cross_was[3]{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const float hand = cross_was[0] * c[0] + cross_was[1] * c[1] + cross_was[2] * c[2] < 0 ? -1.0f : 1.0f;
    if (level_axis == 1) {
        // A casper does not have its board flat: it stands on one end of it, the forward row 0.30 up (17 degrees;
        // measured on held caspers 2026-10-13). Laid flat, the feet no longer fit and the skater falls onto its back.
        // The pose's own tilt is kept: the forward row at that height along the heading, the right row level.
        // The tilt follows the clip's own (the casper's height on the right stick blends clips whose board stands 0.15
        // to 0.36 up: measured 2026-10-14); with the skater's root held upright the clip's board no longer tumbles.
        // `set_board_level_tilt` with a tilt above 0 holds that fixed tilt instead (the old way); 0 follows the clip.
        float rise = level_tilt.load(std::memory_order_relaxed) * level_rise_sign;
        if (level_follow.load(std::memory_order_relaxed)) {
            const float clip = std::clamp(b[1] * level_rise_sign, 0.05f, 0.6f) * level_rise_sign;
            level_rise_now = level_rise_now == 0.0f ? clip : level_rise_now + (clip - level_rise_now) * 0.2f;
            rise = level_rise_now;
        }
        const float run = std::sqrt(1.0f - rise * rise);
        float *up = a, *forward = b, *right = c;
        forward[0] = level_heading[0] * run; forward[1] = rise; forward[2] = level_heading[2] * run;
        right[0] = level_heading[2]; right[1] = 0.0f; right[2] = -level_heading[0];
        for (int turn = 0; turn < 2; ++turn) {
            up[0] = hand * (forward[1] * right[2] - forward[2] * right[1]);
            up[1] = hand * (forward[2] * right[0] - forward[0] * right[2]);
            up[2] = hand * (forward[0] * right[1] - forward[1] * right[0]);
            if (up[1] < 0) break; // upside down, as a casper's board is
            for (int i = 0; i < 3; ++i) right[i] = -right[i];
        }
    } else {
        a[0] = 0.0f; a[1] = level_sign; a[2] = 0.0f;
        for (int i = 0; i < 3; ++i) b[i] = level_heading[i];
        c[0] = hand * (a[1] * b[2] - a[2] * b[1]);
        c[1] = hand * (a[2] * b[0] - a[0] * b[2]);
        c[2] = hand * (a[0] * b[1] - a[1] * b[0]);
    }
    level_steps.fetch_add(1, std::memory_order_relaxed);
}
void hold_rigid(std::uintptr_t board) noexcept {
    const float strength = rigid.load(std::memory_order_relaxed);
    const bool restore = strength <= 0 && rigid_restore.exchange(false, std::memory_order_relaxed);
    if (strength <= 0 && !restore) return;
    float now[3]{};
    for (int i = 0; i < 3; ++i) {
        now[i] = *reinterpret_cast<const float *>(board + drive_strengths[i]);
        // Not the request this code knows (the three are small, finite and never negative): left alone.
        if (!(now[i] >= 0.0f && now[i] <= 2000.0f)) return;
    }
    if (rigid_seen.load(std::memory_order_relaxed) < 0) rigid_seen.store(now[0], std::memory_order_relaxed);
    for (const auto at : drive_strengths) *reinterpret_cast<volatile float *>(board + at) = restore ? 1.0f : strength;
    if (!restore) {
        rigid_restore.store(true, std::memory_order_relaxed);
        rigid_writes.fetch_add(1, std::memory_order_relaxed);
    }
}
std::uint64_t clock_now() noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<std::uint64_t>(now.QuadPart);
}
std::uint64_t clock_frequency() noexcept {
    static const std::uint64_t value = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<std::uint64_t>(f.QuadPart);
    }();
    return value;
}
// How far a flip has turned at `p` of its time. The retail flips' turn rate (a stock heelflip's deck, measured per step
// 2026-10-14): full speed within the first steps, easing off a little toward the catch, then a dead stop. A constant
// rate looked mechanical; this one starts with the flick's snap and slows as if the air held it.
std::atomic<float> ease_in{0.08f}, ease_slow{0.3f};
float flip_ease(float p) noexcept {
    const float a = std::clamp(ease_in.load(std::memory_order_relaxed), 0.0f, 0.5f), k = std::clamp(ease_slow.load(std::memory_order_relaxed), 0.0f, 0.9f);
    const auto area = [&](float x) { // the integral of the turn rate: rising to 1 over [0, a], then falling by k to the end
        if (x <= a) return a > 0 ? x * x / (2.0f * a) : 0.0f;
        const float t = x - a;
        return a / 2.0f + t - k * t * t / (2.0f * (1.0f - a));
    };
    const float total = area(1.0f);
    return total > 0 ? std::clamp(area(std::clamp(p, 0.0f, 1.0f)) / total, 0.0f, 1.0f) : p;
}
// Turn rows a and b of the frame toward each other by `degrees` (a rotation about the third axis).
void turn(float *a, float *b, float degrees) noexcept {
    const float radians = degrees * 3.14159265358979f / 180.0f, c = std::cos(radians), s = std::sin(radians);
    for (int i = 0; i < 3; ++i) {
        const float x = a[i], y = b[i];
        a[i] = c * x + s * y;
        b[i] = -s * x + c * y;
    }
}
bool turn_frame(std::uintptr_t board) noexcept {
    __try {
        const auto frame = *reinterpret_cast<float **>(board + board_frame);
        const auto context = local_context.load(std::memory_order_relaxed);
        if (!frame || !context || reinterpret_cast<std::uintptr_t>(frame) != context) return false;
        seen_count.fetch_add(1, std::memory_order_relaxed);
        hold_rigid(board);
        // (Watched as the game gives it, before it is levelled: this is what the skater's root is doing.)
        set_freestyle_target_heights(frame[5], frame[1]); // <dev> the root trace's
        if (lean_on.load(std::memory_order_relaxed)) {
            track(lean_right_min, lean_right_max, frame[1]);
            track(lean_up_min, lean_up_max, frame[5]);
            track(lean_forward_min, lean_forward_max, frame[9]);
        }
        level_frame(frame);
        if (!spin.on.load(std::memory_order_acquire)) return false;
        if (base_root.load(std::memory_order_relaxed)) {
            float root[12]{};
            const auto rig = base_rig.load(std::memory_order_relaxed);
            if (rig) {
                std::memcpy(root, reinterpret_cast<const void *>(rig + 0x4720), sizeof(root));
                for (int i = 0; i < 12; ++i)
                    if (i % 4 != 3) frame[i] = root[i];
            }
        }
        const float rise = path_rise.load(std::memory_order_relaxed), reach = path_reach.load(std::memory_order_relaxed);
        const float top = std::clamp(path_top.load(std::memory_order_relaxed), 0.1f, 1.0f), hold_end = std::clamp(path_hold.load(std::memory_order_relaxed), top, 1.0f);
        const auto length = spin.length.load(std::memory_order_relaxed);
        const auto elapsed = clock_now() - spin.start.load(std::memory_order_relaxed);
        float progress = length ? static_cast<float>(static_cast<double>(elapsed) / static_cast<double>(length)) : 1.0f;
        if (progress >= 1.0f) {
            // Whole turns leave the board as the clip has it; a half turn (a shuvit) is held until the spin is stopped.
            progress = 1.0f;
            // A turn about the board's up axis is handed back once it is done, while still in the air: the board turned
            // back to the skater's heading and the spin let go in one step (the game takes the skater off a board that
            // lands turned against the root: measured 2026-10-14, a no comply pop shuvit, physics 504 on the landing).
            if (spin.yaw.load(std::memory_order_relaxed) != 0 && turn_back_on.load(std::memory_order_relaxed) && !spin.turned_back.exchange(true))
                (void)request_board_turn_back(&release_board_spin);
        }
        float *right = frame, *up = frame + 4, *forward = frame + 8;
        const float hop_progress = progress;
        if ((rise != 0 || reach != 0) && hold_end < 1.0f) {
            // Out along the path (the frame is the skater's own here: up row 4, side row 0).
            const float out = progress < top ? smooth(progress / top) : progress < hold_end ? 1.0f : 1.0f - smooth((progress - hold_end) / (1.0f - hold_end));
            for (int i = 0; i < 3; ++i) frame[12 + i] += up[i] * rise * out + right[i] * reach * out;
        }
        if (top < 1.0f) progress = std::min(1.0f, progress / top); // the turn done by the top
        progress = flip_ease(progress);
        const float roll = spin.roll.load(std::memory_order_relaxed) * progress, yaw = spin.yaw.load(std::memory_order_relaxed) * progress,
                    pitch = spin.pitch.load(std::memory_order_relaxed) * progress;
        if (const float drop = spin.drop.load(std::memory_order_relaxed) * progress; drop != 0)
            for (int i = 0; i < 3; ++i) frame[12 + i] -= up[i] * drop;
        if (const float hop = spin.hop.load(std::memory_order_relaxed) * std::sin(hop_progress * 3.14159265358979f); hop != 0)
            for (int i = 0; i < 3; ++i) frame[12 + i] += up[i] * hop;
        if (yaw != 0) turn(forward, right, yaw);   // about up
        if (pitch != 0) turn(up, forward, pitch);  // about right
        if (roll != 0) turn(right, up, roll);      // about forward
        turned_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    } __except (1) { return false; }
}
void *drive_target_hook(std::uintptr_t board, float *out, std::uint8_t flag) {
    (void)turn_frame(board);
    const auto next = original.load(std::memory_order_acquire);
    return next ? next(board, out, flag) : out;
}
bool root_code_known(std::uintptr_t at, const unsigned char *bytes, std::size_t size) noexcept {
    __try {
        return std::memcmp(reinterpret_cast<const void *>(at), bytes, size) == 0;
    } __except (1) { return false; }
}
bool code_known(std::uintptr_t at) noexcept {
    __try {
        return std::memcmp(reinterpret_cast<const void *>(at), drive_target_bytes, sizeof(drive_target_bytes)) == 0;
    } __except (1) { return false; }
}
}

bool start_board_hook(std::uintptr_t base) noexcept {
    if (!base || attempted.exchange(true)) return ready.load(std::memory_order_relaxed);
    if (!code_known(base + drive_target)) return false;
    auto *target = reinterpret_cast<void *>(base + drive_target);
    void *next{};
    if (hook_prepare(target, reinterpret_cast<void *>(&drive_target_hook), &next) != HookOk || !next) return false;
    original.store(reinterpret_cast<DriveTarget>(next), std::memory_order_release);
    if (hook_enable(target) != HookOk) return false;
    ready.store(true, std::memory_order_relaxed);
    // The skater's root updates (a held pose's body kept upright); the board works without them.
    if (root_code_known(base + root_update2, root_update2_bytes, sizeof(root_update2_bytes))) {
        auto *target2 = reinterpret_cast<void *>(base + root_update2);
        void *next2{};
        if (hook_prepare(target2, reinterpret_cast<void *>(&root_update2_hook), &next2) == HookOk && next2) {
            root2_original.store(reinterpret_cast<RootUpdate2>(next2), std::memory_order_release);
            (void)hook_enable(target2);
        }
    }
    __try {
        if (std::memcmp(reinterpret_cast<const void *>(base + root_update), root_update_bytes, sizeof(root_update_bytes)) != 0) return true;
    } __except (1) { return true; }
    auto *root_target = reinterpret_cast<void *>(base + root_update);
    void *root_next{};
    if (hook_prepare(root_target, reinterpret_cast<void *>(&root_update_hook), &root_next) == HookOk && root_next) {
        root_original.store(reinterpret_cast<RootUpdate>(root_next), std::memory_order_release);
        (void)hook_enable(root_target);
    }
    return true;
}
std::uint32_t take_root_update2_calls() noexcept { return root2_calls.exchange(0, std::memory_order_relaxed); }
void set_board_context(std::uintptr_t context) noexcept { local_context.store(context, std::memory_order_relaxed); }
void set_board_spin_path(float rise, float reach, float top, float hold_end) noexcept {
    path_rise.store(rise, std::memory_order_relaxed);
    path_reach.store(reach, std::memory_order_relaxed);
    path_top.store(top, std::memory_order_relaxed);
    path_hold.store(hold_end, std::memory_order_relaxed);
}
void set_board_spin_base(std::uintptr_t rig, bool own) noexcept {
    base_rig.store(rig, std::memory_order_relaxed);
    base_root.store(own && rig, std::memory_order_relaxed);
}
void start_board_spin(float roll, float yaw, float pitch, std::uint32_t milliseconds) noexcept {
    base_root.store(false, std::memory_order_relaxed);
    path_rise.store(0, std::memory_order_relaxed);
    path_reach.store(0, std::memory_order_relaxed);
    path_top.store(1.0f, std::memory_order_relaxed);
    path_hold.store(1.0f, std::memory_order_relaxed);
    spin.on.store(false, std::memory_order_release);
    spin.drop.store(0, std::memory_order_relaxed);
    spin.hop.store(0, std::memory_order_relaxed);
    spin.roll.store(roll, std::memory_order_relaxed);
    spin.yaw.store(yaw, std::memory_order_relaxed);
    spin.pitch.store(pitch, std::memory_order_relaxed);
    spin.length.store(clock_frequency() * milliseconds / 1000, std::memory_order_relaxed);
    spin.turned_back.store(false, std::memory_order_relaxed);
    spin.start.store(clock_now(), std::memory_order_relaxed);
    spin.on.store(true, std::memory_order_release);
}
void release_board_spin() noexcept {
    spin.on.store(false, std::memory_order_release);
    spin.drop.store(0, std::memory_order_relaxed);
}
void stop_board_spin() noexcept {
    // A spin with a turn about the board's up axis left the deck heading another way than the clip's board: let go of
    // it there and the board's drive swings it back round under the feet, and the game takes the skater off the board
    // (measured 2026-10-14: a no comply pop shuvit landed at 180 degrees, then physics 504). The board is turned back
    // to the root's heading in the step the spin is let go (a deck looks the same either way round).
    if (spin.on.load(std::memory_order_acquire) && turn_back_on.load(std::memory_order_relaxed) && spin.yaw.load(std::memory_order_relaxed) != 0 &&
        request_board_turn_back(&release_board_spin)) return;
    release_board_spin();
}
void set_board_flip_ease(float in, float slow) noexcept {
    ease_in.store(in, std::memory_order_relaxed);
    ease_slow.store(slow, std::memory_order_relaxed);
}
void set_board_turn_back(bool on) noexcept { turn_back_on.store(on, std::memory_order_relaxed); }
void set_board_rigid(float strength) noexcept { rigid.store(strength > 0 ? strength : 0.0f, std::memory_order_relaxed); }
std::uint32_t take_board_rigid(float &first_seen) noexcept {
    first_seen = rigid_seen.exchange(-1.0f, std::memory_order_relaxed);
    return rigid_writes.exchange(0, std::memory_order_relaxed);
}
void set_board_level_tilt(float rise) noexcept {
    level_follow.store(!(rise > 0.0f), std::memory_order_relaxed);
    if (rise > 0.0f && rise < 0.9f) level_tilt.store(rise, std::memory_order_relaxed);
}
void set_board_level(int mode) noexcept { level_mode.store(mode, std::memory_order_relaxed); }
std::uint32_t take_board_level() noexcept { return level_steps.exchange(0, std::memory_order_relaxed); }
void set_board_lean_watch(bool on) noexcept { lean_on.store(on, std::memory_order_relaxed); }
bool take_board_lean(float values[6]) noexcept {
    values[0] = lean_right_min.exchange(2.0f); values[1] = lean_right_max.exchange(-2.0f);
    values[2] = lean_up_min.exchange(2.0f); values[3] = lean_up_max.exchange(-2.0f);
    values[4] = lean_forward_min.exchange(2.0f); values[5] = lean_forward_max.exchange(-2.0f);
    return values[0] <= values[1];
}
void set_board_drop(float metres) noexcept { spin.drop.store(metres, std::memory_order_relaxed); }
void set_board_hop(float metres) noexcept { spin.hop.store(metres, std::memory_order_relaxed); }
float board_spin_progress() noexcept {
    if (!spin.on.load(std::memory_order_acquire)) return -1.0f;
    const auto length = spin.length.load(std::memory_order_relaxed);
    const auto elapsed = clock_now() - spin.start.load(std::memory_order_relaxed);
    return length ? std::min(1.0f, static_cast<float>(static_cast<double>(elapsed) / static_cast<double>(length))) : 1.0f;
}
bool board_spin_running() noexcept {
    if (!spin.on.load(std::memory_order_acquire)) return false;
    return clock_now() - spin.start.load(std::memory_order_relaxed) < spin.length.load(std::memory_order_relaxed);
}
void board_hook_counts(std::uint32_t &seen, std::uint32_t &turned) noexcept {
    seen = seen_count.exchange(0, std::memory_order_relaxed);
    turned = turned_count.exchange(0, std::memory_order_relaxed);
}
} // namespace dingosdk::tricks
