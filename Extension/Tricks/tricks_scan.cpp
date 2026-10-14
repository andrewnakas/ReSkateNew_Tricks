#include "tricks_scan.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

// <dev> A value search over the game's memory, to find where skate.'s scoring keeps a line's score (the HUD only shows
// copies of it). Every committed, private, read-write page is searched for the value as a 32-bit integer and as a
// float; later passes keep the addresses that hold the next value. The search runs on its own thread.
namespace dingosdk::tricks {
namespace {
struct Scan {
    std::mutex mutex;
    std::vector<std::uintptr_t> ints, floats;
    std::atomic<bool> busy{};
    std::string result{"no search yet"};
    std::uint64_t searched{};
};
Scan &scan() {
    static auto *value = new Scan;
    return *value;
}
bool readable(const MEMORY_BASIC_INFORMATION &info) {
    return info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && (info.Protect & 0xff) == PAGE_READWRITE && !(info.Protect & PAGE_GUARD);
}
bool copy_from(std::uintptr_t at, void *out, std::size_t size) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void *>(at), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool add_int(std::uintptr_t at, std::int32_t value) noexcept {
    __try { *reinterpret_cast<volatile std::int32_t *>(at) += value; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool add_float(std::uintptr_t at, float value) noexcept {
    __try { *reinterpret_cast<volatile float *>(at) += value; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void first(double value) {
    auto &s = scan();
    const auto started = GetTickCount64();
    const auto whole = static_cast<std::int32_t>(std::lround(value));
    const auto real = static_cast<float>(value);
    // The regions to search, then 8 threads over them.
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> regions;
    std::uintptr_t at = 0x10000;
    MEMORY_BASIC_INFORMATION info{};
    while (at < 0x7ffffffeffffULL && VirtualQuery(reinterpret_cast<const void *>(at), &info, sizeof info) == sizeof info) {
        const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress), end = start + info.RegionSize;
        if (readable(info)) regions.emplace_back(start, end);
        at = end;
    }
    constexpr unsigned workers = 8;
    std::vector<std::vector<std::uintptr_t>> ints(workers), floats(workers);
    std::atomic<std::uint64_t> searched{};
    std::vector<std::thread> threads;
    for (unsigned w = 0; w < workers; ++w)
        threads.emplace_back([&, w] {
            std::vector<std::uint8_t> buffer(1 << 20);
            for (std::size_t r = w; r < regions.size(); r += workers)
                for (std::uintptr_t chunk = regions[r].first; chunk < regions[r].second; chunk += buffer.size()) {
                    const auto size = std::min<std::size_t>(buffer.size(), regions[r].second - chunk);
                    if (!copy_from(chunk, buffer.data(), size)) continue;
                    searched += size;
                    for (std::size_t i = 0; i + 4 <= size; i += 4) {
                        std::int32_t n;
                        float f;
                        std::memcpy(&n, buffer.data() + i, 4);
                        std::memcpy(&f, buffer.data() + i, 4);
                        if (n == whole && ints[w].size() < 1'000'000) ints[w].push_back(chunk + i);
                        if (f == real && floats[w].size() < 1'000'000) floats[w].push_back(chunk + i);
                    }
                }
        });
    for (auto &t : threads) t.join();
    std::vector<std::uintptr_t> all_ints, all_floats;
    for (unsigned w = 0; w < workers; ++w) {
        all_ints.insert(all_ints.end(), ints[w].begin(), ints[w].end());
        all_floats.insert(all_floats.end(), floats[w].begin(), floats[w].end());
    }
    std::lock_guard lock(s.mutex);
    s.ints = std::move(all_ints);
    s.floats = std::move(all_floats);
    s.searched = searched;
    s.result = std::format("search for {}: {} ints, {} floats in {} MB, {} ms", value, s.ints.size(), s.floats.size(), searched.load() >> 20, GetTickCount64() - started);
}
void next(double value) {
    auto &s = scan();
    std::vector<std::uintptr_t> ints, floats;
    {
        std::lock_guard lock(s.mutex);
        ints = s.ints;
        floats = s.floats;
    }
    const auto whole = static_cast<std::int32_t>(std::lround(value));
    const auto real = static_cast<float>(value);
    std::erase_if(ints, [&](std::uintptr_t at) { std::int32_t n{}; return !copy_from(at, &n, 4) || n != whole; });
    std::erase_if(floats, [&](std::uintptr_t at) { float f{}; return !copy_from(at, &f, 4) || f != real; });
    std::lock_guard lock(s.mutex);
    s.ints = std::move(ints);
    s.floats = std::move(floats);
    s.result = std::format("kept for {}: {} ints, {} floats", value, s.ints.size(), s.floats.size());
}
} // namespace

std::size_t scan_left() {
    auto &s = scan();
    std::lock_guard lock(s.mutex);
    return s.ints.size() + s.floats.size() + (s.result == "no search yet" ? 1000000 : 0);
}
bool scan_busy() { return scan().busy.load(); }
std::string scan_command(const std::string &what, double value) {
    auto &s = scan();
    if (what == "status" || what == "list") {
        std::lock_guard lock(s.mutex);
        std::string text = s.busy ? "searching... " : "";
        text += s.result;
        if (what == "list") {
            for (std::size_t i = 0; i < s.ints.size() && i < 24; ++i) text += std::format(" i{:x}", s.ints[i]);
            for (std::size_t i = 0; i < s.floats.size() && i < 24; ++i) text += std::format(" f{:x}", s.floats[i]);
        }
        return text;
    }
    if (what == "addi" || what == "addf") {
        // Adds `value` to the kept integers only, or the kept floats only.
        std::lock_guard lock(s.mutex);
        const auto &list = what == "addi" ? s.ints : s.floats;
        if (list.size() > 64) return "error: too many addresses left to write; narrow the search first.";
        unsigned written = 0;
        for (const auto at : list) written += what == "addi" ? add_int(at, static_cast<std::int32_t>(std::lround(value))) : add_float(at, static_cast<float>(value));
        return std::format("added {} at {} {} addresses.", value, written, what == "addi" ? "integer" : "float");
    }
    if (what == "add") {
        // Adds `value` to every kept address (only when a few are left).
        std::lock_guard lock(s.mutex);
        if (s.ints.size() + s.floats.size() > 64) return "error: too many addresses left to write; narrow the search first.";
        unsigned written = 0;
        for (const auto at : s.ints) written += add_int(at, static_cast<std::int32_t>(std::lround(value)));
        for (const auto at : s.floats) written += add_float(at, static_cast<float>(value));
        return std::format("added {} at {} addresses.", value, written);
    }
    if (s.busy.exchange(true)) return "error: a search is running.";
    std::thread([what, value] {
        if (what == "next") next(value);
        else first(value);
        scan().busy.store(false);
    }).detach();
    return std::format("{} search for {} started.", what == "next" ? "next" : "new", value);
}
} // namespace dingosdk::tricks
