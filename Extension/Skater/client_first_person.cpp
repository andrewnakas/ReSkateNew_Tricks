#include "client_source_spawn.h"
#include "client_source_spawn_internal.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "free_flight.h"
#include "Extension/HallOfMeat/hall_of_meat_skeleton.h"
#include <algorithm>
#include <vector>
#include <cmath>

namespace dingosdk::client_source::detail {
bool first_person_write_fov(std::uintptr_t camera, float fov) noexcept {
    if (!std::isfinite(fov) || fov <= 1.0f || fov >= 175.0f || camera < 0x10000 || camera > source_highest - 0x100) return false;
    __try { *reinterpret_cast<volatile float*>(camera + camera_fov_offset) = fov; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Puts back the FreeCamera's own FOV so a later Freecam session is unaffected.
void first_person_restore_fov(InteractiveDebug& debug) noexcept {
    if (debug.first_person_saved_fov > 0 && debug.camera_identity.camera)
        (void)first_person_write_fov(debug.camera_identity.camera, debug.first_person_saved_fov);
    debug.first_person_saved_fov = 0;
}
// Puts back the FOV the camera had before Freecam's own FOV was applied.
void free_camera_restore_fov(InteractiveDebug& debug) noexcept {
    if (debug.free_camera_saved_fov > 0 && debug.camera_owned && debug.camera_identity.camera) {
        if (debug.first_person) {
            // First person put its own value on top; hand it the original instead.
            if (debug.first_person_saved_fov > 0) debug.first_person_saved_fov = debug.free_camera_saved_fov;
        } else {
            (void)first_person_write_fov(debug.camera_identity.camera, debug.free_camera_saved_fov);
        }
    }
    debug.free_camera_saved_fov = 0;
}
FirstPersonArm& first_person_arm() {
    static auto* arm = new FirstPersonArm;
    return *arm;
}
void first_person_disarm() noexcept {
    auto& arm = first_person_arm();
    arm.thread.store(0, std::memory_order_release);
    arm.watched.store(0, std::memory_order_release);
    multiplayer::set_local_hidden_joint(0, 0, {1, 1, 1});
    arm.spring.reset();
    std::lock_guard lock(arm.snapshot_mutex);
    arm.captures = 0;
}
namespace {
// First person. The animation output pose is parent-local {scale, Hamilton
// quaternion, translation} per joint, and joint 1 (AITrajectory) carries the
// character's world placement, so composing from the root joint lands directly
// in world space. Indices are the 395-joint Animation/Dingo/AnimBase_Default_
// Skeleton, which drives every body type (the 137-joint per-body skeletons are
// render rigs). Its Head joint has local +X up, +Y out of the face and -Z toward
// the skater's right; both measured on a live skater on 2026-09-21.
constexpr std::uint32_t first_person_skeleton_joints = 395;
constexpr std::array<std::uint16_t, 10> first_person_head_chain{0, 1, 7, 42, 43, 44, 45, 101, 102, 103};
constexpr std::uint16_t first_person_face = 107, first_person_right_eye = 108, first_person_left_eye = 109;
// Metres ahead of the eyes along the face, so the skater's own face mesh sits
// behind the camera rather than across the near plane.
constexpr float first_person_forward_offset = 0.05f;
}
bool first_person_read(std::uintptr_t address, void* out, std::size_t size) noexcept {
    if (address < 0x10000 || size > 0x10000 || address > source_highest - size) return false;
    __try { std::memcpy(out, reinterpret_cast<const void*>(address), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
namespace {
bool first_person_write(std::uintptr_t address, const void* in, std::size_t size) noexcept {
    if (address < 0x10000 || size > 0x10000 || address > source_highest - size) return false;
    __try { std::memcpy(reinterpret_cast<void*>(address), in, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Shrinks the head joint of the pose just evaluated, on the thread that
// evaluated it, so the skater's head and everything parented to it (face,
// eyes, hair, headwear) collapse to a point instead of filling the view. The
// next update rewrites the whole pose, so nothing needs restoring. Poses sent
// to other players keep the real scale (set_local_hidden_joint).
constexpr std::uint16_t first_person_head_joint = 103;
// The head joint's real scale, packed as three floats; read back when the
// camera composes a pose whose head is already shrunk.
std::atomic<std::uint64_t> first_person_head_scale_xy{}, first_person_head_scale_z{};
void first_person_hide_head(std::uintptr_t base, std::uintptr_t component) noexcept {
    std::uintptr_t holder{};
    if (!first_person_read(component + 0xa0, &holder, 8) || !holder) return;
    const auto pose = multiplayer::read_native_pose_layout(first_person_read, base, holder, 512);
    if (!pose.buffer || pose.count != first_person_skeleton_joints) return;
    const auto scale_address = pose.buffer + first_person_head_joint * 0x30ULL;
    std::array<float, 3> scale{};
    if (!first_person_read(scale_address, scale.data(), sizeof(scale))) return;
    for (const auto value : scale) if (!std::isfinite(value) || value < 0.05f || value > 20.0f) return; // Already hidden.
    multiplayer::set_local_hidden_joint(component, first_person_head_joint, scale);
    std::uint64_t xy{}; std::memcpy(&xy, scale.data(), 8);
    std::uint32_t z{}; std::memcpy(&z, &scale[2], 4);
    first_person_head_scale_xy.store(xy, std::memory_order_relaxed);
    first_person_head_scale_z.store(z, std::memory_order_release);
    constexpr std::array<float, 3> hidden{0.001f, 0.001f, 0.001f};
    (void)first_person_write(scale_address, hidden.data(), sizeof(hidden));
}
struct FirstPersonJoint {
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> position{};
    float scale = 1;
};
std::array<float, 3> first_person_rotate(const std::array<float, 4>& q, const std::array<float, 3>& v) {
    const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]),
        tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    return {v[0] + q[3] * tx + (q[1] * tz - q[2] * ty), v[1] + q[3] * ty + (q[2] * tx - q[0] * tz),
        v[2] + q[3] * tz + (q[0] * ty - q[1] * tx)};
}
// Appends one parent-local joint to an accumulated world transform.
FirstPersonJoint first_person_child(const FirstPersonJoint& parent, std::uintptr_t buffer, std::uint16_t index) {
    std::array<float, 12> bone{}; // scale.xyzw | rotation.xyzw | position.xyzw; .w of scale/position is metadata
    source_require(first_person_read(buffer + index * 0x30ULL, bone.data(), sizeof(bone)), "Head pose is unreadable.");
    // First person shrinks the head it draws; compose with the real scale.
    if (index == first_person_head_joint && bone[0] < 0.05f && bone[1] < 0.05f && bone[2] < 0.05f) {
        const auto z = first_person_head_scale_z.load(std::memory_order_acquire);
        const auto xy = first_person_head_scale_xy.load(std::memory_order_relaxed);
        if (z) { std::memcpy(bone.data(), &xy, 8); std::memcpy(&bone[2], &z, 4); }
    }
    for (const std::size_t i : {0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 10u})
        source_require(std::isfinite(bone[i]) && std::abs(bone[i]) < 1000000.0f, "Head pose is not ready.");
    const auto& q = parent.rotation;
    const auto offset = first_person_rotate(q, {bone[8] * parent.scale, bone[9] * parent.scale, bone[10] * parent.scale});
    FirstPersonJoint child;
    for (std::size_t i = 0; i < 3; ++i) child.position[i] = parent.position[i] + offset[i];
    child.rotation = {q[3] * bone[4] + q[0] * bone[7] + q[1] * bone[6] - q[2] * bone[5],
        q[3] * bone[5] - q[0] * bone[6] + q[1] * bone[7] + q[2] * bone[4],
        q[3] * bone[6] + q[0] * bone[5] - q[1] * bone[4] + q[2] * bone[7],
        q[3] * bone[7] - q[0] * bone[4] - q[1] * bone[5] - q[2] * bone[6]};
    child.scale = parent.scale * (bone[0] + bone[1] + bone[2]) / 3.0f;
    return child;
}
}
// The local skater's animation component, from the verified player-bound skater.
std::uintptr_t first_person_component(std::uintptr_t base, std::uintptr_t client) {
    overlay::DebugModel skater;
    (void)debug_skater(base, client, skater);
    std::uintptr_t component{}, component_type{};
    source_require(first_person_read(skater.skater_identity + 0x628, &component, 8) && component &&
        first_person_read(component, &component_type, 8) && component_type == base + addr::engine::skater_component_vtable,
        "Skater animation is unavailable.");
    return component;
}
// The physics state selector's current state: skater component +0x70 is the
// physics core, core +0x3c0 its context, context +0x1414 the state (no_bail.cpp).
std::optional<bool> first_person_on_foot(std::uintptr_t base, std::uintptr_t client) noexcept {
    try {
        const auto component = first_person_component(base, client);
        std::uintptr_t core{}, core_type{}, context{};
        std::uint32_t state{};
        if (!first_person_read(component + 0x70, &core, 8) || !first_person_read(core, &core_type, 8) ||
            core_type != base + addr::no_bail::bail_core_vtable ||
            !first_person_read(core + 0x3c0, &context, 8) || !first_person_read(context + 0x1414, &state, 4))
            return std::nullopt;
        return state == addr::no_bail::offboard_physics_state;
    } catch (...) { return std::nullopt; }
}
// Writes the head camera into the rows (right, up, backward, position) of a
// native camera matrix, leaving each row's fourth lane as the camera had it.
first_person::Vec3 first_person_head_matrix(std::uintptr_t base, std::uintptr_t component, std::array<float, 16>& matrix) {
    std::uintptr_t holder{};
    source_require(first_person_read(component + 0xa0, &holder, 8) && holder, "Skater animation is unavailable.");
    const auto pose = multiplayer::read_native_pose_layout(first_person_read, base, holder, 512);
    source_require(pose.buffer && pose.count == first_person_skeleton_joints,
        "First person needs the standard skater skeleton.");
    FirstPersonJoint head;
    first_person::Vec3 origin{};
    for (const auto joint : first_person_head_chain) {
        head = first_person_child(head, pose.buffer, joint);
        if (joint == 1) origin = head.position;
    }
    const auto face = first_person_child(head, pose.buffer, first_person_face);
    const auto right_eye = first_person_child(face, pose.buffer, first_person_right_eye).position;
    const auto left_eye = first_person_child(face, pose.buffer, first_person_left_eye).position;
    float separation = 0;
    for (std::size_t i = 0; i < 3; ++i) separation += (right_eye[i] - left_eye[i]) * (right_eye[i] - left_eye[i]);
    separation = std::sqrt(separation) / std::max(head.scale, 0.01f);
    source_require(separation > 0.03f && separation < 0.15f, "Head pose failed its shape check.");
    const auto right = first_person_rotate(head.rotation, {0, 0, -1});
    const auto up = first_person_rotate(head.rotation, {1, 0, 0});
    const auto forward = first_person_rotate(head.rotation, {0, 1, 0});
    for (std::size_t i = 0; i < 3; ++i) {
        matrix[i] = right[i];
        matrix[4 + i] = up[i];
        matrix[8 + i] = -forward[i];
        matrix[12 + i] = (right_eye[i] + left_eye[i]) * 0.5f + forward[i] * first_person_forward_offset * head.scale;
    }
    source_require(valid_flight_transform(matrix), "Head camera pose is invalid.");
    return origin;
}
// Every animation update of the local skater, on whichever thread runs it
// (usually a worker), captures its head pose; reads only, never the camera.
// Each frame runs: client tick (camera) -> the local skater's animation, often
// on a worker thread -> that pose handed to the renderer, on the engine thread
// (measured 2026-09-22). The handoff is the one point that is after this
// frame's pose and still on the camera's thread, so the head is published
// there, from the capture its animation update just took: the camera then
// matches the body being drawn on every frame, with no lag.
void first_person_on_render(std::uintptr_t animation_interface) noexcept {
    auto& arm = first_person_arm();
    const auto component = arm.watched.load(std::memory_order_acquire);
    if (!component) return;
    std::uintptr_t holder{};
    if (!first_person_read(component + 0xa0, &holder, 8) || !holder || animation_interface != holder + 0xc0) return;
    const auto thread = arm.thread.load(std::memory_order_acquire);
    if (!thread || GetCurrentThreadId() != thread) return; // The camera is only written on the engine thread.
    const auto error = GetLastError();
    try {
        FirstPersonArm::Snapshot latest;
        bool have{};
        {
            std::lock_guard lock(arm.snapshot_mutex);
            if (arm.captures) { latest = arm.latest; have = true; }
        }
        // Only this frame's capture (taken moments ago); otherwise keep the tick's.
        if (have && source_flight_now() - latest.time < 0.02) {
            auto matrix = arm.matrix;
            for (const std::size_t i : {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u, 12u, 13u, 14u}) matrix[i] = latest.head[i];
            matrix = arm.spring.sample(matrix, arm.settings, latest.origin);
            source_require(valid_flight_transform(matrix), "First-person spring pose is invalid.");
            arm.transform(arm.camera, &matrix);
            arm.matrix = matrix;
            if (arm.fov > 0) (void)first_person_write_fov(arm.camera, arm.fov);
        }
    } catch (...) {}
    SetLastError(error);
}
void first_person_on_animation(std::uintptr_t component) noexcept {
    auto& arm = first_person_arm();
    if (!component || component != arm.watched.load(std::memory_order_acquire)) return;
    const auto error = GetLastError();
    const auto now = source_flight_now();
    try {
        std::array<float, 16> head{};
        head[15] = 1;
        const auto origin = first_person_head_matrix(arm.watched_base.load(std::memory_order_acquire), component, head);
        {
            std::lock_guard lock(arm.snapshot_mutex);
            arm.latest = {head, origin, now};
            ++arm.captures;
        }
        first_person_hide_head(arm.watched_base.load(std::memory_order_acquire), component);
    } catch (...) {} // The tick keeps publishing and reports why.
    SetLastError(error);
}
}
// ReSkate Tricks: the fandango's back leg (the owner, 2026-10-16: "you crooked grind with your back foot off the board").
// The physics drives to the deck do not move the drawn legs (the pose comes from the clips: run fd1, the foot stayed
// 0.05 m over the deck), so the back leg of the pose just evaluated is bent with a two-bone IK: the foot taken from
// where the clip has it, up off the deck and back past the tail. The back leg is the one whose foot is nearer the back
// truck. Render skeleton joints (animbase_default_renderskeleton): Hips 7; right UpLeg 8, Leg 9, Foot 10; left 341,
// 342, 343; Skateboard_Root 376, Deck 377, Truck_Back 378, Truck_Front 382 (all under joint 1).
namespace dingosdk::client_source::detail {
namespace {
struct LegLift {
    std::atomic<std::uintptr_t> core{}, base{};
    std::atomic<ULONGLONG> until{};
    std::atomic<float> up{0.15f}, back{0.3f}, side{};
    std::atomic<int> leg{-1}; // -1 the foot nearer the back truck, 0 right, 1 left
    std::atomic<std::uintptr_t> component{}; // the skater component whose physics core is `core`, from its animation update
    std::atomic<std::uint32_t> edits{};
    std::atomic<int> why{}, used_leg{-1};
    std::atomic<float> foot_before{}, foot_after{}, moved{}; // the foot's height over the deck (m), and how far it was taken
    // Animation thread only.
    float weight{};
    ULONGLONG last{};
};
LegLift& leg_lift() { static auto* value = new LegLift; return *value; }
struct LV { float x{}, y{}, z{}; };
struct LQ { float x{}, y{}, z{}, w{1}; };
struct LX { LQ q; LV p; };
LV operator+(LV a, LV b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
LV operator-(LV a, LV b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
LV operator*(LV a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(LV a, LV b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
LV cross(LV a, LV b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(LV a) { return std::sqrt(dot(a, a)); }
LV unit(LV a) { const float l = length(a); return l > 1e-6f ? a * (1.0f / l) : LV{}; }
LQ mul(LQ a, LQ b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
LQ conj(LQ q) { return {-q.x, -q.y, -q.z, q.w}; }
LQ unit(LQ q) {
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-6f ? LQ{q.x / l, q.y / l, q.z / l, q.w / l} : LQ{};
}
LV rotate(LQ q, LV v) {
    const auto r = first_person_rotate({q.x, q.y, q.z, q.w}, {v.x, v.y, v.z});
    return {r[0], r[1], r[2]};
}
LX compose(const LX& parent, const LX& local) { return {unit(mul(parent.q, local.q)), parent.p + rotate(parent.q, local.p)}; }
// The shortest turn taking direction a on to b.
LQ turn_between(LV a, LV b) {
    a = unit(a);
    b = unit(b);
    const float d = dot(a, b);
    if (d < -0.9999f) {
        LV axis = unit(cross(a, {1, 0, 0}));
        if (length(axis) < 0.5f) axis = unit(cross(a, {0, 1, 0}));
        return {axis.x, axis.y, axis.z, 0};
    }
    const LV c = cross(a, b);
    return unit(LQ{c.x, c.y, c.z, 1.0f + d});
}
bool read_joint(std::uintptr_t buffer, std::uint16_t index, LX& out) {
    std::array<float, 12> bone{};
    if (!first_person_read(buffer + index * 0x30ULL, bone.data(), sizeof(bone))) return false;
    for (const std::size_t i : {4u, 5u, 6u, 7u, 8u, 9u, 10u}) if (!std::isfinite(bone[i])) return false; // .w of scale/position is metadata
    out ={unit(LQ{bone[4], bone[5], bone[6], bone[7]}), {bone[8], bone[9], bone[10]}};
    return true;
}
bool write_rotation(std::uintptr_t buffer, std::uint16_t index, LQ q) {
    const std::array<float, 4> r{q.x, q.y, q.z, q.w};
    return first_person_write(buffer + index * 0x30ULL + 0x10, r.data(), sizeof(r));
}
void leg_lift_edit(std::uintptr_t component) noexcept {
    auto& f = leg_lift();
    const auto core = f.core.load(std::memory_order_relaxed);
    if (!core || !component) return;
    std::uintptr_t mine{};
    if (!first_person_read(component + 0x70, &mine, 8) || mine != core) return;
    const auto now = GetTickCount64();
    const bool wanted = now < f.until.load(std::memory_order_relaxed);
    const float dt = f.last ? std::clamp(static_cast<float>(now - f.last) / 1000.0f, 0.0f, 0.1f) : 0.0f;
    f.last = now;
    f.weight = std::clamp(f.weight + (wanted ? dt : -dt) / 0.15f, 0.0f, 1.0f);
    if (f.weight <= 0) return;
    const auto error = GetLastError();
    try {
        std::uintptr_t holder{};
        if (!first_person_read(component + 0xa0, &holder, 8) || !holder) { f.why.store(1); SetLastError(error); return; }
        const auto pose = multiplayer::read_native_pose_layout(first_person_read, f.base.load(std::memory_order_relaxed), holder, 512);
        if (!pose.buffer || pose.count != first_person_skeleton_joints) { f.why.store(2); SetLastError(error); return; }
        const auto b = pose.buffer;
        LX hips, ur, lr, fr, ul, ll, fl, sroot, deck, tb, tf;
        if (!read_joint(b, 7, hips) || !read_joint(b, 8, ur) || !read_joint(b, 9, lr) || !read_joint(b, 10, fr) || !read_joint(b, 341, ul) ||
            !read_joint(b, 342, ll) || !read_joint(b, 343, fl) || !read_joint(b, 376, sroot) || !read_joint(b, 377, deck) || !read_joint(b, 378, tb) ||
            !read_joint(b, 382, tf)) { f.why.store(3); SetLastError(error); return; }
        ur = compose(hips, ur); lr = compose(ur, lr); fr = compose(lr, fr);
        ul = compose(hips, ul); ll = compose(ul, ll); fl = compose(ll, fl);
        deck = compose(sroot, deck); tb = compose(deck, tb); tf = compose(deck, tf);
        int leg = f.leg.load(std::memory_order_relaxed);
        if (leg < 0) leg = length(fl.p - tb.p) < length(fr.p - tb.p) ? 1 : 0;
        f.used_leg.store(leg, std::memory_order_relaxed);
        const LX& up_leg = leg ? ul : ur;
        const LX& knee = leg ? ll : lr;
        const LX& foot = leg ? fl : fr;
        const LX& other = leg ? fr : fl;
        // The board's frame from the feet on it (the pose's own board joints do not follow the board on a rail: the foot
        // read 0.3 m under them mid-grind): along the feet toward the tail, up toward the hips.
        const LV forward = unit(other.p - foot.p); // toward the nose
        const LV h = hips.p - (foot.p + other.p) * 0.5f;
        const LV up = unit(h - forward * dot(h, forward));
        const LV side = cross(up, forward);
        if (length(forward) < 0.5f || length(up) < 0.5f) { f.why.store(4); SetLastError(error); return; }
        const float s = f.weight * f.weight * (3.0f - 2.0f * f.weight);
        const LV offset = (up * f.up.load(std::memory_order_relaxed) - forward * f.back.load(std::memory_order_relaxed) + side * f.side.load(std::memory_order_relaxed)) * s;
        const LV a = up_leg.p, k = knee.p, c = foot.p;
        const float l1 = length(k - a), l2 = length(c - k);
        const LV to = c + offset - a;
        const float reach = std::clamp(length(to), std::abs(l1 - l2) + 1e-3f, (l1 + l2) * 0.999f);
        const LV dir = unit(to);
        const LV target = a + dir * reach;
        LV pole = (k - a) - dir * dot(k - a, dir);
        if (length(pole) < 1e-4f) pole = forward;
        pole = unit(pole);
        const float along = (l1 * l1 - l2 * l2 + reach * reach) / (2.0f * reach);
        const LV knee_to = a + dir * along + pole * std::sqrt(std::max(0.0f, l1 * l1 - along * along));
        const LQ r1 = turn_between(k - a, knee_to - a);
        const LQ r2 = turn_between(rotate(r1, c - k), target - knee_to);
        const LQ new_up = unit(mul(r1, up_leg.q)), new_knee = unit(mul(mul(r2, r1), knee.q));
        const bool wrote = write_rotation(b, leg ? 341 : 8, unit(mul(conj(hips.q), new_up))) && write_rotation(b, leg ? 342 : 9, unit(mul(conj(new_up), new_knee)));
        if (wrote) f.edits.fetch_add(1, std::memory_order_relaxed);
        f.foot_before.store(dot(c - other.p, up), std::memory_order_relaxed);
        f.foot_after.store(dot(target - other.p, up), std::memory_order_relaxed);
        f.moved.store(length(target - c), std::memory_order_relaxed);
        f.why.store(wrote ? 0 : 5, std::memory_order_relaxed);
    } catch (...) { f.why.store(6, std::memory_order_relaxed); }
    SetLastError(error);
}
// ReSkate Tricks' pre-grab: a hand on the board's nose or tail (the owner, 2026-10-17: a trigger with the left stick up
// grabs the nose, down the tail). The game's ground grab puts the hand mid-board; the arm of the pose just evaluated is
// bent with the same two-bone IK as the leg (upper arm and forearm), the hand kept as the clip turned it in the world.
// The arm's joints are found by name in the render skeleton (its first bones are the pose's: Hips 7, the board 376..).
struct HandReach {
    std::atomic<std::uintptr_t> core{}, base{};
    std::atomic<ULONGLONG> until{};
    std::atomic<int> hand{};      // 0 right, 1 left
    std::atomic<float> end{1.0f}; // +1 the front truck's end (the nose), -1 the back truck's (the tail)
    std::atomic<float> over{0.12f}, up{0.05f}; // metres past the truck along the deck, and up from the truck's joint
    std::atomic<std::uint32_t> edits{};
    std::atomic<int> why{-1};
    std::atomic<float> gap_before{}, gap_after{};
    std::atomic<std::uint64_t> joints{}; // the arm, forearm and hand found (right), and the path's length, for the log
    std::atomic<std::uintptr_t> component{}; // the skater component whose physics core is `core`, from its animation update
    std::atomic<int> frame{1};                // the deck: 0 the pose's board joints, 1 the feet, 2 the board entity's placement
    std::atomic<float> ankle{0.08f};          // the ankle joints' height over the deck (m)
    std::atomic<float> frame_gap{};           // how far the feet's deck middle is from the board joints' (m)
    std::atomic<float> board_gap{}, board_along{}, board_up{}; // the board entity's place from the feet's deck (m, along, up)
    std::atomic<int> board_axis{-1};          // the board entity matrix's row that runs along the feet
    std::uintptr_t board_entity{}, board_for{}; // render thread: the drawn board, found for this skater component
    LV last_place{};                          // render thread: the character's world place at the last edit
    std::atomic<float> ahead_along{}, ahead_board{}; // the travel direction against the nose direction used, and the board's row
    std::atomic<float> hand_along{};          // where the hand ends, along the deck from its middle (m; + toward the nose)
    // Whether the edits are drawn: each edit's arm rotations, compared at the next hand-off point with what is there then.
    std::array<LQ, 3> written{};
    std::uintptr_t written_buffer{};
    int written_phase{-1};
    std::atomic<std::uint32_t> kept{}, rewritten{}, same_buffer{}, other_buffer{};
    std::atomic<float> worst{}; // the largest turn (deg) found between an edit and the next point, since the last log
    // Animation thread only.
    float weight{};
    ULONGLONG last{};
    bool resolved{}, failed{};
    std::array<std::vector<std::uint16_t>, 2> path;    // right, left: Hips' children down to the shoulder
    std::array<std::array<std::uint16_t, 3>, 2> arm{}; // Arm, ForeArm, Hand
};
HandReach& hand_reach() { static auto* value = new HandReach; return *value; }
bool resolve_arms(HandReach& f) {
    if (f.resolved || f.failed) return f.resolved;
    const auto mesh = hall_of_meat::skeleton_mesh();
    if (!mesh) { f.why.store(7, std::memory_order_relaxed); return false; } // still being read
    const auto& names = mesh->names;
    const auto& parents = mesh->parents;
    const auto find = [&](std::string_view name) -> int {
        const auto at = std::find(names.begin(), names.end(), name);
        return at == names.end() ? -1 : static_cast<int>(at - names.begin());
    };
    // The pose's joints are the render skeleton's by index (the leg's and board's indices were): checked on two of them.
    if (find("Hips") != 7 || find("Skateboard_Root") != 376 || parents.size() != names.size()) { f.failed = true; f.why.store(8); return false; }
    for (int side = 0; side < 2; ++side) {
        const std::string prefix = side ? "Left" : "Right";
        const int arm = find(prefix + "Arm"), fore = find(prefix + "ForeArm"), hand = find(prefix + "Hand");
        if (arm < 0 || fore < 0 || hand < 0 || parents[fore] != arm || parents[hand] != fore || hand >= static_cast<int>(first_person_skeleton_joints)) {
            f.failed = true;
            f.why.store(9);
            return false;
        }
        std::vector<std::uint16_t> path;
        for (int joint = parents[arm]; joint != 7; joint = parents[joint]) {
            if (joint < 0 || path.size() > 16) { f.failed = true; f.why.store(10); return false; }
            path.push_back(static_cast<std::uint16_t>(joint));
        }
        f.path[side].assign(path.rbegin(), path.rend());
        f.arm[side] = {static_cast<std::uint16_t>(arm), static_cast<std::uint16_t>(fore), static_cast<std::uint16_t>(hand)};
    }
    f.resolved = true;
    f.joints.store(f.arm[0][0] | static_cast<std::uint64_t>(f.arm[0][1]) << 16 | static_cast<std::uint64_t>(f.arm[0][2]) << 32 | static_cast<std::uint64_t>(f.path[0].size()) << 48);
    return true;
}
void hand_reach_edit(std::uintptr_t component, int phase) noexcept {
    auto& f = hand_reach();
    const auto core = f.core.load(std::memory_order_relaxed);
    if (!core || !component) return;
    std::uintptr_t mine{};
    if (!first_person_read(component + 0x70, &mine, 8) || mine != core) return;
    const auto now = GetTickCount64();
    const bool wanted = now < f.until.load(std::memory_order_relaxed);
    const float dt = f.last ? std::clamp(static_cast<float>(now - f.last) / 1000.0f, 0.0f, 0.1f) : 0.0f;
    f.last = now;
    f.weight = std::clamp(f.weight + (wanted ? dt : -dt) / 0.12f, 0.0f, 1.0f);
    if (f.weight <= 0) return;
    const auto error = GetLastError();
    try {
        if (!resolve_arms(f)) { SetLastError(error); return; }
        std::uintptr_t holder{};
        if (!first_person_read(component + 0xa0, &holder, 8) || !holder) { f.why.store(1); SetLastError(error); return; }
        const auto pose = multiplayer::read_native_pose_layout(first_person_read, f.base.load(std::memory_order_relaxed), holder, 512);
        if (!pose.buffer || pose.count != first_person_skeleton_joints) { f.why.store(2); SetLastError(error); return; }
        const auto b = pose.buffer;
        const int side = f.hand.load(std::memory_order_relaxed) ? 1 : 0;
        const auto& ids = f.arm[side];
        LX hips, shoulder, arm, fore, hand, sroot, deck, tb, tf;
        if (!read_joint(b, 7, hips) || !read_joint(b, ids[0], arm) || !read_joint(b, ids[1], fore) || !read_joint(b, ids[2], hand) || !read_joint(b, 376, sroot) ||
            !read_joint(b, 377, deck) || !read_joint(b, 378, tb) || !read_joint(b, 382, tf)) { f.why.store(3); SetLastError(error); return; }
        shoulder = hips;
        for (const auto joint : f.path[side]) {
            LX local;
            if (!read_joint(b, joint, local)) { f.why.store(3); SetLastError(error); return; }
            shoulder = compose(shoulder, local);
        }
        arm = compose(shoulder, arm); fore = compose(arm, fore); hand = compose(fore, hand);
        deck = compose(sroot, deck); tb = compose(deck, tb); tf = compose(deck, tf);
        LV forward = unit(tf.p - tb.p); // toward the nose
        LV middle = (tb.p + tf.p) * 0.5f;
        // The trucks' distance from the middle. The pose's board joints' (run q7: the tail's goal came out at half the
        // nose's) is used only with them; the drawn board's is a fixed 0.19 m (a 0.38 m wheelbase).
        float half = length(tf.p - tb.p) * 0.5f;
        const int frame = f.frame.load(std::memory_order_relaxed);
        if (frame >= 1) {
            // The pose's own board joints are not where the board is drawn (run q1: the hand reached them, at the
            // knee, with the tail far below), so the deck is taken from the feet standing on it, as the leg lift does:
            // along the feet (the board joints only say which way the nose is), at the ankles less their height.
            LX ur, lr, fr, ul, ll, fl;
            if (!read_joint(b, 8, ur) || !read_joint(b, 9, lr) || !read_joint(b, 10, fr) || !read_joint(b, 341, ul) || !read_joint(b, 342, ll) || !read_joint(b, 343, fl)) {
                f.why.store(3); SetLastError(error); return;
            }
            ur = compose(hips, ur); lr = compose(ur, lr); fr = compose(lr, fr);
            ul = compose(hips, ul); ll = compose(ul, ll); fl = compose(ll, fl);
            LV along = unit(fr.p - fl.p);
            if (dot(along, forward) < 0) along = along * -1.0f;
            const LV feet = (fr.p + fl.p) * 0.5f;
            const LV h = hips.p - feet;
            const LV feet_up = unit(h - along * dot(h, along));
            const LV feet_deck = feet - feet_up * f.ankle.load(std::memory_order_relaxed);
            f.frame_gap.store(length(feet_deck - middle), std::memory_order_relaxed);
            forward = along;
            middle = feet_deck;
            // The drawn board: its entity's placement (world), into the pose's frame (joint 1's parent: joints 0 and 1
            // place the character in the world).
            const auto base = f.base.load(std::memory_order_relaxed);
            if (f.board_for != component) {
                f.board_for = component;
                f.board_entity = 0;
                std::uintptr_t collection{}, entity{};
                if (first_person_read(component + 0x18, &collection, 8) && first_person_read(collection, &entity, 8)) {
                    try { f.board_entity = multiplayer::read_native_board(first_person_read, base, entity).entity; } catch (...) {}
                }
            }
            LX j0, j1;
            std::uintptr_t collection{};
            std::array<std::uint8_t, 2> layout{};
            std::array<float, 16> m{};
            if (f.board_entity && read_joint(b, 0, j0) && read_joint(b, 1, j1) && first_person_read(f.board_entity + 0x70, &collection, 8) &&
                first_person_read(collection + 9, layout.data(), 2) && layout[0] <= 128 && layout[1] <= 32 &&
                first_person_read(collection + 0x10 + (std::uintptr_t{layout[0]} + 2 * layout[1]) * 0x20, m.data(), sizeof(m))) {
                const LX world = compose(j0, j1);
                const LQ back = conj(world.q);
                const LV moved = rotate(back, world.p - f.last_place);
                f.last_place = world.p;
                const LV place = rotate(back, LV{m[12], m[13], m[14]} - world.p);
                int axis = 0;
                float best = 0;
                LV axes[3];
                for (int i = 0; i < 3; ++i) {
                    axes[i] = unit(rotate(back, LV{m[i * 4], m[i * 4 + 1], m[i * 4 + 2]}));
                    if (std::abs(dot(axes[i], along)) > std::abs(best)) best = dot(axes[i], along), axis = i;
                }
                if (length(moved) > 0.005f && length(moved) < 1.0f) f.ahead_board.store(dot(unit(moved), axes[axis]), std::memory_order_relaxed);
                f.board_axis.store((axis + 1) * (best < 0 ? -1 : 1), std::memory_order_relaxed); // 1..3 the row, negative when it runs toward the tail
                f.board_gap.store(length(place - feet_deck), std::memory_order_relaxed);
                f.board_along.store(dot(place - feet_deck, along), std::memory_order_relaxed);
                f.board_up.store(dot(place - feet_deck, feet_up), std::memory_order_relaxed);
                // The board's own nose is the way the board points: the pose's board joints can be turned round (run q6,
                // riding forward with them pointing back: the tail's hand went to the nose), the board's row never was
                // (it ran with the travel on every forward ride).
                forward = axes[axis] * (best < 0 ? -1.0f : 1.0f);
                if (axis == 2) forward = axes[2];
                if (frame == 2) middle = place;
                // The drawn board's middle along and across the deck, at the feet's height (its origin is below the deck).
                else middle = feet_deck + (place - feet_deck) - feet_up * dot(place - feet_deck, feet_up);
                half = 0.19f;
                if (length(moved) > 0.005f && length(moved) < 1.0f) f.ahead_along.store(dot(unit(moved), forward), std::memory_order_relaxed);
            }
        }
        const LV h = hips.p - middle;
        const LV up = unit(h - forward * dot(h, forward));
        const LV across = cross(up, forward);
        if (length(forward) < 0.5f || length(up) < 0.5f) { f.why.store(4); SetLastError(error); return; }
        const float over = f.over.load(std::memory_order_relaxed);
        const LV tip = middle + forward * ((half + over) * (f.end.load(std::memory_order_relaxed) >= 0 ? 1.0f : -1.0f));
        // On the edge the clip's hand already holds (toe or heel), kept within the deck.
        const LV goal = tip + up * f.up.load(std::memory_order_relaxed) + across * std::clamp(dot(hand.p - middle, across), -0.1f, 0.1f);
        const float s = f.weight * f.weight * (3.0f - 2.0f * f.weight);
        const LV a = arm.p, k = fore.p, c = hand.p;
        const float l1 = length(k - a), l2 = length(c - k);
        const LV to = c + (goal - c) * s - a;
        const float reach = std::clamp(length(to), std::abs(l1 - l2) + 1e-3f, (l1 + l2) * 0.999f);
        const LV dir = unit(to);
        const LV target = a + dir * reach;
        LV pole = (k - a) - dir * dot(k - a, dir);
        if (length(pole) < 1e-4f) pole = up * -1.0f;
        pole = unit(pole);
        const float along = (l1 * l1 - l2 * l2 + reach * reach) / (2.0f * reach);
        const LV elbow = a + dir * along + pole * std::sqrt(std::max(0.0f, l1 * l1 - along * along));
        const LQ r1 = turn_between(k - a, elbow - a);
        const LQ r2 = turn_between(rotate(r1, c - k), target - elbow);
        const LQ new_arm = unit(mul(r1, arm.q)), new_fore = unit(mul(mul(r2, r1), fore.q));
        const bool wrote = write_rotation(b, ids[0], unit(mul(conj(shoulder.q), new_arm))) && write_rotation(b, ids[1], unit(mul(conj(new_arm), new_fore))) &&
                           write_rotation(b, ids[2], unit(mul(conj(new_fore), hand.q)));
        if (wrote) {
            f.edits.fetch_add(1, std::memory_order_relaxed);
            f.written = {unit(mul(conj(shoulder.q), new_arm)), unit(mul(conj(new_arm), new_fore)), unit(mul(conj(new_fore), hand.q))};
            f.written_buffer = b;
            f.written_phase = phase;
        }
        f.gap_before.store(length(goal - c), std::memory_order_relaxed);
        f.hand_along.store(dot(target - middle, forward), std::memory_order_relaxed);
        f.gap_after.store(length(goal - target), std::memory_order_relaxed);
        f.why.store(wrote ? 0 : 5, std::memory_order_relaxed);
    } catch (...) { f.why.store(6, std::memory_order_relaxed); }
    SetLastError(error);
}
// Where the leg and hand are edited: 0 right after the animation update (those edits are not drawn: run pk, a goal 1.2 m
// overhead left the arm as the clip had it), 1 at the render hand-off just before the pose is published, 2 just after.
std::atomic<int> pose_phase{2};
// Each component's animation update: learns which component is the skater's, and edits there in phase 0.
void pose_editors_on_animation(std::uintptr_t component) noexcept {
    std::uintptr_t mine{};
    if (!component || !first_person_read(component + 0x70, &mine, 8) || !mine) return;
    auto& leg = leg_lift();
    auto& hand = hand_reach();
    if (mine == leg.core.load(std::memory_order_relaxed)) leg.component.store(component, std::memory_order_relaxed);
    if (mine == hand.core.load(std::memory_order_relaxed)) hand.component.store(component, std::memory_order_relaxed);
    if (pose_phase.load(std::memory_order_relaxed) != 0) return;
    leg_lift_edit(component);
    hand_reach_edit(component, 0);
}
// The arm's rotations now against the last edit's (a quaternion's sign is free): the largest turn, degrees.
float arm_turn_since_edit(const HandReach& f, std::uintptr_t buffer) {
    const int side = f.hand.load(std::memory_order_relaxed) ? 1 : 0;
    float worst = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        LX now;
        if (!read_joint(buffer, f.arm[side][i], now)) return 180;
        const auto& q = f.written[i];
        const float d = std::min(1.0f, std::abs(q.x * now.q.x + q.y * now.q.y + q.z * now.q.z + q.w * now.q.w));
        worst = std::max(worst, 2.0f * std::acos(d) * 57.2958f);
    }
    return worst;
}
bool holder_is(std::uintptr_t component, std::uintptr_t animation_interface, std::uintptr_t& holder) {
    return component && first_person_read(component + 0xa0, &holder, 8) && holder && animation_interface == holder + 0xc0;
}
// The render hand-off of every skeleton (engine thread).
void pose_editors_on_render(std::uintptr_t animation_interface, std::uintptr_t render_data, bool after) noexcept {
    const auto error = GetLastError();
    const int phase = after ? 2 : 1;
    const int wanted = pose_phase.load(std::memory_order_relaxed);
    auto& hand = hand_reach();
    std::uintptr_t holder{};
    if (const auto component = hand.component.load(std::memory_order_relaxed); holder_is(component, animation_interface, holder)) {
        // Did the last edit survive to here? One made after the animation is checked before the publication, one made
        // before the publication just after it.
        if (hand.written_phase >= 0 && hand.written_phase == phase - 1 && hand.resolved) {
            const float turn = arm_turn_since_edit(hand, hand.written_buffer);
            (turn < 1.0f ? hand.kept : hand.rewritten).fetch_add(1, std::memory_order_relaxed);
            hand.worst.store(std::max(hand.worst.load(std::memory_order_relaxed), turn), std::memory_order_relaxed);
            hand.written_phase = -1;
        }
        if (after) {
            std::uintptr_t published{};
            const auto pose = multiplayer::read_native_pose_layout(first_person_read, hand.base.load(std::memory_order_relaxed), holder, 512);
            if (first_person_read(render_data + 0x10, &published, 8))
                (published == pose.buffer ? hand.same_buffer : hand.other_buffer).fetch_add(1, std::memory_order_relaxed);
        }
        if (wanted == phase) hand_reach_edit(component, phase);
    }
    if (wanted == phase)
        if (const auto component = leg_lift().component.load(std::memory_order_relaxed); holder_is(component, animation_interface, holder))
            leg_lift_edit(component);
    SetLastError(error);
}
bool install_pose_editors(std::uintptr_t base, std::string& detail) noexcept {
    static bool installed{};
    if (installed) return true;
    try {
        if (!multiplayer::install_entity_hooks(base, detail)) return false;
    } catch (...) { detail = "threw"; return false; }
    multiplayer::set_animation_pose_editor(&pose_editors_on_animation);
    multiplayer::set_render_pose_editor(&pose_editors_on_render);
    installed = true;
    return true;
}
}
}
namespace dingosdk {
using namespace client_source::detail;
bool set_leg_lift(std::uintptr_t base, std::uintptr_t core, std::uint32_t milliseconds, float up, float back, float side, int leg, std::string& detail) noexcept {
    auto& f = leg_lift();
    if (!install_pose_editors(base, detail)) return false;
    f.base.store(base, std::memory_order_relaxed);
    f.core.store(core, std::memory_order_relaxed);
    f.up.store(up, std::memory_order_relaxed);
    f.back.store(back, std::memory_order_relaxed);
    f.side.store(side, std::memory_order_relaxed);
    f.leg.store(leg, std::memory_order_relaxed);
    f.until.store(milliseconds ? GetTickCount64() + milliseconds : 0, std::memory_order_relaxed);
    return true;
}
std::string take_leg_lift() {
    auto& f = leg_lift();
    return std::format("{} leg edits, {} leg, foot {:.2f} -> {:.2f} m over the front foot (moved {:.2f}), why {}", f.edits.exchange(0, std::memory_order_relaxed),
                       f.used_leg.load() == 1 ? "left" : f.used_leg.load() == 0 ? "right" : "no", f.foot_before.load(), f.foot_after.load(), f.moved.load(), f.why.load());
}
bool set_hand_reach(std::uintptr_t base, std::uintptr_t core, std::uint32_t milliseconds, int hand, float end, float over, float up, std::string& detail) noexcept {
    auto& f = hand_reach();
    if (!install_pose_editors(base, detail)) return false;
    hall_of_meat::prepare_skeleton(); // the arm's joints by name (read once, in the background)
    f.base.store(base, std::memory_order_relaxed);
    f.core.store(core, std::memory_order_relaxed);
    f.hand.store(hand, std::memory_order_relaxed);
    f.end.store(end, std::memory_order_relaxed);
    f.over.store(over, std::memory_order_relaxed);
    f.up.store(up, std::memory_order_relaxed);
    f.until.store(milliseconds ? GetTickCount64() + milliseconds : 0, std::memory_order_relaxed);
    return true;
}
std::string take_hand_reach() {
    auto& f = hand_reach();
    const auto j = f.joints.load();
    return std::format("{} hand edits at {}, {} hand at the {}, {:.2f} m from the goal before the reach, {:.2f} after, why {} (right arm joints {} {} {}, {} above); "
                       "drawn check: kept {} rewritten {} (worst {:.1f} deg), published buffer same {} other {}; deck from the {} ({:.2f} m from the board joints'; the board entity {:.2f} m from the feet's, {:.2f} along {:.2f} up, axis {}); travel against the nose {:.2f}, the board's row {:.2f}; hand {:.2f} m along the deck", f.edits.exchange(0, std::memory_order_relaxed),
                       pose_phase.load() == 0 ? "the animation" : pose_phase.load() == 1 ? "the hand-off (before)" : "the hand-off (after)",
                       f.hand.load() ? "left" : "right", f.end.load() >= 0 ? "nose" : "tail", f.gap_before.load(), f.gap_after.load(), f.why.load(), j & 0xffff, (j >> 16) & 0xffff,
                       (j >> 32) & 0xffff, j >> 48, f.kept.exchange(0), f.rewritten.exchange(0), f.worst.exchange(0), f.same_buffer.exchange(0), f.other_buffer.exchange(0),
                       f.frame.load() == 2 ? "board entity" : f.frame.load() == 1 ? "feet" : "board joints", f.frame_gap.load(), f.board_gap.load(), f.board_along.load(),
                       f.board_up.load(), f.board_axis.load(), f.ahead_along.load(), f.ahead_board.load(), f.hand_along.load());
}
void set_hand_reach_frame(int frame, float ankle) noexcept {
    hand_reach().frame.store(std::clamp(frame, 0, 2), std::memory_order_relaxed);
    hand_reach().ankle.store(std::clamp(ankle, -0.2f, 0.3f), std::memory_order_relaxed);
}
void set_pose_edit_phase(int phase) noexcept { pose_phase.store(std::clamp(phase, 0, 2), std::memory_order_relaxed); }
int pose_edit_phase() noexcept { return pose_phase.load(std::memory_order_relaxed);
}
}
