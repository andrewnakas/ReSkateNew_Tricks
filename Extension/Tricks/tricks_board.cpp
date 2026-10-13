#include "tricks_board.h"
#include "Engine/Core/Hooks/hooks.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>

namespace dingosdk::tricks {
namespace {
// Game build 20260929. (board, out: 4x4 matrix, flag): copies the target frame at *(board + 0x178) to `out`, optionally
// transforms it, and gives it to the board's drive. The frame's rows are right, up, forward and position.
constexpr std::uintptr_t drive_target = 0x47c3320, board_frame = 0x178;
constexpr unsigned char drive_target_bytes[]{0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x60, 0x48, 0x8b, 0x81, 0x78, 0x01, 0x00, 0x00};
using DriveTarget = void *(*)(std::uintptr_t board, float *out, std::uint8_t flag);
std::atomic<DriveTarget> original{};
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
};
Spin spin;
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
        if (!spin.on.load(std::memory_order_acquire)) return false;
        const auto length = spin.length.load(std::memory_order_relaxed);
        const auto elapsed = clock_now() - spin.start.load(std::memory_order_relaxed);
        float progress = length ? static_cast<float>(static_cast<double>(elapsed) / static_cast<double>(length)) : 1.0f;
        if (progress >= 1.0f) {
            // Whole turns leave the board as the clip has it; a half turn (a shuvit) is held until the spin is stopped.
            progress = 1.0f;
        }
        float *right = frame, *up = frame + 4, *forward = frame + 8;
        const float roll = spin.roll.load(std::memory_order_relaxed) * progress, yaw = spin.yaw.load(std::memory_order_relaxed) * progress,
                    pitch = spin.pitch.load(std::memory_order_relaxed) * progress;
        if (const float drop = spin.drop.load(std::memory_order_relaxed) * progress; drop != 0)
            for (int i = 0; i < 3; ++i) frame[12 + i] -= up[i] * drop;
        if (const float hop = spin.hop.load(std::memory_order_relaxed) * std::sin(progress * 3.14159265358979f); hop != 0)
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
    return true;
}
void set_board_context(std::uintptr_t context) noexcept { local_context.store(context, std::memory_order_relaxed); }
void start_board_spin(float roll, float yaw, float pitch, std::uint32_t milliseconds) noexcept {
    spin.on.store(false, std::memory_order_release);
    spin.drop.store(0, std::memory_order_relaxed);
    spin.hop.store(0, std::memory_order_relaxed);
    spin.roll.store(roll, std::memory_order_relaxed);
    spin.yaw.store(yaw, std::memory_order_relaxed);
    spin.pitch.store(pitch, std::memory_order_relaxed);
    spin.length.store(clock_frequency() * milliseconds / 1000, std::memory_order_relaxed);
    spin.start.store(clock_now(), std::memory_order_relaxed);
    spin.on.store(true, std::memory_order_release);
}
void stop_board_spin() noexcept {
    spin.on.store(false, std::memory_order_release);
    spin.drop.store(0, std::memory_order_relaxed);
}
void set_board_drop(float metres) noexcept { spin.drop.store(metres, std::memory_order_relaxed); }
void set_board_hop(float metres) noexcept { spin.hop.store(metres, std::memory_order_relaxed); }
bool board_spin_running() noexcept {
    if (!spin.on.load(std::memory_order_acquire)) return false;
    return clock_now() - spin.start.load(std::memory_order_relaxed) < spin.length.load(std::memory_order_relaxed);
}
void board_hook_counts(std::uint32_t &seen, std::uint32_t &turned) noexcept {
    seen = seen_count.exchange(0, std::memory_order_relaxed);
    turned = turned_count.exchange(0, std::memory_order_relaxed);
}
} // namespace dingosdk::tricks
