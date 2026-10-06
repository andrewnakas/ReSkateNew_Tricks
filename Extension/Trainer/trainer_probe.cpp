// Diagnostics for the local dev branch only: what the game's push code works from.
#include "trainer_probe.h"
#include "Engine/Core/Hooks/hooks.h"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>
#include <intrin.h>

namespace dingosdk::trainer {
namespace {
using Push = float *(*)(std::uintptr_t, float *, std::uintptr_t, float, float, std::uint8_t *);
std::atomic<Push> push_original{};
std::atomic<std::uint64_t> push_calls{};
PushSample latest; // physics thread writes, the client tick reads: a torn read only garbles a log line
template <class T> T at(std::uintptr_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(T));
    return value;
}
float *push_hook(std::uintptr_t block, float *out, std::uintptr_t context, float start, float end, std::uint8_t *flag) {
    const auto original = push_original.load(std::memory_order_acquire);
    auto *answer = original ? original(block, out, context, start, end, flag) : out;
    if (context && block && (at<std::uint32_t>(context + 0x13c4) & 0x40)) {
        PushSample s;
        s.target = at<float>(context + 0x17f4);
        s.target_default = at<float>(context + 0x17f0);
        s.current = at<float>(context + 0x17fc);
        s.speed = at<float>(context + 0x1800);
        s.cap = at<float>(block + 0x10);
        s.start = start;
        s.end = end;
        s.mass = at<float>(context + 0x1834);
        s.step = at<float>(context + 0x17ec);
        s.cc = at<std::uint32_t>(context + 0x13cc);
        s.dc = at<std::uint32_t>(context + 0x13dc);
        s.animated = flag && *flag;
        s.impulse = out ? out[0] * out[0] + out[1] * out[1] + out[2] * out[2] : 0;
        latest = s;
        push_calls.fetch_add(1, std::memory_order_release);
    }
    return answer;
}
} // namespace
bool watch_execute = false;
bool watch_reads = false; // reading the watched bytes in the handler would trap again
struct Watch {
    std::atomic<std::uintptr_t> address{}, base{};
    std::array<std::atomic<std::uintptr_t>, 32> rva{};
    std::array<std::atomic<std::uint64_t>, 32> count{};
    std::array<std::atomic<float>, 32> last{};
    PVOID handler{};
};
Watch &watch() {
    static auto *value = new Watch;
    return *value;
}
constexpr std::size_t stack_depth = 10;
std::array<std::array<std::array<std::uintptr_t, stack_depth>, 3>, 4> stacks{}; // writer, distinct stack, frame
std::atomic<bool> four_active{};
std::array<std::atomic<std::uint64_t>, 4> four_counts{}, four_others{};
std::uintptr_t four_image{}, four_image_end{};
std::array<std::atomic<std::uintptr_t>, 4> four_readers{}; // the first code seen touching each slot
// A read made inside a system library (memcpy, memcmp...) belongs to whoever called it: walks
// out of the libraries and returns the first address that is in no library (script code), in
// the game or in this DLL. 0: the walk failed.
std::uintptr_t library_caller(const CONTEXT *from, std::uintptr_t own_base, std::uintptr_t own_end) noexcept {
    __try {
        CONTEXT context = *from;
        for (int depth = 0; depth < 12; ++depth) {
            DWORD64 image{};
            if (const auto function = RtlLookupFunctionEntry(context.Rip, &image, nullptr)) {
                void *handler_data{};
                DWORD64 establisher{};
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler_data, &establisher, nullptr);
            } else { // a leaf: the return address is on top of the stack
                context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
                context.Rsp += 8;
            }
            const auto rip = static_cast<std::uintptr_t>(context.Rip);
            if (!rip) return 0;
            if ((rip >= four_image && rip < four_image_end) || (rip >= own_base && rip < own_end)) return rip;
            PVOID module{};
            if (!RtlPcToFileHeader(reinterpret_cast<PVOID>(rip), &module)) return rip;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
LONG CALLBACK watch_handler(EXCEPTION_POINTERS *info) {
    if (four_active.load(std::memory_order_acquire)) {
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 0xf)) return EXCEPTION_CONTINUE_SEARCH;
        const auto hit = info->ContextRecord->Dr6 & 0xf;
        info->ContextRecord->Dr6 = 0;
        auto rip = static_cast<std::uintptr_t>(info->ContextRecord->Rip);
        // Everything but this DLL's own reads counts: the game's scripts run as code outside its image.
        static const auto own = [] {
            MEMORY_BASIC_INFORMATION info{};
            VirtualQuery(&four_active, &info, sizeof(info));
            const auto base = reinterpret_cast<std::uintptr_t>(info.AllocationBase);
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            const auto size = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + static_cast<std::uintptr_t>(dos->e_lfanew))->OptionalHeader.SizeOfImage;
            return std::pair{base, base + size};
        }();
        if (rip >= own.first && rip < own.second) return EXCEPTION_CONTINUE_EXECUTION;
        bool game = rip >= four_image && rip < four_image_end;
        if (PVOID module{}; !game && RtlPcToFileHeader(reinterpret_cast<PVOID>(rip), &module)) {
            const auto caller = library_caller(info->ContextRecord, own.first, own.second);
            if (caller >= own.first && caller < own.second) return EXCEPTION_CONTINUE_EXECUTION; // the trainer's own memcpy
            if (caller) rip = caller;
            game = rip >= four_image && rip < four_image_end;
        }
        for (std::size_t slot = 0; slot < 4; ++slot)
            if (hit & (1ull << slot)) {
                (game ? four_counts : four_others)[slot].fetch_add(1, std::memory_order_relaxed);
                std::uintptr_t none{};
                (void)four_readers[slot].compare_exchange_strong(none, rip, std::memory_order_relaxed);
            }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    auto &w = watch();
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    info->ContextRecord->Dr6 = 0;
    if (watch_execute) info->ContextRecord->EFlags |= 0x10000; // resume past the breakpoint
    // An execution watch is told apart by who called: the return address on the stack.
    const auto rva = (watch_execute ? *reinterpret_cast<const std::uintptr_t *>(info->ContextRecord->Rsp)
                                    : static_cast<std::uintptr_t>(info->ContextRecord->Rip)) - w.base.load(std::memory_order_relaxed);
    float value{};
    if (const auto address = w.address.load(std::memory_order_relaxed); address && !watch_reads && !watch_execute) std::memcpy(&value, reinterpret_cast<const void *>(address), 4);
    for (std::size_t i = 0; i < w.rva.size(); ++i) {
        std::uintptr_t empty{};
        if (w.rva[i].load(std::memory_order_relaxed) == rva || w.rva[i].compare_exchange_strong(empty, rva) || empty == rva) {
            // The stack of this writer's first few hits whose caller differs.
            if (i < stacks.size()) {
                CONTEXT context = *info->ContextRecord;
                std::array<std::uintptr_t, stack_depth> frames{};
                for (std::size_t depth = 0; depth < frames.size(); ++depth) {
                    DWORD64 image{};
                    const auto function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
                    if (!function) break;
                    void *handler_data{};
                    DWORD64 establisher{};
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler_data, &establisher, nullptr);
                    if (!context.Rip) break;
                    frames[depth] = static_cast<std::uintptr_t>(context.Rip) - w.base.load(std::memory_order_relaxed);
                }
                auto &kept = stacks[i];
                for (auto &slot : kept) {
                    if (slot[0] == frames[0] && slot[1] == frames[1] && slot[2] == frames[2]) break;
                    if (!slot[0]) { slot = frames; break; }
                }
            }
            w.count[i].fetch_add(1, std::memory_order_relaxed);
            w.last[i].store(value, std::memory_order_relaxed);
            break;
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
void watch_registers(CONTEXT &context, std::uintptr_t address) {
    context.Dr0 = address;
    auto control = context.Dr7 & ~0x000f0003ull;
    if (address && watch_execute) control |= 1ull; // slot 0: execution (type 0, length 0)
    else if (address) control |= 1ull | ((watch_reads ? 3ull : 1ull) << 16) | (3ull << 18); // slot 0: writes (or reads too), four bytes
    context.Dr7 = control;
}
using SetVelocity = void (*)(std::uintptr_t, const float *);
std::atomic<SetVelocity> set_velocity_original{};
std::uintptr_t image_base{};
std::array<std::atomic<std::uintptr_t>, 32> setter_rva{};
std::array<std::atomic<std::uint64_t>, 32> setter_count{};
std::array<std::atomic<float>, 32> setter_last{};
void set_velocity_hook(std::uintptr_t body, const float *velocity) {
    const auto rva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - image_base;
    if (velocity)
        for (std::size_t i = 0; i < setter_rva.size(); ++i) {
            std::uintptr_t empty{};
            if (setter_rva[i].load(std::memory_order_relaxed) == rva || setter_rva[i].compare_exchange_strong(empty, rva) || empty == rva) {
                setter_count[i].fetch_add(1, std::memory_order_relaxed);
                setter_last[i].store(std::sqrt(velocity[0] * velocity[0] + velocity[2] * velocity[2]), std::memory_order_relaxed);
                break;
            }
        }
    if (const auto original = set_velocity_original.load(std::memory_order_acquire)) original(body, velocity);
}
std::size_t velocity_setters(Writer *out, std::size_t capacity) noexcept {
    std::size_t found{};
    for (std::size_t i = 0; i < setter_rva.size() && found < capacity; ++i)
        if (const auto rva = setter_rva[i].load()) {
            out[found++] = {rva, setter_count[i].exchange(0), setter_last[i].load()};
        }
    return found;
}
bool start_push_probe(std::uintptr_t base) noexcept {
    static bool attempted = false;
    if (attempted || !base) return false;
    attempted = true;
    image_base = base;
    {
        auto *setter = reinterpret_cast<void *>(base + 0x47d6910);
        void *relay{};
        if (hook_prepare(setter, reinterpret_cast<void *>(&set_velocity_hook), &relay) == HookOk && relay) {
            set_velocity_original.store(reinterpret_cast<SetVelocity>(relay), std::memory_order_release);
            (void)hook_enable(setter);
        }
    }
    auto *target = reinterpret_cast<void *>(base + 0x4745060);
    void *original{};
    if (hook_prepare(target, reinterpret_cast<void *>(&push_hook), &original) != HookOk || !original) return false;
    push_original.store(reinterpret_cast<Push>(original), std::memory_order_release);
    return hook_enable(target) == HookOk;
}
std::size_t watch_writes(std::uintptr_t address, std::uintptr_t base) noexcept {
    auto &w = watch();
    watch_reads = (address & 1) != 0; // an odd address asks for reads as well
    watch_execute = (address & 2) != 0; // bit 1: an execution watch of the address's function
    address &= ~std::uintptr_t{3};
    if (!w.handler) w.handler = AddVectoredExceptionHandler(1, watch_handler);
    w.base.store(base);
    w.address.store(address);
    if (address) stacks = {};
    if (address)
        for (std::size_t i = 0; i < w.rva.size(); ++i) {
            w.rva[i].store(0);
            w.count[i].store(0);
        }
    std::size_t armed{};
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
        const auto thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                watch_registers(context, address);
                if (SetThreadContext(thread, &context)) ++armed;
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    CONTEXT own{};
    own.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(GetCurrentThread(), &own)) {
        watch_registers(own, address);
        if (SetThreadContext(GetCurrentThread(), &own)) ++armed;
    }
    return armed;
}
std::size_t writers(Writer *out, std::size_t capacity) noexcept {
    auto &w = watch();
    std::size_t found{};
    for (std::size_t i = 0; i < w.rva.size() && found < capacity; ++i)
        if (const auto rva = w.rva[i].load()) out[found++] = {rva, w.count[i].load(), w.last[i].load()};
    return found;
}
std::size_t writer_stack(std::size_t writer, std::size_t which, std::uintptr_t *out, std::size_t capacity) noexcept {
    if (writer >= stacks.size() || which >= stacks[writer].size()) return 0;
    std::size_t count{};
    for (; count < capacity && count < stack_depth && stacks[writer][which][count]; ++count) out[count] = stacks[writer][which][count];
    return count;
}
namespace {
// One readable region: SEH instead of trusting the snapshot VirtualQuery gave.
std::size_t scan_region(const std::uint8_t *start, std::size_t size, const std::uint32_t *bits, std::size_t count, std::size_t window,
                        FloatHit *hits, std::size_t capacity, std::size_t found) noexcept {
    __try {
        const auto *words = reinterpret_cast<const std::uint32_t *>(start);
        const std::size_t total = size / 4, reach = window / 4;
        for (std::size_t i = 0; i < total; ++i) {
            if (words[i] != bits[0]) continue;
            const std::size_t low = i > reach ? i - reach : 0, high = std::min(total, i + reach + 1);
            FloatHit hit;
            hit.address = reinterpret_cast<std::uintptr_t>(words + i);
            bool all = true;
            for (std::size_t k = 1; k < count && all; ++k) {
                // The match nearest the anchor that an earlier equal number has not taken.
                bool placed = false;
                for (std::size_t distance = 1; distance <= reach && !placed; ++distance)
                    for (const auto j : {i + distance, i - distance}) {
                        if (j < low || j >= high || words[j] != bits[k]) continue;
                        const auto offset = static_cast<std::int32_t>((static_cast<std::int64_t>(j) - static_cast<std::int64_t>(i)) * 4);
                        bool taken = false;
                        for (std::size_t e = 1; e < k; ++e) taken = taken || hit.offsets[e] == offset;
                        if (taken) continue;
                        hit.offsets[k] = offset;
                        placed = true;
                        break;
                    }
                all = placed;
            }
            if (!all) continue;
            if (found < capacity) hits[found] = hit;
            ++found;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}
} // namespace
std::size_t scan_floats(const float *wanted, std::size_t count, std::size_t window, FloatHit *hits, std::size_t capacity, std::size_t &megabytes) noexcept {
    std::array<std::uint32_t, 24> bits{};
    count = std::min(count, bits.size());
    for (std::size_t i = 0; i < count; ++i) std::memcpy(&bits[i], &wanted[i], 4);
    std::size_t found{}, bytes{};
    MEMORY_BASIC_INFORMATION info{};
    for (std::uintptr_t at = 0x10000; at < 0x7fffffff0000ull && VirtualQuery(reinterpret_cast<void *>(at), &info, sizeof(info)) == sizeof(info);
         at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize) {
        if (info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE || info.Type == MEM_IMAGE) continue;
        found = scan_region(static_cast<const std::uint8_t *>(info.BaseAddress), info.RegionSize, bits.data(), count, window, hits, capacity, found);
        bytes += info.RegionSize;
    }
    megabytes = bytes >> 20;
    return found;
}
std::size_t watch_four(const std::uintptr_t *addresses, std::uintptr_t image, std::size_t image_size) noexcept {
    auto &w = watch();
    if (!w.handler) w.handler = AddVectoredExceptionHandler(1, watch_handler);
    four_image = image;
    four_image_end = image + image_size;
    bool any = false;
    for (std::size_t i = 0; i < 4; ++i) any = any || addresses[i];
    four_active.store(any, std::memory_order_release);
    const auto set = [&](CONTEXT &context) {
        context.Dr0 = addresses[0];
        context.Dr1 = addresses[1];
        context.Dr2 = addresses[2];
        context.Dr3 = addresses[3];
        auto control = context.Dr7 & ~0xffff00ffull;
        for (std::size_t slot = 0; slot < 4; ++slot)
            if (addresses[slot]) control |= (1ull << (slot * 2)) | (3ull << (16 + slot * 4)) | (3ull << (18 + slot * 4)); // reads and writes, four bytes
        context.Dr7 = control;
    };
    std::size_t armed{};
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
        const auto thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                set(context);
                if (SetThreadContext(thread, &context)) ++armed;
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    CONTEXT own{};
    own.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(GetCurrentThread(), &own)) {
        set(own);
        if (SetThreadContext(GetCurrentThread(), &own)) ++armed;
    }
    for (auto &count : four_counts) count.store(0, std::memory_order_relaxed);
    for (auto &count : four_others) count.store(0, std::memory_order_relaxed);
    for (auto &reader : four_readers) reader.store(0, std::memory_order_relaxed);
    return armed;
}
void watch_four_readers(std::uintptr_t *readers) noexcept {
    for (std::size_t i = 0; i < 4; ++i) readers[i] = four_readers[i].load(std::memory_order_relaxed);
}
void watch_four_counts(std::uint64_t *counts, std::uint64_t *others) noexcept {
    for (std::size_t i = 0; i < 4; ++i) counts[i] = four_counts[i].load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < 4 && others; ++i) others[i] = four_others[i].load(std::memory_order_relaxed);
}
namespace {
template <class Visit> void each_region(Visit &&visit) {
    MEMORY_BASIC_INFORMATION info{};
    for (std::uintptr_t at = 0x10000; at < 0x7fffffff0000ull && VirtualQuery(reinterpret_cast<void *>(at), &info, sizeof(info)) == sizeof(info);
         at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize) {
        if (info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE || info.Type == MEM_IMAGE) continue;
        visit(reinterpret_cast<std::uintptr_t>(info.BaseAddress), info.RegionSize);
    }
}
void collect_objects(std::uintptr_t start, std::size_t size, std::uintptr_t type, std::vector<std::uintptr_t> &objects) noexcept {
    __try {
        const auto *words = reinterpret_cast<const std::uintptr_t *>(start);
        for (std::size_t i = 0; i < size / 8; ++i)
            if (words[i] == type && objects.size() < 200000) objects.push_back(start + i * 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void collect_runs(std::uintptr_t start, std::size_t size, const std::vector<std::uintptr_t> &objects, std::uintptr_t low, std::uintptr_t high,
                  std::uint32_t wanted, PointerRun *runs, std::size_t capacity, std::size_t &found, std::array<std::uint32_t, 24> &histogram) noexcept {
    __try {
        const auto *words = reinterpret_cast<const std::uintptr_t *>(start);
        std::uint32_t run{};
        const std::size_t total = size / 8;
        for (std::size_t i = 0; i <= total; ++i) {
            const bool hit = i < total && words[i] >= low && words[i] <= high && std::binary_search(objects.begin(), objects.end(), words[i]);
            if (hit) { ++run; continue; }
            if (run >= 2) {
                ++histogram[std::min<std::size_t>(run, histogram.size() - 1)];
                if ((wanted ? run == wanted : true) && found < capacity) runs[found] = {start + (i - run) * 8, run};
                if (wanted ? run == wanted : true) ++found;
            }
            run = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
} // namespace
std::size_t pointer_runs(std::uintptr_t type, std::uint32_t wanted, PointerRun *runs, std::size_t capacity, std::size_t &found,
                         std::array<std::uint32_t, 24> &histogram) noexcept {
    found = 0;
    histogram = {};
    try {
        std::vector<std::uintptr_t> objects;
        each_region([&](std::uintptr_t start, std::size_t size) { collect_objects(start, size, type, objects); });
        std::ranges::sort(objects);
        if (objects.empty()) return 0;
        each_region([&](std::uintptr_t start, std::size_t size) {
            collect_runs(start, size, objects, objects.front(), objects.back(), wanted, runs, capacity, found, histogram);
        });
        return objects.size();
    } catch (...) { return 0; }
}
std::uint64_t push_sample(PushSample &out) noexcept {
    out = latest;
    return push_calls.load(std::memory_order_acquire);
}
} // namespace dingosdk::trainer
