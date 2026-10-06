#include "trainer_camera.h"
#include "Engine/Core/Log/logging.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

namespace dingosdk::trainer {
namespace {
// Game build 25414733. A camera framing set (CameraFramingSetAsset) holds its framings twice:
// as separate objects, which the running camera does not read, and compiled into its root as an
// array sorted by pitch, which it reads every frame (measured: writing the array moves the
// camera at once). One array element is 0x50 bytes: +0x00 offset x y z (right, up, back),
// +0x40 the camera pitch in radians that the framing is for.
// The root itself starts with the word below and holds, at +0x1c, the point the framings turn
// about (x y z), with its pitch limits 89 and 90 a little further on.
constexpr std::size_t element_size = 0x50, element_pitch = 0x40;
constexpr std::uint32_t root_word = 0x0006b100;
constexpr std::size_t pivot_offset = 0x1c, root_span = 0x90;
constexpr float degrees = 0.01745329252f;
struct Element {
    float x, y, z, pitch; // pitch in degrees
};
struct Set {
    CameraRig rig;
    std::size_t count;
    Element elements[3];
};
constexpr Set sets[]{
    {CameraRig::low, 3, {{-1.35f, -0.1f, 2.0f, -45.0f}, {-1.35f, 0.0f, 1.9f, 0.0f}, {-1.35f, 0.2f, 2.0f, 45.0f}}}, // also the mega park's
    {CameraRig::high, 1, {{-1.0f, 0.0f, 3.0f, -12.0f}}},
    {CameraRig::high, 3, {{-1.0f, 0.0f, 3.0f, -75.0f}, {-1.0f, 0.0f, 3.0f, 0.0f}, {-1.0f, 0.0f, 3.0f, 75.0f}}}, // mega park
    {CameraRig::foot, 3, {{0.0f, 0.75f, 2.5f, -75.0f}, {0.0f, 0.0f, 3.0f, 0.0f}, {0.0f, 0.45f, 1.25f, 75.0f}}},
};
struct Pivot {
    CameraRig rig;
    float y;
};
constexpr Pivot pivots[]{{CameraRig::low, 0.85f}, {CameraRig::high, 1.0f}, {CameraRig::high, 1.2f}};
struct Found {
    std::uintptr_t address{}; // of the three numbers
    CameraRig rig{};
    bool pivot{};
    std::array<float, 3> stock{};
    float pitch{}; // radians: what a framing must still hold beside its numbers
};
struct State {
    std::mutex mutex;
    std::array<RigSetting, camera_rigs> wanted{};
    std::vector<Found> found;
    std::atomic<bool> searching{};
    std::uint64_t next_search{};
    int searches{};
};
State &state() {
    static auto *value = new State;
    return *value;
}
bool about(float a, float b) { return std::abs(a - b) < 2e-4f; }
template <class T> bool peek(std::uintptr_t address, T &value) noexcept {
    __try {
        std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool put(std::uintptr_t address, float value) noexcept {
    __try {
        *reinterpret_cast<volatile float *>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool element_at(const float *f, const Element &e) noexcept {
    return about(f[0], e.x) && about(f[1], e.y) && about(f[2], e.z) && about(f[element_pitch / 4], e.pitch * degrees);
}
// One hit: what is at word `i` of a region, if anything known. Plain data only (it runs under __try).
struct Hit {
    const Set *set{};
    const Pivot *pivot{};
};
Hit classify(const std::uint32_t *words, const float *floats, std::size_t i) noexcept {
    const auto first = words[i];
    // The first number of every array is -1.35, -1 or 0; of a root, its word.
    if (first == 0xbfaccccd || first == 0xbf800000 || first == 0) {
        if (floats[i + 2] < 1.0f || floats[i + 2] > 3.5f) return {}; // no array's first distance
        for (const auto &set : sets) {
            bool all = true;
            for (std::size_t k = 0; k < set.count && all; ++k) all = element_at(floats + i + k * element_size / 4, set.elements[k]);
            if (all) return {&set, nullptr};
        }
    } else if (first == root_word && floats[i + pivot_offset / 4] == 0.0f && floats[i + pivot_offset / 4 + 2] == 0.0f) {
        for (const auto &known : pivots) {
            if (!about(floats[i + pivot_offset / 4 + 1], known.y)) continue;
            for (std::size_t k = pivot_offset / 4 + 3; k + 1 < root_span / 4; ++k)
                if (floats[i + k] == 89.0f && floats[i + k + 1] == 90.0f) return {nullptr, &known};
            return {};
        }
    }
    return {};
}
// Fills `hits` (word index and what it is) for one region; returns how many.
std::size_t search_region(std::uintptr_t start, std::size_t size, std::size_t *indices, Hit *hits, std::size_t capacity) noexcept {
    std::size_t count{};
    __try {
        const auto *words = reinterpret_cast<const std::uint32_t *>(start);
        const auto *floats = reinterpret_cast<const float *>(start);
        const std::size_t total = size / 4, tail = 3 * element_size / 4 + root_span / 4;
        for (std::size_t i = 0; i + tail < total && count < capacity; ++i) {
            const auto first = words[i];
            if (first != 0xbfaccccd && first != 0xbf800000 && first != 0 && first != root_word) continue;
            if (const auto hit = classify(words, floats, i); hit.set || hit.pivot) {
                indices[count] = i;
                hits[count++] = hit;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}
void search() {
    auto &s = state();
    std::vector<Found> fresh;
    const auto started = GetTickCount64();
    MEMORY_BASIC_INFORMATION info{};
    std::array<std::size_t, 64> indices{};
    std::array<Hit, 64> hits{};
    for (std::uintptr_t at = 0x10000; at < 0x7fffffff0000ull && VirtualQuery(reinterpret_cast<void *>(at), &info, sizeof(info)) == sizeof(info);
         at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize) {
        if (info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE || info.Type == MEM_IMAGE) continue;
        const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const auto count = search_region(start, info.RegionSize, indices.data(), hits.data(), hits.size());
        for (std::size_t h = 0; h < count && fresh.size() < 256; ++h) {
            const auto address = start + indices[h] * 4;
            if (const auto *set = hits[h].set)
                for (std::size_t k = 0; k < set->count; ++k)
                    fresh.push_back({address + k * element_size, set->rig, false, {set->elements[k].x, set->elements[k].y, set->elements[k].z}, set->elements[k].pitch * degrees});
            else if (const auto *pivot = hits[h].pivot)
                fresh.push_back({address + pivot_offset, pivot->rig, true, {0.0f, pivot->y, 0.0f}, 0.0f});
        }
    }
    {
        std::lock_guard lock(s.mutex);
        // What was already written no longer holds the game's numbers: it stays known.
        for (const auto &old : s.found)
            if (std::ranges::find(fresh, old.address, &Found::address) == fresh.end()) fresh.push_back(old);
        s.found = std::move(fresh);
    }
    logging::write(logging::Level::info, logging::Channel::skater,
                   std::format("Trainer: camera framing data found: {} ({} ms).", camera_rigs_summary(), GetTickCount64() - started));
    s.searching.store(false, std::memory_order_release);
}
} // namespace

void want_camera_rigs(const std::array<RigSetting, camera_rigs> &settings) noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.wanted = settings;
}
void forget_camera_rigs() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.found.clear();
    s.searches = 0;
    s.next_search = 0;
}
std::size_t apply_camera_rigs(std::uint64_t now) noexcept {
    auto &s = state();
    std::size_t written{};
    try {
        std::lock_guard lock(s.mutex);
        const bool any = std::ranges::any_of(s.wanted, [](const RigSetting &r) { return r != RigSetting{}; });
        if (s.found.empty()) {
            // The game's own numbers are in place until something else is wanted: only then look.
            if (any && !s.searching.load(std::memory_order_acquire) && now >= s.next_search && s.searches < 4) {
                ++s.searches;
                s.next_search = now + 15000;
                s.searching.store(true, std::memory_order_release);
                std::thread(search).detach();
            }
            return 0;
        }
        const auto finite = [](float value, float low, float high, float otherwise) { return std::isfinite(value) ? std::clamp(value, low, high) : otherwise; };
        for (const auto &found : s.found) {
            // The level may have been unloaded under us: write only into what is still a framing.
            std::uint32_t word{};
            float pitch{};
            if (found.pivot ? !peek(found.address - pivot_offset, word) || word != root_word
                            : !peek(found.address + element_pitch, pitch) || !about(pitch, found.pitch))
                continue;
            const auto &want = s.wanted[static_cast<std::size_t>(found.rig)];
            const bool foot = found.rig == CameraRig::foot;
            const float distance = finite(want.distance, -1000.0f, 1000.0f, 1.0f), side = finite(want.side, -1000.0f, 1000.0f, 1.0f);
            const float height = finite(want.height, -1000.0f, 1000.0f, 0.0f), raise = finite(want.raise, -1000.0f, 1000.0f, 0.0f);
            std::array<float, 3> target = found.stock;
            if (found.pivot) target[1] += height;
            else {
                target[0] = foot ? side - 1.0f : target[0] * side; // on foot the game centres the camera: metres to the right instead
                target[1] += raise + (foot ? height : 0.0f); // the on-foot camera turns about the skater's root: nothing to move
                target[2] *= distance;
            }
            for (std::size_t i = 0; i < 3; ++i) {
                float current{};
                if (peek(found.address + i * 4, current) && current != target[i] && put(found.address + i * 4, target[i])) ++written;
            }
        }
    } catch (...) {}
    return written;
}
std::string camera_rigs_summary() {
    auto &s = state();
    std::array<std::size_t, camera_rigs> framings{}, roots{};
    {
        std::unique_lock lock(s.mutex, std::try_to_lock); // also called with the lock held
        if (lock.owns_lock())
            for (const auto &found : s.found) ++(found.pivot ? roots : framings)[static_cast<std::size_t>(found.rig)];
    }
    return std::format("low camera {} framings and {} pivots, high camera {} and {}, on foot {} framings", framings[0], roots[0], framings[1], roots[1],
                       framings[2]);
}
} // namespace dingosdk::trainer
