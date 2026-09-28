# SkyGfxDE

x64 ASI plugin for **GTA San Andreas – The Definitive Edition**. It brings back the PS2 look (in the spirit of [aap/skygfx](https://github.com/aap/skygfx)) on top of DE's modern renderer, so Unreal's dynamic shadows and volumetric clouds stay on. It also adds an in-game debug menu for weather, time, noclip and freecam.

> Not affiliated with Rockstar Games, Grove Street Games or the skygfx authors. No game files are included; you need your own copy of the game.

## Features at a glance

- **PS2 colour filter**, driven by the timecyc (skygfx's `ColourFilter_PS2`), converted through Unreal's filmic curve so it matches skygfx on screen.
- **PS2 timecyc support**: load the PS2 `colorcycle.dat` (or any 8/24-hour `timecyc.dat`) into DE's tables at runtime.
- **Timecyc colours** for sky, fog, ambient, sun and clouds on DE's time-of-day actor.
- **GTA-style distance fog** from the timecyc far clip, following the time of day, using DE's own GTA fog path.
- **Darker PS2-style shadows** (indirect fill lowered by the timecyc shadow strength), with DE's dynamic shadow maps kept.
- **Radiosity** (the PS2 glow), with DE's bloom turned down while it runs.
- **SpeedFX** (the original speed blur), ported from the 1.0 exe, including nitro, looking behind/sideways and cutscenes.
- **PS2 rain grain** and **neo water drops** on the lens (rain, splashes, boats, hydrants and fountains).
- **Matte characters**: less plastic-looking ped skin, clothes and hair.
- All post effects are drawn **before the HUD**, so the HUD stays sharp.
- **PC-style draw distance**: DE dropped the PC draw-distance factor from its LOD multiplier; it is put back (default 1.8, adjustable).
- **Debug menu** (Dear ImGui): force or blend weather, set/freeze/speed up time, freecam, noclip. All keys are Ctrl+Shift chords, so it works on 60% keyboards.

## Requirements

- GTA San Andreas – The Definitive Edition, current Steam/Rockstar Games Launcher build (`SanAndreas.exe` SHA-256 `ed7545eb…ac1f0`). Other builds are refused safely (see *Build and check*).
- An ASI loader, e.g. [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).
- Optional: the PS2 `colorcycle.dat` for the full PS2 colours (not included; it's Rockstar's data).

## Keys

All defaults are Ctrl+Shift chords, so they work on 60% keyboards (no F-keys, arrows or numpad needed). You can rebind them under `[Keys]` in `SkyGfxDE.ini`.

| Key | Action |
|---|---|
| Ctrl+Shift+M | Open/close the menu (World and Look tabs) |
| Ctrl+Shift+E | Turn the look on/off (A/B comparison) |
| Ctrl+Shift+R | Reload `SkyGfxDE.ini` |
| Ctrl+Shift+F | Freecam |
| Ctrl+Shift+N | Noclip |

In freecam and noclip: WASD moves, Space goes up, C goes down, the mouse looks around, Shift is ×5, Alt is ×0.2, and the mouse wheel changes freecam speed. While either is active, keyboard and mouse input is kept away from the game.

## World tab (debug tools)

- **Weather.** Three modes:
  - *Game*: the normal region cycle.
  - *Force*: holds one of the 23 weathers.
  - *Blend A → B*: sets the interpolation directly, to compare two weathers side by side.
  
  The panel also shows the current weather pair, the blend value, the forced weather and the region.
- **Time.** Set hour and minute, jump to one of the 8 timecyc keyframes (00, 05, 06, 07, 12, 19, 20, 22), freeze the clock, or change clock speed (0.1× to 60×).
- **Draw distance.** DE's `CCamera::Process` sets `TheCamera.m_fLODDistMultiplier` (`+0x104`) to `70 / FOV` only; the original multiplied that by `CRenderer::ms_lodDistScale`, the PC draw-distance slider (1.2 default, up to 1.8). So DE draws every model and LOD closer than PC even at default settings, which shows as pop-in when driving or flying. SkyGfxDE multiplies it by `[World] LodDistance` (default 1.8, the PC maximum) right after `CCamera::Process`, before the render list is built; cutscenes keep DE's fixed value. The multiplier feeds entity visibility (`0x141173E80`), sector and area streaming (`0x141174A50`, `0x141286CD0`) and the in-car ped range. DE also caps visibility at bound radius + 700 m, so values past ~2.5 change little. Not part of the look toggle.
- **Freecam.** Takes over TheCamera's final matrix after `CCamera::Process`, so you can compare weather at any height. The world streams around the **player**, so use *Move player to camera* before flying far.
- **Noclip.** Moves the player, or their vehicle, along the camera heading with collision off. Collision is restored when you turn it off. If you turn it off high up, you will fall.

## Look tab

Every look setting can be changed live, and **Save to ini** writes the values back to the ini.

- **PS2 colour filter.** Uses skygfx's `ColourFilter_PS2` formula (`gain = 2·postfx1 + 2·postfx2·min(1, 2·alpha2)`) on the timecyc postfx colours. That gain is a display-space multiply; DE's `ColorGain` works on linear colour before Unreal's filmic tone curve, so each channel is converted to the `ColorGain` that gives the same display change at mid grey (UE 4.26 `FilmToneMap` with the film settings of DE's outdoor post-process volume, which overrides slope and toe, then sRGB). A plain gamma-2.2 power, used before, crushed the weak channels in the curve's toe: PS2 LA midday blue came out at 0.37 instead of skygfx's 0.64, which made midday strongly yellow. Shadows still get slightly more of the tint and highlights slightly less, since a gain before a tone curve can't be a display multiply everywhere. The result is multiplied onto the `ColorGain` of every `AGTAPostProcessVolume`, so the brightness option keeps working. With the game's own timecyc (PC/mobile-style postfx values), it is applied as a tint only at `GameTimecycStrength`. A PS2 `timecyc.dat` set in `[Timecyc] File` gets the full filter. The PS2 version's file is `data\colorcycle.dat` (same 52-column format, 23 weathers × 8 hours); copy it next to the `.asi` as e.g. `timecyc_ps2.dat` and set `File=timecyc_ps2.dat`.
- **Grade.** Saturation and contrast multipliers on DE's own values.
- **Atmosphere.** Two modes:
  - **GTA fog** (default). Distance fog based on the original's timecyc far clip, using DE's own GTA fog path (`AGTAHeightFog::bUseGTAValues`) while the modern lighting stays.
    - DE's GTA-fog density mostly follows a per-weather value rather than the timecyc distances. Left as is, it was too thick and the distance slider did nothing, so it is replaced. The air is clear up to half the scaled far clip, and the fog reaches `FogOpacity` (default 0.5) at the scaled far clip and gets thicker beyond it, hiding the low-poly LODs.
    - `FogDistance` multiplies the far clip. The default is 1.8, the original PC draw-distance slider at maximum.
    - The start distance is written into DE's fog override data (class default object + 0x220), which DE copies to the component every frame.
    - The original's own fog started near 0 m (timecyc fog start median 10 m), which is too thick for DE's full-distance world.
    - `test/fog_rows.txt` lists all 552 timecyc rows at the defaults: 469 are clear at 300 m. The other 83 are Foggy SF, rain, sandstorm and one special-effects row, whose far clips are 250 m or less, so they stay foggy as in the original.
    - Volumetric fog stays on: the setting `gta.ShowVolumeFogInClassic` is held on during the fog update.
  - **Modern fog** (`GtaFog=0`). `Haze` scales DE's height fog, and `GroundHaze` removes DE's extra fixed ground-haze layer (a 0.02-density second fog that the original doesn't have).
- **Timecyc colours.** Sky top/bottom, fog, ambient, sun and cloud colours take the timecyc hue. DE's luminance is kept, so auto-exposure stays calibrated.
- **Shadows.** The indirect fill that lights shadowed areas is lowered by `Darkness × timecyc shadow strength`. The sun's shadow maps are untouched, and interior volumes are skipped.
- **SpeedFX.** The original's speed blur, ported from `CPostEffects::SpeedFX` in gta_sa.exe 1.0 US (`0x7030A0`; gta-reversed doesn't reverse it) and the call in `CPostEffects::Render`.
  - The trigger: the player is in a vehicle that isn't a plane, heli, boat or train (type `+0x890`), and not in a cutscene (`CCutsceneMgr::ms_running`, found through its use in `CTimeCycle::CalcColoursForPoint`). The speed `|m_vecMoveSpeed|` picks a row of the original's table (`0x8D5190`).
  - Nitro: a car with `bNosInst` (`handlingFlags +0x5C0 & 0x80000`) whose `m_fTireTemperature` (`+0xC0C`) is below 0 uses `clamp(2 · (moveSpeed · forward) · (m_GasPedal (+0x710) + 1), 0, 1)` when the dot product is above 0.2, even in cutscenes. The offsets come from DE's `CAutomobile::NitrousControl` (`0x14138F290`).
  - The table rows: at 0.6 / 0.7 / 0.8 / 0.9 / 0.93 / 0.96 / 1.0 units per frame the effect draws 1 / 2 / 3 / 3 / 4 / 4 / 5 passes, with shift 4 and wobble 0 / 0 / 0 / 0 / 1 / 2 / 3. 0.6 units per frame is about 108 km/h.
  - Each pass draws a copy of the frame at alpha 36 (`m_SpeedFXAlpha`) with point sampling and clamp. Its UV rectangle shrinks by `shift × 0.0025` per pass, plus a per-frame `rand()` wobble of `wobble × 0.004`, with the original's corner signs.
  - Looking direction (`TheCamera.m_aCams[m_nActiveCam].m_nDirectionWasLooking`; DE: TheCamera `+0x5B` is the active cam, cams are `0x1B8` bytes, the field is at `+0x1C4`): looking behind gives no visible effect; looking sideways halves the shift, removes the wobble and keeps only the right-edge stretch, as in `0x7030A0`.
  - It is drawn before the HUD: just before the backbuffer's second render-target bind of the frame (`OMSetRenderTargets` hook, `[SpeedFX] HudBind`). DE binds the backbuffer 3 times a frame (598 of 600 frames measured) and draws the HUD after the second bind. `TestMode=1` forces the full effect, as the original's `m_bSpeedFXTestMode` did.
- **Radiosity.** The PS2 glow, `CPostEffects::Radiosity` as skygfx's "PS2" path, with gta_sa.exe 1.0's values (filter passes 2, render passes 1, intensity 35, filter correction 2/2).
  - The frame is halved to PS2 scale: 2 halvings of 640×448, plus the halvings that bring this screen's height down to about 448. The first one starts 2+4 PS2 pixels in, so the glow sits 6 PS2 pixels up-left of its source, as on the PS2. `Offset` (0–12 PS2 pixels, default 6) changes that distance.
  - `D = 2·(D − limit/2)`, saturated, with `limit = m_nHighLightMinIntensity · 128/255`: the timecyc "IntensityLimit" column (`CColourSet +0x9C`, DE writes it in `CColourSet::CColourSet` from `0x14523AA00`). The PS2 timecyc has it at 90 at midnight up to 220 at midday, so nights glow more.
  - The result is added with `SRCALPHA, ONE` at alpha `Intensity`/255.
  - The PS2 had no bloom, so while radiosity is on DE's `BloomIntensity` (`FPostProcessSettings +0x21C`, override bit `+0x06` bit 2) is multiplied by `DEBloom` (default 0) on every post-process volume. A jevify pass over all 185 `FPostProcessSettings` fields confirmed `BloomIntensity` as the one strength knob of a glow that stacks with radiosity (the others are bloom shape, convolution, dirt mask, or the sun's lens flare, which is left alone).
- **Grain.** The PS2 rain grain, from `CPostEffects::Render`: a strength moves one step per frame towards `128 · CWeather::Rain` and `Grain(strength/4)` is drawn when neither the camera's nor the player's cull zone is no-rain, the camera is above water (`UnderWaterness ≤ 0`), outside (`CGame::currArea == 0`) and below 900 m. The grain is skygfx's `Grain_PS2`: a 64×64 texture of the PS2 VU random generator masked by the strength (a bitmask), tiled 5×7 per 640×448 and blended as `dst·(1 + 2a·Strength)` (`[Grain] Strength`, 0–2, 1 = PS2). Night vision / infrared grain is not ported (DE draws its own goggles).
- **Water drops.** skygfx's neo water drops (`neoWaterdrops.cpp`): up to 2000 drops on the lens, each showing a flipped, wider window of the frame.
  - Rain adds drops when outside and not looking down (`(180° − camera pitch angle − 40)/150 · Rain · 0.5` per step). DE's `water_splash_big` / `water_splash` / `water_splsh_sml` FX (`0x140AE7C60`, `0x140AE7E60`, `0x140AE8020`) within 10 m splash the lens.
  - Drops slide away from the centre when moving forward and sideways when the camera turns, leave traces, and fade over 2 s. They are cleared underwater (`UnderWaterness > 0.34`) and during the fade when entering or leaving a building (`CEntryExitManager::ms_exitEnterState != 0`, `0x1451B4A28`), hidden in cutscenes and in first person on foot, and off with a top-down camera or when looking around from a first-person car.
  - More sources, as in skygfx: boat splash, wake and water splash particles (`FxSystem_c::AddParticle` `0x140AEFA60` on `Fx_c`'s `prt_boatsplash` / `prt_wake` / `prt_watersplash`, within 40 / 10 / 30 m, `1/(dist/2)` screens of drops), and hydrants and fountains (DE's particle-audio call for `FX_water_hydrant` / `FX_water_fountain` / `FX_water_fnt_tme`, `0x141005E30`, a 20-step splash within 10 m).
  - The simulation runs at a fixed 30 Hz, as the original's per-frame steps assume. The drop mask is drawn procedurally (skygfx reads `dropmask` from `neo\neo.txd`, which isn't shipped here). Blood drops (off by default in skygfx) are not ported.
- All post effects (water drops, SpeedFX, radiosity, grain, in the original's order) are drawn before the HUD at the `HudBind` backbuffer bind, and follow the look toggle.
- **Characters.** Makes peds matte. DE's character materials come from three glossy master materials:
  - skin, `M_Character_VGD`: `GlobalRoughness` 0.5, with a subsurface profile;
  - clothes, `M_Character_Clothes_VGD`: `Roughness` 0.9 × texture, `Specular` 0.5;
  - hair, `M_Character_Hair_VGD`: `Roughness` 0.3, `Specular` 1.0.
  
  Every loaded instance of those masters has its roughness pulled towards 1 and its specular multiplied by `1 − Matte`. Instances are rescanned every second as peds stream in. The change undoes itself when the look is toggled off. The redesigned shapes and textures of the DE character models themselves (the "cartoony" part) can only be changed by replacement models in a pak mod.

Keep the in-game *Classic Atmosphere* option **off**. It destroys the volumetric clouds and swaps in Grove Street's override curves; with it on, only the filter, grade and shadows apply.

## Weather regions

DE keeps all 23 weathers, and its five region weather lists (Countryside, LA, SF, Vegas, Desert; 64 entries each, at `0x145031DC0`–`0x145031ED0`) are **byte-identical** to the original game's (`Weather.def` in gta-reversed). Fog in SF, rain in SF/Countryside and sandstorms in the desert all still occur in their regions at the original rates.

## Install

1. You need an ASI loader for DE (e.g. Ultimate ASI Loader as `version.dll`/`dxgi.dll` in `Gameface\Binaries\Win64`).
2. Download `SkyGfxDE.asi` and `SkyGfxDE.ini` from [Releases](../../releases) (or build them, see below). With the game closed (a running game locks the `.asi`), copy both to `<Game>\Gameface\Binaries\Win64\`.
3. Optional, for the full PS2 colours: copy the PS2 version's `data\colorcycle.dat` next to the `.asi` as `timecyc_ps2.dat` and set `[Timecyc] File=timecyc_ps2.dat` in the ini.
4. In the game, keep *Classic Atmosphere* off. Press Ctrl+Shift+M for the menu.

`SkyGfxDE.log` (next to the `.asi`) logs every hook at startup. After that, every 10 s it logs the postfx colours, the applied gain, the shadow and indirect values, and the fog density (DE value → applied value).

## Build and check

- `build.bat` (VS 2022 x64 build tools) produces `bin\SkyGfxDE.asi`.
- `test\run_check.bat <SanAndreas.exe> <timecyc.dat>` maps the real exe and runs the plugin's own `Install()`: every look and tool signature, the code-layout checks, the timecyc table verification and all hooks. It asserts all resolved addresses against the IDA values, loads the timecyc into the mapped tables, and writes `test\check_result.txt`. A second run on the same inputs produces an identical file.
- `py -3.12 test\e2e_postfx.py` (game closed, the save must load by itself, hands off for ~2.5 minutes) launches the game, types the rainy weather cheat, checks that `CWeather::Rain` rises and that the log shows the hooks and the pre-HUD draw with no exceptions, and writes `test\e2e_postfx.txt`. Screenshots before/after the rain go to `%TEMP%`.

Every address is found by signature. If a look signature doesn't match, nothing is installed. If only a tool signature doesn't match, the look still works and the World tab says the tools are unavailable. Verified on the current Steam/RGL `SanAndreas.exe` (SHA-256 `ed7545eb…ac1f0`).

## Source layout

| File | Contents |
|---|---|
| `src/core.cpp` | Config/ini, hotkeys, log, signature scanning, timecyc loader, look hooks (colour filter, colours, fog, shadows, bloom), Look tab |
| `src/postfx.cpp` | D3D11 post effects: water drops, SpeedFX, radiosity, grain |
| `src/tools.cpp` | Weather/time/freecam/noclip hooks, World tab, game state for the post effects (speed, rain, camera, splash hooks) |
| `src/peds.cpp` | Matte characters (material scalar overrides through `ProcessEvent`) |
| `src/overlay.cpp` | D3D11 Present/ResizeBuffers/OMSetRenderTargets hooks, ImGui, input capture |
| `src/dllmain.cpp` | Entry point; installs the overlay from a worker thread |
| `test/` | Offline check against the real exe, E2E game scripts and their artifacts |
| `imgui/` | Dear ImGui 1.92 (MIT) with DX11/Win32 backends |
| `minhook/` | MinHook (BSD 2-clause) |

## Reverse-engineering notes (DE x64)

| What | Where |
|---|---|
| `CGame::Process` order | clock tick (inlined) → `CWeather::Update` → … → `CCamera::Process` (`sub_141146380`) |
| `CTimeCycle::Update` / `Initialise` | `0x141182230` / `0x14114C250` |
| `CColourSet::CColourSet` | `0x1411816B0` (RW layout 0xAC, then `FSkyColorSet` at +0xAC) |
| `CTimeCycle::m_CurrentColours` | `0x145067020` |
| `AGTATimeOfDay*` | `*(qword_145724750) + 0x688`; target colours +0x458, live colours +0x2B8 |
| Classic Atmosphere flag | `byte_145024151` (`r.gta.UseLightingOverrides`), set by `0x140BB3F00` |
| `AGTAPostProcessVolume::UpdateColorOptions` | `0x140B7DD90` |
| `AGTAHeightFog::UpdateColors` | `0x140B65920` (component +0x2A8: FogDensity +0x1F8, SecondFogData.FogDensity +0x200) |
| `ShouldUseGTAFog` | `0x140B65820`: true if `bUseGTAValues` (fog actor +0x2A0), `gta.overridefog` (`0x145724FD8`), or Classic + time-of-day `bAllowOverrides` |
| GTA fog inputs | time-of-day +0x37E8 / +0x37EC = timecyc FogStart / FarClip × 100 (cm), written by `CTimeCycle::Update`; `m_CurrentColours` +0x54 / +0x50 |
| GTA fog density | `BaseFogDensity` (fog actor +0x29C) / max(5e-6·(far − start), 0.01) · (far − start) / far, blended with weather values |
| `gta.ShowVolumeFogInClassic` | `0x145724FDC` (`UpdateColors+0x84`); without it the GTA path clears `bEnableVolumetricFog` (component +0x268) |
| `UActorComponent::MarkRenderStateDirty` | `0x1431313B0` |
| `CWeather::Update` | `0x1412903B0` |
| `CWeather` Old / New / Forced (int16) | `0x145300000` / `0x1452FFFF0` / `0x145300018` (-1 = none) |
| `CWeather::InterpolationValue` / `WeatherRegion` | `0x1452FFFE8` (float) / `0x145300048` (int16) |
| `CWeather::FindWeatherTypesList` | `0x141291330`; lists at `0x145031ED0` (default), `DC0` LA, `E00` SF, `E40` Vegas, `E80` Desert |
| `CClock::SetGameClock(h, m, day)` | `0x14112B980` |
| `CClock` hours / minutes / seconds | `0x14521270B` / `0x14521270F` / `0x14522A584` (uint16) |
| `CClock` last tick / ms per game minute | `0x14522A58C` / `0x14522AD00` |
| `CTimer::m_snTimeInMilliseconds` | `0x1452397F8` |
| `FindPlayerEntity` (vehicle if driving, else ped) | `0x14116EE70` (ped+0x634 bit 0x100 = in vehicle, vehicle at ped+0x7C8) |
| `CCamera::Process` | `0x14111B2E0`; TheCamera at `0x1453E13E0`, `m_matrix` pointer `0x1453E13F8` |
| `FNamePool` | `0x14570CDC0` (lea rdx in the name-entry accessor `0x141A7BFF0`) |
| `UObject::ProcessEvent` | vtable slot `0x43` (`0x141C7B6B0`) |
| `UMaterialInstance` | `Parent` +0xD0, `ScalarParameterValues` +0xE0 (stride 0x24, value +0x10), resource +0x140 |
| `SetScalarParameterValueInternal` / `GameThread_UpdateMIParameter` | `0x1433AADD0` / `0x1433BC7B0` (native body of `MaterialInstanceDynamic:SetScalarParameterValue`, thunk `0x14389D3B0`) |
| Timecyc tables | `[8 hours][23 weathers]` byte arrays, RVAs in `kTimecycCols` (`core.cpp`) |

## How it was made

The Definitive Edition still runs the original game code (rewritten for x64) underneath Unreal Engine 4.26. Most of the work was finding the original game's functions and data in DE's executable and connecting them to Unreal's renderer:

- **Finding things in DE.** DE has no symbols, so functions and globals were matched in IDA against the original game's layout from [gta-reversed](https://github.com/gta-reversed/gta-reversed). Examples: nitro through the unique `-0.000001` constant stored by `CAutomobile::NitrousControl`, cutscene and cull-zone flags through the `CTimeCycle` fog-reduction check, and the entry/exit state through its `!= 3 ? 0 : 4` reset. Every address is then found at runtime by a unique byte signature, and `test/offline_check.cpp` asserts them against the IDA values.
- **Unreal side.** Class layouts (post-process settings, fog, time of day, materials) come from an SDK dump of the game made with [Dumper-7](https://github.com/Encryqed/Dumper-7).
- **SpeedFX.** gta-reversed has the call site in `CPostEffects::Render` but not the blur itself, and skygfx calls the game's function. So the body (`0x7030A0`), its speed table (`0x8D5190`) and alpha were read from the 1.0 US `gta_sa.exe` in IDA and rebuilt as a D3D11 pass. To keep the HUD sharp, the backbuffer binds per frame were counted (DE binds it 3 times; the HUD is drawn after the second), and the effect runs just before that bind.
- **Radiosity, grain, water drops, colour filter.** Ported from skygfx's source (`postfx.cpp`, `neoWaterdrops.cpp`), with the original game's values, to D3D11 shaders.
- **Checks.** A WARP (software D3D11) harness checked the effects pixel by pixel during development; the E2E scripts in `test/` launch the real game and check its log.

## Credits

- **[aap](https://github.com/aap)** and the contributors of **[skygfx](https://github.com/aap/skygfx)**: the PS2 colour filter formula, radiosity, PS2 grain, the neo water drops, and the research into the PS2 look that this project is built on. The water drop simulation in `src/postfx.cpp` follows skygfx's `neoWaterdrops.cpp` closely.
- **The [gta-reversed](https://github.com/gta-reversed/gta-reversed) team**: the reversed original code (`CPostEffects::Render`, `CTimeCycle`, `CWeather`, `CColourSet`, `CEntryExitManager`, camera and weather layouts) used to find everything in DE.
- **Rockstar North** for the original game, its PS2 look and its timecyc; **Grove Street Games** for the Definitive Edition.
- **[Encryqed](https://github.com/Encryqed)** for [Dumper-7](https://github.com/Encryqed/Dumper-7), used to dump DE's Unreal classes.
- **[Omar Cornut](https://github.com/ocornut)** and contributors for [Dear ImGui](https://github.com/ocornut/imgui) (MIT, `imgui/LICENSE.txt`).
- **[Tsuda Kageyu](https://github.com/TsudaKageyu)** for [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-clause, `minhook/LICENSE.txt`).
- **[ThirteenAG](https://github.com/ThirteenAG)** for [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).
- **[Hex-Rays](https://hex-rays.com/)** IDA, used for all the reverse engineering.

## License

The original code in this repository is under the MIT license (`LICENSE`). Dear ImGui and MinHook keep their own licenses. Logic ported from skygfx and gta-reversed remains credited to their authors above; neither project publishes a license, so if you are one of those authors and want something changed or removed, please open an issue.
