// SkyGfxDE core: config, log, signature scanning, the PS2 look (timecyc colours, PS2 colour filter,
// grading, haze, shadows) and hook installation. See README.md for the reverse-engineering notes.
#include "skygfx.h"
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <share.h>
#include <math.h>
#include "../minhook/MinHook.h"
#include "../imgui/imgui.h"

Config g_cfg;
char g_dir[MAX_PATH];
char g_iniPath[MAX_PATH];
FILE* g_logFile = nullptr;
LookStats g_look;

bool g_active = true;
bool g_customTimecyc = false;
bool g_timecycTableOk = false;
uint8_t* g_curColours = nullptr;
uint8_t** g_singleton = nullptr;
uint8_t* g_classicFlag = nullptr;
uint8_t* g_objects = nullptr;
uint8_t* g_moduleBase = nullptr;

static float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ----------------------------------------------------------------------
// Hotkeys: "Ctrl+Shift+M", "Alt+X", "Tab", "0x75" ... (any combination of Ctrl/Shift/Alt + one key)
// ----------------------------------------------------------------------
bool ParseHotkey(const char* text, Hotkey& out) {
    Hotkey k;
    strncpy_s(k.text, text, _TRUNCATE);
    char buf[64];
    strncpy_s(buf, text, _TRUNCATE);
    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, "+ ", &ctx); tok; tok = strtok_s(nullptr, "+ ", &ctx)) {
        if (!_stricmp(tok, "Ctrl") || !_stricmp(tok, "Control")) k.ctrl = true;
        else if (!_stricmp(tok, "Shift")) k.shift = true;
        else if (!_stricmp(tok, "Alt")) k.alt = true;
        else if (!_stricmp(tok, "None") || !_stricmp(tok, "Off")) k.vk = 0;
        else if (tok[0] && !tok[1] && isalnum((unsigned char)tok[0])) k.vk = toupper((unsigned char)tok[0]);
        else if (!_stricmp(tok, "Tab")) k.vk = VK_TAB;
        else if (!_stricmp(tok, "Space")) k.vk = VK_SPACE;
        else if (!_stricmp(tok, "Backtick") || !strcmp(tok, "`")) k.vk = VK_OEM_3;
        else if (!_stricmp(tok, "Backslash")) k.vk = VK_OEM_5;
        else if (!_stricmp(tok, "Enter")) k.vk = VK_RETURN;
        else if ((tok[0] == 'F' || tok[0] == 'f') && atoi(tok + 1) >= 1 && atoi(tok + 1) <= 24) k.vk = VK_F1 + atoi(tok + 1) - 1;
        else if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) k.vk = (int)strtol(tok, nullptr, 16);
        else return false;
    }
    out = k;
    return true;
}

static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool GameWindowFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Chord held and the game window is in front (GetAsyncKeyState is global).
bool HotkeyDown(const Hotkey& k) {
    if (!k.vk || !GameWindowFocused()) return false;
    return KeyDown(k.vk) && (!k.ctrl || KeyDown(VK_CONTROL)) && (!k.shift || KeyDown(VK_SHIFT)) && (!k.alt || KeyDown(VK_MENU));
}

// ----------------------------------------------------------------------
// Config
// ----------------------------------------------------------------------
static float IniFloat(const char* sec, const char* key, float def) {
    char buf[64], defs[32];
    snprintf(defs, sizeof(defs), "%g", def);
    GetPrivateProfileStringA(sec, key, defs, buf, sizeof(buf), g_iniPath);
    return (float)atof(buf);
}

static void IniKey(const char* key, const char* def, Hotkey& out) {
    char buf[64];
    GetPrivateProfileStringA("Keys", key, def, buf, sizeof(buf), g_iniPath);
    if (!ParseHotkey(buf, out)) {
        Log(1, "ini: [Keys] %s='%s' not understood, using %s", key, buf, def);
        ParseHotkey(def, out);
    }
}

void ReadIni() {
    const char* ini = g_iniPath;
    Config& c = g_cfg;
    c.enabled             = GetPrivateProfileIntA("General", "Enabled", 1, ini) != 0;
    c.logLevel            = GetPrivateProfileIntA("General", "Log", 0, ini);
    IniKey("Menu", "Ctrl+Shift+M", c.keyMenu);
    IniKey("Toggle", "Ctrl+Shift+E", c.keyToggle);
    IniKey("Reload", "Ctrl+Shift+R", c.keyReload);
    IniKey("Freecam", "Ctrl+Shift+F", c.keyFreecam);
    IniKey("Noclip", "Ctrl+Shift+N", c.keyNoclip);
    c.filter              = GetPrivateProfileIntA("ColourFilter", "PS2Filter", 1, ini) != 0;
    c.filterStrength      = Clamp(IniFloat("ColourFilter", "Strength", 1.0f), 0.0f, 10.0f);
    c.gameTimecycStrength = Clamp(IniFloat("ColourFilter", "GameTimecycStrength", 0.35f), 0.0f, 10.0f);
    c.keepBrightness      = Clamp(IniFloat("ColourFilter", "KeepBrightness", 0.5f), 0.0f, 1.0f);
    c.saturation          = Clamp(IniFloat("Grade", "Saturation", 1.15f), 0.0f, 10.0f);
    c.contrast            = Clamp(IniFloat("Grade", "Contrast", 1.05f), 0.1f, 10.0f);
    c.skyStrength         = Clamp(IniFloat("Colours", "Sky", 1.0f), 0.0f, 4.0f);
    c.fogStrength         = Clamp(IniFloat("Colours", "Fog", 1.0f), 0.0f, 4.0f);
    c.ambientStrength     = Clamp(IniFloat("Colours", "Ambient", 0.25f), 0.0f, 4.0f);
    c.sunTint             = Clamp(IniFloat("Colours", "Sun", 0.35f), 0.0f, 4.0f);
    c.cloudTint           = Clamp(IniFloat("Colours", "Clouds", 0.5f), 0.0f, 4.0f);
    c.brightness          = Clamp(IniFloat("Colours", "Brightness", 1.0f), 0.0f, 4.0f);
    c.nightExposure       = Clamp(IniFloat("Colours", "NightExposure", -0.5f), -10.0f, 10.0f);
    c.haze                = Clamp(IniFloat("Atmosphere", "Haze", 0.35f), 0.0f, 20.0f);
    c.groundHaze          = Clamp(IniFloat("Atmosphere", "GroundHaze", 0.0f), 0.0f, 20.0f);
    c.classicSky          = GetPrivateProfileIntA("Atmosphere", "ClassicSky", 1, ini) != 0;
    c.gtaFog              = GetPrivateProfileIntA("Atmosphere", "GtaFog", 0, ini) != 0;
    c.fogDistance         = Clamp(IniFloat("Atmosphere", "FogDistance", 1.8f), 0.5f, 5.0f);
    c.fogOpacity          = Clamp(IniFloat("Atmosphere", "FogOpacity", 0.5f), 0.05f, 0.95f);
    c.shadowDarkness      = Clamp(IniFloat("Shadows", "Darkness", 0.5f), 0.0f, 1.0f);
    c.speedFx             = GetPrivateProfileIntA("SpeedFX", "Enabled", 1, ini) != 0;
    c.speedFxHudBind      = GetPrivateProfileIntA("SpeedFX", "HudBind", 2, ini);
    c.speedFxTestMode     = GetPrivateProfileIntA("SpeedFX", "TestMode", 0, ini) != 0;
    c.radiosity           = GetPrivateProfileIntA("Radiosity", "Enabled", 1, ini) != 0;
    c.radiosityIntensity  = (int)Clamp((float)GetPrivateProfileIntA("Radiosity", "Intensity", 35, ini), 0.0f, 255.0f);
    c.radiosityOffset     = Clamp(IniFloat("Radiosity", "Offset", 0.2f), 0.0f, 64.0f);
    c.deBloom             = Clamp(IniFloat("Radiosity", "DEBloom", 0.0f), 0.0f, 4.0f);
    c.grain               = GetPrivateProfileIntA("Grain", "Enabled", 1, ini) != 0;
    c.grainStrength       = Clamp(IniFloat("Grain", "Strength", 1.0f), 0.0f, 10.0f);
    c.waterDrops          = GetPrivateProfileIntA("WaterDrops", "Enabled", 1, ini) != 0;
    c.maxDrops            = (int)Clamp((float)GetPrivateProfileIntA("WaterDrops", "MaxDrops", 2000, ini), 0.0f, 2000.0f);
    c.lodDistance         = Clamp(IniFloat("World", "LodDistance", 1.8f), 0.1f, 10.0f);
    c.pedMatte            = Clamp(IniFloat("Characters", "Matte", 0.6f), 0.0f, 1.0f);
    c.coronas             = GetPrivateProfileIntA("Coronas", "Enabled", 1, ini) != 0;
    c.coronaSize          = Clamp(IniFloat("Coronas", "Size", 0.5f), 0.0f, 20.0f);
    c.coronaIntensity     = Clamp(IniFloat("Coronas", "Intensity", 1.0f), 0.0f, 100.0f);
    c.coronaFarClip       = Clamp(IniFloat("Coronas", "FarClip", 0.0f), 0.0f, 20000.0f);
    c.lampDrawDistance    = Clamp(IniFloat("StreetLights", "DrawDistance", 150.0f), 0.0f, 5000.0f);
    c.lampShadowDistance  = Clamp(IniFloat("StreetLights", "ShadowDistance", 50.0f), 0.0f, 1000.0f);
    c.otherLightShadows   = GetPrivateProfileIntA("StreetLights", "OtherLightShadows", 1, ini) != 0;
    c.bollardBrightness   = Clamp(IniFloat("StreetLights", "BollardBrightness", 0.5f), 0.0f, 4.0f);
    c.freecamSpeed        = Clamp(IniFloat("Tools", "FreecamSpeed", 20.0f), 0.1f, 1000.0f);
    c.noclipSpeed         = Clamp(IniFloat("Tools", "NoclipSpeed", 15.0f), 0.1f, 1000.0f);
    GetPrivateProfileStringA("Timecyc", "File", "timecyc_ps2.dat", c.timecycFile, MAX_PATH, ini);
}

static void PutFloat(const char* sec, const char* key, float v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3g", v);
    WritePrivateProfileStringA(sec, key, buf, g_iniPath);
}

// Writes the tunable values back (comments in the file are kept by the Windows ini API).
bool SaveIni() {
    const Config& c = g_cfg;
    WritePrivateProfileStringA("ColourFilter", "PS2Filter", c.filter ? "1" : "0", g_iniPath);
    PutFloat("ColourFilter", "Strength", c.filterStrength);
    PutFloat("ColourFilter", "GameTimecycStrength", c.gameTimecycStrength);
    PutFloat("ColourFilter", "KeepBrightness", c.keepBrightness);
    PutFloat("Grade", "Saturation", c.saturation);
    PutFloat("Grade", "Contrast", c.contrast);
    PutFloat("Colours", "Sky", c.skyStrength);
    PutFloat("Colours", "Fog", c.fogStrength);
    PutFloat("Colours", "Ambient", c.ambientStrength);
    PutFloat("Colours", "Sun", c.sunTint);
    PutFloat("Colours", "Clouds", c.cloudTint);
    PutFloat("Colours", "Brightness", c.brightness);
    PutFloat("Colours", "NightExposure", c.nightExposure);
    PutFloat("Atmosphere", "Haze", c.haze);
    PutFloat("Atmosphere", "GroundHaze", c.groundHaze);
    WritePrivateProfileStringA("Atmosphere", "ClassicSky", c.classicSky ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("Atmosphere", "GtaFog", c.gtaFog ? "1" : "0", g_iniPath);
    PutFloat("Atmosphere", "FogDistance", c.fogDistance);
    PutFloat("Atmosphere", "FogOpacity", c.fogOpacity);
    PutFloat("Shadows", "Darkness", c.shadowDarkness);
    WritePrivateProfileStringA("SpeedFX", "Enabled", c.speedFx ? "1" : "0", g_iniPath);
    char hb[8]; snprintf(hb, sizeof(hb), "%d", c.speedFxHudBind);
    WritePrivateProfileStringA("SpeedFX", "HudBind", hb, g_iniPath);
    WritePrivateProfileStringA("SpeedFX", "TestMode", c.speedFxTestMode ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("Radiosity", "Enabled", c.radiosity ? "1" : "0", g_iniPath);
    snprintf(hb, sizeof(hb), "%d", c.radiosityIntensity);
    WritePrivateProfileStringA("Radiosity", "Intensity", hb, g_iniPath);
    PutFloat("Radiosity", "Offset", c.radiosityOffset);
    PutFloat("Radiosity", "DEBloom", c.deBloom);
    WritePrivateProfileStringA("Grain", "Enabled", c.grain ? "1" : "0", g_iniPath);
    PutFloat("Grain", "Strength", c.grainStrength);
    WritePrivateProfileStringA("WaterDrops", "Enabled", c.waterDrops ? "1" : "0", g_iniPath);
    { char md[8]; snprintf(md, sizeof(md), "%d", c.maxDrops); WritePrivateProfileStringA("WaterDrops", "MaxDrops", md, g_iniPath); }
    PutFloat("World", "LodDistance", c.lodDistance);
    PutFloat("Characters", "Matte", c.pedMatte);
    WritePrivateProfileStringA("Coronas", "Enabled", c.coronas ? "1" : "0", g_iniPath);
    PutFloat("Coronas", "Size", c.coronaSize);
    PutFloat("Coronas", "Intensity", c.coronaIntensity);
    PutFloat("Coronas", "FarClip", c.coronaFarClip);
    PutFloat("StreetLights", "DrawDistance", c.lampDrawDistance);
    PutFloat("StreetLights", "ShadowDistance", c.lampShadowDistance);
    WritePrivateProfileStringA("StreetLights", "OtherLightShadows", c.otherLightShadows ? "1" : "0", g_iniPath);
    PutFloat("StreetLights", "BollardBrightness", c.bollardBrightness);
    PutFloat("Tools", "FreecamSpeed", c.freecamSpeed);
    PutFloat("Tools", "NoclipSpeed", c.noclipSpeed);
    const bool ok = WritePrivateProfileStringA("Timecyc", "File", c.timecycFile, g_iniPath) != 0;
    Log(1, "ini %s %s", ok ? "saved to" : "could NOT be saved to", g_iniPath);
    return ok;
}

// ----------------------------------------------------------------------
// Log: next to the .asi, else %LOCALAPPDATA%\SkyGfxDE, else %TEMP%.
// ----------------------------------------------------------------------
static DWORD g_logStart = 0;
static SRWLOCK g_logLock = SRWLOCK_INIT;

void OpenLog() {
    if (g_cfg.logLevel <= 0) return;
    char paths[3][MAX_PATH] = {};
    snprintf(paths[0], MAX_PATH, "%sSkyGfxDE.log", g_dir);
    char env[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", env, MAX_PATH)) {
        char dir[MAX_PATH];
        snprintf(dir, MAX_PATH, "%s\\SkyGfxDE", env);
        CreateDirectoryA(dir, nullptr);
        snprintf(paths[1], MAX_PATH, "%s\\SkyGfxDE.log", dir);
    }
    if (GetTempPathA(MAX_PATH, env)) snprintf(paths[2], MAX_PATH, "%sSkyGfxDE.log", env);
    for (auto& p : paths)
        if (p[0] && (g_logFile = _fsopen(p, "w", _SH_DENYWR)) != nullptr) break;
    g_logStart = GetTickCount();
}

void Log(int level, const char* fmt, ...) {
    if (level > g_cfg.logLevel || !g_logFile) return;
    AcquireSRWLockExclusive(&g_logLock);
    const DWORD ms = GetTickCount() - g_logStart;
    fprintf(g_logFile, "[%5lu.%03lu] ", ms / 1000, ms % 1000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logFile);
    fflush(g_logFile);
    ReleaseSRWLockExclusive(&g_logLock);
}

// ----------------------------------------------------------------------
// Signature scanning (SanAndreas.exe, current Steam/RGL build, UE 4.26 Gameface)
// ----------------------------------------------------------------------
static uint8_t* g_textBegin = nullptr;
static size_t g_textSize = 0;

bool InitTextSection() {
    uint8_t* base = g_moduleBase ? g_moduleBase : (uint8_t*)GetModuleHandleA(nullptr);
    g_moduleBase = base;
    auto* nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            g_textBegin = base + sec->VirtualAddress;
            g_textSize = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// Unique match of `pattern` ("48 8D ?? ...") in .text, or nullptr if absent/ambiguous.
uint8_t* FindUnique(const char* pattern) {
    uint8_t bytes[128];
    bool mask[128];
    size_t n = 0;
    for (const char* p = pattern; *p && n < sizeof(bytes);) {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') { mask[n] = false; bytes[n++] = 0; p += (p[1] == '?') ? 2 : 1; continue; }
        const char hex[3] = { p[0], p[1], 0 };
        bytes[n] = (uint8_t)strtoul(hex, nullptr, 16);
        mask[n++] = true;
        p += 2;
    }
    if (!n || !g_textBegin) return nullptr;
    uint8_t* found = nullptr;
    for (uint8_t* cur = g_textBegin, *end = g_textBegin + g_textSize - n; cur <= end; ++cur) {
        if (cur[0] != bytes[0]) continue;
        size_t k = 1;
        while (k < n && (!mask[k] || cur[k] == bytes[k])) ++k;
        if (k == n) {
            if (found) return nullptr;
            found = cur;
        }
    }
    return found;
}

// `insn` must start with `opcode` (exact bytes); returns the RIP-relative target.
uint8_t* RipAt(uint8_t* insn, const uint8_t* opcode, size_t opLen, size_t insnLen) {
    if (!insn || memcmp(insn, opcode, opLen) != 0) return nullptr;
    return insn + insnLen + *(int32_t*)(insn + opLen);
}

int32_t** g_cloudCVar = nullptr; // r.VolumetricCloud (FindIntCVar)

// Address of the static that holds an int console variable's TConsoleVariableData<int32>* (ShadowedValue[2]: game
// thread, render thread), from its TAutoConsoleVariable registration: lea rdx, L"name"; call [rax+18h]; ...
// call [rdx+58h] (AsVariableInt); lea rcx, ..; mov [rip], rax. Read it when used: it is set by a static initializer.
static int32_t** FindIntCVar(const wchar_t* name) {
    auto* nt = (IMAGE_NT_HEADERS64*)(g_moduleBase + ((IMAGE_DOS_HEADER*)g_moduleBase)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    const size_t len = (wcslen(name) + 1) * sizeof(wchar_t);
    const uint8_t* str = nullptr;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && !str; ++i, ++sec) {
        if (memcmp(sec->Name, ".rdata", 6)) continue;
        const uint8_t* b = g_moduleBase + sec->VirtualAddress;
        for (size_t o = 2; o + len <= sec->Misc.VirtualSize; o += 2)
            if (!memcmp(b + o, name, len) && !*(const wchar_t*)(b + o - 2)) { str = b + o; break; }
    }
    if (!str) return nullptr;
    static const uint8_t leaRdx[] = { 0x48, 0x8D, 0x15 }, asInt[] = { 0xFF, 0x52, 0x58, 0x48, 0x8D, 0x0D }, movRax[] = { 0x48, 0x89, 0x05 };
    for (uint8_t *cur = g_textBegin, *end = g_textBegin + g_textSize - 0x60; cur < end; ++cur) {
        if (RipAt(cur, leaRdx, 3, 7) != str) continue;
        for (uint8_t* p = cur + 7; p < cur + 0x50; ++p)
            if (!memcmp(p, asInt, sizeof(asInt))) {
                return (int32_t**)RipAt(p + 10, movRax, 3, 7); // after lea rcx (7 bytes)
            }
    }
    return nullptr;
}

// ----------------------------------------------------------------------
// Game layouts (DE x64; IDA + Dumper-7 SDK)
// ----------------------------------------------------------------------
namespace CS { // CColourSet (RenderWare part, PC layout) inside CTimeCycle::m_CurrentColours
constexpr size_t Ambient = 0x00;        // float r,g,b (0..1 after CalcColoursForPoint)
constexpr size_t SkyTop = 0x24;         // uint16 r,g,b
constexpr size_t SkyBottom = 0x2A;      // uint16 r,g,b (also the fog colour in the original)
constexpr size_t FarClip = 0x50, FogStart = 0x54; // float, metres (original: linear fog from FogStart to FarClip)
constexpr size_t SunCore = 0x30;        // uint16 r,g,b
constexpr size_t ShadowStrength = 0x48; // int16
constexpr size_t LowClouds = 0x5C;      // uint16 r,g,b
constexpr size_t PostFx1 = 0x78;        // float r,g,b,a (0..255, alpha as in the file)
constexpr size_t PostFx2 = 0x88;        // float r,g,b,a
constexpr size_t SkyColorSet = 0xAC;    // FSkyColorSet (weather-blended UE values, rebuilt every frame)
}
namespace SCS { // FSkyColorSet (GTABase), FLinearColor fields
constexpr size_t Skylight = 0x00, SkyLower = 0x10, SkyUpper = 0x20, SkyReflection = 0x30, VolumetricCloud = 0x60, Fog = 0x118,
                 Sun = 0x158;
}
namespace TOD { // AGTATimeOfDay
constexpr size_t LiveColors = 0x2B8;    // SkyColorSet (used by the renderer)
constexpr size_t TargetColors = 0x458;  // written by CTimeCycle::Update, copied to LiveColors
constexpr size_t OfSingleton = 0x688;   // AGTATimeOfDay* in DE's engine singleton
constexpr size_t SkyLightIntensity = 0x66C; // float, sky light intensity (x SkylightColor alpha)
constexpr size_t CloudyAmount = 0x26A4; // float, CWeather::CloudCoverage (CTimeCycle::Update copies it in)
constexpr size_t VgdOverrideClass = 0x4770; // TSubclassOf<AVGDOverrideData>: DE's GTA fog StartDistance source
}
constexpr size_t UCLASS_CDO = 0x118;          // UClass::ClassDefaultObject
constexpr size_t VGD_FogStartDistance = 0x220; // float StartDistance, then VolumetricFogExtinctionScale (sub_140B51860)
namespace PP { // APostProcessVolume::Settings (FPostProcessSettings)
constexpr size_t Settings = 0x260;
constexpr size_t OverrideByte0 = 0x00;  // bit 2 ColorSaturation, bit 3 ColorContrast, bit 5 ColorGain
constexpr uint8_t SaturationBit = 0x04, ContrastBit = 0x08, ColorGainBit = 0x20;
constexpr uint8_t GradeBits = SaturationBit | ContrastBit | ColorGainBit;
constexpr size_t ColorSaturation = 0x30, ColorContrast = 0x40, ColorGain = 0x60; // FVector4
constexpr size_t OverrideByte6 = 0x06;  // bit 2 = bOverride_BloomIntensity
constexpr uint8_t BloomBit = 0x04;
constexpr size_t BloomIntensity = 0x21C; // float, UE default 0.675
constexpr size_t ExposureBias = 0x314;  // AutoExposureBias, EV (override: byte 0x0A bit 6)
constexpr size_t IsInterior = 0x864;    // AGTAPostProcessVolume::bIsInteriorPostProcess (outside Settings)
}
namespace FOG { // AGTAHeightFog / UExponentialHeightFogComponent
constexpr size_t Component = 0x2A8;     // AGTAHeightFog::HeightFogComponent
constexpr size_t Density = 0x1F8;       // FogDensity (DE: FogParameters.x)
constexpr size_t SecondDensity = 0x200; // SecondFogData.FogDensity (DE: fixed 0.02 ground layer)
constexpr size_t Tod = 0x2B0;           // AGTAHeightFog::TimeOfDayActor
constexpr size_t InscatterColor = 0x20C; // component FogInscatteringColor (FLinearColor)
constexpr size_t UseGtaValues = 0x2A0;  // AGTAHeightFog::bUseGTAValues: DE's timecyc fog path (as in Classic)
}
constexpr size_t Obj_Index = 0x0C;

// ----------------------------------------------------------------------
// Timecyc tables: column order of timecyc.dat -> DE array (RVA), [8 hours][23 weathers]. The RVAs are not hardcoded
// (they move between game builds): CColourSet::CColourSet reads every table once, in a fixed order, as
//   movzx/movsx eax, byte [r9 + r15(__ImageBase) + rva]   43 0F B6|BE 84 39 <rva32>
//   movsx eax, word [r15 + r9*2 + rva]                    43 0F BF 84 4F <rva32>
// so each column is the n-th such load (postfx colours are loaded r,g,b,a; the file has a,r,g,b).
// ----------------------------------------------------------------------
TcCol g_timecycCols[52];
static const struct { int8_t load; uint8_t kind; } kTimecycLoads[52] = {
    { 0, K_U8 }, { 1, K_U8 }, { 2, K_U8 },          // ambient
    { 3, K_U8 }, { 4, K_U8 }, { 5, K_U8 },          // ambient obj
    { -1, K_SKIP }, { -1, K_SKIP }, { -1, K_SKIP }, // directional (unused by the game)
    { 6, K_U8 }, { 7, K_U8 }, { 8, K_U8 },          // sky top
    { 9, K_U8 }, { 10, K_U8 }, { 11, K_U8 },        // sky bottom
    { 12, K_U8 }, { 13, K_U8 }, { 14, K_U8 },       // sun core
    { 15, K_U8 }, { 16, K_U8 }, { 17, K_U8 },       // sun corona
    { 18, K_X10 }, { 19, K_X10 }, { 20, K_X10 },    // sun size, sprite size, sprite brightness
    { 21, K_U8 }, { 22, K_U8 }, { 23, K_U8 },       // shadow, light shadow, pole shadow
    { 24, K_I16 }, { 25, K_I16 }, { 26, K_X10 },    // far clip, fog start, light on ground
    { 27, K_U8 }, { 28, K_U8 }, { 29, K_U8 },       // low clouds
    { 30, K_U8 }, { 31, K_U8 }, { 32, K_U8 },       // bottom clouds
    { 33, K_U8 }, { 34, K_U8 }, { 35, K_U8 }, { 36, K_U8 }, // water rgba
    { 40, K_U8 }, { 37, K_U8 }, { 38, K_U8 }, { 39, K_U8 }, // postfx1 a,r,g,b
    { 44, K_U8 }, { 41, K_U8 }, { 42, K_U8 }, { 43, K_U8 }, // postfx2 a,r,g,b
    { 45, K_U8 }, { 46, K_U8 }, { 47, K_U8 },       // cloud alpha, highlight min, water fog alpha
    { -1, K_SKIP },                                 // directional mult (load 48; DE forces 1.28)
};

// Fills g_timecycCols from the constructor's loads; false if the code does not look as expected (load count, or a
// column's load width differs), and then no table is touched.
static bool ResolveTimecycTable(const uint8_t* ctor, size_t len) {
    struct { uint32_t rva; bool word; } loads[64];
    int n = 0;
    for (size_t i = 0; i + 9 <= len && n < 64; ++i) {
        const uint8_t* p = ctor + i;
        if (p[0] != 0x43 || p[1] != 0x0F || p[3] != 0x84) continue;
        const bool byteLoad = (p[2] == 0xB6 || p[2] == 0xBE) && p[4] == 0x39, wordLoad = p[2] == 0xBF && p[4] == 0x4F;
        if (!byteLoad && !wordLoad) continue;
        loads[n++] = { *(const uint32_t*)(p + 5), wordLoad };
        i += 8;
    }
    if (n != 49) { Log(1, "timecyc: %d table loads in CColourSet::CColourSet (expected 49)", n); return false; }
    for (int c = 0; c < 52; ++c) {
        const auto& l = kTimecycLoads[c];
        if (l.kind == K_SKIP) { g_timecycCols[c] = { 0, K_SKIP }; continue; }
        if (loads[l.load].word != (l.kind == K_I16)) { Log(1, "timecyc: column %d: unexpected load width", c); return false; }
        g_timecycCols[c] = { loads[l.load].rva, l.kind };
    }
    return true;
}

// Loads a timecyc.dat (same format as the game's) into DE's tables. 8-hour files map 1:1; 24-hour files
// (23 weathers x 24 lines) contribute the hours of the 8 original slots.
static const int kSlotHours[kHours] = { 0, 5, 6, 7, 12, 19, 20, 22 };
static int g_timecycGen = 0; // bumped when a timecyc file is loaded: the brightness calibration is redone
bool LoadTimecycFile(const char* path) {
    if (!g_timecycTableOk) { Log(1, "timecyc: tables not found in this game build, %s ignored", path); return false; }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) { Log(1, "timecyc: cannot open %s", path); return false; }
    constexpr int kMaxRows = kWeathers * 24;
    static float values[kMaxRows][52];
    char line[1024];
    int rows = 0, extra = 0;
    while (fgets(line, sizeof(line), f)) {
        for (char* c = line; *c; ++c) if ((unsigned char)*c < 0x20 || *c == ',') *c = ' ';
        const char* p = line;
        while (*p == ' ') ++p;
        if (!*p || *p == '/') continue;
        if (rows == kMaxRows) { ++extra; continue; }
        int n = 0;
        char* end = nullptr;
        for (; n < 52; ++n) {
            const float v = strtof(p, &end);
            if (end == p) break;
            values[rows][n] = v;
            p = end;
        }
        if (n < 51) { Log(1, "timecyc: line %d has %d values (need 52)", rows + 1, n); fclose(f); return false; }
        ++rows;
    }
    fclose(f);
    const int perWeather = rows == kWeathers * kHours ? kHours : rows == kWeathers * 24 ? 24 : 0;
    if (!perWeather || extra) {
        Log(1, "timecyc: %s has %d data lines, expected %d (8 hours) or %d (24 hours) for 23 weathers",
            path, rows + extra, kWeathers * kHours, kWeathers * 24);
        return false;
    }
    for (int w = 0; w < kWeathers; ++w) {
        for (int h = 0; h < kHours; ++h) {
            const float* v = values[w * perWeather + (perWeather == 24 ? kSlotHours[h] : h)];
            const int idx = h * kWeathers + w;
            for (int c = 0; c < 52; ++c) {
                const TcCol& col = g_timecycCols[c];
                uint8_t* arr = g_moduleBase + col.rva;
                switch (col.kind) {
                case K_U8:  arr[idx] = (uint8_t)(int)v[c]; break;
                case K_X10: arr[idx] = (uint8_t)(int)(v[c] * 10.0f + 0.5f); break;
                case K_I16: ((int16_t*)arr)[idx] = (int16_t)(int)v[c]; break;
                default: break;
                }
            }
        }
    }
    Log(1, "timecyc: loaded %s (%d-hour file)", path, perWeather);
    g_customTimecyc = true;
    ++g_timecycGen;
    return true;
}

void ApplyTimecycFile() {
    if (!g_cfg.timecycFile[0] || !g_timecycTableOk) return;
    char path[MAX_PATH];
    if (strchr(g_cfg.timecycFile, ':') || g_cfg.timecycFile[0] == '\\')
        strcpy_s(path, g_cfg.timecycFile);
    else
        snprintf(path, MAX_PATH, "%s%s", g_dir, g_cfg.timecycFile);
    LoadTimecycFile(path);
}

// ----------------------------------------------------------------------
// Colour maths
// ----------------------------------------------------------------------
static float Luma(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
static float ToLinear(float srgb01) { return powf(Clamp(srgb01, 0.0f, 1.0f), 2.2f); }

// Moves `dst` (FLinearColor, alpha kept) towards the hue of the timecyc colour `srgb` (0..1), keeping dst's luminance.
static void TransferHue(float* dst, const float srgb[3], float strength) {
    if (strength <= 0.0f) return;
    const float lin[3] = { ToLinear(srgb[0]), ToLinear(srgb[1]), ToLinear(srgb[2]) };
    const float L = Luma(dst), Lt = Luma(lin);
    for (int i = 0; i < 3; ++i) {
        const float target = Lt > 1e-5f ? L * lin[i] / Lt : L;
        dst[i] += (target - dst[i]) * strength;
    }
}

static void ReadU16Rgb(const uint8_t* p, float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = ((const uint16_t*)p)[i] / 255.0f;
}

// UE 4.26 FilmToneMap (TonemapCommon.ush) on a grey, then the sRGB encode: linear scene value -> display value.
// Film slope, toe, shoulder, black clip, white clip: UE defaults, replaced by the post-process volume's overrides
// when it has them (DE's main volume overrides slope and toe; TrackVolume logs the values).
static float g_film[5] = { 0.88f, 0.55f, 0.26f, 0.0f, 0.04f };
static float FilmDisplay(float x) {
    const float S = g_film[0], Toe = g_film[1], Sh = g_film[2], BC = g_film[3], WC = g_film[4];
    const float TS = 1 + BC - Toe, SS = 1 + WC - Sh;
    float TM;
    if (Toe > 0.8f) TM = (1 - Toe - 0.18f) / S + log10f(0.18f);
    else { const float bt = (0.18f + BC) / TS - 1; TM = log10f(0.18f) - 0.5f * logf((1 + bt) / (1 - bt)) * (TS / S); }
    const float SM = (1 - Toe) / S - TM, ShM = Sh / S - SM;
    if (x <= 0.0f) return 0.0f;
    const float L = log10f(x), st = S * (L + SM);
    const float toe = L < TM ? -BC + 2 * TS / (1 + expf((-2 * S / TS) * (L - TM))) : st;
    const float sho = L > ShM ? (1 + WC) - 2 * SS / (1 + expf((2 * S / SS) * (L - ShM))) : st;
    float t = Clamp((L - TM) / (ShM - TM), 0.0f, 1.0f);
    if (ShM < TM) t = 1 - t;
    t = (3 - 2 * t) * t * t;
    const float v = Clamp(toe + (sho - toe) * t, 0.0f, 1.0f);
    return v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
}

// ColorGain k that turns mid grey (0.18) into `display` x its display value: k = FilmDisplay^-1(...) / 0.18.
static float LinearGainFor(float display) {
    const float target = fminf(display * FilmDisplay(0.18f), 0.999f);
    float lo = 1e-4f, hi = 64.0f;
    for (int i = 0; i < 40; ++i) {
        const float mid = sqrtf(lo * hi);
        (FilmDisplay(mid) < target ? lo : hi) = mid;
    }
    return lo / 0.18f;
}

// skygfx CPostEffects::ColourFilter_PS2: the frame is drawn with postfx1 (MODULATE2X, replace), then
// postfx2 is added with its alpha (also MODULATE2X): display gain = 2*c1 + 2*c2 * min(1, 2*a2), per channel.
// DE applies ColorGain to linear colour before its filmic tone curve, so the gain is converted to the ColorGain that
// gives that display change at mid grey (a plain gamma power crushes the weak channels in the curve's toe: blue at
// PS2 LA midday came out 0.37 instead of 0.64). Shadows still get a little more, highlights a little less.
// With a loaded (PS2) timecyc the filter runs as configured. The game's own timecyc has PC/mobile-style
// postfx values (postfx2 alpha 255, strong colours) that give a heavily orange PS2 gain, so there it is
// applied tint-only at GameTimecycStrength.
static void ComputePs2Gain(const uint8_t* cc, float gain[3]) {
    const float* c1 = (const float*)(cc + CS::PostFx1);
    const float* c2 = (const float*)(cc + CS::PostFx2);
    const float a2 = fminf(1.0f, 2.0f * c2[3] / 255.0f);
    float g[3];
    for (int i = 0; i < 3; ++i) g[i] = 2.0f * c1[i] / 255.0f + 2.0f * c2[i] / 255.0f * a2;
    const float l = Luma(g);
    const float keep = g_customTimecyc ? g_cfg.keepBrightness : 1.0f;
    const float strength = g_customTimecyc ? g_cfg.filterStrength : g_cfg.gameTimecycStrength;
    const float norm = l > 1e-3f ? powf(l, keep) : 1.0f;
    for (int i = 0; i < 3; ++i) gain[i] = Clamp(LinearGainFor(fmaxf(1.0f + (g[i] / norm - 1.0f) * strength, 0.01f)), 0.0f, 4.0f);
}

// ----------------------------------------------------------------------
// Post-process volumes seen in DE's UpdateColorOptions
// ----------------------------------------------------------------------
struct Volume {
    uint8_t* obj; int32_t index, serial;
    float deGain[4], deSat[4], deContrast[4]; // DE's own values (brightness/contrast options), after UpdateColorOptions
    uint8_t deOverride0;                       // DE's override bits in byte 0
    bool origBloomOverride; float origBloom;
    float origBias;                            // AutoExposureBias (DE doesn't rewrite it)
};
static Volume g_volumes[64];
static int g_volumeCount = 0;
static SRWLOCK g_lock = SRWLOCK_INIT;
static float g_gain[3] = { 1, 1, 1 };
static float g_indirect = 1.0f;
static float g_night = 0.0f; // 1 - the original's day/night balance, from the time-of-day colour hook

uint8_t* ObjectItem(int32_t index) {
    if (!g_objects || index < 0) return nullptr;
    __try {
        if (index >= *(int32_t*)(g_objects + 0x14)) return nullptr;
        uint8_t* chunk = (*(uint8_t***)g_objects)[index / 65536];
        return chunk ? chunk + (index % 65536) * 0x18 : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Same UObject in its GObjects slot, same serial, not PendingKill/Unreachable.
static bool Alive(const Volume& v) {
    uint8_t* item = ObjectItem(v.index);
    if (!item) return false;
    __try {
        return *(uint8_t**)item == v.obj && *(int32_t*)(item + 0x10) == v.serial &&
               !(*(int32_t*)(item + 8) & ((1 << 29) | (1 << 28)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void Scale4(float* dst, const float* de, float rgbMul, const float* rgb = nullptr) {
    for (int i = 0; i < 3; ++i) dst[i] = de[i] * rgbMul * (rgb ? rgb[i] : 1.0f);
    dst[3] = de[3];
}

static void WriteVolume(Volume& v) {
    uint8_t* s = v.obj + PP::Settings;
    const bool on = g_active;
    const bool filterOn = on && g_cfg.filter;
    Scale4((float*)(s + PP::ColorGain), v.deGain, 1.0f, filterOn ? g_gain : nullptr);
    Scale4((float*)(s + PP::ColorSaturation), v.deSat, on ? g_cfg.saturation : 1.0f);
    Scale4((float*)(s + PP::ColorContrast), v.deContrast, on ? g_cfg.contrast : 1.0f);
    const uint8_t bits = on ? (uint8_t)(v.deOverride0 | PP::GradeBits) : v.deOverride0;
    s[PP::OverrideByte0] = (uint8_t)((s[PP::OverrideByte0] & ~PP::GradeBits) | (bits & PP::GradeBits));

    // Night exposure: DE lights the night street with its own local lights and neon (the original used prelit night
    // vertex colours), so at 22:00 on the Strip it was ~2x the original's brightness. The exterior volume's
    // AutoExposureBias (override bit 0x0A:6) gets NightExposure EV x night (the original's day/night balance).
    // ponytail: calibrated on one scene (Four Dragons, 22:00, vs the original + skygfx); per-zone if others differ.
    if (!v.obj[PP::IsInterior] && (s[0x0A] & 0x40))
        *(float*)(s + PP::ExposureBias) = v.origBias + (on ? g_cfg.nightExposure * g_night : 0.0f);
    // The PS2 had no bloom: radiosity is its glow, so DE's bloom is scaled by DEBloom while radiosity is on.
    float* bloom = (float*)(s + PP::BloomIntensity);
    if (on && g_cfg.radiosity) {
        s[PP::OverrideByte6] |= PP::BloomBit;
        *bloom = (v.origBloomOverride ? v.origBloom : 0.675f) * g_cfg.deBloom;
    } else {
        if (v.origBloomOverride) s[PP::OverrideByte6] |= PP::BloomBit;
        else s[PP::OverrideByte6] &= (uint8_t)~PP::BloomBit;
        *bloom = v.origBloom;
    }
}

static Volume* TrackVolume(uint8_t* obj) {
    for (int i = 0; i < g_volumeCount; ++i) if (g_volumes[i].obj == obj) return &g_volumes[i];
    const int32_t index = *(int32_t*)(obj + Obj_Index);
    uint8_t* item = ObjectItem(index);
    if (!item || *(uint8_t**)item != obj) return nullptr;
    if (g_volumeCount == (int)(sizeof(g_volumes) / sizeof(g_volumes[0]))) g_volumes[0] = g_volumes[--g_volumeCount];
    Volume& v = g_volumes[g_volumeCount++];
    const uint8_t* s = obj + PP::Settings;
    v.obj = obj; v.index = index; v.serial = *(int32_t*)(item + 0x10);
    v.origBloomOverride = (s[PP::OverrideByte6] & PP::BloomBit) != 0;
    v.origBloom = *(const float*)(s + PP::BloomIntensity);
    v.origBias = *(const float*)(s + PP::ExposureBias);
    const bool filmBits[5] = { (s[4] & 0x80) != 0, (s[5] & 1) != 0, (s[5] & 2) != 0, (s[5] & 4) != 0, (s[5] & 8) != 0 };
    if (!obj[PP::IsInterior])
        for (int i = 0; i < 5; ++i) if (filmBits[i]) g_film[i] = ((const float*)(s + 0x184))[i]; // FilmSlope..FilmWhiteClip
    Log(1, "post-process volume %p tracked (%d total, interior=%d, film slope %.3f toe %.3f shoulder %.3f black %.3f white %.3f, "
           "exposure bias %.2f)", obj, g_volumeCount, obj[PP::IsInterior], g_film[0], g_film[1], g_film[2], g_film[3],
        g_film[4], v.origBias);
    return &v;
}

typedef uintptr_t (*ColorOptions_Fn)(void* volume);
static ColorOptions_Fn o_UpdateColorOptions = nullptr;

static uintptr_t Hooked_UpdateColorOptions(void* volume) {
    const uintptr_t r = o_UpdateColorOptions(volume);
    AcquireSRWLockExclusive(&g_lock);
    __try {
        if (Volume* v = TrackVolume((uint8_t*)volume)) {
            const uint8_t* s = v->obj + PP::Settings;
            memcpy(v->deGain, s + PP::ColorGain, sizeof(v->deGain)); // DE just rewrote these from its options
            memcpy(v->deSat, s + PP::ColorSaturation, sizeof(v->deSat));
            memcpy(v->deContrast, s + PP::ColorContrast, sizeof(v->deContrast));
            v->deOverride0 = s[PP::OverrideByte0];
            WriteVolume(*v);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

// ----------------------------------------------------------------------
// Sky and height fog (AGTATimeOfDay tick, AGTAHeightFog::UpdateColors). Two modes:
// - Classic sky (ClassicSky=1, default): Classic Atmosphere's sky, clouds, sun and fog in the modern lighting (below).
//   Classic's fog is UE's height fog (ShouldUseGTAFog, 0x140B65820), so every material gets it (water, LOD trees,
//   distant terrain), which a fog drawn on top of UE's frame or into UE's fog pass can't do.
// - Modern fog: FogDensity from the sky set plus a fixed 0.02 ground layer, scaled by Haze / GroundHaze after DE
//   writes them (only when DE changed them, so the render state is marked dirty at most once per change).
// ----------------------------------------------------------------------
typedef uintptr_t (*FogUpdate_Fn)(void* fogActor, float dt);
typedef uintptr_t (*MarkDirty_Fn)(void* component);
static FogUpdate_Fn o_FogUpdateColors = nullptr;
static MarkDirty_Fn g_MarkRenderStateDirty = nullptr;

// Classic sky and fog (ClassicSky=1): DE's Classic Atmosphere is the global flag byte_145024151; every
// AGTATimeOfDay method that reads it picks its classic data when the actor allows overrides (+0x37F0; the classic
// override sets at +0x38A8.. and override object +0x4770 instead of the modern ones at +0x28B8..), and
// ShouldUseGTAFog turns on DE's GTA fog. The flag is set only while the time-of-day actor ticks and the fog actor
// updates, so the sky, sun, clouds and fog are Classic's and the renderer's lighting stays modern. Our timecyc
// colours see the flag set inside the tick and leave Classic's colours alone.
// ponytail: the flag is global; a render-thread read during those two game-thread calls sees Classic for that moment.
// Hook the render-side readers if that ever shows as a flicker.
static bool g_classicScoped = false; // game thread: the flag is ours (ClassicScope), not the user's Classic setting
struct ClassicScope {
    bool on;
    ClassicScope() : on(g_active && g_cfg.classicSky && g_classicFlag && !*g_classicFlag) { if (on) *g_classicFlag = 1, g_classicScoped = true; }
    ~ClassicScope() { if (on) *g_classicFlag = 0, g_classicScoped = false; }
};
// The user's Classic Atmosphere setting (the look steps aside), not our scoped flag.
static bool UserClassic() { return g_classicFlag && *g_classicFlag && !g_classicScoped; }

static void GuardedFrame(void (*fn)(), const char* what);
typedef uintptr_t (*TodTick_Fn)(void* tod, float dt, uintptr_t, uintptr_t);
static TodTick_Fn o_TodTick = nullptr;
static uintptr_t Hooked_TodTick(void* tod, float dt, uintptr_t a3, uintptr_t a4) {
    uintptr_t r;
    { ClassicScope c; r = o_TodTick(tod, dt, a3, a4); }
    // An actor tick, so UE's game thread (CTimeCycle::Update is not): UObject calls (ProcessEvent) are safe here.
    GuardedFrame(OtherLightShadowsFrame, "other light shadows");
    return r;
}
static uintptr_t FogUpdate(void* fogActor, float dt) {
    ClassicScope c;
    return o_FogUpdateColors(fogActor, dt);
}
// Weather particles (DE's port of SA's wind spray in storms and the sandstorm's sand; gta.ShowParticleFog) and the
// volumetric clouds met when flying above 220 m (DE's port of CClouds::VolumetricCloudsRender 0x716380;
// gta.ShowVolumeClouds), which DE only runs with Classic Atmosphere.
typedef void (*Void_Fn)();
static Void_Fn o_WeatherParticles = nullptr, o_VolumeClouds = nullptr;
static void Hooked_WeatherParticles() { ClassicScope c; o_WeatherParticles(); }
static void Hooked_VolumeClouds() { ClassicScope c; o_VolumeClouds(); }
// SF's moving fog (CClouds::MovingFogRender's port: gta.ShowMovingFog, FOGGY_SF) and the rainbow after rain
// (CClouds::Render_MaybeRenderRainbows' port, DE's rainbow mesh; gta.ShowOldRainbow for the sprites) also only run
// with the Classic flag.
typedef void (*Rainbow_Fn)(uintptr_t, uintptr_t, uintptr_t);
static Void_Fn o_MovingFog = nullptr;
static Rainbow_Fn o_Rainbow = nullptr;
static void Hooked_MovingFog() { ClassicScope c; o_MovingFog(); }
static void Hooked_Rainbow(uintptr_t a, uintptr_t b, uintptr_t c) { ClassicScope s; o_Rainbow(a, b, c); }

// GTA fog (ClassicSky=0, GtaFog=1): DE's own timecyc fog path, the one Classic Atmosphere uses, with DE's modern sky.
// ShouldUseGTAFog (0x140B65820) is true when bUseGTAValues is set. That path turns volumetric fog off unless
// gta.ShowVolumeFogInClassic is set; the byte is set for the call only. Density and start come from the timecyc far
// clip x FogDistance (PerFrame), the colour from the time-of-day fog colour.
uint8_t* g_volFogInClassic = nullptr; // gta.ShowVolumeFogInClassic value, read only by UpdateColors
static bool GtaFogOn() { return g_active && g_cfg.gtaFog && !g_cfg.classicSky && !UserClassic(); }

static uintptr_t Hooked_FogUpdateColors(void* fogActor, float dt) {
    // DE compares its new value with the component before writing; restore DE's values first so its
    // change detection keeps working, then apply ours.
    static float lastDeMain = -1.0f, lastDeSecond = -1.0f, lastMain = -1.0f, lastSecond = -1.0f;
    static float lastDeCol[4] = { -1, -1, -1, -1 }, lastCol[4] = { -1, -1, -1, -1 };
    static uint8_t* deActor = nullptr;
    static uint8_t deUseGta = 0;
    const bool classic = UserClassic();
    const bool gtaFog = GtaFogOn();
    const bool modernOn = g_active && !g_cfg.classicSky && !classic && !gtaFog;
    uint8_t* actor = (uint8_t*)fogActor;
    uint8_t* comp = nullptr;
    __try {
        if (actor != deActor) { deActor = actor; deUseGta = actor[FOG::UseGtaValues]; }
        actor[FOG::UseGtaValues] = gtaFog ? 1 : deUseGta;
        comp = *(uint8_t**)(actor + FOG::Component);
        if (comp && *(float*)(comp + FOG::Density) == lastMain && *(float*)(comp + FOG::SecondDensity) == lastSecond) {
            *(float*)(comp + FOG::Density) = lastDeMain;
            *(float*)(comp + FOG::SecondDensity) = lastDeSecond;
        }
        if (comp && !memcmp(comp + FOG::InscatterColor, lastCol, 16)) memcpy(comp + FOG::InscatterColor, lastDeCol, 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) { comp = nullptr; }
    const uint8_t volSaved = g_volFogInClassic ? *g_volFogInClassic : 0;
    if (g_volFogInClassic && gtaFog) *g_volFogInClassic = 1;
    const uintptr_t r = FogUpdate(fogActor, dt); // outside __try: ClassicScope has a destructor
    if (g_volFogInClassic) *g_volFogInClassic = volSaved;
    __try {
        if (!comp) return r;
        float* density = (float*)(comp + FOG::Density);
        float* second = (float*)(comp + FOG::SecondDensity);
        lastDeMain = *density;
        lastDeSecond = *second;
        g_look.fogDensityDE = *density;
        g_look.secondFogDE = *second;
        // Classic sky: DE's Classic fog density is a per-weather/hour value, not the timecyc distances; at 06:00
        // countryside it was a wall at ~30 m where the original showed terrain a few hundred metres out. It is capped
        // at the original's fog: RW linear fog from the timecyc FogStart (camera fogPlane) to the far clip (x 1.8, the
        // PC draw distance maximum), so 50% opacity halfway. UE 4.26 height fog on a level ray:
        // opacity = 1 - exp(-(ln 2)^2 x FogDensity / 1000 x cm).
        // GTA fog: DE's density follows a per-weather value more than the timecyc distances (too thick, deaf to
        // FogDistance), so it is replaced: clear up to StartDistance (half the scaled far clip, set in PerFrame), then
        // FogOpacity reached at the scaled far clip.
        // ponytail: level ray at fog height; camera height above the fog actor thins it further.
        const float farClip = *(const float*)(g_curColours + CS::FarClip) * 1.8f;
        const float half = 0.5f * (*(const float*)(g_curColours + CS::FogStart) + farClip);
        const bool classicOn = g_active && g_cfg.classicSky && !classic && half > 1.0f;
        const float cap = classicOn ? 0.693147f * 1000.0f / (0.480453f * half * 100.0f) : 0.0f;
        const float gtaRangeCm = (g_look.gtaFogFar - g_look.gtaFogStart) * 100.0f;
        const bool gtaOn = gtaFog && gtaRangeCm > 0.0f;
        g_look.farClip = farClip;
        const float wantMain = gtaOn ? -logf(1.0f - g_cfg.fogOpacity) * 1000.0f / (0.480453f * gtaRangeCm)
                             : modernOn ? *density * g_cfg.haze : classicOn ? fminf(*density, cap) : *density;
        const float wantSecond = gtaOn || modernOn ? *second * g_cfg.groundHaze : *second;
        g_look.fogDensityApplied = wantMain;
        // GTA fog colour: DE's GTA path copies one static classic colour, which glows at night. Use the time-of-day
        // sky fog colour instead, the way DE's modern path does (rgb x a, a^2), so it darkens with the clock.
        float* col = (float*)(comp + FOG::InscatterColor);
        memcpy(lastDeCol, col, 16);
        float wantCol[4];
        memcpy(wantCol, col, 16);
        const uint8_t* tod = *(uint8_t**)(actor + FOG::Tod);
        if (gtaOn && tod) {
            const float* f = (const float*)(tod + TOD::LiveColors + SCS::Fog);
            wantCol[0] = f[0] * f[3]; wantCol[1] = f[1] * f[3]; wantCol[2] = f[2] * f[3]; wantCol[3] = f[3] * f[3];
        }
        if (wantMain != *density || wantSecond != *second || memcmp(wantCol, col, 16)) {
            *density = wantMain;
            *second = wantSecond;
            memcpy(col, wantCol, 16);
            g_MarkRenderStateDirty(comp);
        }
        memcpy(lastCol, col, 16);
        lastMain = *density;
        lastSecond = *second;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

// ----------------------------------------------------------------------
// Per-frame: after DE's CTimeCycle::Update (game thread)
// ----------------------------------------------------------------------
// Per-frame, after AGTATimeOfDay's colour update (0x140BB7B60): it copies TargetColors to LiveColors and then blends
// DE's own time-of-day overrides into LiveColors (the 11 override sets at +0x28B8; e.g. the orange 22:00 sunset glow,
// which the original's 22:00 timecyc doesn't have). The timecyc colours are applied to LiveColors after that, so the
// overrides can't replace them. DE's live value is the base: its hue moves to the timecyc colour (strength per field),
// and with Brightness its rgb x alpha moves to K x the timecyc colour's linear luminance (fog: rgb only, its alpha is
// DE's fog opacity). K is calibrated per weather: DE's own value for that weather at 12:00 (its sky curves, evaluated
// with the curve evaluator CColourSet::CColourSet uses: TOD +0x7A0 + 0x158 x curve set, weathers mapped to DE's 7
// curve sets by the table at ctor+0x3EF) / that weather's timecyc noon colour, blended between the old and new weather
// like the timecyc. So every weather's midday keeps DE's brightness (auto-exposure stays calibrated) and every other
// hour follows that weather's timecyc.
// The sun is scaled by the original's day/night balance (CCustomBuildingRenderer::UpdateDayNightBalanceParam: day
// 7:00-20:00, fades 6-7 and 20-21): DE's sun and its sunset overrides run until ~23:00, the original's night starts at 21.
static float DayFactor(float hours) {
    if (hours < 6.0f || hours >= 21.0f) return 0.0f;
    if (hours < 7.0f) return hours - 6.0f;
    if (hours < 20.0f) return 1.0f;
    return 21.0f - hours;
}

typedef void (*SkyEval_Fn)(void* curves, float weight, float hours, void* out);
SkyEval_Fn g_SkyEval = nullptr;     // DE's sky curve evaluator (weight 1: writes the whole FSkyColorSet)
const int32_t* g_skyRemap = nullptr; // weather -> DE sky curve set
enum { K_UPPER, K_LOWER, K_REFL, K_FOG, K_COUNT };
static const float kDefaultK[K_COUNT] = { 0.30f, 1.50f, 8.26f, 0.38f }; // EXTRASUNNY_VEGAS, measured before
struct Calib { int gen = -1; float k[K_COUNT]; };
static Calib g_calib[kWeathers];

static const float* CalibrateWeather(uint8_t* tod, int w) {
    Calib& c = g_calib[w];
    if (c.gen == g_timecycGen) return c.k;
    memcpy(c.k, kDefaultK, sizeof(c.k));
    c.gen = g_timecycGen;
    if (!g_SkyEval || !g_skyRemap) return c.k;
    uint8_t* single = *(uint8_t**)g_singleton;
    const int mode = (*(int (**)(void*))(*(uint8_t**)single + 0x318))(single);
    const int set = mode == 2 ? w : g_skyRemap[w];
    alignas(16) float out[0x100] = {}; // FSkyColorSet (0x16C+) with room to spare
    g_SkyEval(tod + 0x7A0 + 0x158 * set, 1.0f, 12.0f, out);
    // timecyc noon (hour slot 4) sky top / bottom of weather w, from the loaded tables
    if (!g_timecycTableOk) return c.k;
    auto tc = [&](int col) { return ToLinear(g_moduleBase[g_timecycCols[col].rva + 4 * kWeathers + w] / 255.0f); };
    const float top[3] = { tc(9), tc(10), tc(11) }, bot[3] = { tc(12), tc(13), tc(14) };
    const struct { size_t off; const float* lin; bool alpha; } f[K_COUNT] = {
        { SCS::SkyUpper, top, true }, { SCS::SkyLower, bot, true }, { SCS::SkyReflection, bot, true }, { SCS::Fog, bot, false } };
    for (int i = 0; i < K_COUNT; ++i) {
        const float* v = out + f[i].off / 4;
        const float de = Luma(v) * (f[i].alpha ? v[3] : 1.0f), t = Luma(f[i].lin);
        if (t > 1e-4f && de > 0.0f && de < 1e4f) c.k[i] = de / t;
    }
    Log(1, "brightness: weather %d (%s) curve set %d: K upper %.3f lower %.3f reflection %.3f fog %.3f", w, kWeatherNames[w],
        set, c.k[0], c.k[1], c.k[2], c.k[3]);
    return c.k;
}

static void ApplyColours(uint8_t* tod, const uint8_t* cc, float hours) {
    float amb[3], top[3], bottom[3], sun[3], clouds[3];
    for (int i = 0; i < 3; ++i) amb[i] = ((const float*)(cc + CS::Ambient))[i];
    ReadU16Rgb(cc + CS::SkyTop, top);
    ReadU16Rgb(cc + CS::SkyBottom, bottom);
    ReadU16Rgb(cc + CS::SunCore, sun);
    ReadU16Rgb(cc + CS::LowClouds, clouds);
    float k[K_COUNT];
    int wa, wb; float wt;
    if (WeatherBlend(&wa, &wb, &wt)) {
        const float* ka = CalibrateWeather(tod, wa); const float* kb = CalibrateWeather(tod, wb);
        for (int i = 0; i < K_COUNT; ++i) k[i] = ka[i] + (kb[i] - ka[i]) * Clamp(wt, 0.0f, 1.0f);
    } else {
        memcpy(k, kDefaultK, sizeof(k));
    }
    struct Field { size_t off; const float* colour; float strength, k; bool alphaScales; } fields[] = {
        { SCS::SkyUpper, top, g_cfg.skyStrength, k[K_UPPER], true },
        { SCS::SkyLower, bottom, g_cfg.skyStrength, k[K_LOWER], true },
        { SCS::SkyReflection, bottom, g_cfg.skyStrength, k[K_REFL], true },
        { SCS::Fog, bottom, g_cfg.fogStrength, k[K_FOG], false },
        { SCS::Skylight, amb, g_cfg.ambientStrength, 0.0f, false },
        { SCS::Sun, sun, g_cfg.sunTint, 0.0f, false },
        { SCS::VolumetricCloud, clouds, g_cfg.cloudTint, 0.0f, false },
    };
    const float b = g_cfg.brightness;
    for (const Field& f : fields) {
        float* c = (float*)(tod + TOD::LiveColors + f.off);
        TransferHue(c, f.colour, f.strength);
        if (f.k <= 0.0f || b <= 0.0f) continue;
        const float lin[3] = { ToLinear(f.colour[0]), ToLinear(f.colour[1]), ToLinear(f.colour[2]) };
        // Sky fields render rgb x alpha. DE fades a layer out with alpha (SkyUpper alpha is 0 at night), so the target
        // product can't be reached by dividing by that alpha (rgb blew up to the 64 clamp: bright cyan night sky with
        // SF's (0, 8, 12) sky top). Alpha moves towards 1 with Brightness instead, and rgb carries the product.
        // Brightness > 1 (typed in) pushes past the timecyc target; alpha stays <= 1 and colours >= 0.
        const float a = f.alphaScales ? c[3] : 1.0f, A = f.alphaScales ? fminf(a + (1.0f - a) * b, 1.0f) : 1.0f;
        const float L = Luma(c), want = fmaxf((L * a + (f.k * Luma(lin) - L * a) * b) / fmaxf(A, 1e-3f), 0.0f);
        const float* hue = L > 1e-5f ? c : lin; // DE black: the timecyc colour gives the hue
        const float s = want / fmaxf(Luma(hue), 1e-5f);
        for (int i = 0; i < 3; ++i) c[i] = fminf(hue[i] * s, 64.0f);
        if (f.alphaScales) c[3] = A;
    }
    const float day = fmaxf(1.0f + (DayFactor(hours) - 1.0f) * b, 0.0f);
    float* s = (float*)(tod + TOD::LiveColors + SCS::Sun);
    for (int i = 0; i < 3; ++i) s[i] *= day;
}

typedef void (*TodColours_Fn)(uint8_t* tod);
static TodColours_Fn o_TodColours = nullptr;

// Classic's override sets relight the whole world (green sky light, orange 1/5-alpha sun, its own post settings:
// 10:00 countryside red/blue 6.4 vs DE's 1.8). With the Classic sky the colour update runs twice: Classic (inside
// ClassicScope), then modern; only Classic's sky, cloud, fog, moon and stars fields (FSkyColorSet ranges below) are
// kept, so the lighting, reflections, exposure and post settings stay modern.
static const struct { uint16_t from, to; } kClassicFields[] = {
    { 0x10, 0x30 },   // SkyLowerColor, SkyUpperColor
    { 0x50, 0x104 },  // CloudParams .. GTAFogParam_Blend (clouds, GTA fog overrides)
    { 0x118, 0x158 }, // FogColor, FogParameters, MoonColor, StarsColor
    { 0x190, 0x194 }, // MovingFogIntensity
};

static void Hooked_TodColours(uint8_t* tod) {
    o_TodColours(tod);
    if (g_classicScoped) {
        alignas(16) uint8_t classic[0x194];
        uint8_t* live = tod + TOD::LiveColors;
        memcpy(classic, live, sizeof(classic));
        *g_classicFlag = 0;
        o_TodColours(tod);
        *g_classicFlag = 1;
        for (const auto& r : kClassicFields) memcpy(live + r.from, classic + r.from, r.to - r.from);
        // No volumetric clouds here to cover the night sky (DE has no other weather fade for it), so do what the
        // original's CClouds::Render (0x7139B2) did: moon and stars x (1 - max(Foggyness, CloudCoverage)). DE's
        // CWeather::Update gives CloudCoverage 1 in the foggy weathers too, so CloudCoverage alone is that max.
        const float clear = 1.0f - Clamp(*(float*)(tod + TOD::CloudyAmount), 0.0f, 1.0f);
        for (float* c = (float*)(live + 0x138); c < (float*)(live + 0x158); ++c) *c *= clear; // MoonColor, StarsColor
    }
    __try {
        const bool classic = UserClassic(); // inside ClassicScope the timecyc still colours Classic's sky
        uint8_t* single = *(uint8_t**)g_singleton;
        if (!g_active || classic || !single || tod != *(uint8_t**)(single + TOD::OfSingleton)) return;
        // the time the colour update itself reads (engine singleton vfunc +0x378: game clock in hours)
        const float hours = (*(float (**)(void*))(*(uint8_t**)single + 0x378))(single);
        g_night = 1.0f - DayFactor(hours);
        ApplyColours(tod, g_curColours, hours);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int logged = 0;
        if (logged++ < 5) Log(1, "exception in time-of-day colours (code 0x%08lX)", GetExceptionCode());
    }
}

static void PollKeys() {
    static bool toggleDown = false, reloadDown = false, freecamDown = false, noclipDown = false;
    const bool toggle = HotkeyDown(g_cfg.keyToggle), reload = HotkeyDown(g_cfg.keyReload);
    const bool freecam = HotkeyDown(g_cfg.keyFreecam), noclip = HotkeyDown(g_cfg.keyNoclip);
    if (toggle && !toggleDown) { g_active = !g_active; Log(1, "look %s", g_active ? "on" : "off"); }
    if (reload && !reloadDown) { ReadIni(); ApplyTimecycFile(); Log(1, "ini reloaded"); }
    if (freecam && !freecamDown) ToolsToggleFreecam();
    if (noclip && !noclipDown) ToolsToggleNoclip();
    toggleDown = toggle; reloadDown = reload; freecamDown = freecam; noclipDown = noclip;
}

static void PerFrame() {
    PollKeys();
    const uint8_t* cc = g_curColours;
    if (g_active && g_cfg.filter) ComputePs2Gain(cc, g_gain);
    else g_gain[0] = g_gain[1] = g_gain[2] = 1.0f;
    const float shadow = Clamp(*(const int16_t*)(cc + CS::ShadowStrength) / 255.0f, 0.0f, 1.0f);
    g_indirect = Clamp(1.0f - g_cfg.shadowDarkness * shadow, 0.1f, 1.0f);

    const bool classic = UserClassic();
    // Volumetric clouds off with the Classic sky, as in Classic Atmosphere (its sky dome draws its own cloud
    // textures): r.VolumetricCloud 0. DE's value is put back when the Classic sky is off.
    if (int32_t* v = g_cloudCVar ? *g_cloudCVar : nullptr) {
        static int32_t deCloud = -1;
        if (g_active && g_cfg.classicSky && !classic) {
            if (deCloud < 0) deCloud = v[0];
            v[0] = v[1] = 0;
        } else if (deCloud >= 0) {
            v[0] = v[1] = deCloud;
            deCloud = -1;
        }
    }
    uint8_t* tod = *g_singleton ? *(uint8_t**)(*g_singleton + TOD::OfSingleton) : nullptr;
    // GTA fog: fog starts at half the scaled timecyc far clip. The original's fog started near 0 m (timecyc fog start
    // median 10 m), too thick for DE's full-distance world. DE copies StartDistance every frame from its fog override
    // data (class default object + 0x220), so the value is written at that source; restored when GTA fog is off.
    g_look.gtaFogFar = *(const float*)(cc + CS::FarClip) * g_cfg.fogDistance;
    g_look.gtaFogStart = g_look.gtaFogFar * 0.5f;
    {
        static float* startSrc = nullptr;
        static float startDE = 0.0f;
        uint8_t* vgdClass = tod ? *(uint8_t**)(tod + TOD::VgdOverrideClass) : nullptr;
        uint8_t* cdo = vgdClass ? *(uint8_t**)(vgdClass + UCLASS_CDO) : nullptr;
        float* src = cdo ? (float*)(cdo + VGD_FogStartDistance) : nullptr;
        if (src != startSrc) {
            if (startSrc) *startSrc = startDE;
            startSrc = src;
            if (src) startDE = *src;
        }
        if (startSrc) *startSrc = GtaFogOn() ? g_look.gtaFogStart * 100.0f : startDE;
    }
    // Shadows: the light filling shadowed areas is DE's sky light. Its update (0x140BBF6D0) sets the component's
    // intensity to AGTATimeOfDay::SkyLightIntensity (+0x66C) x the live SkylightColor alpha; the alpha is rewritten
    // by DE after this hook, so SkyLightIntensity is scaled by 1 - Darkness x timecyc shadow strength (the original
    // drew shadows at that alpha). DE's own value is kept and restored; a value DE writes is taken as the new one.
    static float* skyP = nullptr;
    static float skyDE = 0.0f, skyWritten = NAN;
    float* sp = tod ? (float*)(tod + TOD::SkyLightIntensity) : nullptr;
    if (sp != skyP) { if (skyP) *skyP = skyDE; skyP = sp; if (sp) skyDE = *sp; skyWritten = NAN; }
    if (sp) {
        if (*sp != skyWritten) skyDE = *sp;
        *sp = skyWritten = g_active && !classic ? skyDE * g_indirect : skyDE;
    }
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_volumeCount;) {
        if (!Alive(g_volumes[i])) { g_volumes[i] = g_volumes[--g_volumeCount]; continue; }
        WriteVolume(g_volumes[i++]);
    }
    const int volumes = g_volumeCount;
    ReleaseSRWLockExclusive(&g_lock);

    memcpy(g_look.gain, g_gain, sizeof(g_gain));
    g_look.indirect = g_indirect;
    g_look.shadow = shadow;
    g_look.volumes = volumes;
    g_look.classic = classic;
    g_look.tod = tod;
    ++g_look.frames;

    static DWORD lastLog = 0;
    const DWORD now = GetTickCount();
    if (now - lastLog >= 10000) {
        const float* p1 = (const float*)(cc + CS::PostFx1);
        const float* p2 = (const float*)(cc + CS::PostFx2);
        Log(1, "frames=%u active=%d classic=%d volumes=%d postfx1=(%.0f %.0f %.0f a%.0f) postfx2=(%.0f %.0f %.0f a%.0f) "
               "gain=(%.3f %.3f %.3f) shadow=%.2f indirect=%.2f fog=%.5f->%.5f groundFog=%.4f classicSky=%d gtaFog=%d farClip=%.0f",
            g_look.frames, g_active, classic, volumes, p1[0], p1[1], p1[2], p1[3], p2[0], p2[1], p2[2], p2[3],
            g_gain[0], g_gain[1], g_gain[2], shadow, g_indirect, g_look.fogDensityDE, g_look.fogDensityApplied, g_look.secondFogDE,
            g_cfg.classicSky, GtaFogOn(), g_look.farClip);
        lastLog = now;
    }
}

typedef uintptr_t (*Update_Fn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static Update_Fn o_TimeCycleUpdate = nullptr;
static Update_Fn o_TimeCycleInit = nullptr;

static void GuardedFrame(void (*fn)(), const char* what) {
    __try {
        fn();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int logged = 0;
        if (logged++ < 5) Log(1, "exception in %s (code 0x%08lX)", what, GetExceptionCode());
    }
}

static uintptr_t Hooked_TimeCycleUpdate(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    const uintptr_t r = o_TimeCycleUpdate(a, b, c, d);
    GuardedFrame(PerFrame, "look");
    GuardedFrame(PedsFrame, "characters");
    GuardedFrame(CoronasFrame, "coronas");
    GuardedFrame(StreetLightsFrame, "street lights");
    return r;
}

static uintptr_t Hooked_TimeCycleInit(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    const uintptr_t r = o_TimeCycleInit(a, b, c, d);
    ApplyTimecycFile();
    return r;
}

// ----------------------------------------------------------------------
// Install
// ----------------------------------------------------------------------
struct Sig { const char* name; const char* pattern; uint8_t** out; };

bool Install() {
    if (!InitTextSection()) return false;
    uint8_t *update = nullptr, *init = nullptr, *setClassic = nullptr, *ctor = nullptr, *colorOptions = nullptr, *gobjRef = nullptr,
            *fogUpdate = nullptr, *markDirty = nullptr, *todColours = nullptr, *todTick = nullptr, *weatherParticles = nullptr,
            *volumeClouds = nullptr;
    const Sig sigs[] = {
        { "CTimeCycle::Update", "4C 8B DC 55 56 49 8D 6B A1 48 81 EC C8 00 00 00 45 0F 29 4B A8 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 D7", &update },
        { "CTimeCycle::Initialise", "48 8B C4 88 48 08 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 80 48 81 EC B0 02 00 00 F3 0F 10 05 ?? ?? ?? ?? 48 8D 1D", &init },
        { "SetClassicAtmosphere", "4C 8B DC 55 57 49 8D 6B D8 48 81 EC 18 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 E0 38 15 ?? ?? ?? ?? 48 8B F9 0F 84", &setClassic },
        { "CColourSet::CColourSet", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 48 8B D9 49 63 F8 48 81 C1 AC 00 00 00 E8", &ctor },
        { "AGTAPostProcessVolume::UpdateColorOptions", "48 8B C4 55 41 56 48 8D 68 A1 48 81 EC E8 00 00 00 48 83 3D ?? ?? ?? ?? 00 4C 8B F1 0F 84 ?? ?? ?? ?? 0F 29 78 A8", &colorOptions },
        { "GObjects ref", "48 8D 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? E8 03 00 00 48 8D 0D ?? ?? ?? ??", &gobjRef },
        { "AGTAHeightFog::UpdateColors", "48 8B C4 55 57 48 8D 68 A1 48 81 EC D8 00 00 00 48 83 B9 B0 02 00 00 00 48 8B F9", &fogUpdate },
        { "UActorComponent::MarkRenderStateDirty", "40 53 48 83 EC 20 48 8B D9 0F B6 89 88 00 00 00 0F B6 C1 24 03 3C 03 0F 85", &markDirty },
        // AGTATimeOfDay colour update: LiveColors = TargetColors, then DE's overrides; reads the clock via singleton+0x378
        { "AGTATimeOfDay colour update", "40 53 48 81 EC A0 00 00 00 0F 29 74 24 70 48 8B D9 48 8B 0D ?? ?? ?? ?? 44 0F 29 44 24 50 44 0F 29 4C 24 40 44 0F 29 54 24 30 48 8B 01 0F 29 7C 24 60 FF 90 78 03 00 00 0F B6 05", &todColours },
        { "AGTATimeOfDay::Tick", "4C 8B DC 49 89 5B 08 57 48 81 EC B0 00 00 00 48 8B 3D ?? ?? ?? ?? 48 8B D9 45 0F 29 43 C8 44 0F 28 C1 48 85 FF 0F 84", &todTick },
        { "weather particles (wind spray, sand)", "40 55 48 8D 6C 24 A9 48 81 EC F0 00 00 00 F6 05 ?? ?? ?? ?? 08 0F 85 ?? ?? ?? ?? F6 05 ?? ?? ?? ?? 08 0F 85", &weatherParticles },
        { "volumetric clouds", "40 55 41 57 48 8D AC 24 18 FF FF FF 48 81 EC E8 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 20 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 80 3D", &volumeClouds },
    };
    bool ok = true;
    for (const Sig& s : sigs) {
        *s.out = FindUnique(s.pattern);
        Log(*s.out ? 2 : 1, "sig %-42s %p%s", s.name, *s.out, *s.out ? "" : "  <-- NOT FOUND");
        ok &= *s.out != nullptr;
    }
    if (!ok) { Log(1, "unsupported SanAndreas.exe build: plugin disabled"); return false; }

    static const uint8_t leaRdx[] = { 0x48, 0x8D, 0x15 }, movRcx[] = { 0x48, 0x8B, 0x0D }, cmpDl[] = { 0x38, 0x15 },
                         leaRax[] = { 0x48, 0x8D, 0x05 }, todLoad[] = { 0x48, 0x8B, 0xB9, 0x88, 0x06, 0x00, 0x00 };
    g_curColours = RipAt(update + 0x62, leaRdx, 3, 7);                   // lea rdx, CTimeCycle::m_CurrentColours
    g_singleton = (uint8_t**)RipAt(update + 0xE2, movRcx, 3, 7);         // mov rcx, engine singleton
    g_classicFlag = RipAt(setClassic + 0x1E, cmpDl, 2, 6);               // cmp cs:UseLightingOverrides, dl
    g_objects = RipAt(gobjRef, leaRax, 3, 7);
    const bool todOffsetOk = memcmp(update + 0x2F4, todLoad, sizeof(todLoad)) == 0; // mov rdi, [rcx+688h]
    if (!g_curColours || !g_singleton || !g_classicFlag || !g_objects || !todOffsetOk) {
        Log(1, "unexpected code layout (colours=%p singleton=%p classic=%p objects=%p todOffset=%d): plugin disabled",
            g_curColours, g_singleton, g_classicFlag, g_objects, todOffsetOk);
        return false;
    }
    g_timecycTableOk = ResolveTimecycTable(ctor, 1100);
    Log(1, "timecyc table %s", g_timecycTableOk ? "read from CColourSet::CColourSet" : "NOT found: Timecyc.File ignored");
    g_MarkRenderStateDirty = (MarkDirty_Fn)markDirty;
    static const uint8_t cmpByte[] = { 0x80, 0x3D }; // UpdateColors+0x84: cmp cs:gta.ShowVolumeFogInClassic, 0
    g_volFogInClassic = RipAt(fogUpdate + 0x84, cmpByte, 2, 7);
    if (!g_volFogInClassic) Log(1, "gta.ShowVolumeFogInClassic not found: GTA fog turns volumetric fog off");
    g_cloudCVar = FindIntCVar(L"r.VolumetricCloud");
    Log(1, g_cloudCVar ? "r.VolumetricCloud found" : "r.VolumetricCloud NOT found: volumetric clouds stay with the Classic sky");
    // CColourSet::CColourSet tail: mov edi, [base + rva weather->curve set table + rdi*4] (ctor+0x3EF), then
    // call the sky curve evaluator (ctor+0x41D). Missing: brightness uses the EXTRASUNNY_VEGAS calibration for all.
    static const uint8_t remapLoad[] = { 0x41, 0x8B, 0xBC, 0xBF }, callRel[] = { 0xE8 };
    if (!memcmp(ctor + 0x3EF, remapLoad, sizeof(remapLoad))) g_skyRemap = (const int32_t*)(g_moduleBase + *(const int32_t*)(ctor + 0x3F3));
    g_SkyEval = (SkyEval_Fn)RipAt(ctor + 0x41D, callRel, 1, 5);
    if (!g_skyRemap || !g_SkyEval) { g_skyRemap = nullptr; g_SkyEval = nullptr; Log(1, "sky curve evaluator not found: brightness uses one calibration"); }

    if (MH_Initialize() != MH_OK) return false;
    ok = MH_CreateHook(update, (void*)&Hooked_TimeCycleUpdate, (void**)&o_TimeCycleUpdate) == MH_OK &&
         MH_CreateHook(init, (void*)&Hooked_TimeCycleInit, (void**)&o_TimeCycleInit) == MH_OK &&
         MH_CreateHook(colorOptions, (void*)&Hooked_UpdateColorOptions, (void**)&o_UpdateColorOptions) == MH_OK &&
         MH_CreateHook(fogUpdate, (void*)&Hooked_FogUpdateColors, (void**)&o_FogUpdateColors) == MH_OK &&
         MH_CreateHook(todColours, (void*)&Hooked_TodColours, (void**)&o_TodColours) == MH_OK &&
         MH_CreateHook(todTick, (void*)&Hooked_TodTick, (void**)&o_TodTick) == MH_OK &&
         MH_CreateHook(weatherParticles, (void*)&Hooked_WeatherParticles, (void**)&o_WeatherParticles) == MH_OK &&
         MH_CreateHook(volumeClouds, (void*)&Hooked_VolumeClouds, (void**)&o_VolumeClouds) == MH_OK;
    if (!ok) { Log(1, "hook creation failed"); MH_Uninitialize(); return false; }
    // DE's cloud port draws each quad at |dot| * alpha * 10/255; the original's is * 1/255 (0x7168F0), so DE's clouds
    // are ~10x denser. The same function loads 1/255 at +0xA97: point the 10/255 load at +0xA42 to it.
    static const uint8_t movssXmm1[] = { 0xF3, 0x0F, 0x10, 0x0D }, mulssXmm0[] = { 0xF3, 0x0F, 0x59, 0x05 };
    const uint8_t* tenOver255 = RipAt(volumeClouds + 0xA42, movssXmm1, 4, 8);
    const uint8_t* oneOver255 = RipAt(volumeClouds + 0xA97, mulssXmm0, 4, 8);
    if (tenOver255 && oneOver255 && fabsf(*(const float*)tenOver255 - 10.0f / 255) < 1e-6f &&
        fabsf(*(const float*)oneOver255 - 1.0f / 255) < 1e-7f) {
        int32_t* disp = (int32_t*)(volumeClouds + 0xA42 + 4);
        DWORD old;
        VirtualProtect(disp, 4, PAGE_EXECUTE_READWRITE, &old);
        *disp = (int32_t)(oneOver255 - (volumeClouds + 0xA42 + 8));
        VirtualProtect(disp, 4, old, &old);
        Log(2, "volumetric clouds: quad alpha / 255 (original)");
    } else {
        Log(1, "volumetric clouds: alpha constant not found, DE's 10/255 kept");
    }
    // Moving fog and rainbow: optional (a missing one only loses that effect).
    uint8_t* movingFog = FindUnique("4C 8B DC 55 49 8D 6B 98 48 81 EC 60 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 F0 48 83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 80 3D");
    uint8_t* rainbow = FindUnique("40 55 48 8D 6C 24 A9 48 81 EC E0 00 00 00 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 80 3D");
    if (movingFog) MH_CreateHook(movingFog, (void*)&Hooked_MovingFog, (void**)&o_MovingFog);
    if (rainbow) MH_CreateHook(rainbow, (void*)&Hooked_Rainbow, (void**)&o_Rainbow);
    Log(movingFog ? 2 : 1, "moving fog %p%s", movingFog, movingFog ? "" : ": not found, no SF moving fog");
    Log(rainbow ? 2 : 1, "rainbow %p%s", rainbow, rainbow ? "" : ": not found, no rainbow");
    if (!InstallTools()) Log(1, "debug tools unavailable (see above); the look still works");
    InstallPeds();
    InstallCoronas();
    InstallStreetLights();
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        Log(1, "hook installation failed");
        MH_Uninitialize();
        return false;
    }
    Log(1, "hooks installed: m_CurrentColours=%p singleton=%p classicFlag=%p GObjects=%p", g_curColours, g_singleton, g_classicFlag, g_objects);
    return true;
}

// ----------------------------------------------------------------------
// Look tab (render thread; values are plain floats read by the game thread each frame)
// ----------------------------------------------------------------------
static void Swatch(const char* label, const float srgb[3]) {
    ImGui::ColorButton(label, ImVec4(srgb[0], srgb[1], srgb[2], 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
    ImGui::SameLine();
    ImGui::Text("%-10s %3.0f %3.0f %3.0f", label, srgb[0] * 255.0f, srgb[1] * 255.0f, srgb[2] * 255.0f);
}

static void LinearSwatch(const char* label, const float* lin) {
    const float m = fmaxf(fmaxf(lin[0], lin[1]), fmaxf(lin[2], 1e-4f));
    const float c[3] = { powf(lin[0] / m, 1 / 2.2f), powf(lin[1] / m, 1 / 2.2f), powf(lin[2] / m, 1 / 2.2f) };
    ImGui::ColorButton(label, ImVec4(c[0], c[1], c[2], 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
    ImGui::SameLine();
    ImGui::Text("%-10s %.3f %.3f %.3f (x%.2f)", label, lin[0], lin[1], lin[2], m);
}

// A slider for dragging over the usual range plus a box for typing any value within [hardLo, hardHi] (applied on Enter
// or when the box loses focus; ImGui's scalar inputs don't allow EnterReturnsTrue).
bool SliderBox(const char* label, float* v, float lo, float hi, float hardLo, float hardHi, const char* fmt, int flags) {
    const float sp = ImGui::GetStyle().ItemInnerSpacing.x, box = ImGui::GetFontSize() * 4.5f;
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - box - sp);
    bool changed = ImGui::SliderFloat("##s", v, lo, hi, fmt, flags);
    ImGui::SameLine(0, sp);
    ImGui::SetNextItemWidth(box);
    float t = *v;
    ImGui::InputFloat("##b", &t, 0, 0, "%.3f");
    if (ImGui::IsItemDeactivatedAfterEdit()) { *v = Clamp(t, hardLo, hardHi); changed = true; }
    ImGui::SameLine(0, sp);
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return changed;
}

bool SliderBoxInt(const char* label, int* v, int lo, int hi, int hardLo, int hardHi) {
    const float sp = ImGui::GetStyle().ItemInnerSpacing.x, box = ImGui::GetFontSize() * 4.5f;
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - box - sp);
    bool changed = ImGui::SliderInt("##s", v, lo, hi);
    ImGui::SameLine(0, sp);
    ImGui::SetNextItemWidth(box);
    int t = *v;
    ImGui::InputInt("##b", &t, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) { *v = t < hardLo ? hardLo : t > hardHi ? hardHi : t; changed = true; }
    ImGui::SameLine(0, sp);
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return changed;
}

void LookPanel() {
    ImGui::Checkbox("Effect on", &g_active);
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", g_cfg.keyToggle.text);
    if (g_look.classic) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Classic Atmosphere is ON: only filter/grade/shadows apply");

    if (ImGui::CollapsingHeader("PS2 colour filter", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("PS2 filter", &g_cfg.filter);
        if (g_customTimecyc) {
            SliderBox("Strength", &g_cfg.filterStrength, 0.0f, 2.0f, 0.0f, 10.0f);
            SliderBox("Keep brightness", &g_cfg.keepBrightness, 0.0f, 1.0f, 0.0f, 1.0f);
        } else {
            SliderBox("Strength (game timecyc)", &g_cfg.gameTimecycStrength, 0.0f, 2.0f, 0.0f, 10.0f);
        }
        ImGui::TextDisabled("display gain -> DE ColorGain through the filmic curve at mid grey");
        ImGui::Text("gain  %.3f %.3f %.3f", g_look.gain[0], g_look.gain[1], g_look.gain[2]);
    }
    if (ImGui::CollapsingHeader("Grade", ImGuiTreeNodeFlags_DefaultOpen)) {
        SliderBox("Saturation", &g_cfg.saturation, 0.0f, 2.0f, 0.0f, 10.0f);
        SliderBox("Contrast", &g_cfg.contrast, 0.5f, 2.0f, 0.1f, 10.0f);
    }
    if (ImGui::CollapsingHeader("Atmosphere", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Classic sky and fog (Classic Atmosphere's, modern lighting kept)", &g_cfg.classicSky);
        if (g_cfg.classicSky) {
            ImGui::Text("fog density %.5f   ground layer %.4f (DE's values)", g_look.fogDensityDE, g_look.secondFogDE);
        } else {
            ImGui::Checkbox("GTA fog (timecyc far clip)", &g_cfg.gtaFog);
            if (g_cfg.gtaFog) {
                SliderBox("Fog distance", &g_cfg.fogDistance, 0.5f, 5.0f, 0.5f, 5.0f, "x%.2f");
                SliderBox("Fog at far clip", &g_cfg.fogOpacity, 0.05f, 0.95f, 0.05f, 0.95f);
                SliderBox("Ground haze", &g_cfg.groundHaze, 0.0f, 2.0f, 0.0f, 20.0f);
                ImGui::Text("clear to %.0f m, %.0f%% at %.0f m   density %.5f", g_look.gtaFogStart, g_cfg.fogOpacity * 100.0f,
                            g_look.gtaFogFar, g_look.fogDensityApplied);
            } else {
                SliderBox("Haze (height fog)", &g_cfg.haze, 0.0f, 2.0f, 0.0f, 20.0f);
                SliderBox("Ground haze", &g_cfg.groundHaze, 0.0f, 2.0f, 0.0f, 20.0f);
                ImGui::Text("fog density DE %.5f -> %.5f   ground layer DE %.4f", g_look.fogDensityDE, g_look.fogDensityApplied, g_look.secondFogDE);
            }
        }
    }
    if (ImGui::CollapsingHeader("Timecyc colours", ImGuiTreeNodeFlags_DefaultOpen)) {
        SliderBox("Sky", &g_cfg.skyStrength, 0.0f, 1.0f, 0.0f, 4.0f);
        SliderBox("Fog colour", &g_cfg.fogStrength, 0.0f, 1.0f, 0.0f, 4.0f);
        SliderBox("Ambient", &g_cfg.ambientStrength, 0.0f, 1.0f, 0.0f, 4.0f);
        SliderBox("Sun", &g_cfg.sunTint, 0.0f, 1.0f, 0.0f, 4.0f);
        SliderBox("Clouds", &g_cfg.cloudTint, 0.0f, 1.0f, 0.0f, 4.0f);
        SliderBox("Brightness (timecyc sky, original sun hours)", &g_cfg.brightness, 0.0f, 2.0f, 0.0f, 4.0f);
        SliderBox("Night exposure (EV; + = brighter nights)", &g_cfg.nightExposure, -4.0f, 4.0f, -10.0f, 10.0f, "%.2f");
        if (g_curColours) {
            const uint8_t* cc = g_curColours;
            float top[3], bottom[3], sun[3], clouds[3];
            ReadU16Rgb(cc + CS::SkyTop, top);
            ReadU16Rgb(cc + CS::SkyBottom, bottom);
            ReadU16Rgb(cc + CS::SunCore, sun);
            ReadU16Rgb(cc + CS::LowClouds, clouds);
            ImGui::SeparatorText("timecyc (current)");
            Swatch("sky top", top);
            Swatch("sky bottom", bottom);
            Swatch("sun core", sun);
            Swatch("low clouds", clouds);
            const float* p1 = (const float*)(cc + CS::PostFx1);
            const float* p2 = (const float*)(cc + CS::PostFx2);
            ImGui::Text("postfx1 %3.0f %3.0f %3.0f a%3.0f   postfx2 %3.0f %3.0f %3.0f a%3.0f", p1[0], p1[1], p1[2], p1[3], p2[0], p2[1], p2[2], p2[3]);
            ImGui::Text("shadow strength %.2f", g_look.shadow);
            if (uint8_t* tod = (uint8_t*)g_look.tod) {
                ImGui::SeparatorText("DE sky set (applied)");
                const uint8_t* live = tod + TOD::LiveColors;
                LinearSwatch("sky upper", (const float*)(live + SCS::SkyUpper));
                LinearSwatch("sky lower", (const float*)(live + SCS::SkyLower));
                LinearSwatch("fog", (const float*)(live + SCS::Fog));
                LinearSwatch("skylight", (const float*)(live + SCS::Skylight));
                LinearSwatch("sun", (const float*)(live + SCS::Sun));
                LinearSwatch("clouds", (const float*)(live + SCS::VolumetricCloud));
            }
        }
    }
    if (ImGui::CollapsingHeader("Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
        SliderBox("Darkness", &g_cfg.shadowDarkness, 0.0f, 0.9f, 0.0f, 1.0f);
        ImGui::Text("shadow fill (sky light, uplighting) x%.2f  (%d post-process volumes)", g_look.indirect, g_look.volumes);
    }
    if (ImGui::CollapsingHeader("Characters", ImGuiTreeNodeFlags_DefaultOpen)) PedsPanel();
    if (ImGui::CollapsingHeader("Lamp coronas", ImGuiTreeNodeFlags_DefaultOpen)) CoronasPanel();
    if (ImGui::CollapsingHeader("Street lights", ImGuiTreeNodeFlags_DefaultOpen)) StreetLightsPanel();
    if (ImGui::CollapsingHeader("Post effects", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("SpeedFX (original speed blur)", &g_cfg.speedFx);
        ImGui::SameLine();
        ImGui::Checkbox("Test mode", &g_cfg.speedFxTestMode);
        ImGui::Checkbox("Radiosity (PS2 glow)", &g_cfg.radiosity);
        ImGui::SameLine();
        ImGui::Checkbox("Grain (rain)", &g_cfg.grain);
        ImGui::SameLine();
        ImGui::Checkbox("Water drops", &g_cfg.waterDrops);
        SliderBoxInt("Max water drops", &g_cfg.maxDrops, 0, 2000, 0, 2000);
        SliderBoxInt("Radiosity intensity", &g_cfg.radiosityIntensity, 0, 255, 0, 255);
        SliderBox("Radiosity offset (PS2 px)", &g_cfg.radiosityOffset, 0.0f, 12.0f, 0.0f, 64.0f, "%.1f");
        SliderBox("Grain strength", &g_cfg.grainStrength, 0.0f, 2.0f, 0.0f, 10.0f);
        SliderBox("DE bloom (with radiosity)", &g_cfg.deBloom, 0.0f, 1.0f, 0.0f, 4.0f);
        ImGui::Text("highlight limit %d, rain %.2f, grain mask %d", g_curColours ? *(const int32_t*)(g_curColours + 0x9C) : -1,
                    g_fx.rain, g_fx.grain);
        SliderBoxInt("Before backbuffer bind", &g_cfg.speedFxHudBind, 0, 6, 0, 16);
        ImGui::TextDisabled("row %d, looking %d (0 = at Present, HUD blurred)", g_speedFxRow & 0xFF, g_speedFxRow < 0 ? 0 : g_speedFxRow >> 8);
    }
    ImGui::Separator();
    if (ImGui::Button("Save to ini")) SaveIni();
    ImGui::SameLine();
    if (ImGui::Button("Reload ini")) { ReadIni(); ApplyTimecycFile(); }
    ImGui::SameLine();
    ImGui::TextDisabled("timecyc: %s", g_customTimecyc ? g_cfg.timecycFile : "game's own");
}
