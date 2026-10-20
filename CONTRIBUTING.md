# Working on ReSkate Tricks

ReSkate Tricks is a branch of [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate) that adds new tricks to
skate.: caspers and primos, no comply flips, dolphin / nightmare flips, flip grabs, grabs on rails, the
pre-grab (hand on the nose or tail while riding), fingerflips and more. In game it is the **TRICKS** addon
under the menu's EXTRAS.

- Working branch: **`tricks-2.1`** (ReSkate 2.1.0 merged in). `tricks` is the older line on ReSkate 2.0.x.
- [TRICKS.md](TRICKS.md): every requested trick, who asked, how it is built and where it stands.
- [STATUS.md](STATUS.md): the dev log, newest first. Read its top section before starting; it says what was
  last built, what is proven and what is still open.

## Building

Same requirements as ReSkate (see [README.md](README.md#building-from-source)): Visual Studio 2022 with the C++
workload and CMake 3.24+. Then either the normal preset build, or:

```bat
dev\build.cmd            :: configures once, builds Release, log in dev\build.log
dev\build.cmd ReSkate    :: one target
```

`DINGOSDK_VERSION` (default 2.1.0) is stamped in on the first configure only.

## Trying a build

Copy `build\vs2022-x64\Release\ReSkate.dll` and `ReSkateLauncher.exe` into your ReSkate game folder (keep
a copy of the ones you replace), then start the launcher. The Tricks page in the menu lists every trick
with its controls; the console verb `tricks` (`tricks status`, `tricks <name> ...`) drives them by hand.

## The headless test rig (`dev/`)

| Script | What it does |
|---|---|
| `run.ps1` | Installs this build over the game's ReSkate files (backing them up to `dev\backup\<stamp>`), boots a map, runs console commands, prints the matching log lines, closes the game and puts the old files back. `-KeepInstalled` leaves the build in. |
| `pass.ps1` | Several timed passes of `run.ps1` in one boot, with pad macros. |
| `jumps.py`, `sheet.py` | Read `ReSkate.log` into per-jump tables / contact sheets. |

Settings, all optional:
- `SKATE_DIR`: the game folder (default `C:\Program Files (x86)\Steam\steamapps\common\Skate`).
- `RESKATE_SHOT_SCRIPT`: a script called as `<script> shot <png> <width>` to grab the window at every
  `echo shot-<name>`. Without it, runs still work but take no stills.

Pads are scripted through the console, e.g. `tricks pad a:200 n:500 rt=1+ly=1:400 rt=1:3000` (button or
axis = value : milliseconds, `n` = nothing held). STATUS.md has the pads used for each feature.

`dev/` is ignored apart from these scripts and the reference traces (`dev/trace-*.csv`); logs, stills,
backups and scratch output stay local.

## Rules

- Never commit game files, dumped assets, keys or captures of retail data. Code and our own notes only.
- Keep STATUS.md's top section current when you finish a piece of work: what changed, what is proven
  (and how), what is not.
- One trick or fix per commit, message in the form `Tricks: <what the player sees>`.
- Open a pull request against `tricks-2.1` rather than pushing to it directly.

## License

GPL-3.0, as ReSkate. See [LICENSE](LICENSE).
