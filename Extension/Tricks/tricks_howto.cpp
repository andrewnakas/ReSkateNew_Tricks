#include "tricks_view.h"
#include "Extension/UI/Overlay/skate_menu_internal.h"
#include "Extension/UI/skate_theme.h"
#include <cmath>
#include <format>
#include <string>
#include <vector>

// The HOW TO tab of the Tricks page: a pad diagram and the steps for every trick and every variation. Presentation
// thread only. Stick directions are as seen on the pad: x right, y up; the right stick's angle 0 is right, 90 up.
namespace dingosdk::overlay::menu {
namespace {
enum class Click { none, held, tap };
struct Panel {
    const char *caption;   // under the panel
    Click l3{Click::none};
    std::vector<ImVec2> path; // the right stick's way, from the first point to the last
    bool rest{};              // the last point is held there (a filled dot); otherwise it springs back
    float wedge_from{}, wedge_to{}; // degrees: the place on the right stick that counts (none when equal)
    std::vector<ImVec2> left{};     // the left stick's way, if it moves
};
struct Guide {
    const char *trick;  // tricks_view.h id (nullptr: a rule that applies to several)
    const char *title;
    const char *steps;
    std::vector<Panel> panels;
};
ImVec2 at(float degrees, float length = 1.0f) {
    const float r = degrees * 3.14159265f / 180.0f;
    return {std::cos(r) * length, std::sin(r) * length};
}
constexpr ImVec2 centre{0, 0};

const std::vector<Guide> &guides(tricks::Family family) {
    using F = tricks::Family;
    static const std::vector<Guide> poses{
        {"casper", "Casper (caught in the air)",
         "1. Pop any flip trick the usual way (flick it).\n"
         "2. Hold L3 (left stick click) from the pop on, and keep holding it.\n"
         "3. Sweep the right stick straight from the flick down to the bottom of its rim and leave it there through the landing (any angle in the shaded place, 245 to 307 degrees).\n"
         "4. The board turns upside down in the air and you land standing on its tail. Keep L3 and the stick where they are to stay in it.",
         {{"Pop a flip trick", Click::none, {at(270, 0.9f), at(90, 1)}, false},
          {"L3 held + stick down", Click::held, {centre, at(275)}, true, 245, 307}}},
        {"primo", "Primo (caught in the air)",
         "1. Pop any flip trick.\n"
         "2. Hold L3 from the pop on.\n"
         "3. Sweep the right stick to the lower left (150 to 245 degrees) or the lower right (307 to 40 degrees) and keep it there through the landing.\n"
         "4. You land on the board's edge. Both sides give the same primo unless 'Separate primo for each side' is on.",
         {{"L3 held + stick to the lower right", Click::held, {centre, at(330)}, true, 307, 400},
          {"... or the lower left", Click::held, {centre, at(215)}, true, 150, 245}}},
        {"anticasper", "Anti Casper (caught in the air)",
         "1. Pop any flip trick.\n"
         "2. Hold L3 from the pop on.\n"
         "3. Leave the right stick at the top of its rim (40 to 150 degrees) for a moment (0.15 s: a flick only passes through) and keep it there through the landing.\n"
         "4. The casper's other end: the front foot stands on the nose, which is on the ground, the back foot hooks under the raised tail.",
         {{"L3 held + stick resting up", Click::held, {centre, at(95)}, true, 40, 150}}},
        {"manual_entry", "Casper or primo from a manual",
         "1. Roll into a manual: right stick part of the way down.\n"
         "2. Push the stick all the way down: the tail drags (the manual brake).\n"
         "3. Click L3 with the stick still down: casper. Or click L3 and move the stick to the lower left or right: primo.\n"
         "Within 0.4 s of the tail dragging.",
         {{"Manual", Click::none, {centre, at(270, 0.5f)}, true},
          {"All the way down", Click::none, {at(270, 0.5f), at(270)}, true},
          {"L3: casper (or slide to a side: primo)", Click::held, {at(270), at(272)}, true, 245, 307}}},
        {"manual_entry", "Anti Casper from a nose manual",
         "1. Roll into a nose manual: right stick part of the way up.\n"
         "2. Push the stick all the way up: the nose drags.\n"
         "3. Click L3 with the stick still up: anti casper.",
         {{"Nose manual", Click::none, {centre, at(90, 0.5f)}, true},
          {"All the way up", Click::none, {at(90, 0.5f), at(90)}, true},
          {"L3: anti casper", Click::held, {at(90), at(92)}, true, 40, 150}}},
        {"casper", "Height (casper and anti casper)",
         "While holding the pose, how far the right stick is out sets the height: at the rim the low pose, eased back toward half way the high one.\n"
         "Don't let it reach the middle: that drops the casper (see Leaving a pose).",
         {{"Rim: low", Click::held, {centre, at(275)}, true, 245, 307}, {"Half out: high", Click::held, {at(275), at(275, 0.5f)}, true, 245, 307}}},
        {"casperflip", "Leaving a pose",
         "Let go of L3: the board flips back over (Casper Flip) and you ride away. This is the clean way out.\n"
         "Casper or anti casper: the right stick back at the middle for a quarter second is a fall (you hop off).\n"
         "Primo: the right stick back at the middle puts the board down and you ride away.\n"
         "Switch pose: rest the stick in another pose's place for a quarter second (casper <-> primo).",
         {{"Let go of L3", Click::none, {at(275), at(275)}, true, 245, 307},
          {"Stick to the middle: fall (casper)", Click::held, {at(275), centre}, false}}},
    };
    static const std::vector<Guide> flickouts{
        {"flick_kickflip", "Kickflip (out of a pose)",
         "From a held casper (stick down), swipe the right stick quickly up and to the left and let it spring back. The board pops out of the pose with a kickflip; land it, catch it again, or dark catch it.\n"
         "From an anti casper (stick up) swipe down-left instead. From a primo, swipe up-left.",
         {{"Casper: swipe up-left", Click::held, {at(275), at(130)}, false}, {"Anti casper: swipe down-left", Click::held, {at(95), at(230)}, false}}},
        {"flick_heelflip", "Heelflip (out of a pose)",
         "Swipe up and to the right from a casper (down-right from an anti casper, up-right from a primo).",
         {{"Casper: swipe up-right", Click::held, {at(275), at(50)}, false}, {"Anti casper: swipe down-right", Click::held, {at(95), at(310)}, false}}},
        {"flick_treflip", "Tre Flip (out of a pose)",
         "Swipe straight up from a casper (straight down from an anti casper, straight up from a primo).",
         {{"Casper: swipe up", Click::held, {at(275), at(90)}, false}, {"Anti casper: swipe down", Click::held, {at(95), at(270)}, false}}},
        {"flick_shuvit", "Pop Shuvit (out of a pose)",
         "From a casper or anti casper, a quick swipe to the left rim (under 0.16 s from the held place). Resting there instead changes over to the primo.",
         {{"Quick swipe to the left rim", Click::held, {at(275), at(180)}, false}}},
        {"flick_fsshuvit", "FS Pop Shuvit (out of a pose)",
         "From a casper or anti casper, a quick swipe to the right rim.",
         {{"Quick swipe to the right rim", Click::held, {at(275), at(0)}, false}}},
    };
    static const std::vector<Guide> ground{
        {"groundshuv", "Ground Shuvit (no pop)",
         "1. Riding, hold L3.\n"
         "2. Push the right stick down to its rim.\n"
         "3. Roll it along the rim to a lower diagonal: lower right for frontside, lower left for backside.\n"
         "4. Let go of the stick within a quarter second: the board turns half round under your feet without leaving the ground.",
         {{"L3 + down, roll right, let go: FS", Click::held, {at(270), at(300), at(320), centre}, false},
          {"... roll left: BS", Click::held, {at(270), at(240), at(220), centre}, false}}},
    };
    static const std::vector<Guide> nocomply{
        {"nc_kickflip", "No Comply Kickflip",
         "1. Tap A quickly: the no comply instead of an ollie.\n"
         "2. Pop it with the kickflip flick: right stick down, then up and to the left.\n"
         "The flick that pops the no comply picks the flip; a plain flick (down, then straight up) is the game's own no comply. Works the same from a boneless or a footplant.",
         {{"Tap A", Click::none, {}, false}, {"Down, then up-left", Click::none, {at(270), centre, at(135)}, false}}},
        {"nc_heelflip", "No Comply Heelflip",
         "Tap A, then pop it with the heelflip flick: right stick down, then up and to the right.",
         {{"Tap A", Click::none, {}, false}, {"Down, then up-right", Click::none, {at(270), centre, at(45)}, false}}},
        {"nc_shuvit", "No Comply Shuvit",
         "Tap A, then pop it with the pop shuvit flick: right stick down, then rolled along the rim to the left.",
         {{"Tap A", Click::none, {}, false}, {"Down, rolled to the left rim", Click::none, {at(270), at(235), at(200)}, false}}},
        {"nc_fsshuvit", "No Comply FS Shuvit",
         "Tap A, then pop it with the FS pop shuvit flick: right stick down, then rolled along the rim to the right.",
         {{"Tap A", Click::none, {}, false}, {"Down, rolled to the right rim", Click::none, {at(270), at(305), at(340)}, false}}},
    };
    static const std::vector<Guide> flickit{
        {"dolphin", "Dolphin Flip (forward flip)",
         "Riding, flick the right stick down, then up (the pop), then straight back down through the middle to the bottom rim, all in one quick stroke. "
         "The board does a forward varial: one front flip, nose first, while it turns half way round.",
         {{"Down", Click::none, {centre, at(270)}, false}, {"Up (pop)", Click::none, {at(270), at(90)}, false}, {"Straight back down", Click::none, {at(90), at(270)}, false}}},
        {"dragon", "Dragon Flip",
         "The dolphin's stroke, but the last part goes to a lower diagonal (lower left or lower right): a forward 360, one front flip while the board turns all the way round toward that side.",
         {{"Down", Click::none, {centre, at(270)}, false}, {"Up (pop)", Click::none, {at(270), at(90)}, false}, {"Down to a lower diagonal", Click::none, {at(90), at(230)}, false}}},
    };
    static const std::vector<Guide> air{
        {"air_extraflip", "Extra flip (double flip, nightmare, daydream)",
         "In the air during a flip trick, tap L3 (press and let go within 0.2 s) with the right stick at rest: one more flip the way the trick flips. On a varial kickflip that makes a nightmare flip, on a varial heelflip a daydream flip. Two extras per air at most.",
         {{"Tap L3, stick at rest", Click::tap, {}, false}}},
        {"air_lateshuv", "Late Shuvit",
         "In the air, hold the right stick left or right and tap L3: the board turns half round.",
         {{"Stick left + tap L3", Click::tap, {centre, at(180)}, true}, {"Stick right + tap L3", Click::tap, {centre, at(0)}, true}}},
    };
    static const std::vector<Guide> pregrab{
        {"pregrab", "Nose / Tail Grab (pre-grab)",
         "1. Riding, hold a trigger: RT grabs with the right hand, LT with the left (the trigger alone is the game's own grab, mid-board).\n"
         "2. Push the left stick up for the nose (the end ahead), or down for the tail, for a moment.\n"
         "3. The hand moves to that end. Keep the trigger held; let go of it to stand back up. The left stick can go back to the middle.",
         {{"Trigger + left stick up: nose", Click::none, {}, false, 0, 0, {centre, at(90)}},
          {"... left stick down: tail", Click::none, {}, false, 0, 0, {centre, at(270)}}}},
        {"pregrab_ollie", "Nose / Tail Grab Ollie",
         "From a pre-grab, trigger still held, flick an ollie (right stick down, then up) or a nollie (up, then down). The pop is small and the hand stays on: "
         "in the air it is a nose or tail grab, and you land back in the pre-grab while the trigger is held.",
         {{"Ollie: down, then up", Click::none, {at(270), at(90)}, false}, {"Nollie: up, then down", Click::none, {at(90), at(270)}, false}}},
        {"fingerflip", "Fingerflip",
         "From a pre-grab, flick a kickflip (down, then up to the kick's diagonal), a heelflip (the other diagonal) or a shuvit (down, then to a side). "
         "A small pop, and the hand throws the board in that flip; the feet catch it.",
         {{"Down, then a diagonal", Click::none, {at(270), centre, at(135)}, false}, {"Down, then a side", Click::none, {at(270), at(235), at(180)}, false}}},
    };
    switch (family) {
    case F::pregrab: return pregrab;
    case F::poses: return poses;
    case F::flickouts: return flickouts;
    case F::ground: return ground;
    case F::nocomply: return nocomply;
    case F::flickit: return flickit;
    default: return air;
    }
}

void arrow(ImDrawList *draw, ImVec2 from, ImVec2 to, ImU32 colour, float thickness) {
    draw->AddLine(from, to, colour, thickness);
    const float dx = to.x - from.x, dy = to.y - from.y, length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0f) return;
    const float ux = dx / length, uy = dy / length, size = px(9);
    draw->AddTriangleFilled(to, ImVec2(to.x - ux * size - uy * size * 0.55f, to.y - uy * size + ux * size * 0.55f),
                            ImVec2(to.x - ux * size + uy * size * 0.55f, to.y - uy * size - ux * size * 0.55f), colour);
}
// One panel: the left stick (with L3) and the right stick (with its way and the place that counts).
void draw_panel(const Panel &panel) {
    const float width = px(250), height = px(150), radius = px(42);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), skate_theme::tile, px(8));
    const ImVec2 left(origin.x + px(62), origin.y + px(62)), right(origin.x + px(188), origin.y + px(62));
    const auto point = [&](ImVec2 c, ImVec2 v) { return ImVec2(c.x + v.x * radius, c.y - v.y * radius); };
    // Left stick and its click.
    draw->AddCircleFilled(left, radius, skate_theme::tile_light, 40);
    draw->AddCircle(left, radius, skate_theme::grey_text, 40, px(1.5f));
    if (panel.l3 != Click::none) {
        draw->AddCircleFilled(left, radius * 0.42f, panel.l3 == Click::held ? skate_theme::bar : skate_theme::blue, 32);
        const char *label = panel.l3 == Click::held ? "HOLD" : "TAP";
        const auto size = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(left.x - size.x * 0.5f, left.y - size.y * 0.5f), skate_theme::black, label);
    } else draw->AddCircle(left, radius * 0.42f, skate_theme::grey_text, 32, px(1));
    draw->AddText(ImVec2(left.x - px(8), left.y + radius + px(4)), skate_theme::grey_text, "L3");
    for (std::size_t i = 1; i < panel.left.size(); ++i) {
        const auto a = point(left, panel.left[i - 1]), b = point(left, panel.left[i]);
        if (i + 1 == panel.left.size()) arrow(draw, a, b, skate_theme::official_text, px(3));
        else draw->AddLine(a, b, skate_theme::official_text, px(3));
    }
    // Right stick: the place that counts, then the way.
    draw->AddCircleFilled(right, radius, skate_theme::tile_light, 40);
    if (panel.wedge_to != panel.wedge_from) {
        const float a0 = -panel.wedge_to * 3.14159265f / 180.0f, a1 = -panel.wedge_from * 3.14159265f / 180.0f;
        draw->PathLineTo(right);
        draw->PathArcTo(right, radius, a0, a1, 24);
        draw->PathFillConvex(IM_COL32(255, 177, 11, 70));
    }
    draw->AddCircle(right, radius, skate_theme::grey_text, 40, px(1.5f));
    draw->AddText(ImVec2(right.x - px(8), right.y + radius + px(4)), skate_theme::grey_text, "R");
    if (!panel.path.empty()) {
        for (std::size_t i = 1; i < panel.path.size(); ++i) {
            const auto a = point(right, panel.path[i - 1]), b = point(right, panel.path[i]);
            if (i + 1 == panel.path.size()) arrow(draw, a, b, skate_theme::white, px(3));
            else draw->AddLine(a, b, skate_theme::white, px(3));
        }
        draw->AddCircleFilled(point(right, panel.path.front()), px(4), skate_theme::grey_text);
        const auto end = point(right, panel.path.back());
        if (panel.rest) draw->AddCircleFilled(end, px(7), skate_theme::bar);
    }
    draw->AddText(ImVec2(origin.x + px(8), origin.y + height - px(22)), skate_theme::white, panel.caption);
}
} // namespace

void tricks_howto(SkateMenu &menu, const tricks::View &view) {
    note("Every trick and its variations: the left panel of each pair is the left stick (L3: HOLD = keep it clicked, TAP = click and let go), "
         "the right one the right stick. White arrows are the stick's way, a yellow dot is where it rests, the shaded slice is the place that counts.");
    for (std::size_t f = 0; f < tricks::family_count; ++f) {
        const auto family = static_cast<tricks::Family>(f);
        ImGui::PushID(static_cast<int>(f) + 500);
        const bool family_open = ImGui::CollapsingHeader(tricks::family_catalog[f].name, ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopID();
        if (!family_open) continue;
        int n = 0;
        for (const auto &guide : guides(family)) {
            ImGui::PushID(static_cast<int>(f * 100) + n++);
            const int index = guide.trick ? tricks::trick_index(guide.trick) : -1;
            const bool off = index >= 0 && !(view.trick[index] && view.family[f]);
            ImGui::Indent(px(10));
            const std::string header = off ? std::format("{}  (switched off)", guide.title) : std::string(guide.title);
            if (!ImGui::CollapsingHeader(header.c_str())) {
                ImGui::Unindent(px(10));
                ImGui::PopID();
                continue;
            }
            begin_card(menu, "howto", guide.title, off ? "Switched off on the TRICKS tab" : nullptr);
            const float room = ImGui::GetContentRegionAvail().x;
            float used = 0;
            for (std::size_t i = 0; i < guide.panels.size(); ++i) {
                if (i > 0 && used + px(258) <= room) ImGui::SameLine(0, px(8));
                else if (i > 0) used = 0;
                draw_panel(guide.panels[i]);
                used += px(258);
            }
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(guide.steps);
            ImGui::PopTextWrapPos();
            if (index >= 0 && tricks::trick_catalog[index].points > 0) tag(menu, std::format("{} pts", tricks::trick_catalog[index].points).c_str(), skate_theme::bar);
            end_card();
            ImGui::Unindent(px(10));
            ImGui::PopID();
        }
    }
}
} // namespace dingosdk::overlay::menu
