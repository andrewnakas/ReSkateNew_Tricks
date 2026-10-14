#include "tricks_score_hud.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/hud_corner.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Extension/HallOfMeat/hall_of_meat_hud.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <string>
#include <cstring>
#include <deque>
#include <format>
#include <vector>

namespace dingosdk::tricks {
namespace {
namespace corner = addr::hud_corner;
namespace menu_data = multiplayer::menu_data;
using Address = std::uintptr_t;

// ScoringHUDViewModel's fields and its trick entries' (measured in a live ui dump, 2026-10-14).
constexpr std::uint32_t trick_history = 0x84db5623;  // StackViewModel TrickHistory
constexpr std::uint32_t content_data = 0x25e4d6c8;   // the item's presenter: its data reference
constexpr std::uint32_t trick_name = 0x53bac52d;     // TrickHistoryItemViewModel labels
constexpr std::uint32_t trick_score = 0xd491db6a;
constexpr std::uint32_t trick_completed = 0x0205e59c;
constexpr std::uint32_t label_text = 0x4d8e01b9;     // a label's text
constexpr std::uint32_t trick_item_schema = 0xbf5e8a16;
// The score numbers (display copies the game's scoring scripts write): the line score and its twin (floats), the big
// sequence number (a label), the multiplier (float).
constexpr std::uint32_t line_score = 0x40bff667, line_score_twin = 0x9a6daa54, sequence_label = 0x7d1e237c, multiplier = 0x6a41d8c6;
// Our entries' keys, apart from the game's (0x8000....).
constexpr std::uint32_t key_base = 0x52530000;

struct Owned {
    menu_data::Value item, model;
    std::uint32_t key{};
};
struct Pending {
    std::string name;
    int points{};
    ULONGLONG queued{};
};
struct State {
    Address manager{};
    std::deque<Pending> pending;
    std::vector<Owned> owned;
    menu_data::Value template_model; // our copy of a game entry's model: the look every entry starts from
    std::uint64_t next_id{0x5253544B00000001ULL};
    std::uint32_t next_key{1};
    std::uint32_t added{}, dropped{}, failures{};
    // The points the mod's tricks added to the current line (times the line's multiplier when they were done), and
    // the game's own numbers they are added to (as the game last wrote them).
    float pending_points{};
    bool filter_on{}, filter_warned{};


    int game_sequence{-1};
    std::string shown_sequence;
    std::string last_error;
};
State &state() {
    static auto *value = new State;
    return *value;
}

template <class Use> bool with_ui_models(Address base, Use &&use) {
    if (!base) return false;
    const auto ui = menu_data::read<Address>(base + addr::engine::ui_manager);
    const auto manager = ui ? menu_data::read<Address>(ui + 0x140) : 0;
    if (!manager) return false;
    const menu_data::Context context(base, manager);
    game::ModelWriteLock lock(manager);
    use(context);
    return true;
}
std::uint32_t key_of(const std::vector<std::byte> &items, unsigned index, unsigned stride, std::uint16_t key_at) {
    std::uint32_t key{};
    std::memcpy(&key, items.data() + std::size_t{index} * stride + key_at, sizeof key);
    return key;
}
bool ours(std::uint32_t key) { return (key & 0xffff0000u) == key_base; }

// Our points on top of the game's own numbers. The big number (label 0x7d1e237c) is the sequence in progress: it is
// written here, whenever the game writes it. "LINE:" and the end banner are drawn when the game writes its line
// totals, so ours are added inside those very writes (Hall of Meat's model write hook, shared): the ActiveScoringData
// model (ScoringHUDViewModel 0xcdffb552 -> 0xa8b7e62f) holds the line in progress (0x210) and the line just
// finished (0x450), each with an integer LineScore (0x40bff667); the HUD's line floats (0x40bff667 / 0x9a6daa54)
// follow. When the game's line total goes up a sequence has banked (ours go with it); when it drops the line has
// ended, and what we added goes to the finished line. (Written a tick later, as before, nothing showed: measured.)
constexpr std::uint32_t scoring_data = 0xcdffb552, line_total = 0x40bff667;
constexpr std::uint32_t line_structs[2]{0xf99b169c, 0x2da1d43e};
struct LineFilter {
    std::array<std::atomic<std::uint64_t>, 4> handles{}; // line in progress, line finished (ints), the HUD's two floats
    // The game writes each line structure whole (measured: its handle carries the 40 bytes, LineScore first), so the
    // structures' own handles are matched too, with their size and LineScore's offset in them.
    std::array<std::atomic<std::uint64_t>, 2> structs{};
    std::array<std::atomic<std::uint32_t>, 2> struct_size{}, total_at{};
    std::atomic<std::int32_t> last_line{-1}, last_finished{-1};
    std::atomic<float> sequence{}, line{}, finished{};
    std::atomic<std::int32_t> game_sequence{-1}; // the sequence's own score as the game last wrote it (the big number)
    std::atomic<std::uint32_t> carried{}; // <dev> banks whose increase already held our points
    std::atomic<std::uint32_t> hits{}, calls{}, float_hits{};
    // <dev> the last writes to our fields: which (0..3), the game's value, ours, the line in progress then
    struct Write { std::atomic<std::uint32_t> which{}; std::atomic<float> in{}, out{}; std::atomic<std::int32_t> line{}; };
    std::array<Write, 24> writes{};
    std::atomic<std::uint32_t> write_count{};
    void note(std::uint32_t which, float in, float out) noexcept {
        auto &w = writes[write_count.fetch_add(1, std::memory_order_relaxed) % writes.size()];
        w.which.store(which); w.in.store(in); w.out.store(out); w.line.store(last_line.load());
    }
    std::atomic<std::uint64_t> seen[8]{}; // <dev> the last handles written, to compare with ours
    std::atomic<std::uint64_t> data_model{}; // <dev> ActiveScoringData's handle: writes inside it are logged
    std::atomic<std::uint64_t> data_seen[16]{};
    std::atomic<std::uint32_t> data_calls{};
};
LineFilter &filter() {
    static auto *value = new LineFilter;
    return *value;
}
// The line totals as written ([0] in progress, [1] finished): our points added; the bookkeeping on [0]'s changes.
std::int32_t filtered_total(LineFilter &f, int k, std::int32_t v) noexcept {
    if (k == 0) {
        const auto last = f.last_line.exchange(v, std::memory_order_relaxed);
        if (last >= 0 && v > last) { // a sequence banked: ours go with it
            f.line.store(f.line.load() + f.sequence.exchange(0.0f));
        } else if (last > 0 && v < last) { // the line ended: what we added goes to the finished line
            if (f.finished.load() <= 0) f.finished.store(f.line.load() + f.sequence.load());
            f.line.store(0.0f);
            f.sequence.store(0.0f);
        } else if (last == 0 && v > 0) f.finished.store(0.0f); // a new line
        f.hits.fetch_add(1, std::memory_order_relaxed);
        return v + static_cast<std::int32_t>(std::lround(f.line.load()));
    }
    if (v != f.last_finished.exchange(v, std::memory_order_relaxed) && f.finished.load() <= 0) f.finished.store(f.line.load() + f.sequence.load());
    f.hits.fetch_add(1, std::memory_order_relaxed);
    return v + static_cast<std::int32_t>(std::lround(f.finished.load()));
}
const void *line_filter(std::uint64_t handle, std::uintptr_t, const void *value) noexcept {
    auto &f = filter();
    const auto n = f.calls.fetch_add(1, std::memory_order_relaxed);
    f.seen[n % 8].store(handle, std::memory_order_relaxed);
    thread_local float out_float;
    thread_local std::array<std::byte, 512> out_struct;
    for (int k = 0; k < 2; ++k) {
        if (handle == f.handles[k].load(std::memory_order_relaxed)) {
            std::int32_t v;
            std::memcpy(&v, value, 4);
            (void)filtered_total(f, k, v); // followed only: the game's own value is written
            f.note(static_cast<std::uint32_t>(k), static_cast<float>(v), static_cast<float>(v));
            return value;
        }
        if (handle == f.structs[k].load(std::memory_order_relaxed)) {
            const auto size = f.struct_size[k].load(std::memory_order_relaxed), at = f.total_at[k].load(std::memory_order_relaxed);
            if (!size || size > out_struct.size() || at + 4 > size) return value;
            // Followed only, the game's own value written: the HUD's line float is filled from this struct, so ours added
            // here came back in the float a second time (measured 2026-10-14: 433 + 975 shown as 2,383).
            std::int32_t v;
            std::memcpy(&v, static_cast<const std::byte *>(value) + at, 4);
            (void)filtered_total(f, k, v);
            if (v != f.writes[(f.write_count.load() + f.writes.size() - 1) % f.writes.size()].in.load() || k == 1) f.note(10 + static_cast<std::uint32_t>(k), static_cast<float>(v), static_cast<float>(v));
            return value;
        }
    }
    if (handle == f.handles[2].load(std::memory_order_relaxed) || handle == f.handles[3].load(std::memory_order_relaxed)) {
        float v;
        std::memcpy(&v, value, 4);
        // At a line's end the game writes the finished total (its struct), resets the line in progress and this float to
        // 0, then copies the float into its twin, which the end banner shows (measured 2026-10-14). So: the line float
        // carries the banked points during the line and passes the reset through; the twin, once the line has ended,
        // is the game's finished total with everything we added to the line.
        const bool twin = handle == f.handles[3].load(std::memory_order_relaxed);
        const bool ended = f.last_line.load() == 0 && f.finished.load() > 0;
        // (The end banner reads the line float as the game resets it: with the reset turned into our points alone it said
        // 975, with the 0 passed through it fell back to the game's 112. So the reset becomes the full total.)
        if (ended) out_float = static_cast<float>(std::max(f.last_finished.load(), 0)) + f.finished.load();
        else out_float = v + f.line.load();
        f.note(twin ? 3 : 2, v, out_float);
        f.float_hits.fetch_add(1, std::memory_order_relaxed);
        return &out_float;
    }
    return value;
}
void add_bonus(State &s, const menu_data::Context &context, const menu_data::Value &model) {
    auto &f = filter();
    if (s.pending_points > 0) {
        const float mult = menu_data::read<float>(context.address(context.field(model, multiplier)));
        f.sequence.store(f.sequence.load() + s.pending_points * (mult >= 1.0f && mult < 100.0f ? mult : 1.0f));
        s.pending_points = 0;
        s.shown_sequence.clear(); // write it now
    }
    // The fields the filter looks for (their handles change with the model registry).
    const auto data_ref = menu_data::read<menu_data::Ref>(context.address(context.field(model, scoring_data)));
    if (data_ref.handle && context.type_of(data_ref.handle)) {
        const menu_data::Value data{data_ref.handle, context.type_of(data_ref.handle)};
        for (int k = 0; k < 2; ++k) {
            f.handles[k].store(context.path(data, {line_structs[k], line_total}).handle, std::memory_order_relaxed);
            const auto line_struct = context.field(data, line_structs[k]);
            f.struct_size[k].store(menu_data::size(line_struct.type), std::memory_order_relaxed);
            f.total_at[k].store(context.member(line_struct.type, line_total).offset, std::memory_order_relaxed);
            f.structs[k].store(line_struct.handle, std::memory_order_relaxed);
        }
        f.data_model.store(data.handle, std::memory_order_relaxed);
    }
    f.handles[2].store(context.field(model, line_score).handle, std::memory_order_relaxed);
    f.handles[3].store(context.field(model, line_score_twin).handle, std::memory_order_relaxed);
    if (!s.filter_on) {
        s.filter_on = hall_of_meat::use_model_write_filter(context.base, &line_filter);
        if (!s.filter_on && !std::exchange(s.filter_warned, true))
            logging::log(logging::Level::warning, logging::Channel::skater, "Tricks: the score line cannot carry the mod's points (the UI model write could not be hooked).");
    }
    // The sequence in progress (the big number): ours added whenever the game writes it.
    const bool active = menu_data::read<std::uint8_t>(context.address(context.field(model, corner::hud_widget_active))) != 0;
    if (!active) return;
    const auto sequence_text = context.path(model, {sequence_label, label_text});
    const auto text = context.text(sequence_text);
    if (text != s.shown_sequence) {
        int value{};
        try { value = text.empty() ? 0 : std::stoi(text); } catch (...) { value = -1; }
        s.game_sequence = value;
        f.game_sequence.store(value, std::memory_order_relaxed);
    }
    const float sequence = f.sequence.load();
    if (sequence > 0 && s.game_sequence >= 0) {
        const auto shown = std::to_string(s.game_sequence + static_cast<int>(std::lround(sequence)));
        if (text != shown) context.text(sequence_text, shown);
        s.shown_sequence = shown;
    } else s.shown_sequence = text;
}
void run(const menu_data::Context &context) {
    auto &s = state();
    if (s.manager != context.manager) { // a new registry: what we made went with the old one
        s.manager = context.manager;
        s.owned.clear();
        s.template_model = {};
    }
    const auto roots = context.roots({corner::score_view_schema});
    if (roots.empty()) return;
    add_bonus(s, context, roots.front().model);
    if (s.pending.empty() && s.owned.empty()) return;
    const auto items = context.path(roots.front().model, {trick_history, corner::stack_items});
    unsigned count{}, stride{};
    auto bytes = context.array(items, 64, count, stride);
    menu_data::require(!count || stride == menu_data::stack_item.size, "the trick list's items differ");
    const auto key_at = context.member(context.type(menu_data::stack_item), corner::item_key).offset;
    // Entries the game has dropped (its list is cleared when a line ends): ours are freed.
    for (auto it = s.owned.begin(); it != s.owned.end();) {
        bool listed = false;
        for (unsigned i = 0; i < count; ++i) listed = listed || key_of(bytes, i, stride, key_at) == it->key;
        if (listed) { ++it; continue; }
        context.destroy(it->item);
        context.destroy(it->model);
        it = s.owned.erase(it);
        ++s.dropped;
    }
    // The look to clone: the newest of the game's own entries.
    int game_index = -1;
    for (unsigned i = 0; i < count && game_index < 0; ++i)
        if (!ours(key_of(bytes, i, stride, key_at))) game_index = static_cast<int>(i);
    if (game_index >= 0 && !s.template_model.handle) {
        // The presenter's data is a reference {record, model handle}: read whole (a field lookup through it lands on the
        // model it refers to, not on the reference).
        const auto ref = menu_data::read<menu_data::Ref>(context.address(context.path(context.element(items, game_index), {corner::item_content, content_data})));
        const auto game_model_handle = ref.handle;
        const auto type = context.type_of(game_model_handle);
        menu_data::require(type != 0, "the game's trick entry has no model");
        s.template_model = context.create({trick_item_schema, static_cast<std::uint16_t>(menu_data::size(type))}, s.next_id++);
        context.copy(s.template_model, context.address({game_model_handle, type}));
    }
    while (!s.pending.empty()) {
        const auto pending = s.pending.front();
        if (!s.template_model.handle || game_index < 0) {
            // Nothing to clone yet: kept for 3 s (the game's first trick of the line is usually right behind).
            if (GetTickCount64() - pending.queued > 3000) {
                s.pending.pop_front();
                ++s.failures;
            }
            return;
        }
        s.pending.pop_front();
        Owned entry;
        entry.model = context.create({trick_item_schema, static_cast<std::uint16_t>(menu_data::size(s.template_model.type))}, s.next_id++);
        context.copy(entry.model, context.address(s.template_model));
        context.text(context.path(entry.model, {trick_name, label_text}), pending.name);
        context.text(context.path(entry.model, {trick_score, label_text}), std::to_string(pending.points));
        context.text(context.path(entry.model, {trick_completed, label_text}), std::string{});
        entry.item = context.create(menu_data::stack_item, s.next_id++);
        context.copy(entry.item, context.address(context.element(items, static_cast<unsigned>(game_index))));
        entry.key = key_base | (s.next_key++ & 0xffff);
        context.set(context.field(entry.item, corner::item_key), entry.key);
        {
            const auto data = context.path(entry.item, {corner::item_content, content_data});
            auto ref = menu_data::read<menu_data::Ref>(context.address(data));
            ref.handle = entry.model.handle;
            context.publish(data, &ref);
        }
        std::vector<std::byte> item(menu_data::stack_item.size);
        menu_data::require(memory::read_bytes(context.address(entry.item), item.data(), item.size()), "our trick entry is unavailable");
        bytes.resize(std::size_t{count} * menu_data::stack_item.size);
        bytes.insert(bytes.begin(), item.begin(), item.end()); // the newest first, as the game has it
        ++count;
        context.array(items, bytes, count);
        s.owned.push_back(entry);
        ++s.added;
        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: \"{}\" ({} points) added to the game's trick list ({} entries).", pending.name, pending.points, count);
    }
}
} // namespace

void queue_trick_entry(const std::string &name, int points) noexcept {
    try {
        auto &s = state();
        if (s.pending.size() < 8) s.pending.push_back({name, points, GetTickCount64()});
        s.pending_points += static_cast<float>(std::max(points, 0));
    } catch (...) {}
}
void update_trick_entries(std::uintptr_t base) noexcept {
    auto &s = state();
    if (s.pending.empty() && s.owned.empty() && filter().sequence.load() <= 0 && s.pending_points <= 0 && !s.filter_on) return;
    // Every tick while an entry waits or our points are on the line (the game may write its numbers any frame);
    // otherwise only to see the game drop our entries, ten times a second.
    static ULONGLONG next{};
    const auto now = GetTickCount64();
    if (s.pending.empty() && filter().sequence.load() <= 0 && s.pending_points <= 0 && now < next) return;
    next = now + 100;
    try {
        with_ui_models(base, [&](const menu_data::Context &context) { run(context); });
    } catch (const std::exception &error) {
        ++s.failures;
        s.pending.clear();
        if (s.last_error != error.what()) {
            s.last_error = error.what();
            logging::log(logging::Level::warning, logging::Channel::skater, "Tricks: the game's trick list could not take an entry: {}.", s.last_error);
        }
    } catch (...) { ++s.failures; s.pending.clear(); }
}
bool read_hud_line_score(std::uintptr_t base, float &value) noexcept {
    bool found = false;
    try {
        with_ui_models(base, [&](const menu_data::Context &context) {
            const auto roots = context.roots({corner::score_view_schema});
            if (roots.empty()) return;
            value = menu_data::read<float>(context.address(context.field(roots.front().model, 0x40bff667)));
            found = true;
        });
    } catch (...) { return false; }
    return found;
}
void set_line_total_writes(bool) noexcept {}
std::string trick_entries_status() {
    const auto &s = state();
    return std::format("trick list: {} added, {} dropped by the game, {} failed, {} live", s.added, s.dropped, s.failures, s.owned.size());
}
} // namespace dingosdk::tricks
