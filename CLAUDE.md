# Project notes for Claude

Native Nintendo 3DS port of **Sonic Mania**. This is the owner's fork
(`tinaut1986/RSDKv5-Decompilation`) of SaturnSH2x2's New 3DS port, which is built on
the RSDKv5 decompilation (RSDKModding / Rubberduckycooly). Goal: make it run well on
the **Old 3DS** by moving the renderer from CPU to GPU (citro3d).

The owner already shipped ports of the same kind: Zelda Minish Cap
(`../zelda-tmc-3ds`), Metroid Zero Mission (`../mzm`), Super Metroid (`../sm-3ds`).
They are the reference for workflow, release process, debug tools, GPU renderers
with palette handling, bottom-screen UI and Old 3DS pacing.

**Start every session by reading [`docs/PLAN.md`](docs/PLAN.md).**

## Language

- Chat with the owner in **Spanish (castellano)**.
- Code, comments, identifiers, commit messages and docs: **English**.
- Inherited engine code keeps its own style; do not churn it.

## Layout

| Path | What |
|---|---|
| `RSDKv5/` | The engine (renderer, audio, input, storage). Where the port work happens. |
| `RSDKv5/RSDK/Graphics/Backend/` | `RenderBackend` interface + `SoftwareBackend` (from the `gfx-rewrite` branch). The GPU backend plugs in here. |
| `RSDKv5/RSDK/Graphics/CTR/` | 3DS render device (presentation, frame pacing). |
| `dependencies/Sonic-Mania-Decompilation` | Game logic (submodule). `Game` is a symlink to its `SonicMania/` folder. |
| `3ds/` | RSF, banner, icon, `makerom`/`bannertool`, `build_cia.sh`. |

Game data is **never committed**: `Data.rsdk` from the owner's Steam copy
(Flatpak Steam: `~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Sonic Mania/`)
goes to `sdmc:/3ds/Sonic Mania 3DS/` on the console (`UserStorage.cpp`).

## Build

```sh
./build_3ds.sh            # RSDKv5U.elf / .3dsx in bin/3DS/CTR/
./build_3ds.sh --cia      # also 3ds/out/SonicMania.cia
```

- Needs devkitPro. `libtheora` for 3DS is not in the owner's portlibs and installing it
  needs sudo, so it is unpacked into `.deps/` (gitignored) from
  `pkg.devkitpro.org/packages/3ds-libtheora-1.2.0alpha1-2-any.pkg.tar.xz` (needs a
  pacman user agent). The script picks it up by itself.
- After switching branches run `rm -rf bin` first: make does not notice header changes
  across branch switches and links stale objects.
- The Makefile does not add `-DRETRO_USE_MOD_LOADER=0` or the `Game/` include paths;
  `build_3ds.sh` does.
- Title ID `0x5A230`, name "Sonic Mania 3DS", data folder `sdmc:/3ds/Sonic Mania 3DS/`.
  The SaturnSH2x2 build (`0x3082`, `/3ds/SonicMania/`) can stay installed next to it.

## Git and GitHub

- Everything goes to `origin` = `tinaut1986/RSDKv5-Decompilation`. `upstream` =
  SaturnSH2x2, push URL set to `DISABLED`. Never push or open PRs upstream.
- Same workflow as the owner's other ports:
  - `main` = stable; only `--no-ff` merges of a finished release branch.
  - `release/vX.Y.Z` = working line. Topic branches (`feat/`, `fix/`, `perf/`, `chore/`,
    `docs/`) are cut from the release branch, one topic each, merged back with
    `--no-ff` once the owner has checked them on the console.
  - Beta = tag on the release branch; stable = tag on `main` (push `main` before the tag).
    After every tag rename the release branch to the next patch.
  - Never commit directly on `main` or `release/*`. Commit only when the owner asks.
- `master` is the fork's GitHub default branch (SaturnSH2x2's history); we do not use it.
- Releases/CI/version script are not set up yet: see PLAN (B0.3).

## Tests

Test on hardware whenever performance is involved; emulators say nothing about Old 3DS timings.
