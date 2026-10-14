#include "tricks_view.h"
#include "Extension/UI/Overlay/skate_menu_internal.h"
#include <array>
#include <format>
#include <string>

// The TRICKS page of the Insert menu: every trick the mod adds, switched on or off one by one or a family at a time,
// and the settings for their controls and animation. Presentation thread: reads the tricks snapshot and queues
// `tricks ...` console commands; it never touches the game.
namespace dingosdk::overlay::menu {
// tricks_howto.cpp
void tricks_howto(SkateMenu &menu, const tricks::View &view);
namespace {
struct Page {
    int tab{};
    // The slider under the pointer: its value is held while it is dragged (the snapshot lags a frame or two).
    int editing{-1};
    float edit_value{};
};
Page &page() {
    static auto *value = new Page;
    return *value;
}
void tricks_command(SkateMenu &menu, const CallbacksV3 &callbacks, const std::string &text) { send_console(menu, callbacks, "tricks " + text); }

void tricks_tab(SkateMenu &menu, const CallbacksV3 &callbacks, const tricks::View &view) {
    begin_card(menu, "tricks-general", "SCORING");
    bool score = view.score;
    if (toggle_row(menu, "Score the mod's tricks", "Each trick shows its name and points when it is done", score))
        tricks_command(menu, callbacks, std::format("score {}", score ? "on" : "off"));
    bool names = view.names;
    if (toggle_row(menu, "Trick name notices", "The trick's name in a ReSkate notice", names)) tricks_command(menu, callbacks, std::format("names {}", names ? "on" : "off"));
    end_card();
    // Each family a section that folds away, its own switch first; each trick a switch and a line that opens to how it
    // is done and what it scores.
    for (std::size_t f = 0; f < tricks::family_count; ++f) {
        const auto &family = tricks::family_catalog[f];
        ImGui::PushID(static_cast<int>(f));
        const std::string header = std::format("{}{}###family", family.name, view.family[f] ? "" : "  (off)");
        if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            begin_card(menu, family.id, nullptr, family.hint);
            bool family_on = view.family[f];
            if (toggle_row(menu, "Whole family", "Switches every trick below on or off at once", family_on))
                tricks_command(menu, callbacks, std::format("family {} {}", family.id, family_on ? "on" : "off"));
            for (std::size_t i = 0; i < tricks::trick_count; ++i) {
                const auto &trick = tricks::trick_catalog[i];
                if (static_cast<std::size_t>(trick.family) != f) continue;
                ImGui::PushID(static_cast<int>(i));
                bool on = view.trick[i];
                ImGui::BeginDisabled(!view.family[f]);
                if (ImGui::Checkbox("##on", &on)) tricks_command(menu, callbacks, std::format("enable {} {}", trick.id, on ? "on" : "off"));
                ImGui::EndDisabled();
                ImGui::SameLine();
                const std::string label = trick.points > 0 ? std::format("{}  ({} pts)###trick", trick.name, trick.points) : std::format("{}###trick", trick.name);
                if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextDisabled("%s", trick.hint);
                    ImGui::PopTextWrapPos();
                    if (trick.points > 0) ImGui::Text("Worth %d points.", trick.points);
                    ImGui::TextDisabled("Full steps and a pad diagram: the HOW TO tab.");
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            end_card();
        }
        ImGui::PopID();
    }
}

void settings_tab(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const tricks::View &view) {
    begin_card(menu, "pose-options", "POSE OPTIONS");
    bool height = view.height;
    if (toggle_row(menu, "Casper height on the right stick", "How far the stick is out picks the casper's low, medium or high loop", height))
        tricks_command(menu, callbacks, std::format("height {}", height ? "on" : "off"));
    bool turn = view.anti_turn;
    if (toggle_row(menu, "Anti casper turned to its own side", "Off: the mirrored casper alone (the casper seen from the board's other side)", turn))
        tricks_command(menu, callbacks, std::format("anti turn {}", turn ? "on" : "off"));
    bool two = view.two_primos;
    if (toggle_row(menu, "Separate primo for each side", "Off: both sides of the stick are the same primo (its flick-outs pop more reliably)", two))
        tricks_command(menu, callbacks, std::format("primos {}", two ? "two" : "one"));
    end_card();
    for (std::size_t f = 0; f < tricks::family_count; ++f) {
        bool any = false;
        for (const auto &s : tricks::setting_catalog) any = any || static_cast<std::size_t>(s.family) == f;
        if (!any) continue;
        ImGui::PushID(static_cast<int>(f) + 100);
        if (!ImGui::CollapsingHeader(tricks::family_catalog[f].name, ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PopID();
            continue;
        }
        begin_card(menu, tricks::family_catalog[f].id, nullptr);
        for (std::size_t i = 0; i < tricks::setting_count; ++i) {
            const auto &s = tricks::setting_catalog[i];
            if (static_cast<std::size_t>(s.family) != f) continue;
            field(menu, s.label, s.hint);
            const int index = static_cast<int>(i);
            float value = p.editing == index ? p.edit_value : view.setting[i];
            const std::string id = std::format("##{}", s.key);
            ImGui::SliderFloat(id.c_str(), &value, s.low, s.high, s.format, ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemActive()) {
                p.editing = index;
                p.edit_value = value;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                tricks_command(menu, callbacks, std::format("set {} {:.4g}", s.key, value));
                p.editing = -1;
            } else if (!ImGui::IsItemActive() && p.editing == index) p.editing = -1;
        }
        end_card();
        ImGui::PopID();
    }
    note("Every change is kept in %LOCALAPPDATA%\\ReSkate\\tricks\\tricks.json.");
}
} // namespace

void tricks_page(SkateMenu &menu, const Model &, const CallbacksV3 &callbacks) {
    auto &p = page();
    const auto view = tricks::view();
    if (!view->ready) {
        note("The tricks settings load when a level does.");
        return;
    }
    category_tabs(menu, p.tab, {"TRICKS", "CONTROLS & ANIMATION", "HOW TO"}, "tricks-tabs");
    ImGui::PushID(p.tab);
    ImGui::BeginChild("tricks-tab", ImVec2(0, page_body_height(menu)));
    ImGui::BeginDisabled(!callbacks.queue_console_command);
    if (p.tab == 0) tricks_tab(menu, callbacks, *view);
    else if (p.tab == 1) settings_tab(menu, callbacks, p, *view);
    else tricks_howto(menu, *view);
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::PopID();
}
} // namespace dingosdk::overlay::menu
