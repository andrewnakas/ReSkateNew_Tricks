# ReSkate Tricks: status

## START HERE (written 2026-10-13 ~01:00)
**Next session: plan, then do, three things the owner asked for:** (1) the casper/primo catch taken early in mid-air, cancelling the flip animation like a dark catch does; (2) no bouncing or tilting on landing (casper bounced once in ~9 runs; the owner has not yet confirmed the 00:53 build); (3) the small no-pop shuv on his roll gesture.
- The newest entries are at the **bottom** of this file. Read the 23:29, 00:15, 00:31 and 00:53 entries for the mechanisms and the measured dead ends; the same summary is in the Claude memory file `project_reskate_tricks.md`.
- Installed in the Skate folder: the 00:53 build. Start the launcher with `--no-update` (its auto-update is on). Owner's pre-mod files: `dev\backup\20261012-122134`.
- Nothing is committed. D: dropped off twice on 2026-10-12: offer a local commit.
- The "READY TO TEST" tables just below describe the first evening's click + flick controls (`tricks scheme flick`); the default is now the owner's hold scheme described in the bottom entries.


## READY TO TEST (build installed in the game folder 2026-10-12 ~16:00)

Hold the **left stick click** on the board, then flick the **right stick**. The game doesn't see those flicks, so they don't also ollie.

| Trick | Controls | Checked headlessly |
|---|---|---|
| Casper | click + RS down | Standing and at 16 km/h (rolling, it goes in through a primo for 0.7 s) |
| Primo | click + RS left | Standing and at 17 km/h |
| Primo2 | click + RS right | Standing and at 16 km/h (through a primo when rolling) |
| Switch casper ↔ primo ↔ primo2 | Another flick while holding | Yes |
| Casper flip out | In a casper: RS up | Pops, flips and lands rolling, standing and at speed |
| Casper flip out, second value | In a casper: RS up-left or up-right | Flips out; not confirmed to be a different clip from the first |
| Step out | Let go of the click | Casper steps off; primo and primo2 exit with their FS shuvit clip |
| Casper and primo slides | Enter while rolling | Speed is kept and scrubbed at 2.5 m/s per second; 18 km/h slides about 6 m |

### No comply flips and in-air extras (added ~16:30; scripted checks by board turn rate and stills)

| Trick | Controls | Checked headlessly |
|---|---|---|
| No comply flip | Do a no comply (push, then right stick down). As soon as it starts, let the stick centre and flick: **up-left** kickflip, **up-right** heelflip, **left** pop shuvit, **right** FS pop shuvit, **up** 360 flip, **down** hardflip-style | Kickflip and shuvit: the board turns in the air and lands riding (1,050 and 518 deg/s against 405 for a plain no comply) |
| Cleaner no comply | Automatic: the game's experimental no comply clips are switched on | Lands "Clean" where stock lands "Sketchy", and pops 0.59 m against 0.39 m |
| Double flip (and nightmare / daydream) | In the air during a flip trick, **tap the left stick click** with the right stick at rest: one more flip the way the trick flips. On a varial kickflip that is a nightmare flip, on a varial heelflip a daydream flip | Heelflip + tap: 2,356 deg/s against 1,298, landed. The varial combinations themselves were not run |
| Late shuvit | In the air, tap the click with the right stick held **left or right** | On an ollie: turned and landed |
| Dolphin-style turn | In the air, tap the click with the right stick held **up or down**: the board goes end over end and comes down turned around | Not run yet |

Up to two extras per air.

**Added ~16:55:**
- **Late flip.** The same tap on a plain ollie: the board kickflips under it. Checked: landed riding.
- **Dolphin-style turn.** Checked on an ollie: the board goes end over end (a still caught it upright mid-air) and lands riding.
- **Trick names.** Every mod trick shows its name as a ReSkate notice, top left: "No Comply Kickflip", "Late Flip", "Double Kickflip", "Nightmare Flip", "Daydream Flip", "Double Tre Flip", "Late Shuvit", "Dolphin Flip", "Casper", "Primo", "Casper Flip". `tricks names on|off`. The game's own trick line and score do not include them yet.
- **Boneless / footplant flips.** The code is in: any boneless or footplant the game starts opens the same flick window as a no comply, and it would be named "Boneless <flip>". **Not verified.** My scripted boneless attempts (a trigger held, then A or X) only produced ground grabs, so the right inputs still need finding. Try it by hand: do a boneless, then flick.
- **Dragon flip (~17:00).** In the air, tap the click with the right stick held up-left or up-right: the dolphin turn plus a 360 shuv. Checked on an ollie: landed riding.
- **Casper balance (~17:00).** In a casper, the left stick's up/down drives the game's low/medium/high casper blend (`Float.Intent.Balance`). Wired; the stills show only a slight change, so treat it as unconfirmed.
- **Shuv out of a grind.** Pop out of the grind, then tap the click with the right stick left or right (the late shuvit). Not run on a grind yet.

**Extra console commands:**
- `tricks ncflip on|off|stock|experimental|<trick id>`
- `tricks air on|off`, `tricks air ms <n>` (how long the extra turn takes, default 320) and `tricks air sign 1|-1`
- `tricks spin <roll> <yaw> <pitch> <ms>` turns the board by hand, for experiments.

**Limits:** these have no trick name or score yet (the HUD shows only the underlying trick), and the skater's body does not act the flip. Only the board turns.

**Console (`~`):**
- `tricks slide <m/s per second>` sets the slide friction. 0 gives the game's own, which stops in about a second.
- `tricks controls on|off`
- `tricks freestyle casper|primo|primo2|off`
- `tricks casperflip [9|16]`
- `tricks status`

**Known limits:**
- No trick name or score yet.
- The balance blend on the casper loop isn't driven.
- Tested on flat ground only. On Gravy Train's slope a casper kept its speed downhill.

**Your previous ReSkate files** (from before the first Tricks install today) are in `dev\backup\20261012-122134`. Copy both files from there into the Skate folder to go back.

## Start here
- **Repo:** this directory. Branch `tricks` was cut from upstream ReSkate `v2.0.7` (origin = Dingo-Shenanigans/ReSkate). It is a separate mod from the ReSkate Trainer, which is going upstream.
- **Code:** `Extension/Tricks/`, wired in through `cmake/Runtime.cmake`, `Runtime/client_tick.cpp` (tick) and `Extension/Console/commands.*` (the `tricks` console commands).
- **Requests and per-trick status:** `TRICKS.md`.
- **Hold:** don't build-test or launch the game while the reverse-engineering run has Skate.exe open (owner, 2026-10-12).

## Spike 1: freestyle (caspers and primos). Done on paper 2026-10-12; in-game test pending.

The game ships a complete freestyle animation graph. EA cut the scripts that drive it.

**How the graph works:**
- `onboard.riding.node` goes to `onboard.freestyle.node` while **Bool.Intent.WantsFreestyle** is set.
- The conduit picks a pose by **EnumGS.Intent.FreestyleTrick**: Casper = 0, Primo = 1, Primo2 = 2. The loops switch between poses when that value changes.
- Each freestyle node tags `enum.gs.currentgroundtrick = 7`. The physics selector keeps state 103 on 7, so the board follows the clip.
- **Leaving a casper:**
  - When WantsFreestyle is false and **EnumGS.Intent.FreeStyleFlipTrick** is non-zero, it goes to `casper.fliptrickexit`. The database `cdb_onboard_freestylecasperfliptrick` holds two clips, `casper_exit_flip1` (bitmap {0, 9}) and `casper_exit_flip2` (bitmap {16}). The bitmap-to-value mapping is unconfirmed.
  - When the flip trick is 0, it goes to `casper.exit`.
- Primo and primo2 step off when WantsFreestyle is false.

**What the game's own scripts do:**
- The `enablefreestyle` setting only sets `bool.intent.enablecaspers` (ExperimentalSettings).
- Behind that setting, the gamepad script fills `input_intofreestyle`.
- Nothing reads either one, and nothing sets WantsFreestyle or FreestyleTrick.

**Mod approach:**
- Find the three state objects by their asset ids (a memory search).
- Hook the game's state lookup (0x11dcdd0, the same hook the trainer uses for grind switches).
- Answer held values from the hook.

**Commands:**

| Command | Effect |
|---|---|
| `tricks freestyle casper\|primo\|primo2\|off` | Ride in that pose, or step off |
| `tricks casperflip [v]` | Flip out of a casper; the default value is 9 |
| `tricks status` | Shows live values and how often the game asked for each held state |

**Open questions for the first test:**
1. Does the animation graph read these states through 0x11dcdd0? If it doesn't, `asked` stays 0. The fallback is to write the skater's value directly.
2. Does the intent tree's physics fight the clip? The skater may keep steering or pushing.
3. Which FreeStyleFlipTrick values pick flip1 and flip2?
4. The controller binding: the owner chooses once it works.

**Merge note:** the Trainer fork hooks the same lookup (`trainer_gamestate.cpp`). Once both are upstream, they should share one hook.

## Freestyle in game: WORKING (2026-10-12, run 6, `dev/shots/sheet_run6.png`)

All runs used `dev/run.ps1`. It installs the build, boots Gravy Train, mounts with `tricks pad mount`, then restores your ReSkate files afterwards.

**What did not work:**
- Holding the states at the lookup hook (0x11dcdd0). The animation graph asks for WantsFreestyle, but none of the freestyle states has a slot: their handle is 0, because nothing ever wrote them. So the graph never left riding.
- Writing through the game's own setters without CurrentGroundTrick 7. A slot appears on the first write and the casper-into clip starts in physics state 103. The intent tree then puts CurrentGroundTrick back, so 103 gave way to natural air (200/201) once the clip lifted the board, and the skater stepped off (504).

**What works:**
- Write through the setters every tick: `SetBoolGameState` 0x1090600 and `SetIntGameState` 0x1090650.
- The entity key and the reference offset come from a hook on 0x1090600. It catches the game's own EnableCaspers write at each mount and recognises it by object id and name.
- While in freestyle, write WantsFreestyle=1, FreestyleTrick=pose, FreeStyleFlipTrick=0 and **CurrentGroundTrick=7**.
- The physics then stays in 103 for the whole pose. Casper, primo and switching between them play with no dismount.
- `casperflip` (value 9) pops out: 201 for 28 ticks, then lands riding (100).
- The game's own getters (0x108fa70, 0x108fac0) read the values back, shown in `tricks status`.

**Still open:**
- Is FreeStyleFlipTrick 16 the second flip clip?
- What does each pose look like at riding speed, and do steering and pushing leak in?
- The controller binding.
- Scoring and names (spike 4).

## Controls, slide and state lookup (2026-10-12 afternoon)

**Controls: working in a scripted run on Gravy Train's flat start.** They are on by default; `tricks controls on|off` switches them. Hold the left stick's click on the board, then flick the right stick:

| Right stick | Effect |
|---|---|
| down | casper |
| left | primo |
| right | primo2 |
| up, in a casper | flip out, value 9 |
| up-left or up-right, in a casper | flip out, value 16 (unconfirmed) |
| release the click | step out (a primo exits with the FS shuvit clip) |

- While the click is held or a pose is on, the game's polls have the right stick and the click hidden (`tricks_filter` in `input_capture.cpp`), so the flick doesn't also ollie.
- Trap: the pad has to be recorded on every poll of controller 1, at the innermost nested call. Game polls arrive through forwarded XInput exports flagged as the overlay's.

**Slide: built, not yet measured.**
- `tricks slide <m/s per second>` (default 3) takes speed off each physics step while in a pose in state 103. The code is `tricks_freestyle_slide` in `client_noclip.cpp`.
- Gravy Train's slope drowns the measurement. Use the flat map: `dev/run.ps1 -Level levels/game/dingolevel_reskate_thps_training/dingolevel_reskate_thps_training`.

**State lookup: built, not yet run.**
- The memory search for state objects is flaky: copies come and go, and it once returned a garbage handle. That delayed a casper entered at speed by about a second.
- The replacement is `resolve_state_objects`. It takes the asset domain of the EnableCaspers object the game passes to its setter (u16 at object +0x16) and calls the game's `find_asset(domain, name)` up the domain's parents. That gives the copies the game itself uses.
- `tricks status` shows `resolved yes/no (count)`.

**Other variations in the graph:**
- The casper loop is a blend space on `Float.Intent.Balance` (low/med/high clips). It could be driven from the left stick.
- Primo and primo2 only exit through their FS shuvit clips.

**Open:**
- Run the resolve build.
- Measure the slide on flat ground against a grind's slowdown.
- Confirm value 16.
- Entering at speed.
- Scoring and names.

## Entering at speed (2026-10-12, 15:00–16:00)

**Test rig.**
- Use `dev/run.ps1 -Level levels/game/dingolevel_reskate_thps_training/dingolevel_reskate_thps_training`.
- Each pass runs `trainer tp -5 -6.3 -9.5;wait 4;tricks pad ly=-1:500 n:600 b:150 n:1200 y:200 n:2500;wait 7`. That teleports, walks back to face 180, and mounts.
- Then two pushes (`a:200 n:500 a:200 n:400`) reach about 17 km/h.
- The flat strip runs from z -9.5 to about -21. Beyond it the floor drops away. The spawn is on a halfpipe deck.
- `trainer tp` dismounts. `trainer ground` picks the roof.

**The game's own friction in 103.** With a pose on, it stops the board from 15 km/h in about a second. The slide (`tricks_freestyle_slide`) therefore owns the speed: it starts at the entry speed and falls by `slide` each second, and it resets if the board hits something.

**Entered directly at speed.**
- The casper lies back for about a second, then stands up.
- Primo2 slams.
- Primo is clean.
- Entering casper or primo2 from a primo is clean. The mod therefore routes a rolling entry (over 1.5 m/s) through primo for 700 ms.

**What did not fix the casper lean:**
- ForceAnimSkateboard 0 or 1
- ForcePhysicsSkateboard 1
- AllowHipsTranslationAlignment 0
- Removing the board parts' pitch angular velocity (`tricks level`, part +0x90). That code is still in and harmless.

**Tools added.**
- `tricks state <name> b|i|f <value>` and `tricks fstate` hold any game state by asset name, through `find_state` and the setters. `tricks state read <name> b|i` reads one.
- These are the tools for the B-tier work.

## The procedural board and no comply flips (2026-10-12, 16:00–16:30)

**What does not work for a no comply flip.**
- The game's own late flip does not fire out of a no comply.
- Holding `EnumGS.Intent.FlipTrick` + `Float.Intent.TrickFlipSpeed` only through the no comply's air changes nothing. Held all the time, it makes the game do a real kickflip after landing.
- Holding `Bool.Intent.LateFlip` from the ground takes the skater off the board.
- `nocomply.node` only leaves at its clip's end (`branch_end`), so no state can cut it into a flip clip.

**What works: the procedural board** (`Extension/Tricks/tricks_board.*`).
- Hook 0x47c3320 `(board, out matrix, flag)`. Each step it copies the target frame at `*(board + 0x178)` to the board's angular drive. That frame is the skater's physics context; its rows are right, up, forward, position.
- For the local skater only, the frame is turned about the board's own axes: roll about forward, yaw about up, pitch about right. The context comes from `debug_noclip_bodies(...).context`.
- The drive follows the turned frame. The hook is called about 50 times through a no comply, and the turned board lands.
- Roll sign: +360 is the kickflip's side and -360 the heelflip's. Proven by doubling a heelflip; the other sign cancels it.
- The spin is linear over its length. Whole turns end as the clip has the board.

**Test recipes** (scripted pad, after the mount recipe above):
- No comply: `a:250 a+ry=-1:250`.
- Flick pick: `n:120 rx=-0.7+ry=0.7:150`.
- A flick-it trick that comes out as a heelflip: `a:200 n:500 ry=-1:250 rx=-0.6+ry=0.8:80`.
- Air tap: `n:150 ls:120`.
- `tricks status` prints the last jump's peak board turn rate.

**Next.**
- Trick names and scores for the mod's tricks (spike 4).
- Boneless/footplant flips, with the same spin in the plant's air.
- Ground shuv-its.
- Shuvs out of grinds: is the drive on in grind states?
- Run the dolphin turn and the varial + extra flip.
- 180 into a manual.

## Iteration 3 notes (16:40–16:55)

- **Bug fixed.** A spin that is not whole turns (late shuvit, dolphin turn) stays applied after it ends. It has to be stopped on landing (`stop_board_spin` when the air ends), or the next mount gets a board turned around and fails.
- **Boneless inputs.**
  - A and X are the plant feet, and `input_olliebutton` is the buffered quick ollie.
  - The condition is one hand-grab trigger past 0.999, a plant foot within 0.2 s, and `enumgs.grabtype` 1 or 2.
  - The scripted pad `rt=1:300 rt=1+a:200` gave a "BS Ground Grab" and no plant.
- **Names.** `overlay::notify(info, "TRICK", name)` from the tick works.
- **Still open.**
  - Names in the game's trick line and a score (spike 4).
  - Ground shuv-its: the drive is off in state 100, so the board hook has nothing to turn.
  - 180 into a manual.
  - Muska flip.
  - The casper's lean when entered directly at speed (routed around).

## Next spikes
2. Enum injection: force the flip trick from no comply or boneless (B tier).
3. Procedural board: where the posed `Skateboard_Root` reaches the deck drive (C tier).
4. A name and score hook for new trick names.
5. 180 into a manual: the wrong stance.

## Evening session (2026-10-12, 18:30–19:15): owner playing the 17:01 build; code and research only

**Built, NOT installed, NOT tested** (`builds2022-x64\Release`, 18:50):
- **Ground shuvit (no pop).** Riding (physics 100), hold the right stick left (shuvit) or right (FS shuvit) and click the left stick. The mod holds `CurrentGroundTrick` 7 for the turn (380 ms) so the physics goes to 103 and the board follows a frame, turns that frame half way round, lets 7 go, and drops the turn once the physics has left 103. Speed is kept with the slide code at 0.5. `tricks groundshuv [bs|fs] | on|off | ms <n>`.
- Open risks to check first: does 7 alone take 100 to 103 (the notes say 103 is entered when the rig wants animation, and 7 only *keeps* it), and does the board stay turned afterwards.

**From the reverse-engineering notes (read-through, nothing run):**
- Physics: 100 ground, 101 slide, 103 ground animation, 104 skitching; grinds 400 boardslide, 401 50-50, 402 tipslide, 403 5-0, 404 backslash, 405 darkslide, 406 transition; 202 grind trick. The board's drive to the animated pose is switched on entering 103, 200/203, 201, 600 and 602, and off on entering ground. Grinds use their own heading drive, so the board hook probably cannot turn the board on a rail. A manual is state 100.
- `CurrentGroundTrick` writers: 1 manual, 2 no comply, 3 boneless, 4 footplant, 6 slappy, 0 at the end.
- Trick line: scorables are picked by the skater observer state machine (data). A `DingoScorable` has `TrickName` (plain text, "360 Flip"), `BaseTrickName` (a string id such as `ID_TRICK_FLIP`), `BaseScoreValue`. The HUD is `ScoringHUDViewModel` (schema 0x9b74fc7c) with `TrickHistory`, `SequenceScore`, `LineScore`, `Multiplier`; every write goes through `model_write` 0x19248b0 (Hall of Meat already hooks it). String overrides: `set(store, id, utf8, layer)` 0x331cc20 (Throwdowns uses it).
- Manual stance: the observer picks nose or tail manual from the anim tags `bool.anim.innosemanual` / `intailmanual` and the physics output `IsRidingFakie`; the intent picks nose or tail only by the sign of `input_boardadjust`, with no stance term. Nobody has analysed the 180 bug.

**Next test run (when the game is free), in this order:**
1. `tricks groundshuv` standing, then rolling; `trainer states` for 103; stills.
2. Do a kickflip, then `ui dump 0x9b74fc7c`: read how `TrickHistory` holds names. That decides the route for names in the trick line (rewrite at `model_write`) and for renames ("Tre Flip").
3. Varial kickflip + tap (nightmare), varial heelflip + tap (daydream), late shuvit popped out of a grind.

## 20:27 build installed (owner's hold scheme) - UNTESTED
- New default `tricks scheme hold`: land a flip trick with the right stick part-way left/right = primo/primo2; land with L3 held + stick part-way = casper; centre = leave (casper flips out); flick (edge and back within 0.3 s) = casper flip out + kickflip/heelflip/varial/tre turn; edge held 0.35 s = pose lost (LateFlip intent held 0.4 s to throw the skater off - a stand-in for a real bail). `tricks scheme flick` = the old L3 + flick controls. In the hold scheme air extras fire on a short L3 tap's release.
- Slides: after 0.7 s all horizontal turning is removed from the parts (skater held upright). Owner reported slides at speed are tippy; cause not found.
- Ground shuvit also in this build (untested).
- Previous (17:01) build: dev\backup\20261012-202711-tricks1701.

## 20:54 build installed ~20:56 - UNTESTED
- In-air catch: L3 or LT held 0.2 s in the air + right stick out picks the pose (left/right primo/primo2, up/down casper); board turned to it in the air (`tricks catch on|off`); cancel by centring before landing; edge-fall has a 1 s grace after landing. Mask now hides LT too.
- `tricks upright <0..1>` (default 0.6): parts pulled to one slide velocity and tipping damped from the start of a slide (owner: bobble on contact).

## 21:01 build installed ~21:05 - UNTESTED
- Catch drop: caught board moved 0.1 m down its up axis (`tricks catch drop <m>`).
- Owner on the 20:54 build: the catch is felt, feet sit inside the turned board, and on touchdown the skater falls every time. Suspect: landing on a turned board (`tricks catch off` to confirm). Owner wants the feet posed (casper: back foot over, front foot under the board) - no way to pose the body in the air yet.

## 21:31 build installed - catches land (scripted runs, sheets dev\shots\sheet_catch3.png / sheet_catch4.png)
- Measured causes of the owner's falls: (1) landing on a board turned in the air bails every time -> `catch` now defaults off; (2) zeroing all horizontal turning 0.7 s into a slide / the upright velocity pull -> removed, `upright` defaults 0; (3) a pose left on after a bail blocked the next mount -> poses are cancelled off the board.
- Casper lean on contact: the 0.7 s primo lead-in was being used up during the landing (state 100); it now counts from state 103. Casper then lands without lying back.
- Verified at ~18 km/h, one push + ollie, L3 + stick held from the air: primo (lands, slides, centre = rides away), primo2 (same), casper (lands via primo, centre = flips out and lands), casper flick up-left = Casper Kickflip, landed.
- Test trap: the flat strip is short; at slide 2.5 the skater slides off its end (looks like a bail: 103 -> 504). Use `tricks slide 5` in tests. A run fails to mount if the game window loses focus.
- Not verified: LT as the modifier, edge = fall, catching after a flip trick (only ollies run), landing back into a pose from a flip out.

## 22:07 build installed - catches with a full stick and L3 held through the pop (sheet dev\shots\sheet_catch8.png)
- Owner reports on 21:31: hard to trigger, one primo landed then tipped over, poses flip out at once, wants L3 held through the pop.
- Causes found by scripted runs: (1) a full right stick push leaked to the game for one poll before the tick's mask; the skater then landed crooked and the pose only started at ~0.6 m/s. Fix: `DingoSDKOverlayTricksAutoMask` - in the air, a poll with L3 or LT down is masked inside the XInput filter itself. (2) the catch needed 0.2 s of L3 first: now immediate, and accepted up to 0.35 s after touchdown. (3) centre for 0.15 s flipped out: a pose is now kept by the stick off centre OR L3/LT held; both released 0.25 s = leave. (4) edge = fall is off by default (`tricks edgefall on`). (5) a release from a stick held at the edge was read as a flick: a flick is now an edge visit under 0.22 s.
- Catch arming: the pop's flick (stick at the upper edge on the ground or in the first 0.15 s of air) is not a pick; the stick must pass the centre after it.
- Verified: casper caught with L3 + full stick down, kept on L3 alone for 2.8 s, flips out and lands; primo with L3 held through the pop + full stick; primo with L3 pressed in the air + full stick. All land upright and slide.
- Trap: stick pushed down within ~60 ms of the pop flick cancels the ollie (no air, no catch).

## 22:4x build installed - the owner's hold rules (sheets dev\shots\sheet_rules1.png, sheet_rules2.png)
- Hold = L3/LT AND stick off centre. Casper: stick to the middle = fall, flick = flip out with a turn. Primo: stick held at the outside 0.4 s (1 s grace) = fall, middle = put down and ride away, flick = over to the casper for 0.35 s then its flip out with the turn (landed riding in a run). L3/LT released = clean exit.
- Fall: `tricks fall hop|slam`. hop (default, owner OK'd it) = LateFlip intent held 0.4 s: 103 -> 200 -> 201 -> 504, skater on foot. slam = `request_wipeout()` in no_bail.cpp (the state selector hook returns 300 once): Harsh Slam, verified for both poses.
- Stick mask: auto mask is on whenever the skater is not plainly riding (ground with no pop in the last 0.7 s). A full left/right push ~150 ms after the pop flick was still leaking and delaying the pose by >1 s (flaky). The final widening is NOT re-run.
- Dark catch: scripted RB in a heelflip gave 'Heelflip Underflip', not a dark catch. Game logic: intent state DarkAirGroup/WaitingForDarkInput reads input_darkcatch (RB) / input_classicdarkcatch (B) while bool.anim.isindarkcatchwindow and fliptrickphasetodarkcatch < 1. Ask the owner how they do theirs.

## 22:57 trace build installed
- `tricks trace on|off` (default on): logs\tricks-trace.csv in the Skate folder, one line per change: ms, buttons, lt, rt, sticks, physics, CurrentFlipTrick, Bool.GP.IsDark, Bool.Anim.IsInDarkCatchWindow, pose, speed. Owner records dark catches, then the motions wanted for casper / primo catches.
- Ground shuvit off by default (its input collided with the catch). Owner wants a shuv on a small right-stick flick: no pop, board barely leaves the ground, like the game's late shuv.
- Fixed (not re-run): stale `stick_armed` hid the right stick whenever L3 was held, so the pop could not be pre-loaded with L3 down.

## 23:29 build installed - controls rebuilt from the owner's trace (dev\trace-owner-20261012.csv; sheets sheet_trace1.png, sheet_early1.png)
- Owner's motion: L3 held the whole time; flick-it trick; stick swept straight from the flick to the catch place (no centre crossing) and held through the landing. Stick angle at touchdown (0 right, 90 up): casper 270-304, primo right 311-6, primo left 210-221. `tricks zones 245 307` = the split. The catch is where the stick rests (steady 100 ms) from 120 ms into the air until 350 ms after touchdown; it follows the stick if it moves on.
- **Why poses 'did not start': the landing node only hands to riding at its clip's end - measured 1.11 s after an ollie, 1.33 s after a kickflip - and riding is the only way into freestyle.** onboard.land.node also leaves at branch_end_early while EBool.Intent.Action is true (kickturn, tricksetup, push, manual, footbrake, dismount...). Holding Bool.Intent.TrickSetup = 1 from touchdown until physics 103: pose starts 156 ms after touchdown (`tricks early on|off|<state>`; FootBrake works too).
- Hold rules: L3 AND stick. Stick in the upper half at the edge: brief (<0.26 s) then centre = flick out (left of up kickflip, right heelflip, straight up tre; primo goes via the casper); held 0.45 s = fall. Casper: centre 0.25 s = fall. Primo: centre = put down. L3 released = clean exit. Stick resting in another pose's zone 0.25 s = switch pose.
- Ground shuvit gesture (L3 held, riding): stick down at the edge, rolled to a lower diagonal (296-338 or 205-246), released to centre within 0.25 s. It fires (log) and the HUD showed 'Slide'; NOT confirmed that the board turns (a half turn looks the same).
- `tricks route on|off` (primo lead-in), physics log lines carry ms.
- Verified this build: kickflip + stick to 328 = primo2 in 156 ms; kickflip + stick to 281 = casper (via primo), flips out and lands on release.

## 2026-10-13 00:0x build installed (sheets sheet_air1.png, sheet_air2.png)
- Owner on the 23:29 build: stick feel good; wants the pose caught in the air; poses still tip over ('bobble head'); flick-outs should be immediate, and a flick out of a primo should not visibly go through the casper.
- **Wipeout protection** (`protect_pose` in no_bail.cpp, `tricks protect on|off`): No Bail's own filter is leased while a pose is caught, landed or held. With it, a primo caught with the board on its side in the air (roll +90, drop 0.1 m) lands and the pose starts ~0.2 s later (speed 4.6 -> ~3 m/s). A casper turned in the air (180, or even 90) bounces 201 -> 200 -> 201 twice and loses the pose, with or without the drop: casper's board is left alone in the air (`tricks catch casper 0`). Cause not found.
- Flick out: a swipe up from the held position within 160 ms fires at once (no wait for the centre). Casper: pops and lands (verified). Primo: over to the casper for 200 ms then its flip out - pops with 250 ms (verified), not with 80. Primo2: never popped (primo2.to.casper is 50 ticks) -> `tricks primos one` (default): both sides are the primo. NOT re-run after that change.
- Fall: the hop with protection on left the skater riding after two hops; the fall now drops the protection first. NOT re-run.
- Test trap: run.ps1 matches `shot-<name>` in the whole log, so a name used in an earlier session is 'taken' at once from old lines: use unique shot names per run.

## 2026-10-13 00:15 build installed - THE POSE STARTS IN THE AIR (sheet_airpose1.png, zoom_airpose1.png)
- Owner: feet must take the pose in the air like a dark catch, and land solid; also wants the shuv.
- Dark-catch route (tested, rejected): injecting RB at the flick does fire the game's dark catch (IsDark 32 ms into the air, 'Kickflip Dark Catch'), but a dark landing stays in the game's dark slide (physics 100, IsDark) and never reaches riding, so freestyle cannot start (2 s, no 103). trace: dev\trace-dark1.csv.
- **What works: `tricks airpose on` (default).** While a pose is caught in the air the mod holds `Animation/Dingo/EnumGS.FilteredPhysicsState` at 1 (ground; 2 air, 3 grind) and Bool.Intent.TrickSetup at 1. The animation graph then runs air -> land -> (early exit) -> riding -> freestyle while the board is still up, and in physics 201 the board follows the animation, so feet and board take the pose together. Landing -> 103 in 62 ms (was 156-200 ms, and 1.1-1.3 s before the early exit). No bounce, no bail, casper included. The procedural in-air turn (`catch`) is skipped in this mode.
- Ground shuvit: with `groundshuv ms 2000` the stills show no turned board: the mechanism (CurrentGroundTrick 7 alone to reach 103) probably does not engage from riding. OPEN. Idea: the same trick as above - a ground-animation state that the board follows - or turning the board body directly.

## 2026-10-13 00:31 build installed (sheet_airpose2.png = before, sheet_airpose3.png = after)
- Owner on 00:15: still lands the flip, then plays the into-pose animation on the ground; wants the flip cancelled in the air; still rocks side to side landing with speed.
- 00:15's in-air start was too late to see: the catch latched 120 ms into the air and a casper went in through the primo. Now the catch latches from takeoff (stick steady 100 ms in its zone) and a pose caught in the air goes in directly (no primo lead-in).
- FilteredPhysicsState: not read through the 0x11dcdd0 lookup (asked 0 times). Written in place in the skater's states block every physics step (`poke_state_value`, `set_tricks_step`): 70-76 writes per air, **0 times had the game put it back** - the game only writes it on a change, so one write holds for the whole air.
- Mid-air stills now show the board turned under the feet while airborne (casper upside down with the skater leaning back - the casper-into clip's own lean; primo on its side). Landing -> 103 in 47-79 ms.
- OPEN: rocking on landing at speed; ground shuvit; the into clips are 70 (casper) / 95 (primo) ticks long, so a short air may land mid-clip.

## 2026-10-13 00:53 build installed
- Owner on 00:31: pose still only after landing; rocks far left and right at speed.
- **Rocking root cause: the left stick.** Owner's traces (dev\trace-owner-20261013a/b.csv): while a pose is held his left stick wanders over x -0.9..1.0, y up to 1.0 (the thumb is holding L3 down). Scripted with a wandering left stick: primo thrown off the board 0.5 s into the pose (103 -> 100 -> 200 -> 201 -> 504); with `tricks leftmask on` (default; `DingoSDKOverlayTricksMaskLeft` zeroes LX/LY for the game while a pose is held on the ground, casper Balance fixed at 0.5) the primo held 3.8 s until released. Casper with a wandering stick, mask off: popped into the air 0.44 s in and lost the pose.
- Casper + air pose is FLAKY: 8 clean landings earlier, but one run bounced 201 -> 200 three times from touchdown and lost the pose (mask on, before any left-stick input). Cause unknown.
- Angular-velocity writes at part +0x90 / flag 0x20 have no measurable effect in 103 (rocking meter identical with `tricks stable` on/off): that lever does nothing; the earlier 'upright' knobs probably never did either.
- Why the pose is late in the air: a flip trick's clip carries its own landing and is only left at its branch windows - to riding at branch_end_early (tick 42 of the hardflip clip, with EBool.Intent.Action and FilteredPhysicsState ground), ~0.3 s into the air. The land node has no window but needs EBool.Anim.Is.GenericLanding; forcing Bool.Anim.IsInAir.Loop/PreLand true to get it made caspers bounce and cost most of the speed (reverted; `tricks airpose states ...` to retry). The dark catch has its own early window (branch_end_fliptricktodarkcatch, tick 18). Owner's own catches latch ~0.2 s into the air (stick still travelling). NEXT IDEAS: time-warp the flip clip while a catch is armed (PTMTrickTag TimeWarpScale = Float.Anim.TrickFlipSpeed), or find what else satisfies GenericLanding.
