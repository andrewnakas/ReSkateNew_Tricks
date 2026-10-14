# ReSkate Tricks: status

## START HERE (written 2026-10-13 ~04:30)
**The 04:26 build is installed** (the trainer session may swap it overnight; it was told how to put it back: both files from `build\vs2022-x64\Release`). It carries the four things the owner asked for on 2026-10-13. All of it is checked by scripted runs only: **the owner's hands are the next test.**
- **Casper caught in the air:** the feet and board take the pose 63 ms after the catch (it was ~560 ms, at touchdown). Lands straight into the pose, no bounce: 11 of 11 scripted landings, 3 of them with a wandering left stick.
- **Primo:** starts in the air 0.33 s before touchdown (not at the catch: started earlier, its board is on edge at touchdown, gets knocked flat and the game steps the skater off). 7 of 7 held. Making it start at the catch is OPEN.
- **Flip-outs:** casper kickflip, heelflip, tre flip and primo kickflip all pop and land riding; a new trick or catch can follow as soon as the flip-out is down (it was locked for 2.5 s).
- **Ground shuv:** the roll gesture now turns the board (it never did before): riding goes to the ground animation state for 0.45 s, the board turns 180 with a 5 cm lift, speed kept. A held quarter turn is in `dev\shots\sheet_i1.png` (bottom middle).
- **Open, in order:** (1) owner playtest of all four; (2) primo from the catch; (3) what the skater's stance / the game makes of the board after a ground shuv (not seen in a still); (4) primo rocking: the meter reads 6 to 70 degrees but includes the board's own roll into the pose, so it needs eyes or a better meter; (5) `FreeStyleFlipTrick` 16 and primo2 flick-out, untouched; (6) the dead levers `stable` / `level` / `upright` are still in the code.
- Read the 04:30 entry at the bottom for mechanisms, knobs and what failed. Test helper: `dev\pass.ps1` (several passes per game start, prints only this run's lines).
- Committed locally on `tricks` (not pushed). D: stalled for an hour during the night (see the bottom entry).
- The "READY TO TEST" tables just below describe the first evening's click + flick controls (`tricks scheme flick`); the default is the owner's hold scheme described in the bottom entries.


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

## 2026-10-13 04:30 build installed: early casper, no bounce, re-catch, ground shuv (sheets sheet_i1.png, sheet_j1.png)
Plan: `C:\Users\andre\.claude\plans\peppy-stirring-moler.md`. Every number below is from `dev\pass.ps1` runs on the THPS training map (kickflip or heelflip, one push, ~17 km/h).

**Early catch (casper).**
- New dev log line `Tricks: nodes trick/land/riding` (reads Bool.State.Is.TrickInAir, Is.Landing, Onboard.Riding). It showed the old belief was wrong: with FilteredPhysicsState held at ground the graph was already in the landing node 16 ms after the catch, but that node carries the flip clip on in phase and only leaves at the clip's own early window, ~560 ms later, 30 ms before touchdown.
- Writing `EnumGS.LastFilteredPhysicsState` = 2 as well (`tricks airpose last on|off`, default on) changed nothing on its own.
- **What works:** one term of the graph's generic landing held true from the catch, `Bool.GP.DidInAirTuck` (now the default of `tricks airpose states`; `Name=value` holds an enum). The landing node then plays a generic landing clip whose early exit is at tick 4: landing node for 63 ms, then riding -> freestyle. Pose 63 ms after the catch.
- A term that turns true later does nothing (the row is chosen when the landing node is entered).

**The bounce.** With the pose complete in the air the casper lands on its deck (contact log: wheels 0, truck 0, deck 1) and the selector's landing check holds the air state: 201 -> 200 -> 201 twice, pose lost. This is the same failure as the old `IsInAir.Loop / PreLand` "dead end": it was never those states, it was landing in the pose.
- **Fix (`tricks stomp on|off`, default on):** in the state selector hook (`choose_state`, no_bail.cpp), while a pose is caught, a grounded board (wheels, truck or deck, as the selector counts it) in 200 / 201 for 0.04 s goes straight to 103; a grounded 103 is not left for 200 / 201 either. Result: 201 -> 103 in one step, rocking meter 0.3 degrees (it read 18 to 24 on the old route).
- With a wandering left stick one casper still came back off the ground once (103 for 80 ms, then air, then caught again). The left stick is now hidden in the air too while a pose is caught (`tricks leftmask air on|off`, default on: it costs spinning into a pose). After that: 3 of 3 clean, rocking 5 to 14 degrees.

**Primo.** Started at the catch it is at its way-in clip's tick ~31 on touchdown, board on edge; the board is knocked flat and ~0.55 s later the game asks for a dismount (103 -> 504; stills: skater standing beside an upside-down board). Refusing the dismount (`tricks keep on`, default off) only hides it. A softened touchdown (`tricks soft <ms>`: no part moves up or down for that long) did not help.
- **What works:** the whole in-air start held back until `Float.Physics.TimeToLand` (read in place from the states block, `peek_state_value`) is at most 0.33 s (`tricks airpose primoleft <s>`; `primodelay <ms>` from takeoff stands in if it cannot be read). 0.27 and 0.35 hold, 0.42 fails. It then lands on its wheels (201 -> 100 or 101 -> 103).

**Flip-outs.** Casper kickflip / heelflip / tre flip and primo kickflip (via the casper, 200 ms): all pop, land in 100 and ride. The lock after a flip-out (`leaving`, 2.5 s) now ends 150 ms after the flip-out's touchdown: a second kickflip into a casper straight after a casper kickflip was caught and landed. Untouched: value 16, primo2.

**Ground shuv.** Cause of "fires but does not turn": riding holds the board on its physics, so CurrentGroundTrick 7 never got the physics from 100 to 103. Now the selector hook sends 100 -> 103 for the length of the turn (`hold_ground_animation`), 7 keeps it, the board hook turns the frame +-180 with a lift at the middle (`tricks groundshuv hop <m>`, default 0.05). Log: 103 for ~450 ms, frame turned on every step (27 of 27); the owner's roll gesture fires it both ways; speed kept (4.7 m/s in, 4.6 m/s 0.9 s later). The "10.7 m/s" in the log line at the end of a shuv is the telemetry's one-part speed, not real. `tricks groundshuv quarter` holds a quarter turn for 4 s for a still. The slide holds the mean of the parts' speed during a shuv (`set_freestyle_slide_mean`).

**Test rig notes.** `dev\pass.ps1 -Tag x1 -Passes '<commands>',...` (macros KICKCASPER, KICKPRIMO, ...LONG). Stills taken with `echo shot-` land seconds late (the script polls the log): only a pose held for several seconds can be photographed.

**The machine.** From 01:37 to 02:30 D: stalled (queue of 7, no throughput, a one-line write took 12 s) while memory was also full; my linker and the trainer session's both hung in I/O wait. Two links at once on D: seem to trigger it.

## 2026-10-13 21:08 build: skater kept upright, casper landing bounce

- Owner (playing the 09:50 build): catches look great, board mostly stable, but the skater bends at the ankles until face or back hits the ground on landing; later a ~30 degree whole-body rock and a bounce on casper landings.
- Measured (dev trace, `tricks rootup watch`): board's physical frame and the clip's board pose both still, yet the rig's root frame (rig +0x4720) rolls to ~90 degrees in 0.6 s and back in 1.3 s. It is rebuilt each step by the root update 0x47784b0 (called from the rig-follow 0x4773e40 / 0x4774f50); a write after the physics step is overwritten and made it worse. Fix: hook 0x47784b0, set the root's up row straight up right after it (heading latched while upright). `tricks rootup on|off|watch` (default on). Stills dev\shots\u1 (fixed) vs k1 (game's) at +0.6 s.
- Casper bounce: lands deck first 15 cm into the ground and was thrown 1.7 m back up (state stayed 103, so the state log never showed it). Slide `soft` (no vertical motion) now defaults to 300 ms: board settles in 63 ms.
- Not the cause: Balance blend, rider part velocities (soft 3000 / upright 1 changed nothing), the clip's board pose.
- Checked headlessly on San Vansterdam: casper, primo, casper and primo kickflip flip-outs all land riding. Owner playtest pending.

## 2026-10-13 21:55: casper body sink is physical (open)

- Owner on the 21:08 build: casper still flops (butt on the ground), primo bends a little at the ankles, legs looked twisted. The upright root (hook 0x47784b0) did not change the drawn body; now off by default (`tricks rootup on` to compare; the root's inverse at rig +0x4760 is kept in step with it).
- New dev trace (rootup watch): the rider's 25 physics parts' height over the board. Primo: highest 1.32 m, steady. Casper: 0.86 at touchdown, sinks to 0.2-0.4 m by 0.6-1.2 s, stands back up to 1.2 m by 1.7 s. So the body is the ragdoll being pulled down, not the root frame.
- No effect: snap 50 / 64 (not the way-in clip), soft 3000, upright 1, Balance 0 / 1, held states bool.physics.muscles.followanimation 1, bool.anim.twoboneanimfeet.enabled 0, bool.slopeadjust.leftfootenabled 0, bool.anim.forcedisableragdollcollision 1 (all found and written).
- Lead: the five rider-to-deck drives (world-step.md 1351: pelvis anchor 0.905 m "up" in the deck's frame, feet 0.04-0.11 m, stiffness 1e4). With the deck upside down that anchor is under the ground; on edge it is sideways (primo's small ankle bend). Their per-step update is not in private/decomp; next = find and decompile it (ghidra project in private/ghidra), then flip the anchors' up while a casper is held.

## 2026-10-13 23:35 build: casper flop fixed, anti-casper added

**The flop (owner: "butt touches the ground").** Measured, not guessed (dev\rf2..rf7.txt):
- The rider-to-deck drives are not the cause. Rows found by body pair: r43..47 fixed (pelvis point 0.90 m up the deck), r55..57 set again by the game each step. Turning the fixed rows to an upright deck (`tricks riderframe on`) changed nothing; with the root upright as well it got worse. Every other drive on the rider's bodies is static through the sink. `riderframe` stays as a dev command, off.
- The clip's board in skater space (rig +0x49e0) is a steady casper (up -0.96) throughout. The root update's position source is board part 8, also steady.
- **The cause:** the root update (0x47784b0) builds the root from the frame at `[rig + 0x59b0] + 0x5b0` (rows side/up/forward, +0x5e0 position). In a casper that frame tips onto its side: up row 1.00 -> 0.06 at 0.55 s -> back by 1.5 s, side row going to 1.00. The root copies it and the rider parts follow ~60 ms later (highest part 0.86 -> 0.20 m).
- **Fix:** in the root-update hook, before the game's update, that frame is set upright on its heading (`upright_root_source`, client_noclip.cpp; `tricks rootup on`, now the default, also keeps the old after-update fix). Casper: highest part 0.88 -> 1.0 by 0.4 s -> 1.17 m steady; primo unchanged 1.29..1.33. Owner watched the run: "that looked good".
- Checked: casper hold + L3 exit, casper kickflip flick-out (pops, lands riding), primo hold. One run was knocked off by a car (San Vansterdam traffic): not the trick.

**Anti-casper** (`tricks anti on|off|steady <ms>`, on): the game ships no clips, so it is the casper with `Animation/Dingo/Bool.Anim.Mirrored` held (A/B stills rfaplain* vs rfamir*: the flag mirrors the body). Caught with L3 + the right stick resting up (between the primos), 150 ms steady and not in the first 120 ms of the air (a flick passes the top). Owner on the first try (flag held only while freestyle was on): it snapped to the plain casper right before the way in and again at the end. Now the mirror goes on at the catch (in the air, before the way in) and stays on after the pose until the way out is in the air, the skater is off the board, or riding again. In the pose the stick rules are the casper's upside down (swipe down = flick out, centre = fall). The board's held tilt now takes its sign from the clip at the latch (anti: forward row -0.30, the other end down). Verified headlessly: catch at 96 deg, mirror on at physics 201, 12 s hold upright (1.30 m), L3 exit mirror off in the air, lands riding; swipe-down kickflip out pops and lands. Still: dev\shots\rfbantiA.png. Owner feel check pending (which foot is on top).

NEXT: ground entries from manuals (tail manual fully down + L3 + stick -> casper / primo; nose manual fully down -> anti-casper); then the right-stick board height (owner: later).

## 2026-10-13 23:58 build: poses from manuals

- Owner's ask: the manual pushed all the way down, then into a casper or primo (tail) or the anti-casper (nose).
- Measured: the manual's brake (stick fully back / fully forward) is a node of its own: `Bool.Anim.InTailManual` / `InNoseManual` go false and `Bool.Anim.ManualBrake` true while it plays. So the brake is credited to the manual seen before it (`manual_kind`).
- Rule (`manual_entry`, on): the brake of a tail manual seen in the last 0.4 s + L3 + the stick resting in a pose's place (casper or primo) starts that pose on the ground, the same way the air catch does; a nose manual's brake allows the anti-casper (stick up).
- Verified headlessly (dev\rfd.txt): tail manual brake + L3 + down -> casper "from a manual", 103 held 3.3 s, body 1.2 m, L3 exit to riding; the same then stick to the side -> primo (via the casper; the script pressed L3 with the stick still down), put down to riding; nose manual brake + L3 + up -> anti-casper, mirror on before the pose, off in the exit's air, riding.
- Dev log line `Tricks: manual tail/nose/brake ...` on every change.

## 2026-10-14 00:03 build: manual entries start their way in from tick 0

- Owner on the 23:58 run: "the nose manual to anti casper bounces and flips around".
- Change: a pose started from a manual sets the casper/primo way-in transitions to start at tick 0 (the board turns over from where the manual has it); an air catch puts the snap ticks (22 / 38) back. `set_transition_start` now keeps the two transition assets (cleared on leaving a level), so a catch costs two writes.
- Headless (dev\rfe.txt): nose manual -> anti-casper still shows a lift in the rider parts (1.55 -> 1.77 m at 0.27 s, then 1.32 steady); air-caught casper unchanged (0.83 -> 1.16, no flop). Owner's eye pending on whether the flip-round is gone. If not: the mirror switching on at once on the ground (no air to hide it) is the next suspect; and the lift may be the into clip's own pop.
