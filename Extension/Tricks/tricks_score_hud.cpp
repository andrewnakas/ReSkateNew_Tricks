#include "tricks_score_hud.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/hud_corner.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
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
    float sequence_bonus{}, line_bonus{}, finished_bonus{}, pending_points{};
    float game_line{-1}, shown_line{-1};
    std::array<std::int32_t, 2> game_line_total{-1, -1}, shown_line_total{-1, -1};
    ULONGLONG inactive_since{};
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

// Our points on top of the game's own numbers (2026-10-14, read from live dumps): the big number is the sequence in
// progress (label 0x7d1e237c); "LINE:" is the line's banked total, the integer LineScore in the scoring data model the
// HUD refers to (ScoringHUDViewModel 0xcdffb552 -> model 0xa8b7e62f, its current line 0xf99b169c, LineScore
// 0x40bff667); the HUD's own line floats (0x40bff667 / 0x9a6daa54) follow it. A mod trick's points (times the
// multiplier) join the sequence; when the game banks the sequence (its LineScore goes up) they move to the line.
// Whenever the game writes one of these, ours is added before it shows.
constexpr std::uint32_t scoring_data = 0xcdffb552, line_total = 0x40bff667;
// ActiveScoringData holds two line structures with a LineScore each (0x210 and 0x450); which one "LINE:" shows is not
// known yet, so both carry our banked points, each followed on its own.
constexpr std::uint32_t line_structs[2]{0xf99b169c, 0x2da1d43e};
std::atomic<bool> write_line_totals{false}; // <dev> `tricks score lines on`
void add_bonus(State &s, const menu_data::Context &context, const menu_data::Value &model) {
    const bool active = menu_data::read<std::uint8_t>(context.address(context.field(model, corner::hud_widget_active))) != 0;
    if (s.pending_points > 0) {
        const float mult = menu_data::read<float>(context.address(context.field(model, multiplier)));
        s.sequence_bonus += s.pending_points * (mult >= 1.0f && mult < 100.0f ? mult : 1.0f);
        s.pending_points = 0;
        s.shown_sequence.clear(); // write it now
    }
    // The line ends with the HUD hiding, and its last sequence banks as it does (the end-of-line "LINE:" banner): our
    // points are kept through that, and given up only once the HUD has been off for 5 s (or a new line starts).
    const auto now = GetTickCount64();
    if (active) s.inactive_since = 0;
    else if (!s.inactive_since) s.inactive_since = now;
    if (!active && now - s.inactive_since > 5000) {
        s.sequence_bonus = s.line_bonus = s.finished_bonus = 0;
        s.game_line = -1;
        s.game_line_total = {-1, -1};
        s.game_sequence = -1;
        return;
    }
    // The line's banked total, as the game keeps it (an integer in the scoring data model).
    const auto data_ref = menu_data::read<menu_data::Ref>(context.address(context.field(model, scoring_data)));
    const menu_data::Value data{data_ref.handle, data_ref.handle ? context.type_of(data_ref.handle) : 0};
    // [0] is the line in progress, [1] the line just finished: when a line ends the game moves its total from [0] (back
    // to 0) to [1], and the end banner ("LINE: n") shows [1] (measured 2026-10-14).
    bool banked = false, finished = false, started = false;
    std::array<std::int32_t, 2> totals{};
    if (data.type)
        for (int k = 0; k < 2; ++k) {
            totals[k] = menu_data::read<std::int32_t>(context.address(context.path(data, {line_structs[k], line_total})));
            if (totals[k] != s.shown_line_total[k]) { // the game wrote it
                if (k == 0 && s.game_line_total[0] >= 0 && totals[0] > s.game_line_total[0]) banked = true;
                if (k == 0 && s.game_line_total[0] > 0 && totals[0] < s.game_line_total[0]) finished = true;
                if (k == 0 && s.game_line_total[0] == 0 && totals[0] > 0) started = true;
                s.game_line_total[k] = totals[k];
            }
        }
    if (finished || banked || started) // <dev>
        logging::log(logging::Level::info, logging::Channel::skater, "Tricks: score line {} (game line {} / {}, our sequence {:.0f}, our line {:.0f}, our finished {:.0f}).",
                     finished ? "finished" : banked ? "banked" : "started", totals[0], totals[1], s.sequence_bonus, s.line_bonus, s.finished_bonus);
    if (started) s.finished_bonus = 0; // a new line: the last one's banner is gone
    if (finished) { // everything ours goes to the finished line
        s.finished_bonus = s.sequence_bonus + s.line_bonus;
        s.sequence_bonus = s.line_bonus = 0;
        s.shown_sequence.clear();
    } else if (banked) { // a sequence banked: ours go with it
        s.line_bonus += s.sequence_bonus;
        s.sequence_bonus = 0;
        s.shown_sequence.clear();
    }
    if (data.type)
        for (int k = 0; k < 2; ++k) {
            const float bonus = k == 0 ? s.line_bonus : s.finished_bonus;
            const auto want = s.game_line_total[k] + static_cast<std::int32_t>(std::lround(bonus));
            // Not written: "LINE:" and the end banner kept the game's own total with these written (2026-10-14: the
            // finished line set to 1087, the banner still said 112), and what else reads ActiveScoringData is not known.
            if (write_line_totals && bonus > 0 && totals[k] != want) {
                const auto field = context.path(data, {line_structs[k], line_total});
                context.publish(field, &want);
                logging::log(logging::Level::info, logging::Channel::skater, "Tricks: line total {} set to {} (the game's {}).", k, want, s.game_line_total[k]); // <dev>
            }
            s.shown_line_total[k] = bonus > 0 ? want : totals[k];
        }
    if (!active) return;
    // The HUD's own line floats follow the banked total.
    const float line = menu_data::read<float>(context.address(context.field(model, line_score)));
    if (line != s.shown_line) s.game_line = line;
    const float want_line = s.game_line + s.line_bonus;
    if (write_line_totals && s.line_bonus > 0 && line != want_line) {
        context.publish(context.field(model, line_score), &want_line);
        context.publish(context.field(model, line_score_twin), &want_line);
    }
    s.shown_line = s.line_bonus > 0 ? want_line : line;
    // The sequence in progress (the big number).
    const auto sequence_text = context.path(model, {sequence_label, label_text});
    const auto text = context.text(sequence_text);
    if (text != s.shown_sequence) {
        int value{};
        try { value = text.empty() ? 0 : std::stoi(text); } catch (...) { value = -1; }
        s.game_sequence = value;
    }
    if (s.sequence_bonus > 0 && s.game_sequence >= 0) {
        const auto shown = std::to_string(s.game_sequence + static_cast<int>(std::lround(s.sequence_bonus)));
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
    if (s.pending.empty() && s.owned.empty() && s.sequence_bonus <= 0 && s.line_bonus <= 0 && s.finished_bonus <= 0 && s.pending_points <= 0) return;
    // Every tick while an entry waits or our points are on the line (the game may write its numbers any frame);
    // otherwise only to see the game drop our entries, ten times a second.
    static ULONGLONG next{};
    const auto now = GetTickCount64();
    if (s.pending.empty() && s.sequence_bonus <= 0 && s.line_bonus <= 0 && s.finished_bonus <= 0 && s.pending_points <= 0 && now < next) return;
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
void set_line_total_writes(bool on) noexcept { write_line_totals.store(on); }
std::string trick_entries_status() {
    const auto &s = state();
    return std::format("trick list: {} added, {} dropped by the game, {} failed, {} live", s.added, s.dropped, s.failures, s.owned.size());
}
} // namespace dingosdk::tricks
