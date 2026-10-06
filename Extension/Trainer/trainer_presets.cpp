#include "trainer_presets.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <span>
#include <string_view>
#include <utility>

namespace dingosdk::trainer {
bool value_used(std::uint16_t offset) {
    static constexpr std::uint16_t used[]{
#include "trainer_used.inc"
    };
    return std::binary_search(std::begin(used), std::end(used), offset);
}
// The game ignores these named values; the graphs on the right are what it reads. Linking them
// lets "ollie height" and "spin speed" be plain numbers instead of graph multipliers.
const std::vector<Link> &value_links() {
    static const std::vector<Link> links{
        {"physicsmode.jumpmaxheight", "physicsjump.maxheightvsspeed x"},
        {"physicsmode.jumpminheight", "physicsjump.minheightvsspeed x"},
        {"physicsairstates.maxspinspeed", "physicsbodyspin.propbodyspinvstime x"},
        {"physicsairstates.maxspinspeed", "physicsbodyspin.pbsvst_easy x"},
        {"physicsairstates.maxspinspeed", "physicsbodyspin.maxdeltavstime x"},
        {"physicsairstates.maxspinspeed", "physicsbodyspin.maxdeltavstimeeasy x"},
        // Pushing: the tuning's top pushing speed only gates whether a push may start. The speeds
        // pushes aim for are the push class's (trainer_classes.h), scaled here by the same ratio.
        {"physicspush.maxpushablespeed", "push.maxpushspeedlight"},
        {"physicspush.maxpushablespeed", "push.maxpushspeedmedium"},
        {"physicspush.maxpushablespeed", "push.maxpushspeedstrong"},
        {"physicspush.maxpushablespeed", "push.minmediumpushoverridespeed"},
    };
    return links;
}
// The two short lists of the Tune tab. Only values that do something: read by the game, linked
// above, or a field of one of the game's data-defined classes.
std::string_view essential_name(std::string_view key, int *rank, std::uint8_t *modes) {
    struct Name {
        std::string_view key, label;
        std::uint8_t modes;
    };
    constexpr std::uint8_t r = mode_realistic, f = mode_fun, both = mode_realistic | mode_fun, t = mode_trial;
    static constexpr Name names[]{
        {"physicsmode.jumpmaxheight", "Ollie height (max)", both},
        {"physicsmode.jumpminheight", "Ollie height (min, light pop)", both},
        {"physicsmode.grindjumpcommonmax", "Pop out of grinds (max)", both},
        {"physicsmode.grindjumpcommonmin", "Pop out of grinds (min, a quick pop)", both},
        {"physicsmode.grindjumpboardslidemax", "Pop out of board slides (max)", both},
        {"physicsmode.grindjumpboardslidemin", "Pop out of board slides (min, a quick pop)", both},
        {"physicsmode.grindjumptipslidemax", "Pop out of nose and tail slides (max)", both},
        {"physicsmode.grindjumptipslidemin", "Pop out of nose and tail slides (min, a quick pop)", both},
        {"physicsmode.jumpminheightmanual", "Pop out of a manual (min)", both | t},
        {"physicsmode.jumpdeltaminheightmanual", "Pop out of a manual: extra for a held pop", both | t},
        {"physicsonboardjumptuning.jumpadjustvelocityboostlimitground", "Ollie: speed the stick can add (m/s)", both},
        {"physicsonboardjumptuning.jumpadjustvelocityboostlimitgrind", "Pop out of a grind: speed the stick can add (m/s)", both},
        {"physicstrajectory.grindlandingvelscalar", "Grinds: speed kept landing on one", r},
        {"onboard_speedmodel.gravityacceleration", "On board: gravity (9.81 m/s2)", both},
        {"onboard_speedmodel.maxgravityacceleration", "On board: gravity limit (m/s2)", both},
        {"speedmodel.maxpositiveacceleration", "On board: fastest speeding up (m/s2)", f},
        {"speedmodel.maxnegativeacceleration", "On board: fastest slowing down (m/s2)", f},
        {"onboard_speedmodel.noinputtime", "On board: seconds of rolling before friction grows", r},
        {"physicsairstates.mountrunlaunchz", "Get on board from a jog: forward speed (m/s)", both | t},
        {"physicsairstates.mountrunlaunchy", "Get on board from a jog: hop (m/s)", r | t},
        {"physicsairstates.mountsprintlaunchz", "Get on board from a sprint: forward speed (m/s)", both | t},
        {"physicsairstates.mountsprintlaunchy", "Get on board from a sprint: hop (m/s)", r | t},
        {"physicsairstates.minbodyfliptimevsupy x", "Body flips: air time needed (x)", both | t},
        {"physicsairstates.bodyflipmingrabtimefraction", "Body flips: share of the air time held in a grab", both | t},
        {"physicsfootplant.leglengthonlanding", "Landing: leg length (lower crouches deeper)", both | t},
        {"physicsfeet.maxlandingforce", "Landing: leg force limit", r | t},
        {"physicsfeet.landingforcetime", "Landing: time the legs push back (s)", r | t},
        {"physicsmode.wipeoutbadlandingscalar", "Bail on bad landings: strictness", both},
        {"physicsfootplant.wipeoutmaxlandingspeeddown", "Bail: landing speed limit, downward (m/s)", both},
        {"physicsfootplant.wipeoutmaxlandingspeedhoriz", "Bail: landing speed limit, sideways (m/s)", both},
        {"onboard_powerslide.frictionscalar_revert", "Revert: friction", both},
        {"onboard_powerslide.frictionscalar_autorevert", "Auto revert: friction", both},
        {"onboard_powerslide.powerslide_forwardforcescalar_revert", "Revert: forward force", both},
        {"onboard_powerslide.powerslide_forwardforcescalar_autorevert", "Auto revert: forward force", both},
        {"physicsmode.pumpeffectfactor", "Pumping: strength", both},
        {"physicsmode.pumpmaxacceleration", "Pumping: fastest speed gain (m/s2)", both},
        {"physicsmode.unintentionalpumpscalar", "Pumping: speed gained without pumping", r},
        {"physicsjump.jumpybonusmax", "Jump bonus (max)", f},
        {"physicspush.maxpushablespeed", "Push speed (9.25 = the game's; scales every push)", both},
        {"push.maxpushspeedlight", "Push: a tapped push settles at (m/s)", r},
        {"push.maxpushspeedmedium", "Push: a held push reaches (m/s)", r},
        {"push.maxpushspeedstrong", "Push: top speed (m/s)", r},
        {"physicsmode.autopushenabled", "Auto push (keeps a rolling skater going)", f},
        {"physicspush.maxspeedforautopush", "Auto push speed (m/s)", f},
        {"physicsreckoning.flipscalar", "Body flip speed", both},
        {"physicsreckoning.flipmaxspeed", "Body flip speed limit", both},
        {"physicsmode.perfectbodyflips", "Perfect body flips (exactly one rotation)", f},
        {"physicsairstates.maxspinspeed", "Body spin speed", both},
        {"physicsmode.maxautobodyspinspeed", "Auto body spin speed", both},
        {"physicsmode.easybodyspins", "Easy body spins", f},
        {"heldflip.straightflipcatchtime", "Flip catch time: kickflips and heelflips (s)", r},
        {"heldflip.shuvmincatchtime", "Flip catch time: shuvits (s)", r},
        {"heldflip.bigflipmincatchtime", "Flip catch time: big flips (s)", r},
        {"physicsmode.speedwobblestartspeed", "Speed wobble starts at (m/s)", both},
        {"physicsmode.grindlockdist", "Grind lock-on distance", both},
        {"physicsgrind.commonfrictionscalar", "Grind friction", both},
        {"physicsmode.makesurfacessmooth", "Smooth surfaces", f},
        {"onboard_powerslide.powerslide_forwardforcescalar_slide", "Powerslide: forward force", f},
        {"onboard_powerslide.frictionscalar_slide", "Powerslide: friction", both},
        {"physicsmode.wipeoutcheckforbadlanding", "Bail on bad landings", both},
        {"physicsmode.wipeout_groundxzacceleration", "Bail: sideways hit limit", both},
        {"wipeoutfallspeed.normalmaxspeed_ground", "Bail: fall speed limit on the ground (m/s)", r},
        {"wipeoutfallspeed.normalmaxspeed_grind", "Bail: fall speed limit in a grind (m/s)", r},
        {"jump.basejumpheight", "On foot: jump height (m)", both},
        {"jump.maxjumpheight", "On foot: highest jump (m)", both},
        {"jump.maxjumpvelocity", "On foot: jump speed limit (m/s)", f},
        {"jump.aircontrolforce", "On foot: steering in the air", f},
        {"locomotion.sprintspeed", "On foot: sprint speed (m/s)", both},
        {"locomotion.sprintspeedboost", "On foot: sprint boost speed (m/s)", f},
        {"flumping.flumptuckmaxrotationvelocity", "On foot: flip rotation speed", both},
        {"flumping.rollmaxvelocity", "On foot: roll speed (m/s)", f},
        {"flumping.rollvelocitymultiplier", "On foot: roll speed gain", f},
        {"wipeout.spreadeaglegravity", "Glide: gravity (-8; nearer 0 falls slower)", f},
        {"wipeout.spreadeagleairresistance", "Glide: air resistance", f},
        {"wipeout.spreadeagleaircontrolmax", "Glide: steering", f},
        {"wipeout.torpedoaircontrolmax", "Torpedo: steering (fast)", f},
        {"wipeout.defaultaircontrol", "Falling: steering", f},
        {"wipeout.defaultairresistance", "Falling: air resistance", f},
        {"wallrun.wallrunjumpupvelocityboost", "Wallrun: jump up boost", f},
        {"wallrun.wallrungravity", "Wallrun: gravity", f},
        {"physicstrucks.truckzposfront", "Front truck position (next respawn)", r},
        {"physicstrucks.truckzposback", "Back truck position (next respawn)", r},
    };
    const auto found = std::ranges::find(names, key, &Name::key);
    const bool known = found != std::end(names);
    if (rank) *rank = known ? static_cast<int>(found - std::begin(names)) + 1 : 0;
    if (modes) *modes = known ? found->modes : std::uint8_t{};
    return known ? found->label : std::string_view{};
}

// One plain sentence per named value. Kept beside the names so the two stay in step.
std::string_view essential_help(std::string_view key) {
    struct Help {
        std::string_view key, text;
    };
    static constexpr Help help[]{
        {"physicsmode.jumpmaxheight", "How high the skater's body rises on a full, held ollie, in metres. 1.575 is the game's own; Skate 3 used 1.71."},
        {"physicsmode.jumpminheight", "How high a quick tap of an ollie goes, in metres. Bring it close to the max and every ollie is the same height."},
        {"physicsmode.grindjumpcommonmax", "The highest pop out of a truck grind (50-50, 5-0, nosegrind...), in metres."},
        {"physicsmode.grindjumpcommonmin", "The lowest pop out of a truck grind: what a quick flick gives. A pop is never lower than this."},
        {"physicsmode.grindjumpboardslidemax", "The highest pop out of a boardslide or lipslide, in metres."},
        {"physicsmode.grindjumpboardslidemin", "The lowest pop out of a boardslide or lipslide: what a quick flick gives."},
        {"physicsmode.grindjumptipslidemax", "The highest pop out of a noseslide or tailslide, in metres."},
        {"physicsmode.grindjumptipslidemin", "The lowest pop out of a noseslide or tailslide: what a quick flick gives."},
        {"physicsmode.jumpminheightmanual", "The pop height out of a manual, in metres. Not confirmed to do anything yet: try it and tell us."},
        {"physicsonboardjumptuning.jumpadjustvelocityboostlimitground", "Pushing the left stick as you pop adds speed that way; this is the most it adds, in m/s."},
        {"physicsonboardjumptuning.jumpadjustvelocityboostlimitgrind", "The same stick boost when popping out of a grind. It is lower than on the ground, which is why a pop out of a grind can feel slower."},
        {"physicspush.maxpushablespeed", "The speed pushing can take you to, in m/s (1 m/s = 3.6 km/h). Every kind of push scales with it."},
        {"onboard_speedmodel.gravityacceleration", "The pull of gravity the board's speed model uses, in m/s2. Higher should mean faster drops and less float; lower floats more."},
        {"onboard_speedmodel.maxgravityacceleration", "The most gravity may speed the board up down a slope, in m/s2. Skate 3 capped this at 7."},
        {"physicsairstates.mountrunlaunchz", "How fast you roll away after pressing Y to get on the board while jogging, in m/s. Not confirmed to do anything yet."},
        {"physicsairstates.mountsprintlaunchz", "How fast you roll away after pressing Y to get on the board while sprinting, in m/s. Not confirmed to do anything yet."},
        {"physicsairstates.minbodyfliptimevsupy x", "A body flip needs this much air time. x 0.5 halves it, so flips start from lower pops. Not confirmed to do anything yet."},
        {"physicsreckoning.flipscalar", "How fast a front flip or back flip rotates. 1 is the game's own; Skate 3 used 1.25."},
        {"physicsreckoning.flipmaxspeed", "The fastest a body flip may rotate. Skate 3 used 5, half of this game's 10."},
        {"physicsairstates.maxspinspeed", "How fast the skater's body spins in the air, in degrees per second at full stick."},
        {"physicsmode.speedwobblestartspeed", "The speed where the board starts to wobble, in m/s. Higher: stable for longer."},
        {"physicsmode.grindlockdist", "How far from a rail or ledge the board still snaps onto it, in metres. Skate 3: 0.9 normal, 0.15 hardcore."},
        {"physicsgrind.commonfrictionscalar", "How quickly a grind loses speed. Lower slides further."},
        {"onboard_powerslide.frictionscalar_slide", "How hard a powerslide brakes. Higher stops sooner."},
        {"onboard_powerslide.frictionscalar_revert", "How much speed a revert costs. Higher loses more; 0 loses none."},
        {"onboard_powerslide.frictionscalar_autorevert", "How much speed the automatic revert after a spin landing costs."},
        {"onboard_powerslide.powerslide_forwardforcescalar_revert", "A push forward during a revert. Higher gains speed out of reverts."},
        {"physicsmode.pumpeffectfactor", "How much speed pumping a transition gives. 13 is the game's own; Skate 3 used 18."},
        {"physicsmode.pumpmaxacceleration", "The fastest pumping may speed you up. 13 is the game's own; Skate 3 used 10."},
        {"physicsfootplant.leglengthonlanding", "How straight the legs are when the board lands (1 = straight). Lower should crouch deeper on impact. Not confirmed yet."},
        {"physicsmode.wipeoutbadlandingscalar", "How strict the game is about landing crooked. The switch above turns the check off altogether."},
        {"physicsfootplant.wipeoutmaxlandingspeeddown", "Land falling faster than this (m/s) and you bail. Skate 3 used 10."},
        {"physicsfootplant.wipeoutmaxlandingspeedhoriz", "Land with more sideways speed than this (m/s) and you bail. Skate 3 used 11."},
        {"physicsmode.wipeout_groundxzacceleration", "How hard a sideways hit has to be to knock you off. Higher: harder to bail."},
        {"jump.basejumpheight", "How high a jump on foot goes, in metres."},
        {"locomotion.sprintspeed", "Sprinting speed on foot, in m/s."},
        {"wipeout.spreadeaglegravity", "Gravity while gliding spread-eagle. -8 is the game's own; nearer 0 falls slower."},
    };
    const auto found = std::ranges::find(help, key, &Help::key);
    return found == std::end(help) ? std::string_view{} : found->text;
}

PresetDial preset_dial(std::string_view name) {
    struct Dial {
        std::string_view name, title;
        std::uint8_t modes;
    };
    constexpr std::uint8_t r = mode_realistic, f = mode_fun, both = mode_realistic | mode_fun;
    static constexpr Dial dials[]{
        {"Super Ollie", "Ollie height", both},
        {"Grind Pop", "Pop out of grinds and slides", both},
        {"Fast", "Push speed", both},
        {"Gravity", "On board: gravity (higher: less float)", both},
        {"Board Mount", "Get-on-board speed (untested)", both},
        {"Easy Body Flips", "Body flips: air time needed (untested)", both},
        {"Soft Landings", "Landing speed you can take", both},
        {"Deep Landings", "Landing: leg length (lower crouches deeper; untested)", both},
        {"Revert Friction", "Revert friction (higher loses more speed)", both},
        {"Pump Power", "Pumping strength", both},
        {"Fast Flips", "Body flip speed", both},
        {"Fast Spins", "Body spin speed", both},
        {"Hard To Bail", "Bail resistance (higher: harder to bail)", both},
        {"Sticky Grinds", "Grind lock-on", both},
        {"Slick Grinds", "Grind friction", both},
        {"Moon Jump", "On foot: jump height", both},
        {"Fast On Foot", "On foot: sprint speed", both},
        {"Fast Parkour Flips", "On foot: flip and roll speed", f},
        {"Super Glide", "Glide: gravity (lower falls slower)", f},
        {"Torpedo Boost", "Torpedo and falling: steering", f},
        {"Realistic", "", r},
        {"Mega Pop", "", 0},
        {"No Speed Wobble", "", f},
        {"Auto Push", "", f},
        {"Smooth Surfaces", "", f},
        {"Long Wheelbase", "", both},
        {"Skate 3", "", both},
        {"Skate 3 Hardcore", "", both},
        {"Skate 3 Easy", "", both},
    };
    const auto found = std::ranges::find(dials, name, &Dial::name);
    return found == std::end(dials) ? PresetDial{} : PresetDial{found->title, found->modes};
}

// Skate 3's own tuning, for the values this game still has under the same class and field name
// and that differ: this game's tuning grew out of Skate 3's. Hardcore and Easy are Skate 3's
// difficulty settings on top of the same set.
namespace {
struct Skate3Value {
    std::string_view id;
    double value;
};
#include "trainer_skate3.inc"
std::vector<PresetRule> skate3_rules(std::span<const Skate3Value> difficulty) {
    std::map<std::string_view, double> values;
    for (const auto &row : skate3_normal) values[row.id] = row.value;
    for (const auto &row : difficulty) values[row.id] = row.value;
    std::vector<PresetRule> rules;
    for (const auto &[id, value] : values) rules.push_back({id, false, value, false, true});
    return rules;
}
} // namespace

// Patterns are matched against the lower-case ids the game's own data gives its tuning
// (run `trainer dump` for the list), so a preset reaches every value a pattern names in
// whatever build is running and simply skips the ones that build lacks.
const std::vector<BuiltinPreset> &builtin_presets() {
    static const std::vector<BuiltinPreset> presets{
        // Rules name values the game was found to read (trainer_used.inc) or linked values
        // (value_links). Jump height comes from the PhysicsJump height graphs; body flips
        // from FlipScalar and FlipMaxSpeed unless PerfectBodyFlips forces exactly one rotation;
        // body spins from the PhysicsBodyspin graphs and MaxAutoBodySpinSpeed. The top pushing
        // speed is the trainer's own doing (set_push_top); the game skips its other push values.
        {"Super Ollie", "Huge ollies: about three times the height at any speed.",
         {{"physicsmode.jumpmaxheight", true, 3.0}, {"physicsmode.jumpminheight", true, 3.0},
          {"physicsjump.absoluteminheight", true, 2.0}, {"physicsjump.jumpybonusmax", true, 3.0},
          {"physicsmode.grindjump", true, 2.5}}},
        {"Grind Pop", "Pops out of grinds, board slides and nose and tail slides go twice as high, quick pops included.",
         {{"physicsmode.grindjump", true, 2.0}}},
        {"Gravity", "The board falls and drops in faster: less float. Below 1 floats more.",
         {{"onboard_speedmodel.gravityacceleration", true, 1.3}, {"onboard_speedmodel.maxgravityacceleration", true, 1.3}}},
        {"Board Mount", "A calmer roll-away when you get on the board (Y) from a jog or a sprint.",
         {{"physicsairstates.mount launchz", true, 0.5}}},
        {"Easy Body Flips", "Front flips and back flips start from much lower pops.",
         {{"physicsairstates.minbodyfliptimevsupy", true, 0.3, true}, {"physicsairstates.bodyflipmingrabtimefraction", true, 0.3}}},
        {"Soft Landings", "Land from bigger drops and with more sideways speed before a bail.",
         {{"physicsfootplant.wipeoutmaxlandingspeed", true, 1.5}, {"physicswipeout.maxspeedlandingonboard", true, 1.5}}},
        {"Deep Landings", "The legs give more on impact, so big drops compress the skater further.",
         {{"physicsfootplant.leglengthonlanding", true, 0.85}}},
        {"Revert Friction", "Reverts and auto reverts scrub three times the speed, as in the older games.",
         {{"onboard_powerslide.frictionscalar_revert", true, 3.0}, {"onboard_powerslide.frictionscalar_autorevert", true, 3.0}}},
        {"Pump Power", "Pumping transitions gives 1.4 times the speed (Skate 3's strength).",
         {{"physicsmode.pumpeffectfactor", true, 1.4}}},
        {"Fast Flips", "Front flips and back flips rotate three times as fast.",
         {{"physicsreckoning.flipscalar", true, 3.0}, {"physicsreckoning.flipmaxspeed", true, 3.0},
          {"physicsmode.perfectbodyflips", false, 0.0}}},
        {"Fast Spins", "Body spins rotate three times as fast.",
         {{"physicsairstates.maxspinspeed", true, 3.0}, {"physicsmode.maxautobodyspinspeed", true, 3.0}}},
        {"Realistic", "Lower pop, slower pushing and rotation, earlier speed wobble, easier to bail.",
         {{"physicsmode.jumpmaxheight", true, 0.75}, {"physicsmode.jumpminheight", true, 0.8},
          {"physicsmode.grindjump", true, 0.8}, {"physicspush.maxpushablespeed !camera", true, 0.75},
          {"physicsmode.speedwobblestartspeed", true, 0.7},
          {"physicsreckoning.flipscalar", true, 0.8}, {"physicsairstates.maxspinspeed", true, 0.75},
          {"physicsmode.maxautobodyspinspeed", true, 0.75}, {"physicsmode.grindlockdist", true, 0.7}, {"jump.basejumpheight", true, 0.85}, {"jump.maxjumpheight", true, 0.85},
          {"locomotion.sprintspeed", true, 0.85}, {"wipeoutfallspeed.", true, 0.8},
          {"physicswipeout.wipeout_ maxspeed", true, 0.8}, {"physicsmode.wipeout_ acceleration", true, 0.8}}},
        {"Mega Pop", "Ollies and grind pops go about twice as high.",
         {{"physicsmode.jumpmaxheight", true, 2.0}, {"physicsmode.jumpminheight", true, 1.6},
          {"physicsjump.jumpybonusmax", true, 2.0}, {"physicsmode.grindjump", true, 1.6}}},
        {"Fast", "Every push is 1.8 times as fast.",
         {{"physicspush.maxpushablespeed !camera", true, 1.8}, {"physicspush.maxspeedforautopush", true, 1.8},
          {"physicsmode.speedwobblestartspeed", true, 3.0}}},
        {"Moon Jump", "On foot: jumps about three times as high.",
         {{"jump.basejumpheight", true, 3.0}, {"jump.maxjumpheight", true, 3.0}, {"jump.maxjumpvelocity", true, 2.0}}},
        {"Fast On Foot", "On foot: sprint nearly twice as fast.",
         {{"locomotion.sprintspeed", true, 1.8}, {"locomotion.sprintspeedboost", true, 1.8}}},
        {"Fast Parkour Flips", "On foot: flips and rolls rotate and travel much faster.",
         {{"flumping.flumptuckmaxrotationvelocity", true, 2.5}, {"flumping.rollmaxvelocity", true, 2.0}, {"flumping.rollvelocitymultiplier", true, 1.5}}},
        {"Super Glide", "Spread-eagle falls slowly and steers hard.",
         {{"wipeout.spreadeaglegravity", true, 0.35}, {"wipeout.spreadeagleaircontrolmax", true, 2.0}, {"wipeout.spreadeagleaircontrolmin", true, 2.0}}},
        {"Torpedo Boost", "Torpedo steers and carries much harder, and so does a plain fall.",
         {{"wipeout.torpedoaircontrolmax", true, 2.5}, {"wipeout.defaultaircontrol", true, 2.0}}},
        {"No Speed Wobble", "The board stays steady at any speed.", {{"physicsmode.speedwobblestartspeed", true, 20.0}}},
        {"Auto Push", "Once rolling, the skater keeps gaining speed up to the auto push speed (8 m/s).", {{"physicsmode.autopushenabled", false, 1.0}}},
        {"Hard To Bail", "Much larger impacts are needed before a wipeout.",
         {{"physicswipeout.wipeout_ force", true, 3.0}, {"physicswipeout.wipeout_ acceleration", true, 3.0},
          {"physicswipeout.wipeout_ maxspeed", true, 2.5}, {"physicswipeout.wipeout_ maxdisp", true, 3.0}, {"physicswipeout.wipeout_ relativevel", true, 3.0},
          {"wipeout.on forcelimits", true, 3.0}, {"wipeout.runoutwipeoutforcelimits", true, 3.0},
          {"physicsmode.wipeout_ acceleration", true, 3.0}, {"physicsmode.wipeoutcheckforbadlanding", false, 0.0}}},
        {"Sticky Grinds", "Grinds lock on from further away and at higher speeds.",
         {{"physicsmode.grindlockdist", true, 2.0}, {"physicstrajectory.grindmaxspeedsqr", true, 4.0},
          {"physicstrajectory.grindmaxspeeddownontogrind", true, 2.0}, {"physicstrajectory.grindlandingmaxangle", true, 2.0}}},
        {"Slick Grinds", "Grinds and slides keep their speed.",
         {{"physicsgrind.commonfrictionscalar", true, 0.4}, {"physicsgrind.curbfrictionscalar", true, 0.4}}},
        {"Smooth Surfaces", "Rough ground rides like polished concrete.", {{"physicsmode.makesurfacessmooth", false, 1.0}}},
        {"Long Wheelbase", "Trucks 10 cm further out at both ends. Takes effect on the next respawn.",
         {{"physicstrucks.truckzposfront", false, 0.0143}, {"physicstrucks.truckzposback", false, 0.0143}}},
        {"Skate 3", "Skate 3's own numbers for everything this game still shares with it: pop, grind pops, pushing, pumping, "
                    "steering, manuals, body spins and flips, bails. Normal difficulty.",
         skate3_rules({})},
        {"Skate 3 Hardcore", "Skate 3 on Hardcore: lower pops, weaker pushes, tight grind lock-on, slow auto spins.",
         skate3_rules(skate3_hardcore)},
        {"Skate 3 Easy", "Skate 3 on Easy: full-height pops, strong pushes, generous grind lock-on.", skate3_rules(skate3_easy)},
    };
    return presets;
}
} // namespace dingosdk::trainer
