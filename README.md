# SkyGfxDE

x64 ASI plugin for **GTA San Andreas – The Definitive Edition** that brings back the PS2 look (in the spirit of [aap/skygfx](https://github.com/aap/skygfx)) on top of DE's modern renderer, so Unreal's dynamic shadows and volumetric clouds stay on.

## Why not "Classic Atmosphere"?

The in-game Classic Atmosphere option (`r.gta.UseLightingOverrides`) does not use the original timecyc. It swaps DE's sky curves for Grove Street's own override set (`AGTATimeOfDay::VGDOverrides`), destroys the volumetric cloud actor, spawns the old mesh clouds and fog, and clamps the sun colour. Meanwhile DE still runs the original RenderWare `CTimeCycle` every frame; the modern renderer just ignores most of it. This plugin feeds the timecyc back into the modern renderer.

## What it does

All changes are made every frame after DE's `CTimeCycle::Update`:

1. **PS2 colour filter.** It uses skygfx's `ColourFilter_PS2` formula (`gain = 2·postfx1 + 2·postfx2·min(1, 2·alpha2)`) on the timecyc's postfx colours. The result is converted to linear and multiplied onto the `ColorGain` of every `AGTAPostProcessVolume`. DE writes that gain itself from the brightness option in `UpdateColorOptions`, so the plugin multiplies rather than replaces it, and brightness keeps working.
2. **Timecyc colours.** Sky top/bottom, fog (sky bottom, as in the original), ambient/sky light, sun and volumetric-cloud colours take the timecyc hue. DE's luminance is kept, so auto-exposure and HDR stay calibrated.
3. **Darker dynamic shadows.** `IndirectLightingIntensity`, the sky light and indirect fill that light shadowed areas, is lowered by `Darkness × timecyc shadow strength`. Shadows are therefore darkest at midday and softer at night or when overcast. The sun's shadow maps are untouched, and interior volumes are skipped.
4. **Optional timecyc.dat.** A PS2 (or any) `timecyc.dat` can be loaded into DE's timecyc tables. Both 8-hour and 24-hour files work; for 24-hour files the hours of the game's 8 slots are used.

With Classic Atmosphere on, only the filter and shadow parts apply. Keep it **off**.

### PS2 timecyc

skygfx's PS2 filter is designed for the **PS2 timecyc**. DE's own timecyc has PC/mobile-style postfx values (postfx2 alpha 255, strong colours). Applied at full strength, those turn the whole screen orange. So:
- **Game timecyc (default):** the filter is applied as a tint only, at `GameTimecycStrength` (0.35).
- **PS2 timecyc:** put the PS2 `timecyc.dat` next to the `.asi` and set `[Timecyc] File=timecyc.dat`. The filter then runs as configured (`Strength`, `KeepBrightness`). The PS2 file is not included; it comes from your PS2 copy of the game or from community packs.

## Install

1. You need an ASI loader for DE (e.g. Ultimate ASI Loader as `version.dll` / `dxgi.dll` in `Gameface\Binaries\Win64`).
2. Copy `bin\SkyGfxDE.asi` and `bin\SkyGfxDE.ini` to `<Game>\Gameface\Binaries\Win64\` while the game is closed (a running game locks the `.asi`).
3. In game: **F10** toggles the effect for A/B comparison, and **F11** re-reads `SkyGfxDE.ini` so you can tune it live.

`SkyGfxDE.log` (next to the `.asi`) logs the hooks at startup, then every 10 s the current postfx colours, the applied gain, shadow strength, indirect intensity and the number of tracked volumes.

## Build

`build.bat` (VS 2022 x64 build tools) produces `bin\SkyGfxDE.asi`.

## Check against the real exe

`test\run_check.bat <SanAndreas.exe> <timecyc.dat>` maps the exe and runs the plugin's own `Install()`: all signatures, code-layout checks, timecyc table verification and hook installation. It then loads the timecyc file into the mapped tables. The results go to `test\check_result.txt`; repeated runs on the same inputs produce an identical file.

Every address is found by signature. If anything doesn't match, the plugin installs nothing and the game runs unmodded. Verified on the current Steam/RGL `SanAndreas.exe` (SHA-256 `ed7545eb…ac1f0`).

## Reverse-engineering notes (DE x64)

| What | Where |
|---|---|
| `CTimeCycle::Update` | `0x141182230` |
| `CTimeCycle::Initialise` (TIMECYC.DAT) | `0x14114C250` |
| `CColourSet::CColourSet` | `0x1411816B0`; RW layout 0xAC (PC), then an `FSkyColorSet` at +0xAC |
| `CTimeCycle::m_CurrentColours` | `0x145067020` |
| `AGTATimeOfDay*` | `*(qword_145724750) + 0x688`; target colours +0x458, live colours +0x2B8 |
| Classic Atmosphere flag | `byte_145024151` (`r.gta.UseLightingOverrides`), set by `0x140BB3F00` |
| `AGTAPostProcessVolume::UpdateColorOptions` | `0x140B7DD90` (writes ColorSaturation/Contrast/Gamma/Gain) |
| Timecyc tables | `[8 hours][23 weathers]` byte arrays, RVAs in `kTimecycCols`; postfx alphas stored as in the file, DirectionalMult forced to 1.28 |
