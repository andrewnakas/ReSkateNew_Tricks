#pragma once
// Diagnostics for the local dev branch only.
#include <array>
#include <cstddef>
#include <cstdint>

namespace dingosdk::trainer {
struct PushSample {
    float target{}, target_default{}, current{}, speed{}, cap{}, start{}, end{}, mass{}, step{}, impulse{};
    std::uint32_t cc{}, dc{};
    bool animated{};
};
bool start_push_probe(std::uintptr_t base) noexcept;
// The latest push the game worked out, and how many it has worked out so far.
std::uint64_t push_sample(PushSample &out) noexcept;
// A hardware breakpoint on writes of the four bytes at `address` (0: off). Returns threads armed.
std::size_t watch_writes(std::uintptr_t address, std::uintptr_t base) noexcept;
struct Writer {
    std::uintptr_t rva{};
    std::uint64_t count{};
    float last{};
};
// The code that wrote there since the watch began, by instruction (the one after the write).
std::size_t writers(Writer *out, std::size_t capacity) noexcept;
// The return addresses above writer `writer` (the first four writers, three distinct stacks each).
std::size_t writer_stack(std::size_t writer, std::size_t which, std::uintptr_t *out, std::size_t capacity) noexcept;
// Callers of the game's velocity setter since the last call (rva, count, last horizontal speed).
std::size_t velocity_setters(Writer *out, std::size_t capacity) noexcept;
// Every place in the process's writable memory where `wanted[0]` sits with all the other numbers
// within `window` bytes either side (four-byte aligned). For each hit: its address and each
// number's offset from it. Returns the megabytes searched.
struct FloatHit {
    std::uintptr_t address{};
    std::array<std::int32_t, 24> offsets{};
};
std::size_t scan_floats(const float *wanted, std::size_t count, std::size_t window, FloatHit *hits, std::size_t capacity, std::size_t &megabytes) noexcept;
// Four hardware watches at once (0: none) on reads and writes of four bytes each; counts only
// accesses made by the game's own code. `watch_four` re-arms and zeroes the counts.
std::size_t watch_four(const std::uintptr_t *addresses, std::uintptr_t image, std::size_t image_size) noexcept;
void watch_four_counts(std::uint64_t *counts, std::uint64_t *others = nullptr) noexcept; // `others`: reads by code outside the game
// The first code address that touched each slot since the watches were set (0: none).
void watch_four_readers(std::uintptr_t *readers) noexcept;
// Runs of consecutive eight-byte pointers to objects whose first eight bytes are `type`.
struct PointerRun {
    std::uintptr_t address{};
    std::uint32_t length{};
};
// Returns how many objects of that type exist; fills `runs` with the runs of `wanted` pointers
// exactly (0: any run of two or more), `histogram[n]` with how many runs have n pointers.
std::size_t pointer_runs(std::uintptr_t type, std::uint32_t wanted, PointerRun *runs, std::size_t capacity, std::size_t &found,
                         std::array<std::uint32_t, 24> &histogram) noexcept;
// Every copy in memory of the number that keeps a flip trick ahead of the landing (see the .cpp).
std::size_t flick_warp_limits(std::uintptr_t *out, std::size_t capacity) noexcept;
bool poke_float(std::uintptr_t address, float value) noexcept;
// Where `pattern` sits in readable memory outside the modules (read-only pages included).
std::size_t scan_bytes(const std::uint8_t *pattern, std::size_t length, std::size_t align, std::uintptr_t *out, std::size_t capacity) noexcept;
} // namespace dingosdk::trainer
