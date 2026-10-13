#include "tricks_states.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <string_view>

namespace dingosdk::tricks {
namespace {
// The skater's pages hang off its AntDrivenComponent: + 0x68 is the animatable, which leads to the
// block of pages (known by its vtable) two ways, + 0xe8 -> + 0x8 and + 0x1d8 -> + 0x248; page n is
// at block + 0x60 + n * 8. A handle's low 12 bits pick the page, the next 11 the value's place in
// it, and a bit map at page + 0x71c says whether the page holds it.
constexpr std::uintptr_t ant_driven_vtable = 0x63aaee8, states_block_vtable = 0x61a8bd8;
constexpr std::uintptr_t component_collection = 0x70, collection_count = 0x8, collection_first = 0x20, collection_step = 0x20;
constexpr std::uintptr_t driven_animatable = 0x68, block_pages = 0x60, page_map = 0x71c;
constexpr std::uintptr_t block_ways[2][2]{{0xe8, 0x8}, {0x1d8, 0x248}};
constexpr std::uintptr_t object_after_id = 0x10, object_name = 0x18, object_handle = 0x28;

template <class T> bool peek(std::uintptr_t address, T &value) noexcept {
    __try {
        std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(T));
        return true;
    } __except (1) { return false; }
}
bool pointer(std::uintptr_t value) noexcept { return value > 0x10000 && value < 0x7fffffffffffull && !(value & 3); }

struct StateObject {
    std::uint8_t id[16];
    std::string_view name;
    std::atomic<std::uintptr_t> at{};
};
// The asset ids are the game data's (animation/dingo, build 20260929).
StateObject objects[state_count]{
    {{0xd7, 0x76, 0xa1, 0xd7, 0x4d, 0xae, 0xa5, 0xf7, 0xd7, 0xa1, 0x76, 0xd7, 0xae, 0x4d, 0xf7, 0xa5}, "Animation/Dingo/Bool.Intent.WantsFreestyle"},
    {{0x91, 0x28, 0xec, 0x7c, 0x31, 0x1b, 0xac, 0x4b, 0x7c, 0xec, 0x28, 0x91, 0x1b, 0x31, 0x4b, 0xac}, "Animation/Dingo/EnumGS.Intent.FreestyleTrick"},
    {{0x24, 0x08, 0x12, 0x75, 0xda, 0x59, 0x13, 0x16, 0x75, 0x12, 0x08, 0x24, 0x59, 0xda, 0x16, 0x13}, "Animation/Dingo/EnumGS.Intent.FreeStyleFlipTrick"},
    {{0x65, 0x71, 0x12, 0x9a, 0xe5, 0xba, 0x8a, 0x15, 0x9a, 0x12, 0x71, 0x65, 0xba, 0xe5, 0x15, 0x8a}, "Animation/Dingo/Bool.Intent.EnableCaspers"},
    {{0x0e, 0xd7, 0xd4, 0x89, 0xd5, 0x00, 0xe8, 0x73, 0x89, 0xd4, 0xd7, 0x0e, 0x00, 0xd5, 0x73, 0xe8}, "Animation/Dingo/Enum.GS.CurrentGroundTrick"},
};
// Bools are one byte, enumerations four.
constexpr bool is_enum[state_count]{false, true, true, false, true};

bool is_object(const StateObject &object, std::uintptr_t id_at) noexcept {
    __try {
        if (std::memcmp(reinterpret_cast<const void *>(id_at), object.id, sizeof(object.id)) != 0) return false;
        const auto name = *reinterpret_cast<const std::uintptr_t *>(id_at + object_after_id + object_name);
        if (name <= id_at || name > id_at + 0x400) return false;
        return _strnicmp(reinterpret_cast<const char *>(name), object.name.data(), object.name.size()) == 0 &&
               *reinterpret_cast<const char *>(name + object.name.size()) == 0;
    } __except (1) { return false; }
}
bool has_handle(std::uintptr_t at) noexcept {
    std::uint32_t handle{};
    return peek(at + object_handle, handle) && (handle >> 24);
}
void look_at_region(std::uintptr_t start, std::size_t size) noexcept {
    __try {
        const auto *words = reinterpret_cast<const std::uint32_t *>(start);
        for (std::size_t i = 0; i + 0x20 < size / 4; ++i) {
            for (auto &object : objects) {
                std::uint32_t first;
                std::memcpy(&first, object.id, 4);
                if (words[i] != first) continue;
                // An object can be in memory more than once and only one copy has its handle filled
                // in: a copy with a handle replaces one without.
                const auto known = object.at.load(std::memory_order_relaxed), here = start + i * 4 + object_after_id;
                if (known == here || !is_object(object, start + i * 4)) continue;
                if (known && has_handle(known)) continue;
                if (known && !has_handle(here)) continue;
                object.at.store(here, std::memory_order_relaxed);
            }
        }
    } __except (1) {}
}

std::uintptr_t value_address(StateObject &object, std::uintptr_t block) noexcept {
    const auto at = object.at.load(std::memory_order_relaxed);
    if (!at) return 0;
    if (!is_object(object, at - object_after_id)) {
        object.at.store(0, std::memory_order_relaxed); // unloaded with its level
        return 0;
    }
    std::uint32_t handle{}, map{};
    std::uintptr_t page{};
    if (!peek(at + object_handle, handle) || !(handle >> 24)) return 0;
    const std::uint32_t index = handle & 0xfff, place = (handle >> 12) & 0x7ff;
    if (index >= 16 || !peek(block + block_pages + index * 8, page) || !pointer(page) || !peek(page + page_map + (place >> 5) * 4, map) ||
        !(map >> (place & 31) & 1))
        return 0;
    return page + place;
}
std::uintptr_t states_block(std::uintptr_t base, std::uintptr_t entity) noexcept {
    std::uintptr_t collection{}, owner{};
    std::uint8_t count{};
    if (!entity || !peek(entity + component_collection, collection) || !pointer(collection) || !peek(collection, owner) || owner != entity ||
        !peek(collection + collection_count, count) || count > 128)
        return 0;
    for (unsigned i = 0; i < count; ++i) {
        std::uintptr_t component{}, type{}, animatable{};
        if (!peek(collection + collection_first + i * collection_step, component) || !pointer(component) || !peek(component, type) ||
            type != base + ant_driven_vtable)
            continue;
        if (!peek(component + driven_animatable, animatable) || !pointer(animatable)) return 0;
        for (const auto &way : block_ways) {
            std::uintptr_t holder{}, block{};
            if (peek(animatable + way[0], holder) && pointer(holder) && peek(holder + way[1], block) && pointer(block) && peek(block, type) &&
                type == base + states_block_vtable)
                return block;
        }
        return 0;
    }
    return 0;
}

// The game's lookup of a state's value: (state object, context with the pages) -> where the value
// is, 0 when the context has none. These are its first bytes.
constexpr std::uintptr_t state_lookup = 0x11dcdd0;
constexpr unsigned char state_lookup_bytes[]{0x48, 0x83, 0xec, 0x28, 0x80, 0x79, 0x2b, 0x00, 0x4c, 0x8b, 0xd2, 0x4c, 0x8b, 0xc9};
using StateLookup = std::uintptr_t (*)(std::uintptr_t object, std::uintptr_t context);
struct Held {
    std::atomic<bool> on{};
    std::atomic<std::uint32_t> value{}, asked{};
    volatile std::uint32_t answer[4]{}; // what the asker reads (and may write through)
};
Held held[state_count];
std::atomic<StateLookup> lookup_original{};
std::atomic<std::uint64_t> holds_until{};
std::atomic<bool> lookup_attempted{}, lookup_ready{};
bool holds_alive() noexcept { return GetTickCount64() < holds_until.load(std::memory_order_relaxed); }

// One more state answered at the lookup, by its object: whatever the game's own code writes, a reader gets this value
// while it is held (the animation graph's copy of the physics state, for a pose caught in the air).
struct HeldObject {
    std::atomic<std::uintptr_t> object{};
    std::atomic<std::uint32_t> value{}, asked{};
    std::atomic<std::uint64_t> until{};
    volatile std::uint32_t answer[4]{};
};
HeldObject held_object;
std::uintptr_t state_lookup_hook(std::uintptr_t object, std::uintptr_t context) {
    if (object && object == held_object.object.load(std::memory_order_relaxed) && GetTickCount64() < held_object.until.load(std::memory_order_relaxed)) {
        held_object.asked.fetch_add(1, std::memory_order_relaxed);
        held_object.answer[0] = held_object.value.load(std::memory_order_relaxed);
        return reinterpret_cast<std::uintptr_t>(&held_object.answer[0]);
    }
    if (holds_alive())
        for (std::size_t i = 0; i < state_count; ++i) {
            auto &h = held[i];
            if (!h.on.load(std::memory_order_relaxed) || objects[i].at.load(std::memory_order_relaxed) != object) continue;
            h.asked.fetch_add(1, std::memory_order_relaxed);
            // Put back for every asker: the game may write through the answer.
            h.answer[0] = h.value.load(std::memory_order_relaxed);
            return reinterpret_cast<std::uintptr_t>(&h.answer[0]);
        }
    const auto original = lookup_original.load(std::memory_order_acquire);
    return original ? original(object, context) : 0;
}
bool code_known(std::uintptr_t at, const unsigned char *bytes, std::size_t size) noexcept {
    __try {
        return std::memcmp(reinterpret_cast<const void *>(at), bytes, size) == 0;
    } __except (1) { return false; }
}
bool lookup_code_known(std::uintptr_t base) noexcept { return code_known(base + state_lookup, state_lookup_bytes, sizeof(state_lookup_bytes)); }

// The scripts' game-state natives: (TheKey: pointer to the entity key, GameState: pointer to the state reference
// (asset pointer, bit 2 a flag), value). The getters write their value through a third pointer.
constexpr std::uintptr_t set_bool_native = 0x1090600, set_int_native = 0x1090650, get_bool_native = 0x108fa70, get_enum_native = 0x108fac0;
constexpr unsigned char set_bool_bytes[]{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x41, 0x0f, 0xb6, 0xd8, 0x48, 0xc7, 0x44, 0x24, 0x48};
constexpr unsigned char set_int_bytes[]{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x41, 0x8b, 0xd8, 0x48, 0xc7, 0x44, 0x24, 0x48};
constexpr unsigned char get_bool_bytes[]{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x49, 0x8b, 0xd8, 0x41, 0xc6, 0x00, 0x00};
constexpr unsigned char get_enum_bytes[]{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x33, 0xc0, 0x4c, 0x8d, 0x0d};
using SetBool = void (*)(const std::uint64_t *key, const std::uintptr_t *state, std::uint8_t value);
using SetInt = void (*)(const std::uint64_t *key, const std::uintptr_t *state, std::int32_t value);
using GetBool = void (*)(const std::uint64_t *key, const std::uintptr_t *state, std::uint8_t *out);
using GetEnum = void (*)(const std::uint64_t *key, const std::uintptr_t *state, std::int32_t *out);
std::atomic<SetBool> set_bool_original{};
std::atomic<bool> setters_attempted{}, setters_ready{}, key_known{};
std::atomic<std::uint64_t> captured_key{};
// The state reference the game passes points this far before our object (`at`): learned from the EnableCaspers write.
std::atomic<std::intptr_t> reference_delta{};
std::atomic<bool> delta_known{};
std::atomic<bool> resolved{}; // every state came from the game's asset lookup: the memory search leaves them alone

void set_bool_hook(const std::uint64_t *key, const std::uintptr_t *state, std::uint8_t value) {
    auto &caspers = objects[static_cast<std::size_t>(State::enable_caspers)];
    std::uintptr_t reference{};
    std::uint64_t k{};
    if (key && state && peek(reinterpret_cast<std::uintptr_t>(state), reference) && peek(reinterpret_cast<std::uintptr_t>(key), k) &&
        (reference &= ~std::uintptr_t{4})) {
        // Is this EnableCaspers? Its object (id, then name at + 0x18) sits within a few words of where the reference
        // points; the memory search need not have found it yet.
        for (std::intptr_t delta = -0x40; delta <= 0x40; delta += 8) {
            const auto at = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(reference) + delta);
            if (!is_object(caspers, at - object_after_id)) continue;
            reference_delta.store(delta, std::memory_order_relaxed);
            delta_known.store(true, std::memory_order_relaxed);
            caspers.at.store(at, std::memory_order_relaxed);
            captured_key.store(k, std::memory_order_relaxed);
            key_known.store(true, std::memory_order_release);
            break;
        }
    }
    const auto original = set_bool_original.load(std::memory_order_acquire);
    if (original) original(key, state, value);
}
std::uintptr_t reference_of(State state) noexcept {
    const auto at = objects[static_cast<std::size_t>(state)].at.load(std::memory_order_relaxed);
    if (!at || !delta_known.load(std::memory_order_relaxed)) return 0;
    return at - reference_delta.load(std::memory_order_relaxed);
}
}

bool search_state_objects() noexcept {
    if (resolved.load(std::memory_order_relaxed)) return state_objects_known();
    MEMORY_BASIC_INFORMATION info{};
    for (std::uintptr_t at = 0x10000; at < 0x7fffffff0000ull && VirtualQuery(reinterpret_cast<void *>(at), &info, sizeof(info)) == sizeof(info);
         at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize) {
        if (info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE || info.Type != MEM_PRIVATE) continue;
        look_at_region(reinterpret_cast<std::uintptr_t>(info.BaseAddress), info.RegionSize);
    }
    return state_objects_known();
}
bool state_objects_known() noexcept {
    // EnableCaspers is not needed to drive the freestyle node.
    for (std::size_t i = 0; i < state_count; ++i)
        if (static_cast<State>(i) != State::enable_caspers && !objects[i].at.load(std::memory_order_relaxed)) return false;
    return true;
}
void forget_state_objects() noexcept {
    for (auto &object : objects) object.at.store(0, std::memory_order_relaxed);
}
int state_reason(std::uintptr_t base, std::uintptr_t entity, State state, std::uint32_t &handle) noexcept {
    handle = 0;
    auto &object = objects[static_cast<std::size_t>(state)];
    const auto at = object.at.load(std::memory_order_relaxed);
    if (!at) return 0;
    if (!peek(at + object_handle, handle) || !(handle >> 24)) return 1;
    const auto block = states_block(base, entity);
    return block && value_address(object, block) ? 3 : 2;
}
bool read_state(std::uintptr_t base, std::uintptr_t entity, State state, std::uint32_t &value) noexcept {
    const auto i = static_cast<std::size_t>(state);
    const auto block = states_block(base, entity);
    const auto at = block ? value_address(objects[i], block) : 0;
    if (!at) return false;
    if (is_enum[i]) return peek(at, value);
    std::uint8_t flag{};
    if (!peek(at, flag)) return false;
    value = flag;
    return true;
}
bool start_holds(std::uintptr_t base) noexcept {
    if (!base || lookup_attempted.exchange(true)) return lookup_ready.load(std::memory_order_relaxed);
    if (!lookup_code_known(base)) return false;
    auto *target = reinterpret_cast<void *>(base + state_lookup);
    void *original{};
    if (hook_prepare(target, reinterpret_cast<void *>(&state_lookup_hook), &original) != HookOk || !original) return false;
    lookup_original.store(reinterpret_cast<StateLookup>(original), std::memory_order_release);
    if (hook_enable(target) != HookOk) return false;
    lookup_ready.store(true, std::memory_order_relaxed);
    return true;
}
void hold(State state, bool on, std::uint32_t value) noexcept {
    auto &h = held[static_cast<std::size_t>(state)];
    h.value.store(value, std::memory_order_relaxed);
    h.on.store(on, std::memory_order_relaxed);
}
void hold_object(std::uintptr_t object, std::uint32_t value, std::uint64_t milliseconds) noexcept {
    held_object.value.store(value, std::memory_order_relaxed);
    held_object.object.store(object, std::memory_order_relaxed);
    held_object.until.store(GetTickCount64() + milliseconds, std::memory_order_relaxed);
}
std::uint32_t held_object_asked() noexcept { return held_object.asked.exchange(0, std::memory_order_relaxed); }
// A state's value where the skater's animation keeps it (its page in the states block), written in place.
bool poke_state_value(std::uintptr_t base, std::uintptr_t entity, std::uintptr_t at, std::uint32_t value, std::uint32_t *before) noexcept {
    const auto block = states_block(base, entity);
    std::uint32_t handle{}, map{};
    std::uintptr_t page{};
    if (!block || !at || !peek(at + object_handle, handle) || !(handle >> 24)) return false;
    const std::uint32_t index = handle & 0xfff, place = (handle >> 12) & 0x7ff;
    if (index >= 16 || !peek(block + block_pages + index * 8, page) || !pointer(page) || !peek(page + page_map + (place >> 5) * 4, map) ||
        !(map >> (place & 31) & 1))
        return false;
    __try {
        auto *stored = reinterpret_cast<volatile std::uint32_t *>(page + place);
        if (before) *before = *stored;
        *stored = value;
        return true;
    } __except (1) { return false; }
}
void keep_holds_alive() noexcept { holds_until.store(GetTickCount64() + 500, std::memory_order_relaxed); }
std::uint32_t held_asked(State state) noexcept { return held[static_cast<std::size_t>(state)].asked.exchange(0, std::memory_order_relaxed); }

// The other states, by name, from the asset domain the game's own EnableCaspers reference lives in (and its parents,
// as the native lookup searches): the very objects the game uses, which a memory search can only guess at (a state
// object can be in memory more than once, and copies come and go with bundles).
int resolve_state_objects(std::uintptr_t base) noexcept {
    const auto find = game::native_data().find_asset;
    const auto caspers = objects[static_cast<std::size_t>(State::enable_caspers)].at.load(std::memory_order_relaxed);
    if (!find || !caspers || !key_known.load(std::memory_order_acquire)) return -1;
    int found = 0;
    __try {
        std::uint16_t first{};
        if (!peek(caspers + 0x16, first)) return -1;
        for (std::size_t i = 0; i < state_count; ++i) {
            auto &object = objects[i];
            if (static_cast<State>(i) == State::enable_caspers) continue;
            std::uint16_t domain = first;
            for (int hops = 0; hops < 32 && domain < 0xbbf; ++hops) {
                std::uintptr_t owner{};
                if (!peek(base + addr::engine::domain_owners + domain * std::uintptr_t{8}, owner) || !owner) break;
                const auto asset = find(domain, object.name.data());
                if (asset && is_object(object, asset - object_after_id)) {
                    object.at.store(asset, std::memory_order_relaxed);
                    ++found;
                    break;
                }
                if (!peek(owner + 0x42, domain)) break;
            }
        }
    } __except (1) { return -2; }
    resolved.store(found == static_cast<int>(state_count) - 1, std::memory_order_relaxed);
    return found;
}
bool state_objects_resolved() noexcept { return resolved.load(std::memory_order_relaxed); }

std::uintptr_t find_state(std::uintptr_t base, const char *name) noexcept {
    const auto find = game::native_data().find_asset;
    const auto caspers = objects[static_cast<std::size_t>(State::enable_caspers)].at.load(std::memory_order_relaxed);
    if (!find || !caspers || !name || !key_known.load(std::memory_order_acquire)) return 0;
    __try {
        std::uint16_t domain{};
        if (!peek(caspers + 0x16, domain)) return 0;
        for (int hops = 0; hops < 32 && domain < 0xbbf; ++hops) {
            std::uintptr_t owner{};
            if (!peek(base + addr::engine::domain_owners + domain * std::uintptr_t{8}, owner) || !owner) return 0;
            if (const auto asset = find(domain, name)) {
                // The name it carries is the one asked for (a state object: name at + 0x18).
                const auto text = *reinterpret_cast<const std::uintptr_t *>(asset + object_name);
                if (text && _stricmp(reinterpret_cast<const char *>(text), name) == 0) return asset;
                return 0;
            }
            if (!peek(owner + 0x42, domain)) return 0;
        }
    } __except (1) {}
    return 0;
}
// SetFloatGameState: (key, state reference, value in xmm2).
constexpr std::uintptr_t set_float_native = 0x10906a0;
constexpr unsigned char set_float_bytes[]{0x48, 0x83, 0xec, 0x38, 0xc5, 0xf8, 0x29, 0x74, 0x24, 0x20, 0x4c, 0x8d, 0x0d};
using SetFloat = void (*)(const std::uint64_t *key, const std::uintptr_t *state, float value);
bool write_state_at(std::uintptr_t base, std::uintptr_t state, int kind, double value) noexcept {
    if (!state || !setters_ready.load(std::memory_order_relaxed) || !skater_key_known()) return false;
    const std::uint64_t key = captured_key.load(std::memory_order_relaxed);
    const std::uintptr_t reference = state - reference_delta.load(std::memory_order_relaxed);
    __try {
        if (kind == 0) set_bool_original.load(std::memory_order_acquire)(&key, &reference, static_cast<std::uint8_t>(value != 0));
        else if (kind == 1) reinterpret_cast<SetInt>(base + set_int_native)(&key, &reference, static_cast<std::int32_t>(value));
        else if (kind == 2 && code_known(base + set_float_native, set_float_bytes, sizeof(set_float_bytes)))
            reinterpret_cast<SetFloat>(base + set_float_native)(&key, &reference, static_cast<float>(value));
        else return false;
        return true;
    } __except (1) { return false; }
}
bool read_state_at(std::uintptr_t base, std::uintptr_t state, int kind, double &value) noexcept {
    if (!state || !setters_ready.load(std::memory_order_relaxed) || !skater_key_known()) return false;
    const std::uint64_t key = captured_key.load(std::memory_order_relaxed);
    const std::uintptr_t reference = state - reference_delta.load(std::memory_order_relaxed);
    __try {
        if (kind == 0) {
            std::uint8_t out{};
            reinterpret_cast<GetBool>(base + get_bool_native)(&key, &reference, &out);
            value = out;
        } else if (kind == 1) {
            std::int32_t out{};
            reinterpret_cast<GetEnum>(base + get_enum_native)(&key, &reference, &out);
            value = out;
        } else return false;
        return true;
    } __except (1) { return false; }
}

bool start_setter_watch(std::uintptr_t base) noexcept {
    if (!base || setters_attempted.exchange(true)) return setters_ready.load(std::memory_order_relaxed);
    if (!code_known(base + set_bool_native, set_bool_bytes, sizeof(set_bool_bytes)) || !code_known(base + set_int_native, set_int_bytes, sizeof(set_int_bytes)) ||
        !code_known(base + get_bool_native, get_bool_bytes, sizeof(get_bool_bytes)) || !code_known(base + get_enum_native, get_enum_bytes, sizeof(get_enum_bytes)))
        return false;
    auto *target = reinterpret_cast<void *>(base + set_bool_native);
    void *original{};
    if (hook_prepare(target, reinterpret_cast<void *>(&set_bool_hook), &original) != HookOk || !original) return false;
    set_bool_original.store(reinterpret_cast<SetBool>(original), std::memory_order_release);
    if (hook_enable(target) != HookOk) return false;
    setters_ready.store(true, std::memory_order_relaxed);
    return true;
}
bool skater_key_known() noexcept { return key_known.load(std::memory_order_acquire) && delta_known.load(std::memory_order_relaxed); }
std::uint64_t skater_key() noexcept { return captured_key.load(std::memory_order_relaxed); }
void forget_skater_key() noexcept {
    key_known.store(false, std::memory_order_relaxed);
    resolved.store(false, std::memory_order_relaxed);
}
bool write_state(std::uintptr_t base, State state, std::uint32_t value) noexcept {
    if (!setters_ready.load(std::memory_order_relaxed) || !skater_key_known()) return false;
    const std::uint64_t key = captured_key.load(std::memory_order_relaxed);
    const std::uintptr_t reference = reference_of(state);
    if (!reference) return false;
    __try {
        if (is_enum[static_cast<std::size_t>(state)])
            reinterpret_cast<SetInt>(base + set_int_native)(&key, &reference, static_cast<std::int32_t>(value));
        else
            set_bool_original.load(std::memory_order_acquire)(&key, &reference, static_cast<std::uint8_t>(value != 0));
        return true;
    } __except (1) { return false; }
}
bool game_read_state(std::uintptr_t base, State state, std::int32_t &value) noexcept {
    if (!setters_ready.load(std::memory_order_relaxed) || !skater_key_known()) return false;
    const std::uint64_t key = captured_key.load(std::memory_order_relaxed);
    const std::uintptr_t reference = reference_of(state);
    if (!reference) return false;
    __try {
        if (is_enum[static_cast<std::size_t>(state)]) {
            std::int32_t out{};
            reinterpret_cast<GetEnum>(base + get_enum_native)(&key, &reference, &out);
            value = out;
        } else {
            std::uint8_t out{};
            reinterpret_cast<GetBool>(base + get_bool_native)(&key, &reference, &out);
            value = out;
        }
        return true;
    } __except (1) { return false; }
}
} // namespace dingosdk::tricks
