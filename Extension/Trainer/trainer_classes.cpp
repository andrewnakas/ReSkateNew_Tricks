#include "trainer_classes.h"
#include "Engine/Core/Log/logging.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string_view>
#include <vector>

namespace dingosdk::trainer {
namespace {
constexpr std::size_t max_copies = 12; // a class found more often than this is not told apart from chance
// A copy: where it starts, and how it is laid out. The class's own layout has every field at
// its natural alignment (curve references between the numbers); the other holds only the
// numbers, one eight-byte slot each in the same order.
struct Copy {
    std::uintptr_t base{};
    bool slots{};
    bool operator==(const Copy &) const = default;
};
using Copies = std::array<std::vector<Copy>, class_count>;
std::uintptr_t field_offset(const ClassSpec &spec, std::size_t i, bool slots) noexcept {
    return slots ? i * 8 : class_fields[spec.first + i].offset;
}
// The game's native code keeps its own packed copy of some of these numbers, filled when a
// skater spawns, and reads that instead of the class: found the same way and written together
// with it. Seen in memory beside the push class (four-byte numbers in a row):
//   0.25 0.85 2.356 1.5 0.7 [light] [medium] [strong] [long hold] [short hold] [medium override] [horse push] 0.9 0 6.25
struct MirrorSlot {
    float value;       // what a fresh copy holds here
    const char *field; // the class field this slot follows ("push.MaxPushSpeedLight"), or null
    bool checked;      // part of the pattern a copy is recognised by
};
struct Mirror {
    std::size_t anchor; // the slot searched for
    std::array<MirrorSlot, 15> slots;
};
constexpr Mirror mirrors[]{
    {10, {{{0.25f, nullptr, true}, {0.85f, nullptr, true}, {0, nullptr, false}, {1.5f, nullptr, true}, {0.7f, nullptr, true},
           {4.0f, "push.MaxPushSpeedLight", true}, {8.5f, "push.MaxPushSpeedMedium", true}, {9.5f, "push.MaxPushSpeedStrong", true},
           {0.45f, "push.LongInputHoldTime_S", true}, {0.15f, "push.ShortInputHoldTime_S", true},
           {7.2f, "push.MinMediumPushOverrideSpeed", true}, {12.0f, "push.ForwardToPushEngageHorsePushSpeed", true},
           {0.9f, nullptr, true}, {0, nullptr, false}, {6.25f, nullptr, true}}}},
};
constexpr std::size_t mirror_count = sizeof(mirrors) / sizeof(mirrors[0]);
using MirrorCopies = std::array<std::vector<std::uintptr_t>, mirror_count>;
// The class field a mirror slot follows, as an index into class_fields (-1: none).
int mirror_field(const MirrorSlot &slot) noexcept {
    if (!slot.field) return -1;
    const std::string_view wanted(slot.field);
    for (std::size_t c = 0; c < class_count; ++c) {
        const std::string_view key(class_specs[c].key);
        if (wanted.size() <= key.size() || !wanted.starts_with(key) || wanted[key.size()] != '.') continue;
        for (std::size_t i = 0; i < class_specs[c].count; ++i)
            if (wanted.substr(key.size() + 1) == class_fields[class_specs[c].first + i].name) return static_cast<int>(class_specs[c].first + i);
    }
    return -1;
}
// A curve of the flip trick tuning: where its points are and the outputs they shipped with.
// A point is 0x1c bytes with its input at +0xc and its output at +0x14 (the layout the physics
// tuning's FloatCurves have); the count sits four bytes before the first point.
struct FlipCurve {
    std::uintptr_t curve{}, points{};
    std::vector<float> outputs;
    float low{}, high{}; // the bounds the curve clamps its output to (+0x20, +0x24)
};
constexpr std::size_t curve_point = 0x1c, curve_x = 0xc, curve_y = 0x14, curve_points_at = 0x18;
// The point that tells what a curve holds: its largest output (some curves start at 0).
std::size_t probe_point(const FlipCurve &curve) noexcept {
    std::size_t best{};
    for (std::size_t i = 1; i < curve.outputs.size(); ++i)
        if (std::abs(curve.outputs[i]) > std::abs(curve.outputs[best])) best = i;
    return best;
}
std::uintptr_t probe_address(const FlipCurve &curve) noexcept { return curve.points + probe_point(curve) * curve_point + curve_y; }
float probe_output(const FlipCurve &curve) noexcept { return curve.outputs[probe_point(curve)]; }
struct Found {
    std::mutex mutex;
    std::vector<FlipCurve> flip;
    float flip_wanted{1}, flip_written{1};
    Copies copies;
    MirrorCopies mirror_copies;
    std::array<std::array<int, 15>, mirror_count> mirror_fields{};
    std::array<float, class_field_count> wanted{};               // what each field should hold
    std::array<float, class_field_count> written{};              // what the copies hold (the defaults until written)
    bool ready{};
    std::atomic<bool> searching{};
    std::atomic<std::uint64_t> searches{};
    bool dirty{};
};
Found &found() {
    static auto *value = [] {
        auto *f = new Found;
        for (std::size_t i = 0; i < class_field_count; ++i) f->wanted[i] = f->written[i] = class_fields[i].stock;
        for (std::size_t m = 0; m < mirror_count; ++m)
            for (std::size_t i = 0; i < mirrors[m].slots.size(); ++i) f->mirror_fields[m][i] = mirror_field(mirrors[m].slots[i]);
        return f;
    }();
    return *value;
}
std::uint32_t bits(float value) {
    std::uint32_t result;
    std::memcpy(&result, &value, 4);
    return result;
}
// Does the memory at `base` hold class `spec` with every field at its default (or, for a copy
// the trainer has written, at what it wrote)? `low`/`high` bound the readable region.
bool matches(const ClassSpec &spec, Copy copy, std::uintptr_t low, std::uintptr_t high, const float *expected) noexcept {
    for (std::size_t i = 0; i < spec.count; ++i) {
        const auto address = copy.base + field_offset(spec, i, copy.slots);
        if (address < low || address + 4 > high) return false;
        const float want = expected[spec.first + i];
        if (want == 0) continue; // a field with no default of its own: whatever the game put there
        if (*reinterpret_cast<const std::uint32_t *>(address) != bits(want)) return false;
    }
    return true;
}
// Does the memory at `base` hold mirror `m`, its followed slots at `expected` (indexed like class_fields)?
bool mirror_matches(std::size_t m, const std::array<int, 15> &fields, std::uintptr_t base, std::uintptr_t low, std::uintptr_t high,
                    const float *expected) noexcept {
    const auto &mirror = mirrors[m];
    if (base < low || base + mirror.slots.size() * 4 > high) return false;
    for (std::size_t i = 0; i < mirror.slots.size(); ++i) {
        if (!mirror.slots[i].checked) continue;
        const float want = fields[i] >= 0 ? expected[fields[i]] : mirror.slots[i].value;
        if (*reinterpret_cast<const std::uint32_t *>(base + i * 4) != bits(want)) return false;
    }
    return true;
}
struct Anchor {
    std::uint32_t pattern;
    std::uint16_t spec; // a class, or class_count + a mirror
};
void search_region(std::uintptr_t start, std::size_t size, const std::vector<Anchor> &anchors, std::uint64_t filter,
                   Copies &copies, MirrorCopies &mirror_copies, const std::array<std::array<int, 15>, mirror_count> &mirror_fields,
                   const float *expected) noexcept {
    __try {
        const auto *words = reinterpret_cast<const std::uint32_t *>(start);
        const std::size_t total = size / 4;
        for (std::size_t i = 0; i < total; ++i) {
            const auto word = words[i];
            if (!((filter >> (word % 61)) & 1)) continue;
            for (const auto &anchor : anchors) {
                if (anchor.pattern != word) continue;
                if (anchor.spec >= class_count) {
                    const std::size_t m = anchor.spec - class_count;
                    const auto base = start + i * 4 - mirrors[m].anchor * 4;
                    if (mirror_matches(m, mirror_fields[m], base, start, start + size, expected) && mirror_copies[m].size() <= max_copies)
                        mirror_copies[m].push_back(base);
                    continue;
                }
                const auto &spec = class_specs[anchor.spec];
                for (const bool slots : {false, true}) {
                    const Copy copy{start + i * 4 - field_offset(spec, spec.anchor, slots), slots};
                    if (copy.base < start || !matches(spec, copy, start, start + size, expected)) continue;
                    auto &list = copies[anchor.spec];
                    if (list.size() <= max_copies) list.push_back(copy);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
bool still_there(const ClassSpec &spec, Copy copy, const float *expected) noexcept {
    __try {
        const auto base = copy.base;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void *>(base), &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE)
            return false;
        const auto low = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        return matches(spec, copy, low, low + info.RegionSize, expected);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool mirror_still_there(std::size_t m, const std::array<int, 15> &fields, std::uintptr_t base, const float *expected) noexcept {
    __try {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void *>(base), &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE)
            return false;
        const auto low = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        return mirror_matches(m, fields, base, low, low + info.RegionSize, expected);
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
template <class T> bool peek(std::uintptr_t address, T &value) noexcept {
    __try {
        std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class Visit> void each_region(Visit &&visit) {
    MEMORY_BASIC_INFORMATION info{};
    for (std::uintptr_t at = 0x10000; at < 0x7fffffff0000ull && VirtualQuery(reinterpret_cast<void *>(at), &info, sizeof(info)) == sizeof(info);
         at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize) {
        if (info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE || info.Type == MEM_IMAGE) continue;
        visit(reinterpret_cast<std::uintptr_t>(info.BaseAddress), info.RegionSize);
    }
}
void collect_typed(std::uintptr_t start, std::size_t size, std::uintptr_t type, std::vector<std::uintptr_t> &objects) noexcept {
    __try {
        const auto *words = reinterpret_cast<const std::uintptr_t *>(start);
        for (std::size_t i = 0; i < size / 8; ++i)
            if (words[i] == type && objects.size() < 100000) objects.push_back(start + i * 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void collect_eights(std::uintptr_t start, std::size_t size, const std::vector<std::uintptr_t> &objects, std::vector<std::uintptr_t> &runs) noexcept {
    __try {
        const auto *words = reinterpret_cast<const std::uintptr_t *>(start);
        const std::size_t total = size / 8;
        std::size_t run{};
        for (std::size_t i = 0; i <= total; ++i) {
            const bool hit = i < total && words[i] >= objects.front() && words[i] <= objects.back() && std::binary_search(objects.begin(), objects.end(), words[i]);
            if (hit) { ++run; continue; }
            if (run == 8 && runs.size() < 64) runs.push_back(start + (i - 8) * 8);
            run = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// One curve's points: false unless it has `count` points starting at (x0, y0).
bool read_curve(std::uintptr_t curve, std::uint32_t count, float x0, float y0, FlipCurve &out) noexcept {
    std::uintptr_t points{};
    std::uint32_t n{};
    float x{}, y{};
    if (!peek(curve + curve_points_at, points) || !points || !peek(points - 4, n) || (n & 0x7fffffffu) != count) return false;
    if (!peek(points + curve_x, x) || !peek(points + curve_y, y) || std::abs(x - x0) > 0.01f || std::abs(y - y0) > 0.01f) return false;
    out.curve = curve;
    out.points = points;
    if (!peek(curve + 0x20, out.low) || !peek(curve + 0x24, out.high)) return false;
    out.outputs.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!peek(points + i * curve_point + curve_y, y)) return false;
        out.outputs.push_back(y);
    }
    return true;
}
// The flip trick tuning's speed curves. `curve_type`: the first eight bytes of any FloatCurve.
// The class's fields in memory order (the game's data): ExperimentalFlipTrickSpeedCurve,
// LegacyFlipTrickSpeedCurve, two flick height curves, GestureSpeedToFlipSpeed,
// OllieSpeedToFlipSpeed, two flick sensitivity curves. As they ship: the first runs
// (0, 0.4)..(0.9, 1.4) on three points, the second starts at (0.35, 0.65), the fifth at
// (0, 0.05), the sixth is flat at 1.
std::vector<FlipCurve> find_flip_curves(std::uintptr_t curve_type) {
    std::vector<FlipCurve> result;
    std::vector<std::uintptr_t> objects, runs;
    each_region([&](std::uintptr_t start, std::size_t size) { collect_typed(start, size, curve_type, objects); });
    std::ranges::sort(objects);
    if (objects.size() < 8) return result;
    each_region([&](std::uintptr_t start, std::size_t size) { collect_eights(start, size, objects, runs); });
    for (const auto run : runs) {
        std::array<std::uintptr_t, 8> curves{};
        if (!peek(run, curves)) continue;
        // In memory order: 0 the flip speed against the flick, 1 the same for the legacy controls,
        // 2 a multiplier for a high flick (1 to 2: left alone), 3, 6 and 7 the speed a flick's height
        // and the stick sensitivity add (0 to 2.75), 4 flick speed to flip speed, 5 ollie speed to
        // flip speed. Scaling only 0, 1, 4 and 5 slowed a flip by a third at x0.4: the rest is 3, 6, 7.
        std::array<FlipCurve, 7> speed;
        if (!read_curve(curves[0], 3, 0.0f, 0.4f, speed[0]) || !read_curve(curves[1], 3, 0.35f, 0.65f, speed[1]) ||
            !read_curve(curves[4], 3, 0.0f, 0.05f, speed[2]) || !read_curve(curves[5], 2, 0.0f, 1.0f, speed[3]) ||
            !read_curve(curves[3], 4, 0.0f, 0.0f, speed[4]) || !read_curve(curves[6], 4, 0.0f, 0.0f, speed[5]) ||
            !read_curve(curves[7], 4, 0.0f, 0.0f, speed[6]))
            continue;
        for (auto &curve : speed)
            if (std::ranges::none_of(result, [&](const FlipCurve &known) { return known.points == curve.points; })) result.push_back(std::move(curve));
    }
    return result;
}
DWORD WINAPI search(void *) noexcept {
    auto &f = found();
    try {
        // The defaults are what a fresh copy holds.
        std::array<float, class_field_count> stock{};
        for (std::size_t i = 0; i < class_field_count; ++i) stock[i] = class_fields[i].stock;
        std::vector<Anchor> anchors;
        std::uint64_t filter{};
        for (std::size_t c = 0; c < class_count; ++c) {
            const auto pattern = bits(class_fields[class_specs[c].first + class_specs[c].anchor].stock);
            anchors.push_back({pattern, static_cast<std::uint16_t>(c)});
            filter |= std::uint64_t{1} << (pattern % 61);
        }
        for (std::size_t m = 0; m < mirror_count; ++m) {
            const auto pattern = bits(mirrors[m].slots[mirrors[m].anchor].value);
            anchors.push_back({pattern, static_cast<std::uint16_t>(class_count + m)});
            filter |= std::uint64_t{1} << (pattern % 61);
        }
        Copies fresh;
        MirrorCopies fresh_mirrors;
        const auto started = GetTickCount64();
        std::size_t bytes{};
        MEMORY_BASIC_INFORMATION info{};
        for (std::uintptr_t at = 0x10000; at < 0x7fffffff0000ull && VirtualQuery(reinterpret_cast<void *>(at), &info, sizeof(info)) == sizeof(info);
             at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize) {
            if (info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE || info.Type == MEM_IMAGE) continue;
            search_region(reinterpret_cast<std::uintptr_t>(info.BaseAddress), info.RegionSize, anchors, filter, fresh, fresh_mirrors, f.mirror_fields,
                          stock.data());
            bytes += info.RegionSize;
        }
        std::size_t classes{}, copies{};
        {
            std::lock_guard lock(f.mutex);
            for (std::size_t c = 0; c < class_count; ++c) {
                // Copies the trainer has already written no longer hold the defaults: keep the
                // ones that still hold what it wrote.
                std::vector<Copy> kept;
                for (const auto copy : f.copies[c])
                    if (std::ranges::find(fresh[c], copy) == fresh[c].end() && still_there(class_specs[c], copy, f.written.data())) kept.push_back(copy);
                if (fresh[c].size() > max_copies) fresh[c].clear();
                // A fresh copy holds the defaults: bring it level with what the older ones hold.
                for (const auto copy : fresh[c])
                    for (std::size_t i = 0; i < class_specs[c].count; ++i)
                        if (const auto index = class_specs[c].first + i; f.written[index] != class_fields[index].stock)
                            (void)put(copy.base + field_offset(class_specs[c], i, copy.slots), f.written[index]);
                kept.insert(kept.end(), fresh[c].begin(), fresh[c].end());
                f.copies[c] = std::move(kept);
                classes += !f.copies[c].empty();
                copies += f.copies[c].size();
            }
            for (std::size_t m = 0; m < mirror_count; ++m) {
                std::vector<std::uintptr_t> kept;
                for (const auto base : f.mirror_copies[m])
                    if (std::ranges::find(fresh_mirrors[m], base) == fresh_mirrors[m].end() && mirror_still_there(m, f.mirror_fields[m], base, f.written.data()))
                        kept.push_back(base);
                if (fresh_mirrors[m].size() > max_copies) fresh_mirrors[m].clear();
                for (const auto base : fresh_mirrors[m])
                    for (std::size_t i = 0; i < mirrors[m].slots.size(); ++i)
                        if (const auto index = f.mirror_fields[m][i]; index >= 0 && f.written[index] != class_fields[index].stock)
                            (void)put(base + i * 4, f.written[index]);
                kept.insert(kept.end(), fresh_mirrors[m].begin(), fresh_mirrors[m].end());
                f.mirror_copies[m] = std::move(kept);
                copies += f.mirror_copies[m].size();
            }
            f.ready = true;
            f.dirty = true;
        }
        // The flip trick curves: any FloatCurve gives the type to look for, and the push class
        // keeps one 0x10 after its first number.
        std::uintptr_t curve_type{};
        std::size_t flip_found{};
        {
            std::uintptr_t push_copy{};
            {
                std::lock_guard lock(f.mutex);
                for (std::size_t c = 0; c < class_count && !push_copy; ++c)
                    if (std::string_view(class_specs[c].key) == "push")
                        for (const auto copy : f.copies[c])
                            if (!copy.slots) { push_copy = copy.base; break; }
            }
            std::uintptr_t curve{};
            if (push_copy && peek(push_copy + 0x10, curve) && curve) (void)peek(curve, curve_type);
        }
        if (curve_type) {
            bool have;
            {
                std::lock_guard lock(f.mutex);
                have = !f.flip.empty();
            }
            // Curves already scaled no longer look like the shipped ones: those are kept while they
            // still hold what was written. Fresh ones (the game builds new ones with a new skater)
            // hold the game's own outputs and are brought level.
            (void)have;
            auto curves = find_flip_curves(curve_type);
            std::lock_guard lock(f.mutex);
            std::erase_if(f.flip, [&](const FlipCurve &curve) {
                float y{};
                return curve.outputs.empty() || !peek(probe_address(curve), y) ||
                       std::abs(y - probe_output(curve) * f.flip_written) > 0.001f * std::max(1.0f, std::abs(y));
            });
            for (auto &curve : curves) {
                if (std::ranges::any_of(f.flip, [&](const FlipCurve &known) { return known.points == curve.points; })) continue;
                if (f.flip_written != 1) {
                    for (std::size_t i = 0; i < curve.outputs.size(); ++i) (void)put(curve.points + i * curve_point + curve_y, curve.outputs[i] * f.flip_written);
                    (void)put(curve.curve + 0x20, curve.low > 0 ? curve.low * f.flip_written : curve.low);
                    (void)put(curve.curve + 0x24, curve.high > 0 ? curve.high * f.flip_written : curve.high);
                }
                f.flip.push_back(std::move(curve));
            }
            flip_found = f.flip.size();
            f.dirty = true;
        }
        logging::write(logging::Level::info, logging::Channel::skater,
                       std::format("Trainer: found {} of {} game tuning classes ({} copies) and {} flip trick speed curves in {} MB, {} ms.", classes,
                                   class_count, copies, flip_found, bytes >> 20, GetTickCount64() - started));
    } catch (...) {}
    f.searches.fetch_add(1, std::memory_order_release);
    f.searching.store(false, std::memory_order_release);
    return 0;
}
// The game puts its own numbers back whenever it builds these objects again (a respawn, a
// teleport, a new session) and may build new ones beside them: what was written once does not
// stay written. Once a second, every copy that is wanted different from the game's own is
// checked: one that went back to the game's number is written again, one that holds neither is
// someone else's memory now and is forgotten.
bool same(float a, float b) noexcept { return std::abs(a - b) <= 1e-5f * std::max(1.0f, std::abs(b)); }
void write_flip_curve(const FlipCurve &curve, float factor) noexcept {
    for (std::size_t i = 0; i < curve.outputs.size(); ++i) (void)put(curve.points + i * curve_point + curve_y, curve.outputs[i] * factor);
    // The bounds move with the outputs, or the curve clamps the scaled values back.
    (void)put(curve.curve + 0x20, curve.low > 0 ? curve.low * factor : curve.low);
    (void)put(curve.curve + 0x24, curve.high > 0 ? curve.high * factor : curve.high);
}
} // namespace

bool refresh_classes(std::uint64_t now) noexcept {
    auto &f = found();
    static std::uint64_t next{}, next_log{};
    if (now < next) return false;
    next = now + 1000;
    std::unique_lock lock(f.mutex, std::try_to_lock);
    if (!lock.owns_lock() || !f.ready || f.searching.load(std::memory_order_acquire)) return false;
    bool lost = false;
    std::size_t repaired{};
    if (f.flip_written != 1) {
        std::erase_if(f.flip, [&](const FlipCurve &curve) {
            float y{};
            if (curve.outputs.empty() || !peek(probe_address(curve), y)) return true;
            if (same(y, probe_output(curve) * f.flip_written)) return false;
            if (!same(y, probe_output(curve))) return true;
            write_flip_curve(curve, f.flip_written);
            ++repaired;
            return false;
        });
        lost = lost || f.flip.empty();
    }
    for (std::size_t c = 0; c < class_count; ++c) {
        const auto &spec = class_specs[c];
        std::size_t probe = spec.count;
        for (std::size_t i = 0; i < spec.count && probe == spec.count; ++i)
            if (f.written[spec.first + i] != class_fields[spec.first + i].stock) probe = i;
        if (probe == spec.count) continue; // the class holds the game's own: nothing to keep
        std::erase_if(f.copies[c], [&](Copy copy) {
            float value{};
            if (!peek(copy.base + field_offset(spec, probe, copy.slots), value)) return true;
            if (same(value, f.written[spec.first + probe])) return false;
            if (!same(value, class_fields[spec.first + probe].stock)) return true;
            for (std::size_t i = 0; i < spec.count; ++i)
                if (const auto index = spec.first + i; f.written[index] != class_fields[index].stock)
                    (void)put(copy.base + field_offset(spec, i, copy.slots), f.written[index]);
            ++repaired;
            return false;
        });
        lost = lost || f.copies[c].empty();
    }
    for (std::size_t m = 0; m < mirror_count; ++m) {
        const auto &fields = f.mirror_fields[m];
        std::size_t probe = fields.size();
        for (std::size_t i = 0; i < mirrors[m].slots.size() && probe == fields.size(); ++i)
            if (fields[i] >= 0 && f.written[fields[i]] != class_fields[fields[i]].stock) probe = i;
        if (probe == fields.size()) continue;
        std::erase_if(f.mirror_copies[m], [&](std::uintptr_t base) {
            float value{};
            if (!peek(base + probe * 4, value)) return true;
            if (same(value, f.written[fields[probe]])) return false;
            if (!same(value, class_fields[fields[probe]].stock)) return true;
            for (std::size_t i = 0; i < mirrors[m].slots.size(); ++i)
                if (fields[i] >= 0 && f.written[fields[i]] != class_fields[fields[i]].stock) (void)put(base + i * 4, f.written[fields[i]]);
            ++repaired;
            return false;
        });
        lost = lost || f.mirror_copies[m].empty();
    }
    if ((repaired || lost) && now >= next_log) {
        next_log = now + 10000;
        logging::write(logging::Level::info, logging::Channel::skater,
                       std::format("Trainer: the game put its own numbers back in {} tuning objects: written again{}.", repaired,
                                   lost ? "; some are gone, looking for the new ones" : ""));
    }
    return lost;
}

namespace {
} // namespace

void find_classes() noexcept {
    auto &f = found();
    if (f.searching.exchange(true, std::memory_order_acq_rel)) return;
    const auto thread = CreateThread(nullptr, 0, search, nullptr, 0, nullptr);
    if (thread) {
        SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(thread);
    } else {
        f.searching.store(false, std::memory_order_release);
    }
}
bool finding_classes() noexcept { return found().searching.load(std::memory_order_acquire); }
std::uint64_t class_searches() noexcept { return found().searches.load(std::memory_order_acquire); }
std::size_t class_copies(std::size_t index) noexcept {
    auto &f = found();
    if (index >= class_count) return 0;
    std::lock_guard lock(f.mutex);
    return f.copies[index].size();
}
void want_class_value(std::size_t field, float value) noexcept {
    auto &f = found();
    if (field >= class_field_count || !(value == value)) return;
    std::lock_guard lock(f.mutex);
    if (f.wanted[field] != value) {
        f.wanted[field] = value;
        f.dirty = true;
    }
}
bool classes_wanted() noexcept {
    auto &f = found();
    std::lock_guard lock(f.mutex);
    if (f.flip_wanted != 1) return true;
    for (std::size_t i = 0; i < class_field_count; ++i)
        if (f.wanted[i] != class_fields[i].stock) return true;
    return false;
}
std::size_t apply_classes() noexcept {
    auto &f = found();
    std::unique_lock lock(f.mutex, std::try_to_lock);
    if (!lock.owns_lock() || !f.ready || !f.dirty) return 0;
    f.dirty = false;
    std::size_t count{};
    if (f.flip_wanted != f.flip_written && !f.flip.empty()) {
        // A curve whose first output is not what was last written is no longer that curve.
        std::erase_if(f.flip, [&](const FlipCurve &curve) {
            float y{};
            return curve.outputs.empty() || !peek(probe_address(curve), y) || std::abs(y - probe_output(curve) * f.flip_written) > 0.001f * std::max(1.0f, std::abs(y));
        });
        for (const auto &curve : f.flip) {
            for (std::size_t i = 0; i < curve.outputs.size(); ++i)
                if (put(curve.points + i * curve_point + curve_y, curve.outputs[i] * f.flip_wanted)) ++count;
            // The bounds move with the outputs, or the curve clamps the scaled values back.
            (void)put(curve.curve + 0x20, curve.low > 0 ? curve.low * f.flip_wanted : curve.low);
            (void)put(curve.curve + 0x24, curve.high > 0 ? curve.high * f.flip_wanted : curve.high);
        }
        f.flip_written = f.flip_wanted;
    }
    for (std::size_t m = 0; m < mirror_count; ++m) {
        auto &list = f.mirror_copies[m];
        bool differs = false;
        for (const auto index : f.mirror_fields[m]) differs = differs || (index >= 0 && f.wanted[index] != f.written[index]);
        if (!differs || list.empty()) continue;
        std::erase_if(list, [&](std::uintptr_t base) { return !mirror_still_there(m, f.mirror_fields[m], base, f.written.data()); });
        for (std::size_t i = 0; i < mirrors[m].slots.size(); ++i)
            if (const auto index = f.mirror_fields[m][i]; index >= 0 && f.wanted[index] != f.written[index])
                for (const auto base : list)
                    if (put(base + i * 4, f.wanted[index])) ++count;
    }
    for (std::size_t c = 0; c < class_count; ++c) {
        const auto &spec = class_specs[c];
        auto &list = f.copies[c];
        bool differs = false;
        for (std::size_t i = 0; i < spec.count && !differs; ++i) differs = f.wanted[spec.first + i] != f.written[spec.first + i];
        if (!differs) continue;
        // A copy that no longer holds what was last written is someone else's memory now.
        std::erase_if(list, [&](Copy copy) { return !still_there(spec, copy, f.written.data()); });
        for (std::size_t i = 0; i < spec.count; ++i) {
            const auto index = spec.first + i;
            if (f.wanted[index] == f.written[index]) continue;
            for (const auto copy : list)
                if (put(copy.base + field_offset(spec, i, copy.slots), f.wanted[index])) ++count;
            f.written[index] = f.wanted[index];
        }
    }
    return count;
}
void want_flip_speed(float factor) noexcept {
    auto &f = found();
    if (!(factor >= 0.005f) || !(factor <= 1000.0f)) return;
    std::lock_guard lock(f.mutex);
    if (f.flip_wanted != factor) {
        f.flip_wanted = factor;
        f.dirty = true;
    }
}
std::size_t flip_curves() noexcept {
    auto &f = found();
    std::lock_guard lock(f.mutex);
    return f.flip.size();
}
std::size_t class_field_copies(std::size_t field, FieldCopy *out, std::size_t capacity) noexcept {
    auto &f = found();
    std::lock_guard lock(f.mutex);
    std::size_t count{};
    // Native code's copies first: where there is one, it is what the game reads.
    for (std::size_t m = 0; m < mirror_count; ++m)
        for (std::size_t i = 0; i < mirrors[m].slots.size(); ++i)
            if (f.mirror_fields[m][i] == static_cast<int>(field))
                for (const auto base : f.mirror_copies[m])
                    if (count < capacity) out[count++] = {base + i * 4, 'n'};
    for (std::size_t c = 0; c < class_count; ++c) {
        const auto &spec = class_specs[c];
        if (field < spec.first || field >= static_cast<std::size_t>(spec.first) + spec.count) continue;
        for (const bool slots : {true, false})
            for (const auto copy : f.copies[c])
                if (copy.slots == slots && count < capacity) out[count++] = {copy.base + field_offset(spec, field - spec.first, slots), slots ? 's' : 'c'};
    }
    return count;
}
std::string classes_summary() {
    auto &f = found();
    std::lock_guard lock(f.mutex);
    std::string result = f.ready ? "" : "(no search has finished yet) ";
    for (std::size_t c = 0; c < class_count; ++c) {
        const auto slots = std::ranges::count_if(f.copies[c], [](Copy copy) { return copy.slots; });
        result += std::format("{}{}:{}+{}", c ? ", " : "", class_specs[c].key, f.copies[c].size() - static_cast<std::size_t>(slots), slots);
    }
    for (std::size_t m = 0; m < mirror_count; ++m) result += std::format(", native copy {}:{}", m, f.mirror_copies[m].size());
    result += std::format(", flip trick speed curves:{}", f.flip.size());
    return result;
}
} // namespace dingosdk::trainer
