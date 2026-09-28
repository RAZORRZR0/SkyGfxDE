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
    int   logLevel = 1;
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
    // atmosphere
    float haze = 0.35f;        // DE main height fog density / opacity multiplier (modern fog only)
    float groundHaze = 0.0f;   // DE's fixed second fog layer (0.02) multiplier (modern fog only)
    bool  gtaFog = true;       // fog from the timecyc far clip (DE's GTA fog path, bUseGTAValues), modern lighting kept
    float fogDistance = 1.8f;  // timecyc far clip multiplier (original PC draw distance slider max)
    float fogOpacity = 0.5f;   // GTA fog opacity at the scaled far clip; clear up to half of it
    // shadows
    float shadowDarkness = 0.5f;
    // Post effects drawn by the overlay before the HUD (postfx.cpp)
    bool  speedFx = true;          // CPostEffects::SpeedFX, the original's speed blur
    int   speedFxHudBind = 2;      // draw before the Nth backbuffer bind of a frame (the HUD's); 0 = at Present
    bool  speedFxTestMode = false; // CPostEffects::m_bSpeedFXTestMode: full effect (input 1.0) always
    bool  radiosity = true;        // CPostEffects::Radiosity, PS2 highlight glow (limit = timecyc highlight column)
    int   radiosityIntensity = 35; // m_RadiosityIntensity (gta_sa.exe 0x8D5118)
    float radiosityOffset = 6.0f;  // glow offset up-left in PS2 pixels ((2^passes - 1) * correction 2 = 6 on the PS2)
    float deBloom = 0.0f;          // DE's BloomIntensity multiplier while radiosity is on (the PS2 had no bloom)
    bool  grain = true;            // PS2 rain grain (CPostEffects::Render rain branch, skygfx Grain_PS2)
    float grainStrength = 1.0f;    // grain alpha multiplier (1 = PS2)
    bool  waterDrops = true;       // skygfx neo water drops on the lens (rain, water splashes)
    float lodDistance = 1.8f;      // TheCamera.m_fLODDistMultiplier x this (PC draw distance slider: 1.2 default, 1.8 max)
    // characters: roughness towards 1, specular x(1 - matte) on DE's glossy ped materials
    float pedMatte = 0.6f;
    // timecyc
    char  timecycFile[MAX_PATH] = "";
    // tools
    float freecamSpeed = 20.0f;      // m/s
    float freecamSensitivity = 0.15f; // degrees per mouse count
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

// ---------------------------------------------------------------- look (core.cpp)
extern bool g_active;             // effect on/off (toggle key / panel)
extern bool g_customTimecyc;
extern bool g_timecycTableOk;
extern uint8_t* g_curColours;     // CTimeCycle::m_CurrentColours
extern uint8_t** g_singleton;     // DE engine singleton
extern uint8_t* g_classicFlag;
extern uint8_t* g_volFogInClassic; // gta.ShowVolumeFogInClassic storage
extern uint8_t* g_objects;
extern uint8_t* g_namePool;       // FNamePool
uint8_t* ObjectItem(int32_t index); // GObjects slot (FUObjectItem) or nullptr
void PedsFrame();                  // game thread, from the look hook
void PedsPanel();                  // render thread, Look tab
bool LoadTimecycFile(const char* path);
void ApplyTimecycFile();

struct LookStats {
    float gain[3];
    float indirect;
    float shadow;
    int volumes;
    bool classic;
    void* tod;
    unsigned frames;
    float fogDensityDE, fogDensityApplied, secondFogDE;
    bool gtaFog;
    float fogStart, farClip; // metres, as given to DE's GTA fog path (timecyc x FogDistance)
};
extern LookStats g_look;

// timecyc column layout (shared with the offline check)
enum ColKind : uint8_t { K_SKIP, K_U8, K_X10, K_I16 };
struct TcCol { uint32_t rva; uint8_t kind; };
extern const TcCol kTimecycCols[52];
constexpr int kHours = 8, kWeathers = 23;

// ---------------------------------------------------------------- tools (tools.cpp)
extern const char* const kWeatherNames[kWeathers];
bool GameWindowFocused();             // core.cpp: the game window is the foreground window
void ToolsPanel();                     // render thread, inside ImGui frame
bool ToolsWantCapture();               // freecam/noclip active: keep keyboard/mouse away from the game
void ToolsWriteAnchors(FILE* f);       // resolved addresses as "name rva" lines (offline check artifact)
void ToolsAddMouseDelta(long dx, long dy);
void ToolsAddWheel(int notches);
void ToolsToggleFreecam();
void ToolsToggleNoclip();

// ---------------------------------------------------------------- overlay (overlay.cpp)
bool InstallOverlay();                 // worker thread: finds IDXGISwapChain::Present via a dummy device
extern volatile bool g_menuOpen;
void LookPanel();                      // core.cpp: look settings tab

// ---------------------------------------------------------------- post effects (postfx.cpp, render thread)
struct ID3D11Device; struct ID3D11DeviceContext; struct IDXGISwapChain; struct ID3D11RenderTargetView;
// Water drops, SpeedFX, radiosity and grain onto the backbuffer (rtv); the context state is saved and restored.
void PostFxDraw(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc, ID3D11RenderTargetView* rtv);
void PostFxReleaseSized();             // before ResizeBuffers: drops the backbuffer-sized textures
