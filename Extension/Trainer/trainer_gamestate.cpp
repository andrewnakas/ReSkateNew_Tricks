#include "trainer_gamestate.h"
#include <atomic>
#include <cmath>
#include <cstring>

namespace dingosdk::trainer {
namespace {
// Game build 20260929.
//
// A game state is an asset object in memory: its 16-byte asset id, then the object, with its
// name at + 0x18 and at + 0x28 the handle the game finds its value by (0x11dcd40): the low 12
// bits pick one of the skater's state pages, the next 11 the value's place in that page, and a
// bit map at page + 0x71c says whether the page holds it. The handles differ from level to
// level, so they are read from the objects each time; the objects are found by their asset id
// in the memory search (trainer_classes.cpp).
//
// The skater's pages hang off its AntDrivenComponent: + 0x68 is the animatable, which leads to
// the block of pages (known by its vtable) two ways, + 0xe8 -> + 0x8 and + 0x1d8 -> + 0x248;
// page n is at block + 0x60 + n * 8.
constexpr std::uintptr_t ant_driven_vtable = 0x63aaee8, states_block_vtable = 0x61a8bd8;
constexpr std::uintptr_t component_collection = 0x70, collection_count = 0x8, collection_first = 0x20, collection_step = 0x20;
constexpr std::uintptr_t driven_animatable = 0x68, block_pages = 0x60, page_map = 0x71c;
constexpr std::uintptr_t block_ways[2][2]{{0xe8, 0x8}, {0x1d8, 0x248}};
constexpr std::uintptr_t object_after_id = 0x10, object_name = 0x18, object_handle = 0x28;
constexpr std::uint32_t trick_count = 59;

template <class T> bool peek(std::uintptr_t address, T &value) noexcept {
    __try {
        std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(T));
        return true;
    } __except (1) { return false; }
}
bool put(std::uintptr_t address, float value) noexcept {
    __try {
        *reinterpret_cast<volatile float *>(address) = value;
        return true;
    } __except (1) { return false; }
}
bool pointer(std::uintptr_t value) noexcept { return value > 0x10000 && value < 0x7fffffffffffull && !(value & 3); }

// The two states a flip trick's speed is driven by: the speed its clip plays at, which the game
// copies from the trick scripts' Float.Intent.TrickFlipSpeed once when the board is flicked
// (writing the copy in the air slows or speeds the flip; writing the scripts' number then does
// nothing, and nothing that sets the pop reads either), and the trick that is turning.
struct StateObject {
    std::uint8_t id[16];
    std::string_view name;
    std::atomic<std::uintptr_t> at{};
};
StateObject flip_speed_object{{0xb0, 0x7b, 0x6c, 0x08, 0x42, 0xe7, 0xef, 0xc7, 0x08, 0x6c, 0x7b, 0xb0, 0xe7, 0x42, 0xc7, 0xef},
                              "Animation/Dingo/Float.Anim.TrickFlipSpeed"};
StateObject flip_trick_object{{0x17, 0x54, 0x40, 0x3c, 0xa7, 0x06, 0x61, 0x3d, 0x3c, 0x40, 0x54, 0x17, 0x06, 0xa7, 0x3d, 0x61},
                              "Animation/Dingo/EnumGS.Anim.CurrentFlipTrick"};
StateObject pumping_object{{0xab, 0x5b, 0xd5, 0x21, 0x9f, 0xa4, 0xb0, 0xfb, 0x21, 0xd5, 0x5b, 0xab, 0xa4, 0x9f, 0xfb, 0xb0}, "Animation/Dingo/Bool.Intent.Pumping"};
StateObject pump_strength_object{{0x54, 0xc6, 0x59, 0x6a, 0xbb, 0xad, 0xfc, 0x05, 0x6a, 0x59, 0xc6, 0x54, 0xad, 0xbb, 0x05, 0xfc},
                                 "Animation/Dingo/Float.Intent.UserTriggeredPumpStrength"};
StateObject pump_assist_object{{0xcb, 0xdf, 0x5b, 0x53, 0x14, 0x53, 0xc8, 0x29, 0x53, 0x5b, 0xdf, 0xcb, 0x53, 0x14, 0x29, 0xc8},
                               "Animation/Dingo/Float.Anim.PumpingAssistAmount"};
StateObject *const objects[]{&flip_speed_object, &flip_trick_object, &pumping_object, &pump_strength_object, &pump_assist_object};

// The object behind an asset id seen at `id_at`, when it is that state's (its name says so).
bool is_object(const StateObject &object, std::uintptr_t id_at) noexcept {
    __try {
        if (std::memcmp(reinterpret_cast<const void *>(id_at), object.id, sizeof(object.id)) != 0) return false;
        const auto name = *reinterpret_cast<const std::uintptr_t *>(id_at + object_after_id + object_name);
        if (name <= id_at || name > id_at + 0x400) return false;
        return _strnicmp(reinterpret_cast<const char *>(name), object.name.data(), object.name.size()) == 0 &&
               *reinterpret_cast<const char *>(name + object.name.size()) == 0;
    } __except (1) { return false; }
}
// Where the skater's value of that state is; 0 when it has none.
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

// EnumGS.Anim.CurrentFlipTrick, as the game names its values.
constexpr std::string_view game_trick_names[trick_count]{
    "None", "Ollie", "Kickflip", "Heelflip", "PopShuvit", "VarialKickflip", "InwardHeelflip", "FSPopShuvit", "VarialHeelflip", "Hardflip",
    "360PopShuvit", "360Flip", "360InwardHeelflip", "FS360PopShuvit", "Laserflip", "360Hardflip", "Nollie", "N_Kickflip", "N_Heelflip",
    "N_PopShuvit", "N_Hardflip", "N_VarialHeelflip", "N_FSPopShuvit", "N_VarialKickflip", "N_InwardHeelflip", "N_FS360PopShuvit",
    "N_360Hardflip", "N_Laserflip", "N_360PopShuvit", "N_360Flip", "N_360InwardHeelflip", "OlliePop", "N_OlliePop", "Wallie", "FSWallie",
    "BSWallie", "Impossible", "FrontFoot_Impossible", "N_Impossible", "N_BackFoot_Impossible", "LateFlipTrick", "HeldFlipTrick", "BonedOllie",
    "KickflipUnderflip", "HeelflipUnderflip", "BSPopShuvitUnderflip", "FSPopShuvitUnderflip", "NollieKickflipUnderflip",
    "NollieHeelflipUnderflip", "NollieBSPopShuvitUnderflip", "NollieFSPopShuvitUnderflip", "HospitalFlip", "FlowerFlip", "NollieHospitalFlip",
    "NollieFlowerFlip", "ImpossibleUnderflip", "FrontFootImpossibleUnderflip", "NollieImpossibleUnderflip", "NollieBackFootImpossibleUnderflip"};
// Index into flip_tricks for each value, -1 where a trick has no speed of its own.
constexpr std::int8_t trick_slider[trick_count]{
    -1, -1, 0, 1, 2, 6, 11, 3, 7, 10,
    4, 8, 13, 5, 9, 12, -1, 0, 1,
    2, 10, 7, 3, 6, 11, 5,
    12, 9, 4, 8, 13, -1, -1, -1, -1,
    -1, 14, 15, 14, 15, -1, -1, -1,
    -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1};
}

void find_state_objects(std::uintptr_t start, std::size_t size) noexcept {
    __try {
        const auto *words = reinterpret_cast<const std::uint32_t *>(start);
        for (std::size_t i = 0; i + 0x20 < size / 4; ++i)
            for (auto *object : objects) {
                std::uint32_t first;
                std::memcpy(&first, object->id, 4);
                if (words[i] == first && !object->at.load(std::memory_order_relaxed) && is_object(*object, start + i * 4))
                    object->at.store(start + i * 4 + object_after_id, std::memory_order_relaxed);
            }
    } __except (1) {}
}
bool flip_states_found() noexcept {
    return flip_speed_object.at.load(std::memory_order_relaxed) && flip_trick_object.at.load(std::memory_order_relaxed);
}
FlipState read_flip_state(std::uintptr_t base, std::uintptr_t entity) noexcept {
    if (!flip_states_found()) return {};
    const auto block = states_block(base, entity);
    if (!block) return {};
    FlipState state;
    const auto speed_at = value_address(flip_speed_object, block), trick_at = value_address(flip_trick_object, block);
    if (!speed_at || !trick_at || !peek(trick_at, state.trick) || !peek(speed_at, state.speed)) return {};
    // Anything else there means this is not the layout this was written for.
    if (state.trick >= trick_count || !std::isfinite(state.speed) || state.speed < 0 || state.speed > 1000) return {};
    state.speed_at = speed_at;
    return state;
}
PumpState read_pump_state(std::uintptr_t base, std::uintptr_t entity) noexcept {
    PumpState state;
    if (!pumping_object.at.load(std::memory_order_relaxed) && !pump_strength_object.at.load(std::memory_order_relaxed)) return state;
    const auto block = states_block(base, entity);
    if (!block) return state;
    std::uint8_t flag{};
    float number{};
    if (const auto at = value_address(pumping_object, block); at && peek(at, flag)) {
        state.found = true;
        state.pumping = flag != 0;
    }
    if (const auto at = value_address(pump_strength_object, block); at && peek(at, number) && std::isfinite(number)) state.strength = number;
    if (const auto at = value_address(pump_assist_object, block); at && peek(at, number) && std::isfinite(number)) state.assist = number;
    return state;
}
std::array<std::uintptr_t, 6> state_pages(std::uintptr_t base, std::uintptr_t entity) noexcept {
    std::array<std::uintptr_t, 6> pages{};
    if (const auto block = states_block(base, entity))
        for (std::size_t k = 0; k < pages.size(); ++k)
            if (!peek(block + block_pages + k * 8, pages[k]) || !pointer(pages[k])) pages[k] = 0;
    return pages;
}
bool write_flip_speed(const FlipState &state, float speed) noexcept {
    return state && std::isfinite(speed) && put(state.speed_at, speed);
}
int flip_trick_index(std::uint32_t trick) noexcept { return trick < trick_count ? trick_slider[trick] : -1; }
std::string_view flip_trick_name(std::uint32_t trick) noexcept { return trick < trick_count ? game_trick_names[trick] : std::string_view{}; }
}
