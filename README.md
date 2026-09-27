# SkyGfxDE

x64 ASI plugin for **GTA San Andreas – The Definitive Edition**. It brings back the PS2 look (in the spirit of [aap/skygfx](https://github.com/aap/skygfx)) on top of DE's modern renderer, so Unreal's dynamic shadows and volumetric clouds stay on. It also adds an in-game debug menu for weather, time, noclip and freecam.

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
- **Freecam.** Takes over TheCamera's final matrix after `CCamera::Process`, so you can compare weather at any height. The world streams around the **player**, so use *Move player to camera* before flying far.
- **Noclip.** Moves the player, or their vehicle, along the camera heading with collision off. Collision is restored when you turn it off. If you turn it off high up, you will fall.

## Look tab

Every look setting can be changed live, and **Save to ini** writes the values back to the ini.

- **PS2 colour filter.** Uses skygfx's `ColourFilter_PS2` formula (`gain = 2·postfx1 + 2·postfx2·min(1, 2·alpha2)`) on the timecyc postfx colours. The result is multiplied onto the `ColorGain` of every `AGTAPostProcessVolume`, so the brightness option keeps working. With the game's own timecyc (PC/mobile-style postfx values), it is applied as a tint only at `GameTimecycStrength`. A PS2 `timecyc.dat` set in `[Timecyc] File` gets the full filter. The PS2 version's file is `data\colorcycle.dat` (same 52-column format, 23 weathers × 8 hours); copy it next to the `.asi` as e.g. `timecyc_ps2.dat` and set `File=timecyc_ps2.dat`.
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
  - The frame is halved to PS2 scale: 2 halvings of 640×448, plus the halvings that bring this screen's height down to about 448. The first one starts 2+4 PS2 pixels in, so the glow sits 6 PS2 pixels up-left of its source, as on the PS2.
  - `D = 2·(D − limit/2)`, saturated, with `limit = m_nHighLightMinIntensity · 128/255`: the timecyc "IntensityLimit" column (`CColourSet +0x9C`, DE writes it in `CColourSet::CColourSet` from `0x14523AA00`). The PS2 timecyc has it at 90 at midnight up to 220 at midday, so nights glow more.
  - The result is added with `SRCALPHA, ONE` at alpha `Intensity`/255.
  - The PS2 had no bloom, so while radiosity is on DE's `BloomIntensity` (`FPostProcessSettings +0x21C`, override bit `+0x06` bit 2) is multiplied by `DEBloom` (default 0) on every post-process volume. A jevify pass over all 185 `FPostProcessSettings` fields confirmed `BloomIntensity` as the one strength knob of a glow that stacks with radiosity (the others are bloom shape, convolution, dirt mask, or the sun's lens flare, which is left alone).
- **Grain.** The PS2 rain grain, from `CPostEffects::Render`: a strength moves one step per frame towards `128 · CWeather::Rain` and `Grain(strength/4)` is drawn when neither the camera's nor the player's cull zone is no-rain, the camera is above water (`UnderWaterness ≤ 0`), outside (`CGame::currArea == 0`) and below 900 m. The grain is skygfx's `Grain_PS2`: a 64×64 texture of the PS2 VU random generator masked by the strength (a bitmask), tiled 5×7 per 640×448 and blended as `dst·(1 + 2a)`. Night vision / infrared grain is not ported (DE draws its own goggles).
- **Water drops.** skygfx's neo water drops (`neoWaterdrops.cpp`): up to 2000 drops on the lens, each showing a flipped, wider window of the frame.
  - Rain adds drops when outside and not looking down (`(180° − camera pitch angle − 40)/150 · Rain · 0.5` per step). DE's `water_splash_big` / `water_splash` / `water_splsh_sml` FX (`0x140AE7C60`, `0x140AE7E60`, `0x140AE8020`) within 10 m splash the lens.
  - Drops slide away from the centre when moving forward and sideways when the camera turns, leave traces, and fade over 2 s. They are cleared underwater (`UnderWaterness > 0.34`), hidden in cutscenes and in first person on foot, and off with a top-down camera or when looking around from a first-person car.
  - The simulation runs at a fixed 30 Hz, as the original's per-frame steps assume. The drop mask is drawn procedurally (skygfx reads `dropmask` from `neo\neo.txd`, which isn't shipped here). Not hooked: boat splash/wake particles, hydrants/fountains, and blood drops (off by default in skygfx).
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
2. With the game closed (a running game locks the `.asi`), copy `bin\SkyGfxDE.asi` and `bin\SkyGfxDE.ini` to `<Game>\Gameface\Binaries\Win64\`.

`SkyGfxDE.log` (next to the `.asi`) logs every hook at startup. After that, every 10 s it logs the postfx colours, the applied gain, the shadow and indirect values, and the fog density (DE value → applied value).

## Build and check

- `build.bat` (VS 2022 x64 build tools) produces `bin\SkyGfxDE.asi`.
- `test\run_check.bat <SanAndreas.exe> <timecyc.dat>` maps the real exe and runs the plugin's own `Install()`: every look and tool signature, the code-layout checks, the timecyc table verification and all hooks. It asserts all resolved addresses against the IDA values, loads the timecyc into the mapped tables, and writes `test\check_result.txt`. A second run on the same inputs produces an identical file.
- `py -3.12 test\e2e_postfx.py` (game closed, the save must load by itself, hands off for ~2.5 minutes) launches the game, types the rainy weather cheat, checks that `CWeather::Rain` rises and that the log shows the hooks and the pre-HUD draw with no exceptions, and writes `test\e2e_postfx.txt`. Screenshots before/after the rain go to `%TEMP%`.

Every address is found by signature. If a look signature doesn't match, nothing is installed. If only a tool signature doesn't match, the look still works and the World tab says the tools are unavailable. Verified on the current Steam/RGL `SanAndreas.exe` (SHA-256 `ed7545eb…ac1f0`).

## Source layout

| File | Contents |
|---|---|
| `src/core.cpp` | Config/ini, hotkeys, log, signature scanning, look hooks, Look tab |
| `src/tools.cpp` | Weather/time/freecam/noclip hooks and World tab |
| `src/overlay.cpp` | D3D11 Present/ResizeBuffers hooks, ImGui, input capture |
| `src/dllmain.cpp` | Entry point; installs the overlay from a worker thread |
| `imgui/` | Dear ImGui 1.92 (MIT) with DX11/Win32 backends |

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
