# SacrificeModern

A native C++20 / x64 reconstruction of **Sacrifice** (Shiny Entertainment, 2000), built to run the original game's data on modern Windows.

> **Work in progress.** Much of the game already runs: menus, campaign flow, skirmish, terrain, creatures, spells, combat, HUD, music and sound. It is not yet a complete port.

## Working so far

- Menus, campaign flow, the single-player campaign and offline skirmish against the computer
- Terrain, creatures, spells, combat, HUD, music and sound
- Saved games, stored in the port's replay journal format
- Direct3D 11 renderer with XAudio2 audio; FFmpeg decodes audio and video
- Optional Remastered Look, smoothed drawing between ticks and enemy health bars (each marked in code as a modern addition)
- F10 developer inspector, F9 free camera, `--debug-trial` quick start, and `sacrifice_tools` for command-line asset inspection
- An experimental WebGPU/WebAssembly browser build (see below)

Not included: multiplayer and the Scapex map editor.

Known issues: there are still bugs in spells, the computer opponent AI and the campaign, so expect some spells, enemy behaviour and missions to differ from the original or break.

## Contributing

Contributions are welcome. Bug reports with steps to reproduce, fixes, and comparisons against the original game's behaviour all help, especially for spells, the AI and the campaign. Open an issue or a pull request.

## Requirements

- Windows with **Visual Studio 2026** (Desktop development with C++, Windows SDK)
- An original copy of Sacrifice (e.g. GOG). No game files are included in this repository.

CMake, Ninja and vcpkg are found automatically. Dependencies are pinned in `vcpkg.json` and built into `out/` on first configure. FFmpeg is a pinned prebuilt LGPL shared build (see `cmake/ffmpeg-prebuilt.cmake`), downloaded into `out/ffmpeg` instead of being compiled.

## Build

```powershell
.\build.ps1 -Preset release
```

Other presets: `debug`, `asan`. Add `-Package` to produce a standalone build in `out/package`.

## Run

```powershell
.\out\release\SacrificeModern.exe --game-dir C:\Path\To\Sacrifice
```

The game folder is found automatically if the build sits inside it. Only the original `data` and `maps` folders are read, never modified.

Useful extras:

- `--debug-trial`: skip the menus and jump straight into the Trial skirmish map
- **F10**: developer inspector (assets, maps, models, animations, audio)
- **F9**: free camera
- `sacrifice_tools.exe`: command-line asset validation and inspection

## Web build (outdated)

There is also a WebGPU browser version:

```powershell
.\build.ps1 -Web -Preset release -Package -Serve
```

**The WebGPU renderer is outdated** and lags behind the native Direct3D 11 renderer. It may be missing recent features or not work at all.

## License

[GNU AGPL v3](LICENSE).

## Third-party

FFmpeg (prebuilt LGPL build from BtbN/FFmpeg-Builds), plus zlib, nlohmann-json and Dear ImGui via vcpkg. Their licenses are installed with the package.

Sacrifice is a trademark of its respective owners. This project is an unofficial fan reconstruction and is not affiliated with them.
