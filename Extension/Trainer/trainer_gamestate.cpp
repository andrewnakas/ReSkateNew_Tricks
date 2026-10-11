#include "trainer_gamestate.h"
#include "Engine/Core/Hooks/hooks.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

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
// Float.Anim.TrickFlipSpeedClamped: what the six impossible clips (seq_c_proto_onb_trick_o_impossible_*) play at, where
// every other trick clip plays at TrickFlipSpeed (the game's clip data, 2026-10-11).
StateObject flip_clamped_object{{0x36, 0xac, 0x11, 0x7c, 0x47, 0xc8, 0x20, 0x44, 0x7c, 0x11, 0xac, 0x36, 0xc8, 0x47, 0x44, 0x20},
                                "Animation/Dingo/Float.Anim.TrickFlipSpeedClamped"};
StateObject flip_trick_object{{0x17, 0x54, 0x40, 0x3c, 0xa7, 0x06, 0x61, 0x3d, 0x3c, 0x40, 0x54, 0x17, 0x06, 0xa7, 0x3d, 0x61},
                              "Animation/Dingo/EnumGS.Anim.CurrentFlipTrick"};
StateObject pumping_object{{0xab, 0x5b, 0xd5, 0x21, 0x9f, 0xa4, 0xb0, 0xfb, 0x21, 0xd5, 0x5b, 0xab, 0xa4, 0x9f, 0xfb, 0xb0}, "Animation/Dingo/Bool.Intent.Pumping"};
StateObject pump_strength_object{{0x54, 0xc6, 0x59, 0x6a, 0xbb, 0xad, 0xfc, 0x05, 0x6a, 0x59, 0xc6, 0x54, 0xad, 0xbb, 0x05, 0xfc},
                                 "Animation/Dingo/Float.Intent.UserTriggeredPumpStrength"};
StateObject pump_assist_object{{0xcb, 0xdf, 0x5b, 0x53, 0x14, 0x53, 0xc8, 0x29, 0x53, 0x5b, 0xdf, 0xcb, 0x53, 0x14, 0x29, 0xc8},
                               "Animation/Dingo/Float.Anim.PumpingAssistAmount"};
// Off the board: how fast a flip in the air (jump, then RB and the left stick) turns, and how quickly it gets there.
StateObject body_flip_speed_object{{0xbc, 0x5b, 0x7b, 0x1d, 0x12, 0xd8, 0xa1, 0x2a, 0x1d, 0x7b, 0x5b, 0xbc, 0xd8, 0x12, 0x2a, 0xa1},
                                   "Animation/Dingo/Float.Intent.BodyFlipSpeedMod"};
StateObject body_flip_rate_object{{0x74, 0x34, 0x08, 0x3f, 0x5e, 0x6f, 0xb4, 0xfa, 0x3f, 0x08, 0x34, 0x74, 0x6f, 0x5e, 0xfa, 0xb4},
                                  "Animation/Dingo/Float.Intent.BodyFlipAccelerationRate"};
// Which manual the player is asking for (0: none).
StateObject manual_object{{0x77, 0x52, 0xaa, 0x61, 0x87, 0xb9, 0xfb, 0xaa, 0x61, 0xaa, 0x52, 0x77, 0xb9, 0x87, 0xaa, 0xfb}, "Animation/Dingo/Enum.Intent.Manual"};
StateObject grind_nudge_object{{0x68, 0xa1, 0x85, 0xe5, 0x51, 0x0c, 0x22, 0x2f, 0xe5, 0x85, 0xa1, 0x68, 0x0c, 0x51, 0x2f, 0x22},
                               "Animation/Dingo/Float.Intent.GrindStabilityNudge"};
StateObject grind_control_objects[]{
    {{0x7b, 0x19, 0x3e, 0x26, 0xbb, 0xd0, 0xda, 0x97, 0x26, 0x3e, 0x19, 0x7b, 0xd0, 0xbb, 0x97, 0xda}, "Animation/Dingo/Float.Intent.GrindControlLR"},
    {{0xa6, 0x4e, 0xb2, 0xa5, 0x2b, 0x0d, 0x8a, 0x6d, 0xa5, 0xb2, 0x4e, 0xa6, 0x0d, 0x2b, 0x6d, 0x8a}, "Animation/Dingo/Float.Intent.GrindControlUD"},
    {{0x63, 0x68, 0x2d, 0xaa, 0x47, 0xdf, 0x36, 0x3a, 0xaa, 0x2d, 0x68, 0x63, 0xdf, 0x47, 0x3a, 0x36}, "Animation/Dingo/Float.Intent.GrindControlHeading"},
    {{0x19, 0xa9, 0xd6, 0xa6, 0xa1, 0xfc, 0x03, 0xa1, 0xa6, 0xd6, 0xa9, 0x19, 0xfc, 0xa1, 0xa1, 0x03}, "Animation/Dingo/Float.Intent.GrindControlPitch"},
    {{0xd3, 0x1e, 0x65, 0x0c, 0xa2, 0xb6, 0xac, 0xe0, 0x0c, 0x65, 0x1e, 0xd3, 0xb6, 0xa2, 0xe0, 0xac}, "Animation/Dingo/Float.Intent.GrindHeadingControlMultiplier"},
    {{0xc9, 0xc1, 0xeb, 0xed, 0xe1, 0xd0, 0xfa, 0xd3, 0xed, 0xeb, 0xc1, 0xc9, 0xd0, 0xe1, 0xd3, 0xfa}, "Animation/Dingo/Float.Intent.GrindPitchControlMultiplier"},
    {{0xc4, 0x16, 0x23, 0xba, 0x53, 0x14, 0x47, 0x92, 0xba, 0x23, 0x16, 0xc4, 0x14, 0x53, 0x92, 0x47}, "Animation/Dingo/Float.Intent.GrindBaseHeading"},
    {{0xff, 0x4b, 0xa6, 0xad, 0xeb, 0xf7, 0x7b, 0x65, 0xad, 0xa6, 0x4b, 0xff, 0xf7, 0xeb, 0x65, 0x7b}, "Animation/Dingo/Float.Intent.GrindBasePitch"},
    {{0x85, 0xc4, 0x26, 0x2b, 0xd5, 0x4e, 0x37, 0x04, 0x2b, 0x26, 0xc4, 0x85, 0x4e, 0xd5, 0x04, 0x37}, "Animation/Dingo/Enum.GS.Anim.CurrentGrindType"},
    {{0x7f, 0xf7, 0xde, 0xa2, 0x14, 0xd2, 0x6b, 0x3b, 0xa2, 0xde, 0xf7, 0x7f, 0xd2, 0x14, 0x3b, 0x6b}, "Animation/Dingo/Enum.GS.Anim.CurrentSkateGrindType"},
    {{0xef, 0xb2, 0xb6, 0x2b, 0x9a, 0xee, 0xc0, 0x5d, 0x2b, 0xb6, 0xb2, 0xef, 0xee, 0x9a, 0x5d, 0xc0}, "Animation/Dingo/Bool.Intent.GrindControlHasActiveInput"},
};
StateObject *const objects[]{&manual_object, &flip_speed_object, &flip_trick_object, &pumping_object, &pump_strength_object, &pump_assist_object,
                             &body_flip_speed_object, &body_flip_rate_object, &flip_clamped_object, &grind_nudge_object};

StateObject grind_objects[]{
    {{0xda, 0x09, 0x5b, 0x51, 0xf4, 0xff, 0x36, 0x3e, 0x51, 0x5b, 0x09, 0xda, 0xff, 0xf4, 0x3e, 0x36}, "Animation/Dingo/Bool.Intent.NoPopGrindExit"},
    {{0xf2, 0xe4, 0x06, 0x75, 0x27, 0xf5, 0x50, 0x71, 0x75, 0x06, 0xe4, 0xf2, 0xf5, 0x27, 0x71, 0x50}, "Animation/Dingo/Bool.Intent.EnableGrindExitToGrind"},
    {{0x84, 0xbc, 0x0c, 0x36, 0x3f, 0x38, 0x7a, 0xe8, 0x36, 0x0c, 0xbc, 0x84, 0x38, 0x3f, 0xe8, 0x7a}, "Animation/Dingo/Bool.Intent.EnableGrindExitDoubleGrinds"},
    {{0xa6, 0x67, 0x3c, 0x9d, 0x49, 0x99, 0x66, 0x4c, 0x9d, 0x3c, 0x67, 0xa6, 0x99, 0x49, 0x4c, 0x66}, "Animation/Dingo/Bool.Intent.EnableStrongerGrindExits"},
    {{0xab, 0xcf, 0xb0, 0x33, 0x0f, 0xd3, 0xf3, 0x74, 0x33, 0xb0, 0xcf, 0xab, 0xd3, 0x0f, 0x74, 0xf3}, "Animation/Dingo/Bool.Intent.EnableCameraRelativeGrindExits"},
    {{0x66, 0x51, 0xab, 0xf7, 0x0d, 0xc7, 0x38, 0x76, 0xf7, 0xab, 0x51, 0x66, 0xc7, 0x0d, 0x76, 0x38}, "Animation/Dingo/Bool.Intent.EnableGrindDirectControl"},
    {{0xfe, 0x2a, 0x2c, 0xdf, 0x68, 0x36, 0x38, 0x34, 0xdf, 0x2c, 0x2a, 0xfe, 0x36, 0x68, 0x34, 0x38}, "Animation/Dingo/Bool.Intent.EnableGrindLinearDrive"},
    {{0x11, 0xe9, 0x62, 0x1b, 0x14, 0x3c, 0x1e, 0xca, 0x1b, 0x62, 0xe9, 0x11, 0x3c, 0x14, 0xca, 0x1e}, "Animation/Dingo/Bool.Intent.EnableEATGrindControl"},
    {{0x41, 0x6b, 0x36, 0x87, 0xd4, 0x0d, 0x92, 0x1b, 0x87, 0x36, 0x6b, 0x41, 0x0d, 0xd4, 0x1b, 0x92}, "Animation/Dingo/Bool.Intent.GrindToBoardslide"},
    {{0x5a, 0xf8, 0x53, 0xe2, 0x2f, 0x5b, 0x89, 0x02, 0xe2, 0x53, 0xf8, 0x5a, 0x5b, 0x2f, 0x02, 0x89}, "Animation/Dingo/Bool.Intent.DisableLegacyCopingGrinds"},
    {{0x8c, 0x0f, 0xfd, 0x43, 0x45, 0x81, 0xa8, 0xc4, 0x43, 0xfd, 0x0f, 0x8c, 0x81, 0x45, 0xc4, 0xa8}, "Animation/Dingo/Bool.Intent.AllowAirToGrind"},
};
// Transition skating: what the physics says about the lip, in the order of TransitionState (four flags, a number, an
// enumeration, three vectors).
StateObject transition_objects[]{
    {{0x7d, 0xbc, 0xc5, 0x80, 0xd7, 0xd9, 0xb2, 0xed, 0x80, 0xc5, 0xbc, 0x7d, 0xd9, 0xd7, 0xed, 0xb2}, "Animation/Dingo/Bool.Physics.FoundLipTrick"},
    {{0xf7, 0x40, 0x6e, 0xeb, 0x30, 0x84, 0x8a, 0x2f, 0xeb, 0x6e, 0x40, 0xf7, 0x84, 0x30, 0x2f, 0x8a}, "Animation/Dingo/Bool.Physics.IsLipTricking"},
    {{0xf4, 0xe2, 0x75, 0xd3, 0x3a, 0xb6, 0x36, 0x20, 0xd3, 0x75, 0xe2, 0xf4, 0xb6, 0x3a, 0x20, 0x36}, "Animation/Dingo/Bool.Physics.IsHandplanting"},
    {{0xa0, 0x93, 0xef, 0x64, 0xcd, 0xf1, 0xd7, 0xfc, 0x64, 0xef, 0x93, 0xa0, 0xf1, 0xcd, 0xfc, 0xd7}, "Animation/Dingo/Bool.Physics.IsTrajectoryTargetingVert"},
    {{0x4d, 0x51, 0x11, 0xcd, 0x2f, 0xad, 0x80, 0x9c, 0xcd, 0x11, 0x51, 0x4d, 0xad, 0x2f, 0x9c, 0x80}, "Animation/Dingo/Float.Intent.TransitionInput"},
    {{0x00, 0xdf, 0x0d, 0x4f, 0xfa, 0x34, 0x89, 0x81, 0x4f, 0x0d, 0xdf, 0x00, 0x34, 0xfa, 0x81, 0x89}, "Animation/Dingo/EnumGS.Physics.LipDropInType"},
    {{0x91, 0x7e, 0x62, 0x00, 0xa7, 0xe0, 0x4d, 0x9c, 0x00, 0x62, 0x7e, 0x91, 0xe0, 0xa7, 0x9c, 0x4d}, "Animation/Dingo/Vec3.Physics.LipDirection"},
    {{0x09, 0xda, 0x55, 0x9c, 0xe0, 0x97, 0x4a, 0xa8, 0x9c, 0x55, 0xda, 0x09, 0x97, 0xe0, 0xa8, 0x4a}, "Animation/Dingo/Vec3.Physics.LipNearestPoint"},
    {{0x27, 0x56, 0x8a, 0xee, 0x40, 0x00, 0x61, 0xce, 0xee, 0x8a, 0x56, 0x27, 0x00, 0x40, 0xce, 0x61}, "Animation/Dingo/Vec3.Prediction.ReckoningGroundNormal"},
};
bool put_flag(std::uintptr_t address, bool value) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint8_t *>(address) = value ? 1 : 0;
        return true;
    } __except (1) { return false; }
}

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
    -1, 16, 0, 1, 2, 6, 11, 3, 7, 10,
    4, 8, 13, 5, 9, 12, 16, 0, 1,
    2, 10, 7, 3, 6, 11, 5,
    12, 9, 4, 8, 13, 16, 16, -1, -1,
    -1, 14, 15, 17, 18, 19, -1, 16,
    -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1};
}

void find_state_objects(std::uintptr_t start, std::size_t size) noexcept {
    __try {
        const auto *words = reinterpret_cast<const std::uint32_t *>(start);
        const auto look = [&](StateObject &object, std::size_t i) {
            std::uint32_t first;
            std::memcpy(&first, object.id, 4);
            if (words[i] != first) return;
            // An object can be in memory more than once, and only one copy has its handle filled in (seen on the grind
            // switches, 2026-10-11): a copy with a handle replaces one without.
            const auto known = object.at.load(std::memory_order_relaxed), here = start + i * 4 + object_after_id;
            std::uint32_t old_handle{}, handle{};
            if (known == here || !is_object(object, start + i * 4)) return;
            if (known && (!peek(known + object_handle, old_handle) || old_handle >> 24)) return;
            if (known && (!peek(here + object_handle, handle) || !(handle >> 24))) return;
            object.at.store(here, std::memory_order_relaxed);
        };
        for (std::size_t i = 0; i + 0x20 < size / 4; ++i) {
            for (auto *object : objects) look(*object, i);
            for (auto &object : grind_objects) look(object, i);
            for (auto &object : grind_control_objects) look(object, i);
            for (auto &object : transition_objects) look(object, i);
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
    state.trick_at = trick_at;
    if (const auto at = value_address(flip_clamped_object, block); at && peek(at, state.clamped) && std::isfinite(state.clamped)) state.clamped_at = at;
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
BodyFlipState read_body_flip_state(std::uintptr_t base, std::uintptr_t entity) noexcept {
    BodyFlipState state;
    if (!body_flip_speed_object.at.load(std::memory_order_relaxed) && !body_flip_rate_object.at.load(std::memory_order_relaxed)) return state;
    const auto block = states_block(base, entity);
    if (!block) return state;
    float number{};
    if (const auto at = value_address(body_flip_speed_object, block); at && peek(at, number) && std::isfinite(number)) {
        state.speed_at = at;
        state.speed = number;
    }
    if (const auto at = value_address(body_flip_rate_object, block); at && peek(at, number) && std::isfinite(number)) {
        state.rate_at = at;
        state.rate = number;
    }
    return state;
}
GrindControl read_grind_control(std::uintptr_t base, std::uintptr_t entity) noexcept {
    GrindControl control;
    const auto block = states_block(base, entity);
    if (!block) return control;
    float *const numbers[]{&control.lr, &control.ud, &control.heading, &control.pitch, &control.heading_multiplier, &control.pitch_multiplier,
                           &control.base_heading, &control.base_pitch};
    std::uint32_t *const words[]{&control.grind_type, &control.skate_grind_type, &control.active_input};
    for (std::size_t i = 0; i < std::size(grind_control_objects); ++i) {
        const auto at = value_address(grind_control_objects[i], block);
        if (!at) continue;
        if (i < std::size(numbers)) {
            float value{};
            if (!peek(at, value) || !std::isfinite(value)) continue;
            *numbers[i] = value;
        } else if (i + 1 < std::size(grind_control_objects)) {
            std::uint32_t value{};
            if (!peek(at, value)) continue;
            *words[i - std::size(numbers)] = value;
        } else {
            std::uint8_t value{};
            if (!peek(at, value)) continue;
            control.active_input = value;
        }
        control.found |= 1u << i;
    }
    return control;
}
bool read_grind_nudge(std::uintptr_t base, std::uintptr_t entity, float &nudge) noexcept {
    if (!grind_nudge_object.at.load(std::memory_order_relaxed)) return false;
    const auto block = states_block(base, entity);
    const auto at = block ? value_address(grind_nudge_object, block) : 0;
    float value{};
    if (!at || !peek(at, value) || !std::isfinite(value)) return false;
    nudge = value;
    return true;
}
std::int32_t read_manual_intent(std::uintptr_t base, std::uintptr_t entity) noexcept {
    if (!manual_object.at.load(std::memory_order_relaxed)) return -1;
    const auto block = states_block(base, entity);
    std::uint32_t value{};
    if (!block) return -1;
    const auto at = value_address(manual_object, block);
    return at && peek(at, value) && value < 64 ? static_cast<std::int32_t>(value) : -1;
}
struct ForcedSetting {
    std::atomic<int> mode{-1};
    std::atomic<std::uint32_t> asked{};
};
ForcedSetting forced_settings[grind_settings.size() + transition_settings.size()]; // the grind settings, then the transition ones
// Which of forced_settings the setting asset at `asset` is (its key is the string at + 0x60); -1: none of them.
int grind_setting_of(std::uintptr_t asset) noexcept {
    std::uintptr_t name{};
    char text[48]{};
    if (!peek(asset + 0x60, name) || !pointer(name & ~std::uintptr_t{3}) || !peek(name, text)) {
        // A short string at the end of a page: byte by byte.
        if (!name) return -1;
        std::memset(text, 0, sizeof(text));
        for (std::size_t i = 0; i + 1 < sizeof(text); ++i)
            if (!peek(name + i, text[i]) || !text[i]) break;
    }
    text[sizeof(text) - 1] = 0;
    for (std::size_t i = 0; i < grind_settings.size(); ++i)
        if (_stricmp(text, std::string(grind_settings[i].setting).c_str()) == 0) return static_cast<int>(i);
    for (std::size_t i = 0; i < transition_settings.size(); ++i)
        if (_stricmp(text, std::string(transition_settings[i].setting).c_str()) == 0) return static_cast<int>(grind_settings.size() + i);
    return -1;
}

// The game's lookup of a state's value: (state object, context with the pages) -> where the value is, 0 when the
// context has none. All of it is 0x79 bytes; these are its first.
constexpr std::uintptr_t state_lookup = 0x11dcdd0;
constexpr unsigned char state_lookup_bytes[]{0x48, 0x83, 0xec, 0x28, 0x80, 0x79, 0x2b, 0x00, 0x4c, 0x8b, 0xd2, 0x4c, 0x8b, 0xc9};
using StateLookup = std::uintptr_t (*)(std::uintptr_t object, std::uintptr_t context);
struct HeldSwitch {
    std::atomic<std::uintptr_t> object{};
    std::atomic<int> mode{-1};
    std::atomic<std::uint32_t> asked{}, answered{};
    volatile std::uint8_t value[16]{};
};
HeldSwitch held_switches[std::size(grind_objects)];
struct HeldNumber {
    std::atomic<std::uintptr_t> object{};
    std::atomic<float> value{};
    std::atomic<bool> on{};
    std::atomic<std::uint32_t> asked{};
    volatile float answer[4]{};
};
HeldNumber held_numbers[3]; // the grind heading and pitch control multipliers, and the heading itself
std::atomic<StateLookup> lookup_original{};
std::atomic<bool> lookup_attempted{}, lookup_ready{};
std::uintptr_t state_lookup_hook(std::uintptr_t object, std::uintptr_t context) {
    for (auto &held : held_switches) {
        if (held.object.load(std::memory_order_relaxed) != object) continue;
        held.asked.fetch_add(1, std::memory_order_relaxed);
        const int mode = held.mode.load(std::memory_order_relaxed);
        if (mode < 0) break;
        held.answered.fetch_add(1, std::memory_order_relaxed);
        // The game may write through the answer too (its scripts set these): the value is put back for every asker.
        held.value[0] = mode ? 1 : 0;
        return reinterpret_cast<std::uintptr_t>(&held.value[0]);
    }
    for (auto &held : held_numbers) {
        if (!object || held.object.load(std::memory_order_relaxed) != object) continue;
        held.asked.fetch_add(1, std::memory_order_relaxed);
        if (!held.on.load(std::memory_order_relaxed)) break;
        held.answer[0] = held.value.load(std::memory_order_relaxed); // put back for every asker: the scripts write through the answer too
        return reinterpret_cast<std::uintptr_t>(&held.answer[0]);
    }
    const auto original = lookup_original.load(std::memory_order_acquire);
    return original ? original(object, context) : 0;
}
// The game's other lookup, (context with the pages, state object) -> where the value is: most scripts and the grind code
// read and write through this one (91 callers against 6). Only the held numbers are answered here.
constexpr std::uintptr_t state_lookup_pages = 0x11dcd40;
constexpr unsigned char state_lookup_pages_bytes[]{0x48, 0x89, 0x5c, 0x24, 0x08, 0x80, 0x7a, 0x2b, 0x00, 0x76, 0x66, 0x44, 0x8b, 0x5a, 0x28};
using StateLookupPages = std::uintptr_t (*)(std::uintptr_t context, std::uintptr_t object);
std::atomic<StateLookupPages> lookup_pages_original{};
std::atomic<bool> numbers_held{};
std::uintptr_t state_lookup_pages_hook(std::uintptr_t context, std::uintptr_t object) {
    if (numbers_held.load(std::memory_order_relaxed) && object)
        for (auto &held : held_numbers) {
            if (held.object.load(std::memory_order_relaxed) != object) continue;
            held.asked.fetch_add(1, std::memory_order_relaxed);
            if (!held.on.load(std::memory_order_relaxed)) break;
            held.answer[0] = held.value.load(std::memory_order_relaxed);
            return reinterpret_cast<std::uintptr_t>(&held.answer[0]);
        }
    return lookup_pages_original.load(std::memory_order_relaxed)(context, object);
}
bool lookup_code_known(std::uintptr_t base) noexcept {
    __try {
        return std::memcmp(reinterpret_cast<const void *>(base + state_lookup), state_lookup_bytes, sizeof(state_lookup_bytes)) == 0;
    } __except (1) { return false; }
}
bool lookup_pages_code_known(std::uintptr_t base) noexcept {
    __try {
        return std::memcmp(reinterpret_cast<const void *>(base + state_lookup_pages), state_lookup_pages_bytes, sizeof(state_lookup_pages_bytes)) == 0;
    } __except (1) { return false; }
}
bool start_state_holds(std::uintptr_t base) noexcept {
    if (!base || lookup_attempted.exchange(true)) return lookup_ready.load(std::memory_order_relaxed);
    if (!lookup_code_known(base)) return false;
    auto *target = reinterpret_cast<void *>(base + state_lookup);
    void *original{};
    if (hook_prepare(target, reinterpret_cast<void *>(&state_lookup_hook), &original) != HookOk || !original) return false;
    lookup_original.store(reinterpret_cast<StateLookup>(original), std::memory_order_release);
    if (hook_enable(target) != HookOk) return false;
    // The second lookup: left as it is when its first bytes are not the known ones.
    const bool known = lookup_pages_code_known(base);
    auto *pages_target = reinterpret_cast<void *>(base + state_lookup_pages);
    void *pages_original{};
    if (known && hook_prepare(pages_target, reinterpret_cast<void *>(&state_lookup_pages_hook), &pages_original) == HookOk && pages_original) {
        lookup_pages_original.store(reinterpret_cast<StateLookupPages>(pages_original), std::memory_order_release);
        (void)hook_enable(pages_target);
    }
    lookup_ready.store(true, std::memory_order_relaxed);
    return true;
}
void force_grind_setting(std::size_t index, int mode) noexcept {
    if (index < grind_settings.size()) forced_settings[index].mode.store(mode, std::memory_order_relaxed);
}
std::uint32_t grind_setting_asked(std::size_t index) noexcept {
    return index < grind_settings.size() ? forced_settings[index].asked.exchange(0, std::memory_order_relaxed) : 0;
}
void force_transition_setting(std::size_t index, int mode) noexcept {
    if (index < transition_settings.size()) forced_settings[grind_settings.size() + index].mode.store(mode, std::memory_order_relaxed);
}
std::uint32_t transition_setting_asked(std::size_t index) noexcept {
    return index < transition_settings.size() ? forced_settings[grind_settings.size() + index].asked.exchange(0, std::memory_order_relaxed) : 0;
}
TransitionState read_transition_state(std::uintptr_t base, std::uintptr_t entity) noexcept {
    TransitionState state;
    const auto block = states_block(base, entity);
    if (!block) return state;
    bool *const flags[]{&state.lip_found, &state.lip_tricking, &state.handplanting, &state.targeting_vert};
    std::array<float, 3> *const vectors[]{&state.lip_direction, &state.lip_nearest, &state.ground_normal};
    for (std::size_t i = 0; i < std::size(transition_objects); ++i) {
        const auto at = value_address(transition_objects[i], block);
        if (!at) continue;
        if (i < 4) {
            std::uint8_t value{};
            if (!peek(at, value)) continue;
            *flags[i] = value != 0;
        } else if (i == 4) {
            float value{};
            if (!peek(at, value) || !std::isfinite(value)) continue;
            state.transition_input = value;
        } else if (i == 5) {
            if (!peek(at, state.drop_in_type)) continue;
        } else {
            std::array<float, 3> value{};
            if (!peek(at, value) || !std::isfinite(value[0]) || !std::isfinite(value[1]) || !std::isfinite(value[2])) continue;
            *vectors[i - 6] = value;
        }
        state.found |= 1u << i;
    }
    return state;
}
std::optional<bool> forced_gameplay_bool(const void *reference) noexcept {
    std::uintptr_t asset{};
    if (!reference || !peek(reinterpret_cast<std::uintptr_t>(reference), asset)) return {};
    asset &= ~std::uintptr_t{4};
    if (!asset) return {};
    // Which setting an asset is, remembered per thread for a few seconds (the game asks several times a frame; an
    // address can be another asset after a level change).
    struct Entry { std::uintptr_t asset{}; int setting{-1}; std::uint64_t until{}; };
    thread_local Entry cache[64];
    auto &entry = cache[((asset >> 4) ^ (asset >> 12)) & 63];
    const auto now = GetTickCount64();
    if (entry.asset != asset || now >= entry.until) entry = {asset, grind_setting_of(asset), now + 5000};
    if (entry.setting < 0) return {};
    auto &forced = forced_settings[static_cast<std::size_t>(entry.setting)];
    forced.asked.fetch_add(1, std::memory_order_relaxed);
    const int mode = forced.mode.load(std::memory_order_relaxed);
    if (mode < 0) return {};
    return mode != 0;
}
void grind_switch_asked(std::size_t index, std::uint32_t &asked, std::uint32_t &answered) noexcept {
    asked = answered = 0;
    if (index >= std::size(held_switches)) return;
    asked = held_switches[index].asked.exchange(0, std::memory_order_relaxed);
    answered = held_switches[index].answered.exchange(0, std::memory_order_relaxed);
}
int grind_switch_status(std::uintptr_t base, std::uintptr_t entity, std::size_t index, std::uint32_t &handle) noexcept {
    handle = 0;
    if (index >= std::size(grind_objects)) return 0;
    const auto at = grind_objects[index].at.load(std::memory_order_relaxed);
    if (!at) return 0;
    if (!peek(at + object_handle, handle) || !(handle >> 24)) return 1;
    const auto block = states_block(base, entity);
    return block && value_address(grind_objects[index], block) ? 3 : 2;
}
void hold_grind_switches(std::uintptr_t base, std::uintptr_t entity, const std::int8_t *modes, std::int8_t *live) noexcept {
    static_assert(std::size(grind_objects) == grind_switches.size());
    const auto block = states_block(base, entity);
    for (std::size_t i = 0; i < std::size(grind_objects); ++i) {
        live[i] = -1;
        std::uint8_t flag{};
        // For the lookup hook: which object, and what it is held at.
        held_switches[i].object.store(grind_objects[i].at.load(std::memory_order_relaxed), std::memory_order_relaxed);
        held_switches[i].mode.store(modes[i], std::memory_order_relaxed);
        const auto at = block ? value_address(grind_objects[i], block) : 0;
        if (!at || !peek(at, flag)) continue;
        live[i] = flag != 0;
        if (modes[i] >= 0 && (flag != 0) != (modes[i] != 0)) (void)put_flag(at, modes[i] != 0);
    }
}
void hold_grind_multipliers(std::uintptr_t base, std::uintptr_t entity, float turn, float tip, bool on_grind, std::uint32_t *asked) noexcept {
    const float wanted[]{turn, tip};
    const auto block = on_grind ? states_block(base, entity) : 0;
    for (std::size_t i = 0; i < 2; ++i) {
        auto &object = grind_control_objects[4 + i]; // the two multipliers, in the table's order
        held_numbers[i].object.store(object.at.load(std::memory_order_relaxed), std::memory_order_relaxed);
        const bool on = std::isfinite(wanted[i]) && wanted[i] > 0.0f;
        held_numbers[i].value.store(on ? wanted[i] : 0.0f, std::memory_order_relaxed);
        held_numbers[i].on.store(on, std::memory_order_relaxed);
        if (asked) asked[i] = held_numbers[i].asked.exchange(0, std::memory_order_relaxed);
        // The value itself as well, for a reader that kept its address; the scripts set it again when the next grind begins.
        if (block && wanted[i] > 0.0f)
            if (const auto at = value_address(object, block)) (void)put(at, wanted[i]);
    }
}
std::uint32_t hold_grind_heading(std::uintptr_t base, std::uintptr_t entity, bool on, float heading) noexcept {
    auto &held = held_numbers[2];
    auto &object = grind_control_objects[2]; // Float.Intent.GrindControlHeading
    held.object.store(object.at.load(std::memory_order_relaxed), std::memory_order_relaxed);
    held.value.store(std::isfinite(heading) ? heading : 0.0f, std::memory_order_relaxed);
    held.on.store(on, std::memory_order_relaxed);
    bool any = false;
    for (const auto &one : held_numbers) any = any || one.on.load(std::memory_order_relaxed);
    numbers_held.store(any, std::memory_order_relaxed);
    if (on)
        if (const auto block = states_block(base, entity))
            if (const auto at = value_address(object, block)) (void)put(at, heading);
    return held.asked.exchange(0, std::memory_order_relaxed);
}
bool write_state_number(std::uintptr_t at, float value) noexcept { return at && std::isfinite(value) && put(at, value); }
std::array<std::uintptr_t, 6> state_pages(std::uintptr_t base, std::uintptr_t entity) noexcept {
    std::array<std::uintptr_t, 6> pages{};
    if (const auto block = states_block(base, entity))
        for (std::size_t k = 0; k < pages.size(); ++k)
            if (!peek(block + block_pages + k * 8, pages[k]) || !pointer(pages[k])) pages[k] = 0;
    return pages;
}
bool write_flip_speed(const FlipState &state, float speed) noexcept {
    if (!state || !std::isfinite(speed)) return false;
    if (state.clamped_at) (void)put(state.clamped_at, speed);
    return put(state.speed_at, speed);
}
int flip_trick_index(std::uint32_t trick) noexcept { return trick < trick_count ? trick_slider[trick] : -1; }
std::string_view flip_trick_name(std::uint32_t trick) noexcept { return trick < trick_count ? game_trick_names[trick] : std::string_view{}; }
}
