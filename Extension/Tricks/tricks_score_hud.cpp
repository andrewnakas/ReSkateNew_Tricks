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

void run(const menu_data::Context &context) {
    auto &s = state();
    if (s.manager != context.manager) { // a new registry: what we made went with the old one
        s.manager = context.manager;
        s.owned.clear();
        s.template_model = {};
    }
    const auto roots = context.roots({corner::score_view_schema});
    if (roots.empty()) return;
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
    } catch (...) {}
}
void update_trick_entries(std::uintptr_t base) noexcept {
    auto &s = state();
    if (s.pending.empty() && s.owned.empty()) return;
    // Every tick while an entry waits; otherwise only to see the game drop ours, ten times a second.
    static ULONGLONG next{};
    const auto now = GetTickCount64();
    if (s.pending.empty() && now < next) return;
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
std::string trick_entries_status() {
    const auto &s = state();
    return std::format("trick list: {} added, {} dropped by the game, {} failed, {} live", s.added, s.dropped, s.failures, s.owned.size());
}
} // namespace dingosdk::tricks
