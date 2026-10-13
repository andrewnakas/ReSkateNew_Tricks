#include "tricks.h"
#include "Extension/Console/commands.h"

// The `tricks` console commands.
namespace dingosdk::console {
void register_tricks_commands(Commands &registry) {
    struct Verb {
        const char *name;
        const char *description;
        const char *usage;
    };
    const Verb verbs[]{
        {"status", "What ReSkate Tricks is doing", nullptr},
        {"freestyle", "Ride in a casper or primo (the game's own freestyle animations)", "casper|primo|primo2|off"},
        {"casperflip", "Flip out of a casper", "[FreeStyleFlipTrick value]"},
        {"state", "Hold any game state at a value (diagnostic)", "<name> b|i|f <value> | read <name> b|i | clear | list"},
        {"fstate", "Hold a game state while freestyle is on (diagnostic)", "<name> b|i|f <value>"},
        {"names", "Show the names of the mod's tricks as notices", "on|off"},
        {"air", "In-air extras: one more flip, late shuvit, dolphin turn", "on|off | sign 1|-1 | ms <length>"},
        {"spin", "Turn the board procedurally (diagnostic)", "<roll> <yaw> <pitch> <ms> | off | ms <length>"},
        {"ncflip", "No comply flips: arm one, or switch them and the experimental clips", "<trick>|on|off|stock|experimental"},
        {"catch", "Turn the board to the pose in the air when a casper or primo is caught", "on|off | drop <metres>"},
        {"upright", "How firmly a slide holds the skater upright", "[0 to 1]"},
        {"airpose", "Start a caught pose's animation in the air", "on|off | last on|off | states <state[=value]>..."},
        {"keep", "Refuse the game's own dismount while a pose is held", "on|off"},
        {"soft", "Hold a landed pose's parts level for a moment from the touchdown", "<ms>"},
        {"rigid", "Hold a posed board to its pose with this drive strength (the game's own is 1)", "<0 to 1000>"},
        {"level2", "Keep a held pose level and on one heading", "on|off | delay <ms>"},
        {"norunout", "Stand the game's wipeout and run-out checks down while a pose is held", "on|off"},
        {"snap", "Start the casper and primo way-in clips part of the way in", "<casper tick> <primo tick>"},
        {"stomp", "Land a caught pose straight onto the ground, with no bounce", "on|off"},
        {"leftmask", "Hide the left stick from the game while a pose is held", "on|off | air on|off"},
        {"stable", "Keep a held casper or primo from tipping", "on|off"},
        {"primos", "One primo for both sides of the stick, or primo and primo2", "one|two"},
        {"protect", "No wipeout while a casper or primo is caught, landed or held", "on|off"},
        {"early", "Leave the landing early into a caught pose (the intent held for it)", "on|off|<bool state>"},
        {"route", "Go into a rolling casper or primo2 through the primo", "on|off"},
        {"zones", "Where the right stick's casper place ends and the primos' begin (degrees, 270 is down)", "<left> <right>"},
        {"trace", "Record the controller and the skater's state to logs/tricks-trace.csv", "on|off"},
        {"fall", "What losing a casper or primo does", "hop|slam"},
        {"edgefall", "Lose a pose when the right stick is held at its edge", "on|off"},
        {"scheme", "Freestyle controls: landed into and held on the right stick, or click + flick", "hold|flick"},
        {"groundshuv", "Ground shuvit (no pop): do one, or switch the control", "[bs|fs] | on|off | ms <length>"},
        {"level", "Keep the board from pitching up in a casper or primo slide", "on|off"},
        {"slide","How hard casper and primo slides scrub speed (m/s each second)", "[m/s]"},
        {"controls","Left stick click + right stick freestyle controls", "on|off"},
        {"pad", "Scripted controller for test runs", "<steps>|mount|off"},
        {"mode", "How the freestyle states are driven (diagnostic)", "set|hold"},
        {"search","Search memory for the freestyle states again (diagnostic)", nullptr},
    };
    for (const auto &verb : verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto rest = argument(verb.usage, Type::text, true);
            rest.rest = true;
            arguments.push_back(std::move(rest));
        }
        auto entry = action(std::string("tricks ") + verb.name, verb.description, Group::gameplay, std::move(arguments));
        entry.run = [name = std::string(verb.name)](const Model &, const Values &values, const Output &out) {
            std::vector<std::string> words;
            if (!values.empty())
                if (const auto *text = std::get_if<std::string>(&values[0])) {
                    std::size_t start = 0;
                    while (start < text->size()) {
                        auto end = text->find(' ', start);
                        if (end == std::string::npos) end = text->size();
                        if (end > start) words.push_back(text->substr(start, end - start));
                        start = end + 1;
                    }
                }
            out(tricks::command(name, words));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
