# Requested tricks

The requests come from the Discord thread "What new tricks would you want to see for ReSkate?" and its Reddit cross-post (2026-10-10 to 2026-10-12). Status values: `todo`, `research`, `in progress`, `needs test`, `shipped`, `parked`.

Tiers describe how a trick gets built:
- **A**: hidden content the game already ships.
- **B**: recombining existing states and clips.
- **C**: a procedural board on an existing body clip.
- **D**: needs animation the game does not have.
- **E**: a fix or naming change.

| # | Trick | Asked by | Tier | Route | Status |
|---|---|---|---|---|---|
| 1 | Casper flip as a variant of the hospital flip | strumzino, kash, Die. | A | Casper → `onboard.freestyle.casper.fliptrickexit` (two exit clips) | ready to test (click + RS up in a casper) |
| 2 | Caspers and primos | Die. | A | `onboard.freestyle.*`: hold WantsFreestyle and FreestyleTrick | needs test: casper flop fixed 2026-10-13 (root source frame held upright) |
| 2b | Anti-casper (nose-side casper) | owner | A+ | The casper played mirrored (Bool.Anim.Mirrored) with the skater turned half round, caught with the right stick resting up | needs test (headless: holds, exits, flicks out both ways) |
| 2d | Casper height on the stick | owner | A | Right stick out (edge low, toward the middle high) drives the loop's blend space (Float.Intent.Balance) | needs test |
| 2c | Caspers / primos / anti-casper from a manual | owner | B | Tail or nose manual pushed into its brake, then L3 + stick to the pose | needs test (headless: all three start and exit) |
| 3 | Casper slides (dark catch into a casper, slide on slick materials) | strumzino | A+C | Casper loop plus slide physics by material | ready to test (slide carries speed, `tricks slide`); per-material friction todo |
| 4 | Primo exit into FS shuvit | (hidden content) | A | `primo_exit_fs_shuvit` clip | ready to test (let go of the click in a primo) |
| 5 | No comply flips and shuvs | Punkrek, strumzino | B | No comply state, then a flip enum and the flip clip | ready to test: flick after the no comply starts (procedural board turn; no name/score yet) |
| 6 | Boneless/footplant fingerflips and flips | strumzino, Punkrek | B | Boneless state plus the fingerflip clips | code in (boneless/footplant opens the flick window); not verified, boneless inputs not found for the scripted pad |
| 7 | One-foot grabs with fingerflips | Punkrek | B | Grab-to-fingerflip | todo |
| 8 | Ground shuv-its, not pop shuvs (as in Session) | Don Kedic | B | Riding sent to the ground animation state for the turn; the board's frame turned 180 with a small lift | needs test: L3 held, stick down, rolled to a lower diagonal, released (fires and turns the board every step, re-checked 2026-10-14; not played by hand) |
| 9 | Push shuvs out of grinds | Gitty | B | Grind exit into a shuv | partly: pop out, then click + stick left/right (late shuvit); not run on a grind |
| 10 | Nightmare flip and Daydream flip | strumzino | C | Varial clip plus an extra roll | ready to test: tap the click in the air on a varial flip (extra flip verified on a heelflip; varials not run) |
| 11 | Dolphin flip and 360 dolphin/dragon flip | sake, DS | C | Board pitch rotation done procedurally | ready to test: in-air click with the stick up/down (dolphin turn) or on an up-diagonal (dragon flip: dolphin + 360 shuv); both checked on an ollie |
| 12 | Muska flip | Don Kedic | C | 180 body varial heelflip | todo: needs the owner to say which Muska flip is meant |
| 13 | Hurricane grind (animation and registration) | strumzino | D | No clip exists; registration only | parked |
| 14 | Grabs during grinds | Punkrek | D? | Trigger while grinding: the game's own ground grab held on the rail (grab states + the graph's physics family held at riding) | ready to test (2026-10-16: hand 0.04-0.06 m from the deck through the grind, 3/3) |
| 15 | Wallpush | Krispy Kreme Massacre | D | Wallride plus an impulse; no clip exists | parked |
| 16 | Project 8-style wallride animation | Krispy Kreme Massacre | D | New animation | parked |
| 17 | Nata spin | Luca.Antrix | D | No clip exists | parked |
| 18 | Revert flip (Skate 3 tricklining) | WinterWZ | trainer | Timing and physics only | ReSkate Trainer |
| 19 | 180 into a manual registers the wrong stance | strumzino | E | Stance bit in the manual scoring | research |
| 20 | Custom trick names ("Tre Flip", "Nightmare") | strumzino | E | Runtime display-name override, with no EBX edits | partly: mod tricks show their names as notices (`tricks names`); renaming the game's own tricks todo |
| 21 | Kickflip indy (flip grab: trigger during a flip) | owner | C | The mod's flip out and up in front, back on to the game's grab board; the game's own grab hand takes it | ready to test (2026-10-16: hand 0.07 m from the deck from ~65% to the landing, 12/12; no step over 0.05 m / 23 deg) |
