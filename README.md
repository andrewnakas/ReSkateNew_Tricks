# ReSkate Tricks

**New tricks for skate. (2025) on ReSkate:** caspers, primos, flick-outs, dolphin and dragon flips, no comply flips, ground shuvs, grab grinds, kickflip indys, nose and tail pre-grabs, and more. They are built from the game's own animations, states and physics, plus a procedural board where the game has no clip.

ReSkate Tricks is a fork of [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate) (GPL-3.0). Everything ReSkate does still works the same way: see [README-ReSkate.md](README-ReSkate.md) for installing, offline play and hosting. Tricks is an **addon**: open the menu's **EXTRAS** and go to **TRICKS**. Every trick has its own switch there, along with its settings and a HOW TO page with pad diagrams. The settings are saved in `tricks.json`.

> **Status: work in progress.** Each trick listed as proven here passed scripted controller runs in the real game, with logged numbers and stills. Feel and looks still need hands-on play, and some tricks are still experimental. The full lab notebook is [STATUS.md](STATUS.md), and the per-trick tracker is [TRICKS.md](TRICKS.md).

---

## Where the requests came from

The trick list comes from the community: the Discord thread *"What new tricks would you want to see for ReSkate?"* and its Reddit cross-post (October 2026), plus the owner's own ideas. [TRICKS.md](TRICKS.md) credits every request and sorts it into a tier:

| Tier | How it gets built |
|---|---|
| **A** | Hidden content the game already ships (caspers, primos, the casper flip's exit clips) |
| **B** | Existing states and clips put together in new ways (no comply flips, ground shuvs, grab grinds) |
| **C** | A procedural board on an existing body clip (dolphin, dragon, nightmare flip, kickflip indy) |
| **D** | Needs animation the game doesn't have (hurricane, wallpush, nata spin): parked |
| **E** | A fix or a naming change (custom trick names, stance registration) |

## The tricks

### Freestyle: caspers and primos
| Trick | How | State |
|---|---|---|
| **Casper / primo** | Land into one, or hold it with L3 and the right stick. The right stick picks where (casper, primo, primo2). | Proven. The casper flop is fixed (the root's source frame is held upright), and landings don't bounce. |
| **Anti-casper** (nose side) | The casper mirrored, with the skater turned half round. Caught with the right stick resting up. | Needs a test |
| **Casper height on the stick** | Stick at the edge is low; toward the middle is high (the loop's own blend space) | Needs a test |
| **Caspers and primos from a manual** | Push a nose or tail manual into its brake, then L3 and the stick | Proven headlessly |
| **Casper slides** | A dark catch into a casper; the slide carries speed | Ready to test |
| **Primo exit into FS shuvit** | Let go of the click in a primo (a hidden game clip) | Ready to test |
| **Casper flip / flick-outs** | Click with the right stick up in a casper, or flick out of a casper or primo. The board rolls back over: half flip, bigger flip, tre... | Proven: 9/9 casper flick-outs landed. Primo flick-outs land 6/6 on both primo sides: they turn only in the air the game predicts, and take 85% of it. |

### Flips
| Trick | How | State |
|---|---|---|
| **Dolphin flip / 360 dolphin (dragon)** | A straight-up or straight-down stroke for the dolphin. A dragon is a dolphin plus a 360 shuv, from a lower diagonal. Works nollie and fakie too. | Proven (owner approved) |
| **Nightmare / daydream flip** | Tap the click in the air on a varial flip: one more roll | Ready to test |
| **Late flip, double flip, late shuvit** | In-air extras | Proven headlessly |
| **No comply flips and shuvs** | Flick after the no comply starts | Proven: they land |
| **Ground shuvit** (Session style, no pop) | L3 held, the stick down and rolled to a lower diagonal, then let go. FS and BS. | Proven: 6/6 on a flat street |
| **Kickflip indy (flip grab)** | Hold the trigger during a flip. The board comes out and up in front and flips; the game's own grab hand catches it and brings it back under the feet. | Proven: the hand stays 0.07 m from the deck from about 65% of the trick to the landing (12/12). A no-teleport guard caps every frame at 0.05 m / 23°. |

### Grabs
| Trick | How | State |
|---|---|---|
| **Grab grinds** | Trigger while grinding: RT indy, LT melon, both for a double grab. They score with the grind's own name ("Indy BS 50-50", "Double Grab BS 50-50"). A tweaked grab landed on a rail is a fall. | Proven: the hand stays 0.04–0.06 m from the deck, in a deep crouch |
| **Pre-grab (nose / tail held while riding)** | Trigger plus the right stick up for the nose or down for the tail. Ollie or nollie out of it with the hand on (Playcub Ollie, Nose / Tail Grab Ollie), or flick a finger flip. | Proven headlessly. The hand is drawn at the render hand-off. |
| **Both hands on the board** | The other trigger adds that hand on its rail, or on the nose / tail with the left stick. A smaller ollie or nollie comes out of it. | New, needs a test |
| **Fandango** | A in a crooked grind takes the back foot off | Experimental, off by default |

### Score and names
- The mod's tricks are named as notices, added to the game's own trick list and counted in the LINE score.
- Grab grinds take the game's own grind names.

### Parked (needs animation the game doesn't have)
Hurricane grind, wallpush, the Project 8-style wallride and the nata spin. The revert flip lives in [ReSkate Trainer](https://github.com/andrewnakas/reskate-trainer).

---

## How it works

None of the game's files are edited. Everything happens at runtime, inside ReSkate's extension layer.

- **The game's own pops.**
  - The mod doesn't fake an ollie. It plays a right-stick *stroke* to the game's input polls, and the game's own gesture reader pops the trick. So the height, the timing and the scoring are the game's.
  - The stick is hidden from the game only during the moves that need it hidden. A stock kickflip or pop shuv shows 0 masked polls and 0 hook turns in every regression run.
- **The game's own animation graph.**
  - Caspers, primos and grabs are the game's own hidden freestyle and grab states, held in the animation graph.
  - Example: a grab grind holds the graph's physics family at *riding*, so the game's ground grab keeps playing on the rail.
- **A procedural board where there is no clip.**
  - Flips the game has no animation for (dolphin, dragon, flick-outs, the kickflip indy's flip) are turned on the board's own physics body: the *kinematic spin*.
  - The spin blends onto the game's own board target at the end. It ends under the rider's feet, and a guard keeps any single frame from jumping.
- **Edits at the render hand-off.**
  - Where the drawn pose has to change (a hand on the nose, a leg lifted), it is edited at the point the renderer takes it. Edits made earlier are overwritten before the frame is drawn.
- **Tested headlessly.**
  - `dev/pass.ps1` boots the game with a scripted controller (`tricks pad ...`), runs a trick macro and logs numbers: hand-to-deck distance, air time, the worst frame step.
  - It also takes stills mid-trick (`tricks spinshot`).
  - A trick is called proven only when those numbers pass.

The reverse-engineering notes are kept in [STATUS.md](STATUS.md). They cover the hand grab IK context, the graph state names, the grab type enums and the grind type enums.

## Building

Build the same way as ReSkate (CMake, Visual Studio 2022 x64; see [README-ReSkate.md](README-ReSkate.md)). `dev/build.cmd` wraps it. Copy `ReSkate.dll` and `ReSkateLauncher.exe` from the build into your ReSkate folder, and **back up the old ones first**.

The console has a `tricks` command family for every trick and setting: `tricks status`, `tricks enable <trick> on|off`, `tricks set <setting> <value>`, plus dev tools (`tricks pad`, `tricks spinshot`, `tricks trace`).

---

## Roadmap: into ReSkate Studio's trick tool

Today every trick lives in code (`Extension/Tricks`), and its tuning is in `tricks.json` and the `tricks` console. The plan for a future build is to merge all of it into a **trick studio in ReSkate Studio**: tricks become things you author and tune there, not things you change in C++.

1. **Tricks as data.** Each trick becomes a definition:
   - its input (the stick stroke, the trigger and the timing window);
   - the game states and clips it holds;
   - the board's turn (roll / yaw / pitch, its rate curve, the dip and the no-teleport limits);
   - its grab rules (which hand, which rail, nose or tail);
   - its landing rules and its name and score.

   The runtime in this repo becomes the player that reads those definitions.
2. **Studio panels for today's switches.** The TRICKS page and the `tricks` console verbs (stroke, dip, grab rise and reach, flick-out timing, casper height zones) become live sliders with a board-path preview, instead of hand-edited JSON.
3. **Shareable trick packs.** Trick sets are exported the way Studio already builds map and asset packs, so players can share new tricks without a new DLL.
4. **The test rig in Studio.** The headless pad macros and the proof numbers (hand-to-deck distance, air time, worst frame step) run from Studio. A trick then ships only when its numbers pass, the same rule this branch follows.
5. **Upstream.** Once the tricks are data-driven and stable, offer the addon back to ReSkate.

### Open items before then
- Goofy-stance runs of the primo and casper flick-outs.
- Feel checks from play:
  - the kickflip indy's board height;
  - the primo short-hop quick flip;
  - the casper flick-out's pop, which is stock heelflip height.
- The rare "first BS ground shuv plays another flip": not reproduced. A log line now records it when it happens.
- Not done yet:
  - one-foot grabs with finger flips;
  - the Muska flip;
  - renaming the game's own tricks;
  - the stance bit for a 180 into a manual.

## Credits

- [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate) and its contributors: the base this addon is built on.
- The community members who asked for these tricks, credited by name in [TRICKS.md](TRICKS.md).
- Built by [andrewnakas](https://github.com/andrewnakas) with Claude Code.

ReSkate Tricks is released under the **GPL-3.0**, like ReSkate ([LICENSE](LICENSE)). skate. is a trademark of Electronic Arts. This project is not affiliated with EA or Full Circle, and it ships no game files.
