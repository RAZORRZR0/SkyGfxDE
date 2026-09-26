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
    float gamma = 2.2f;
    // grading on top of DE's own values
    float saturation = 1.15f;
    float contrast = 1.05f;
    // timecyc colours
    float skyStrength = 1.0f, fogStrength = 1.0f, ambientStrength = 1.0f, sunTint = 0.35f, cloudTint = 0.5f;
    // atmosphere
    float haze = 0.35f;        // DE main height fog density / opacity multiplier (modern fog only)
    float groundHaze = 0.0f;   // DE's fixed second fog layer (0.02) multiplier (modern fog only)
    bool  gtaFog = true;       // DE's own timecyc fog path (bUseGTAValues): fog start / far clip from the timecyc
    float fogDistance = 1.8f;  // timecyc fog start / far clip multiplier (original PC draw distance slider max)
    // shadows
    float shadowDarkness = 0.5f;
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
