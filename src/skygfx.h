#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------- config (SkyGfxDE.ini)
struct Hotkey {
    bool ctrl = false, shift = false, alt = false;
    int vk = 0;          // 0 = unbound
    char text[32] = "";  // as written in the ini
};

struct Config {
    bool  enabled = true;
    int   logLevel = 0;
    // keys (modifier chords by default: works on 60% keyboards without F-keys/arrows/numpad)
    Hotkey keyMenu, keyToggle, keyReload, keyFreecam, keyNoclip;
    // colour filter
    bool  filter = true;
    float filterStrength = 1.0f;
    float gameTimecycStrength = 0.35f;
    float keepBrightness = 0.5f;
    // grading on top of DE's own values
    float saturation = 1.15f;
    float contrast = 1.05f;
    // timecyc colours
    float skyStrength = 1.0f, fogStrength = 1.0f, ambientStrength = 1.0f, sunTint = 0.35f, cloudTint = 0.5f;
    float brightness = 1.0f;   // sky/fog brightness from the timecyc and the original's day/night sun (0 = DE's)
    float nightExposure = -0.5f; // EV added to DE's outdoor exposure at night (x the original's day/night balance)
    // atmosphere
    float haze = 0.35f;        // DE main height fog density / opacity multiplier (modern fog only)
    float groundHaze = 0.0f;   // DE's fixed second fog layer (0.02) multiplier (modern fog only)
    bool  classicSky = true;   // Classic Atmosphere's sky, clouds, sun and fog; the modern lighting kept
    bool  gtaFog = false;      // ClassicSky=0 only: fog from the timecyc far clip (DE's GTA fog path), modern sky kept
    float fogDistance = 1.8f;  // GTA fog: timecyc far clip multiplier (original PC draw distance slider max)
    float fogOpacity = 0.5f;   // GTA fog: opacity at the scaled far clip; clear up to half of it
    // shadows
    float shadowDarkness = 0.5f;
    // Post effects drawn by the overlay before the HUD (postfx.cpp)
    bool  speedFx = true;          // CPostEffects::SpeedFX, the original's speed blur
    int   speedFxHudBind = 2;      // draw before the Nth backbuffer bind of a frame (the HUD's); 0 = at Present
    bool  speedFxTestMode = false; // CPostEffects::m_bSpeedFXTestMode: full effect (input 1.0) always
    bool  radiosity = true;        // CPostEffects::Radiosity, PS2 highlight glow (limit = timecyc highlight column)
    int   radiosityIntensity = 35; // m_RadiosityIntensity (gta_sa.exe 0x8D5118)
    float radiosityOffset = 0.2f;  // glow offset up-left in PS2 pixels (the PS2's was (2^passes - 1) * correction 2 = 6)
    float deBloom = 0.0f;          // DE's BloomIntensity multiplier while radiosity is on (the PS2 had no bloom)
    bool  grain = true;            // PS2 rain grain (CPostEffects::Render rain branch, skygfx Grain_PS2)
    float grainStrength = 1.0f;    // grain alpha multiplier (1 = PS2)
    bool  waterDrops = true;       // skygfx neo water drops on the lens (rain, water splashes)
    int   maxDrops = 2000;         // most drops on screen at once (skygfx MAXDROPS 2000)
    float lodDistance = 1.8f;      // TheCamera.m_fLODDistMultiplier x this (PC draw distance slider: 1.2 default, 1.8 max)
    // characters: roughness towards 1, specular x(1 - matte) on DE's glossy ped materials
    float pedMatte = 0.6f;
    // distant lamp coronas (coronas.cpp, Project2DFX SALodLights)
    bool  coronas = true;
    float coronaSize = 0.5f;      // SALodLights CoronaRadiusMultiplier
    float coronaIntensity = 1.0f; // UGTACoronaComponent intensity at full alpha
    float coronaFarClip = 0.0f;   // m; 0 = timecyc far clip x1.8 (g_look.farClip)
    // street lights (streetlights.cpp): DE's lamp light components
    float lampDrawDistance = 150.0f;  // m: gta.streetlightdistance (DE: 4 cm, lamp lights culled); 0 = DE's
    float lampShadowDistance = 50.0f; // m: gta.streetlight.shadowdistance (DE: 4 cm, no shadows); 0 = DE's
    bool  otherLightShadows = true;   // park bollards cast shadows (DE: bNeverCastShadows)
    float bollardBrightness = 0.5f;   // park bollard light intensity multiplier (1 = DE's)
    // timecyc
    char  timecycFile[MAX_PATH] = "timecyc_ps2.dat";
    // tools
    float freecamSpeed = 20.0f;      // m/s
    float noclipSpeed = 15.0f;       // m/s
};
extern Config g_cfg;

// SpeedFX table, gta_sa.exe 1.0 US 0x8D5190 (7 x {speed, passes, shift, wobble}); m_SpeedFXAlpha 0x8D5104 = 36.
struct SpeedFxRow { float speed; int passes, shift, wobble; };
constexpr SpeedFxRow kSpeedFx[7] = { { 0.6f, 1, 4, 0 }, { 0.7f, 2, 4, 0 }, { 0.8f, 3, 4, 0 }, { 0.9f, 3, 4, 0 },
                                      { 0.93f, 4, 4, 1 }, { 0.96f, 4, 4, 2 }, { 1.0f, 5, 4, 3 } };
constexpr int kSpeedFxAlpha = 36;
extern volatile int g_speedFxRow; // tools.cpp (game thread) -> postfx.cpp (render thread)

// Game state for the post effects, written by the game thread once per frame after CCamera::Process, read by the
// render thread (plain values; a torn read only affects one frame).
struct FxState {
    float cam[16];      // TheCamera matrix rows: right, forward, up, pos (x, y, z, pad)
    float rain;         // CWeather::Rain
    int   grain;        // PS2 grain alpha mask for this frame, 0 = none
    bool  noRain;       // WaterDrops::NoRain: cull zone no-rain (camera or player), interior, or noDrops
    bool  noDrops;      // WaterDrops::NoDrops: CWeather::UnderWaterness > 0.339731634
    bool  dropsEnabled; // not a top-down camera, not looking around in a 1st-person car
    bool  firstPerson;  // camera mode MODE_1STPERSON (16)
    bool  hideDrops;    // cutscene, or 1st-person camera on foot
};
extern FxState g_fx;
extern volatile int g_splash;  // WaterDrops::ms_splashDuration request from DE's water splash FX (game thread)
extern volatile LONG g_dropFill; // WaterDrops::FillScreenMoving amount (float bits) from boat splash / wake particles
extern char g_dir[MAX_PATH];
extern char g_iniPath[MAX_PATH];

void ReadIni();
bool SaveIni();
bool ParseHotkey(const char* text, Hotkey& out);
bool HotkeyDown(const Hotkey& k);

void Log(int level, const char* fmt, ...);
void OpenLog();
extern FILE* g_logFile;

// ---------------------------------------------------------------- binary helpers
extern uint8_t* g_moduleBase;
bool InitTextSection();
uint8_t* FindUnique(const char* pattern);
uint8_t* RipAt(uint8_t* insn, const uint8_t* opcode, size_t opLen, size_t insnLen);
bool Install();       // look hooks + tools hooks; false = unsupported build, nothing installed
bool InstallTools();  // called by Install
bool InstallPeds();   // called by Install
bool InstallCoronas(); // called by Install, before MH_EnableHook
bool InstallStreetLights(); // called by Install

// ---------------------------------------------------------------- look (core.cpp)
extern bool g_active;             // effect on/off (toggle key / panel)
extern bool g_customTimecyc;
extern bool g_timecycTableOk;
extern uint8_t* g_curColours;     // CTimeCycle::m_CurrentColours
extern uint8_t** g_singleton;     // DE engine singleton
extern uint8_t* g_classicFlag;
extern uint8_t* g_objects;
extern uint8_t* g_namePool;       // FNamePool
uint8_t* ObjectItem(int32_t index); // GObjects slot (FUObjectItem) or nullptr
void PedsFrame();                  // game thread, from the look hook
void PedsPanel();                  // render thread, Look tab
bool LoadTimecycFile(const char* path);
void ApplyTimecycFile();
void CoronasFrame();               // game thread, from the look hook
void CoronasPanel();               // render thread, Look tab
void StreetLightsFrame();          // game thread, from the look hook
void OtherLightShadowsFrame();     // game thread, from the look hook
void StreetLightsPanel();          // render thread, Look tab
// UObject / FName helpers (peds.cpp), game thread
namespace UO { constexpr size_t Flags = 0x08, Class = 0x10, Name = 0x18, Outer = 0x20, Index = 0x0C; constexpr int ProcessEventSlot = 0x43; }
int32_t FindName(const char* s);   // comparison index of an ANSI name in FNamePool, or -1 (walks the pool)
bool NameIs(int32_t idx, const char* s);
int32_t NameOf(const uint8_t* obj);
int32_t ClassOf(const uint8_t* obj);
bool LiveAt(int32_t i, const uint8_t* obj); // GObjects slot i still holds obj, not PendingKill/Unreachable
bool OnGameThread();
// Slider over [lo, hi] + a box for typed values within [hardLo, hardHi] (core.cpp, render thread)
bool SliderBox(const char* label, float* v, float lo, float hi, float hardLo, float hardHi, const char* fmt = "%.3f", int flags = 0);
bool SliderBoxInt(const char* label, int* v, int lo, int hi, int hardLo, int hardHi);

struct LookStats {
    float gain[3];
    float indirect;
    float shadow;
    int volumes;
    bool classic;
    void* tod;
    unsigned frames;
    float fogDensityDE, fogDensityApplied, secondFogDE, farClip;
    float gtaFogStart, gtaFogFar; // GTA fog: metres (timecyc far clip x FogDistance)
};
extern LookStats g_look;

// timecyc column layout (shared with the offline check)
enum ColKind : uint8_t { K_SKIP, K_U8, K_X10, K_I16 };
struct TcCol { uint32_t rva; uint8_t kind; };
extern TcCol g_timecycCols[52]; // filled from CColourSet::CColourSet at Install (g_timecycTableOk)
constexpr int kHours = 8, kWeathers = 23;

// ---------------------------------------------------------------- tools (tools.cpp)
extern const char* const kWeatherNames[kWeathers];
bool GameWindowFocused();             // core.cpp: the game window is the foreground window
extern uint8_t*  g_hours;   // CClock::ms_nGameClockHours (nullptr without the tools)
extern uint8_t*  g_minutes; // CClock::ms_nGameClockMinutes
extern uint32_t* g_timeMs;  // CTimer::m_snTimeInMilliseconds
extern int32_t*  g_currArea; // CGame::currArea (0 = outside)
void ToolsPanel();                     // render thread, inside ImGui frame
bool ToolsWantCapture();               // freecam/noclip active: keep keyboard/mouse away from the game
bool WeatherBlend(int* oldW, int* newW, float* t); // CWeather Old/New/InterpolationValue (false before InstallTools)
void ToolsWriteAnchors(FILE* f);       // resolved addresses as "name rva" lines (offline check artifact)
void ToolsAddWheel(int notches);
void ToolsToggleFreecam();
void ToolsToggleNoclip();

// ---------------------------------------------------------------- overlay (overlay.cpp)
bool InstallOverlay();                 // worker thread: finds IDXGISwapChain::Present via a dummy device
extern volatile bool g_menuOpen;
void LookPanel();                      // core.cpp: look settings tab

// ---------------------------------------------------------------- post effects (postfx.cpp, render thread)
struct ID3D11Device; struct ID3D11DeviceContext; struct ID3D11Texture2D; struct ID3D11RenderTargetView;
// Water drops, SpeedFX, radiosity and grain onto the backbuffer (back / rtv); the context state is saved and restored.
void PostFxDraw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* back, ID3D11RenderTargetView* rtv);
// D3D12: the effects on `scene` (the frame copied at the HUD bind), composited onto the backbuffer under DE's HUD
void PostFxDrawUnderHud(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* back, ID3D11RenderTargetView* rtv,
                        ID3D11Texture2D* scene);
void PostFxReleaseSized();             // before ResizeBuffers: drops the backbuffer-sized textures
