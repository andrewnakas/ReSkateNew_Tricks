#include "trainer_jump.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Skater/no_bail.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <intrin.h>

namespace dingosdk::trainer {
namespace {
// Game build 25414733. void set_trajectory(trajectory, parameters): the launch velocity is
// the vector at parameters + 0x80. Measured with a scripted controller:
//  - a no comply calls it from 0x482f646 on entering the pop (state 200) after a crouch
//    (state 103) of about 0.20 s; an ollie crouches for 0.08 s and launches elsewhere;
//  - a boneless calls it from 0x47691f8 while the foot is planted (state 602).
constexpr std::uintptr_t set_trajectory = 0x4752aa0, no_comply_call = 0x482f646, boneless_call = 0x47691f8;
constexpr std::array<unsigned char, 32> set_trajectory_prefix{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0x88, 0xbb, 0x00,
                                                              0x00, 0x00, 0x48, 0x8b, 0xd9, 0x0f, 0x85, 0xf6, 0x00, 0x00, 0x00,
                                                              0xc5, 0xfc, 0x28, 0x05, 0x62, 0x7d, 0xe8, 0x01, 0xc5, 0xfc};
// An ollie (riding or out of a manual) launches from 0x4824f7f with the state 103 after 100.
constexpr std::uintptr_t ollie_call = 0x4824f7f;
constexpr std::array<unsigned char, 5> ollie_call_bytes{0xe8, 0x1c, 0xdb, 0xf2, 0xff};
constexpr std::array<unsigned char, 5> no_comply_call_bytes{0xe8, 0x55, 0x34, 0xf2, 0xff}, boneless_call_bytes{0xe8, 0xa3, 0x98, 0xfe, 0xff};
constexpr std::uint32_t crouch_state = 103, pop_state = 200, plant_state = 602;
constexpr float no_comply_crouch = 0.12f; // seconds: between an ollie's crouch and a no comply's
constexpr std::size_t velocity_y = 0x84 / sizeof(float);

using SetTrajectory = void (*)(std::uintptr_t, float *);
struct Hooked {
    std::atomic<SetTrajectory> original{};
    std::uintptr_t base{};
    std::atomic<float> no_comply{1}, boneless{1}, manual{1};
    std::atomic<std::uint64_t> manual_until{}; // GetTickCount64: the skater was in a manual until about then
    std::atomic<std::int32_t> manual_intent{-1};
    bool ollie_known{};
    // The last few launches, for the trainer's log line: written by the physics thread, read
    // by the client tick.
    std::array<std::atomic<std::uint64_t>, 8> seen{};
    std::atomic<std::uint32_t> written{}, read{};
    bool attempted{}, ready{};
// <dev>
    // Every call of the setter (caller, the local skater's state and the one before, upward speed).
    struct Call { std::atomic<std::uint64_t> where{}, speed{}; };
    std::array<Call, 32> calls{};
    std::atomic<std::uint32_t> call_count{};
    std::array<float, 64> last_parameters{}; // of the newest call (a torn copy is fine for looking)
    std::array<float, 96> last_trajectory{}; // the trajectory object after that call
    std::atomic<float> test_scale{1};        // multiplies the upward speed of the ollie launch (caller 0x4824f84)
    std::atomic<int> test_slot{-1};          // a float of the parameters to overwrite at every call ...
    std::atomic<float> test_value{};         // ... with this
// </dev>
};
Hooked &hooked() {
    static auto *value = new Hooked;
    return *value;
}
void set_trajectory_hook(std::uintptr_t trajectory, float *parameters) {
    auto &h = hooked();
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - h.base;
// <dev>
    if (parameters) {
        const auto watch = watched_physics_state();
        auto &call = h.calls[h.call_count.fetch_add(1, std::memory_order_relaxed) % h.calls.size()];
        std::uint32_t bits[3];
        std::memcpy(&bits[0], &parameters[velocity_y], 4);
        std::memcpy(&bits[1], &parameters[velocity_y - 1], 4);
        std::memcpy(&bits[2], &parameters[velocity_y + 1], 4);
        call.where.store((std::uint64_t{caller & 0xffffffffu} << 32) | (std::uint64_t{watch.valid ? watch.state & 0xffffu : 0xffffu} << 16) |
                         (watch.valid ? watch.previous & 0xffffu : 0xffffu), std::memory_order_relaxed);
        call.speed.store((std::uint64_t{bits[0]} << 32) | bits[1], std::memory_order_relaxed);
        std::memcpy(h.last_parameters.data(), parameters, sizeof(float) * h.last_parameters.size());
        if (caller == 0x4824f84) parameters[velocity_y] *= h.test_scale.load(std::memory_order_relaxed);
        if (const int slot = h.test_slot.load(std::memory_order_relaxed); slot >= 0 && slot < 64)
            parameters[slot] = h.test_value.load(std::memory_order_relaxed);
    }
// </dev>
    if (parameters && (caller == no_comply_call + 5 || caller == boneless_call + 5)) {
        const auto watch = watched_physics_state(); // the local skater's
        Trick trick{};
        if (caller == boneless_call + 5 && watch.valid && watch.state == plant_state) trick = Trick::boneless;
        else if (caller == no_comply_call + 5 && watch.valid && watch.state == pop_state && watch.previous == crouch_state &&
                 watch.previous_seconds >= no_comply_crouch)
            trick = Trick::no_comply;
        const float up = parameters[velocity_y];
        // Any launch that still goes up: riding downhill takes most of the upward speed away, and
        // those launches were being left alone.
        if (trick != Trick::none && std::isfinite(up) && up > 0.05f) {
            const float factor = (trick == Trick::boneless ? h.boneless : h.no_comply).load(std::memory_order_relaxed);
            if (factor != 1 && up * factor < 200.0f) parameters[velocity_y] = up * factor;
            std::uint32_t bits[2];
            std::memcpy(&bits[0], &up, 4);
            std::memcpy(&bits[1], &factor, 4);
            // 31 bits of each float (both positive) and the trick in the top bit.
            const auto packed = (std::uint64_t{bits[0]} << 32) | (std::uint64_t{bits[1]} << 1) | (trick == Trick::boneless ? 1u : 0u);
            h.seen[h.written.fetch_add(1, std::memory_order_relaxed) % h.seen.size()].store(packed, std::memory_order_release);
        }
    }
    // The ollie's launch, out of a manual (the game pops lower from one; see set_manual_pop).
    if (parameters && h.ollie_known && caller == ollie_call + 5 && GetTickCount64() < h.manual_until.load(std::memory_order_relaxed)) {
        const float up = parameters[velocity_y], factor = h.manual.load(std::memory_order_relaxed);
        if (factor != 1 && std::isfinite(up) && up > 0.05f && up * factor < 200.0f) parameters[velocity_y] = up * factor;
    }
    if (const auto original = h.original.load(std::memory_order_acquire)) original(trajectory, parameters);
// <dev>
    if (trajectory) std::memcpy(h.last_trajectory.data(), reinterpret_cast<const void *>(trajectory), sizeof(float) * h.last_trajectory.size());
    if (const int slot = h.test_slot.load(std::memory_order_relaxed) - 100; trajectory && slot >= 0 && slot < 96)
        reinterpret_cast<float *>(trajectory)[slot] = h.test_value.load(std::memory_order_relaxed);
// </dev>
}
bool known_bytes(std::uintptr_t at, const unsigned char *bytes, std::size_t size) {
    __try {
        return std::memcmp(reinterpret_cast<const void *>(at), bytes, size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool known_code(std::uintptr_t base) {
    __try {
        return std::memcmp(reinterpret_cast<const void *>(base + set_trajectory), set_trajectory_prefix.data(), set_trajectory_prefix.size()) == 0 &&
               std::memcmp(reinterpret_cast<const void *>(base + no_comply_call), no_comply_call_bytes.data(), no_comply_call_bytes.size()) == 0 &&
               std::memcmp(reinterpret_cast<const void *>(base + boneless_call), boneless_call_bytes.data(), boneless_call_bytes.size()) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool start_trick_heights(std::uintptr_t base) noexcept {
    auto &h = hooked();
    if (h.attempted || !base) return h.ready;
    h.attempted = true;
    try {
        if (!known_code(base)) {
            logging::write(logging::Level::warning, logging::Channel::skater,
                           "Trainer: this game build's trick code is not the known one; no comply and boneless heights are off.");
            return false;
        }
        h.base = base;
        h.ollie_known = known_bytes(base + ollie_call, ollie_call_bytes.data(), ollie_call_bytes.size());
        auto *target = reinterpret_cast<void *>(base + set_trajectory);
        void *original{};
        if (hook_prepare(target, reinterpret_cast<void *>(&set_trajectory_hook), &original) != HookOk) return false;
        if (!original) {
            (void)hook_remove(target);
            return false;
        }
        h.original.store(reinterpret_cast<SetTrajectory>(original), std::memory_order_release);
        if (hook_enable(target) != HookOk) return false;
        h.ready = true;
        logging::write(logging::Level::info, logging::Channel::skater, "Trainer: no comply and boneless heights ready.");
    } catch (...) {}
    return h.ready;
}
void set_trick_heights(float no_comply, float boneless) noexcept {
    const auto speed = [](float height) { return std::isfinite(height) && height > 0 ? std::sqrt(std::min(height, 1.0e6f)) : 1.0f; };
    hooked().no_comply.store(speed(no_comply), std::memory_order_relaxed);
    hooked().boneless.store(speed(boneless), std::memory_order_relaxed);
}
// <dev>
std::string trajectory_calls() {
    auto &h = hooked();
    const auto count = h.call_count.exchange(0, std::memory_order_relaxed);
    std::string text = std::to_string(count) + " calls (manual intent " + std::to_string(h.manual_intent.load()) + ", ollie call " +
                       (h.ollie_known ? "known" : "NOT known") + "):";
    for (std::uint32_t i = 0; i < count && i < h.calls.size(); ++i) {
        const auto where = h.calls[i].where.load(std::memory_order_relaxed), speed = h.calls[i].speed.load(std::memory_order_relaxed);
        const std::uint32_t bits[2]{static_cast<std::uint32_t>(speed >> 32), static_cast<std::uint32_t>(speed)};
        float up{}, x{};
        std::memcpy(&up, &bits[0], 4);
        std::memcpy(&x, &bits[1], 4);
        char line[96];
        std::snprintf(line, sizeof line, " [%llx st %u<%u up %.2f x %.2f]", static_cast<unsigned long long>(where >> 32),
                      static_cast<unsigned>((where >> 16) & 0xffff), static_cast<unsigned>(where & 0xffff), up, x);
        text += line;
    }
    return text;
}
std::string trajectory_parameters() {
    auto &h = hooked();
    std::string text;
    const auto list = [&](const float *values, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            char item[32];
            std::snprintf(item, sizeof item, "%s%.4g", i % 4 ? " " : (i ? " | " : ""), static_cast<double>(values[i]));
            text += item;
        }
    };
    list(h.last_parameters.data(), h.last_parameters.size());
    text += " || object: ";
    list(h.last_trajectory.data(), h.last_trajectory.size());
    return text;
}
void trajectory_test(float scale, int slot, float value) noexcept {
    hooked().test_scale.store(scale, std::memory_order_relaxed);
    hooked().test_value.store(value, std::memory_order_relaxed);
    hooked().test_slot.store(slot, std::memory_order_relaxed);
}
// </dev>
void set_manual_pop(float height, std::int32_t intent) noexcept {
    auto &h = hooked();
    h.manual.store(std::isfinite(height) && height > 0 ? std::sqrt(std::min(height, 1.0e6f)) : 1.0f, std::memory_order_relaxed);
    h.manual_intent.store(intent, std::memory_order_relaxed);
    // The intent goes back to none as the stick is pulled for the pop: the launch comes within half a second.
    if (intent > 0) h.manual_until.store(GetTickCount64() + 600, std::memory_order_relaxed);
}
bool take_trick_launch(TrickLaunch &out) noexcept {
    auto &h = hooked();
    const auto written = h.written.load(std::memory_order_acquire);
    auto read = h.read.load(std::memory_order_relaxed);
    if (read == written) return false;
    if (written - read > h.seen.size()) read = written - static_cast<std::uint32_t>(h.seen.size());
    const auto packed = h.seen[read % h.seen.size()].load(std::memory_order_acquire);
    h.read.store(read + 1, std::memory_order_relaxed);
    const std::uint32_t bits[2]{static_cast<std::uint32_t>(packed >> 32), static_cast<std::uint32_t>(packed >> 1) & 0x7fffffffu};
    out.trick = packed & 1 ? Trick::boneless : Trick::no_comply;
    std::memcpy(&out.up_speed, &bits[0], 4);
    std::memcpy(&out.factor, &bits[1], 4);
    return true;
}
} // namespace dingosdk::trainer
