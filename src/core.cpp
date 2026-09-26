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
    c.logLevel            = GetPrivateProfileIntA("General", "Log", 1, ini);
    IniKey("Menu", "Ctrl+Shift+M", c.keyMenu);
    IniKey("Toggle", "Ctrl+Shift+E", c.keyToggle);
    IniKey("Reload", "Ctrl+Shift+R", c.keyReload);
    IniKey("Freecam", "Ctrl+Shift+F", c.keyFreecam);
    IniKey("Noclip", "Ctrl+Shift+N", c.keyNoclip);
    c.filter              = GetPrivateProfileIntA("ColourFilter", "PS2Filter", 1, ini) != 0;
    c.filterStrength      = Clamp(IniFloat("ColourFilter", "Strength", 1.0f), 0.0f, 2.0f);
    c.gameTimecycStrength = Clamp(IniFloat("ColourFilter", "GameTimecycStrength", 0.35f), 0.0f, 2.0f);
    c.keepBrightness      = Clamp(IniFloat("ColourFilter", "KeepBrightness", 0.5f), 0.0f, 1.0f);
    c.gamma               = Clamp(IniFloat("ColourFilter", "Gamma", 2.2f), 1.0f, 3.0f);
    c.saturation          = Clamp(IniFloat("Grade", "Saturation", 1.15f), 0.0f, 2.0f);
    c.contrast            = Clamp(IniFloat("Grade", "Contrast", 1.05f), 0.5f, 2.0f);
    c.skyStrength         = Clamp(IniFloat("Colours", "Sky", 1.0f), 0.0f, 1.0f);
    c.fogStrength         = Clamp(IniFloat("Colours", "Fog", 1.0f), 0.0f, 1.0f);
    c.ambientStrength     = Clamp(IniFloat("Colours", "Ambient", 1.0f), 0.0f, 1.0f);
    c.sunTint             = Clamp(IniFloat("Colours", "Sun", 0.35f), 0.0f, 1.0f);
    c.cloudTint           = Clamp(IniFloat("Colours", "Clouds", 0.5f), 0.0f, 1.0f);
    c.haze                = Clamp(IniFloat("Atmosphere", "Haze", 0.35f), 0.0f, 2.0f);
    c.groundHaze          = Clamp(IniFloat("Atmosphere", "GroundHaze", 0.0f), 0.0f, 2.0f);
    c.gtaFog              = GetPrivateProfileIntA("Atmosphere", "GtaFog", 1, ini) != 0;
    c.fogDistance         = Clamp(IniFloat("Atmosphere", "FogDistance", 1.8f), 0.5f, 5.0f);
    c.fogOpacity          = Clamp(IniFloat("Atmosphere", "FogOpacity", 0.5f), 0.05f, 0.95f);
    c.shadowDarkness      = Clamp(IniFloat("Shadows", "Darkness", 0.5f), 0.0f, 0.9f);
    c.pedMatte            = Clamp(IniFloat("Characters", "Matte", 0.6f), 0.0f, 1.0f);
    c.freecamSpeed        = Clamp(IniFloat("Tools", "FreecamSpeed", 20.0f), 1.0f, 500.0f);
    c.freecamSensitivity  = Clamp(IniFloat("Tools", "FreecamSensitivity", 0.15f), 0.01f, 2.0f);
    c.noclipSpeed         = Clamp(IniFloat("Tools", "NoclipSpeed", 15.0f), 1.0f, 500.0f);
    GetPrivateProfileStringA("Timecyc", "File", "", c.timecycFile, MAX_PATH, ini);
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
    PutFloat("ColourFilter", "Gamma", c.gamma);
    PutFloat("Grade", "Saturation", c.saturation);
    PutFloat("Grade", "Contrast", c.contrast);
    PutFloat("Colours", "Sky", c.skyStrength);
    PutFloat("Colours", "Fog", c.fogStrength);
    PutFloat("Colours", "Ambient", c.ambientStrength);
    PutFloat("Colours", "Sun", c.sunTint);
    PutFloat("Colours", "Clouds", c.cloudTint);
    PutFloat("Atmosphere", "Haze", c.haze);
    PutFloat("Atmosphere", "GroundHaze", c.groundHaze);
    WritePrivateProfileStringA("Atmosphere", "GtaFog", c.gtaFog ? "1" : "0", g_iniPath);
    PutFloat("Atmosphere", "FogDistance", c.fogDistance);
    PutFloat("Atmosphere", "FogOpacity", c.fogOpacity);
    PutFloat("Shadows", "Darkness", c.shadowDarkness);
    PutFloat("Characters", "Matte", c.pedMatte);
    PutFloat("Tools", "FreecamSpeed", c.freecamSpeed);
    PutFloat("Tools", "FreecamSensitivity", c.freecamSensitivity);
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
constexpr size_t Skylight = 0x00, SkyLower = 0x10, SkyUpper = 0x20, VolumetricCloud = 0x60, Fog = 0x118, Sun = 0x158;
}
namespace TOD { // AGTATimeOfDay
constexpr size_t LiveColors = 0x2B8;    // SkyColorSet (used by the renderer)
constexpr size_t TargetColors = 0x458;  // written by CTimeCycle::Update, copied to LiveColors
constexpr size_t OfSingleton = 0x688;   // AGTATimeOfDay* in DE's engine singleton
constexpr size_t VgdOverrideClass = 0x4770; // TSubclassOf<AVGDOverrideData>: DE's GTA fog StartDistance source
}
constexpr size_t UCLASS_CDO = 0x118;          // UClass::ClassDefaultObject
constexpr size_t VGD_FogStartDistance = 0x220; // float StartDistance, then VolumetricFogExtinctionScale (sub_140B51860)
namespace PP { // APostProcessVolume::Settings (FPostProcessSettings)
constexpr size_t Settings = 0x260;
constexpr size_t OverrideByte0 = 0x00;  // bit 2 ColorSaturation, bit 3 ColorContrast, bit 5 ColorGain
constexpr uint8_t SaturationBit = 0x04, ContrastBit = 0x08, ColorGainBit = 0x20;
constexpr uint8_t GradeBits = SaturationBit | ContrastBit | ColorGainBit;
constexpr size_t OverrideByte16 = 0x16; // bit 0 = bOverride_IndirectLightingIntensity
constexpr uint8_t IndirectBit = 0x01;
constexpr size_t ColorSaturation = 0x30, ColorContrast = 0x40, ColorGain = 0x60; // FVector4
constexpr size_t IndirectLightingIntensity = 0x464;
constexpr size_t IsInterior = 0x864;    // AGTAPostProcessVolume::bIsInteriorPostProcess (outside Settings)
}
namespace FOG { // AGTAHeightFog / UExponentialHeightFogComponent
constexpr size_t Component = 0x2A8;     // AGTAHeightFog::HeightFogComponent
constexpr size_t Tod = 0x2B0;           // AGTAHeightFog::TimeOfDayActor
constexpr size_t InscatterColor = 0x20C; // component FogInscatteringColor (FLinearColor)
constexpr size_t Density = 0x1F8;       // FogDensity (DE: FogParameters.x)
constexpr size_t SecondDensity = 0x200; // SecondFogData.FogDensity (DE: fixed 0.02 ground layer)
constexpr size_t UseGtaValues = 0x2A0;  // AGTAHeightFog::bUseGTAValues: DE's timecyc fog path (as in Classic)
constexpr size_t EnableVolumetric = 0x268; // component bEnableVolumetricFog (the GTA path clears it)
}
constexpr size_t Obj_Index = 0x0C;

// ----------------------------------------------------------------------
// Timecyc tables: column order of timecyc.dat -> DE array (RVA), [8 hours][23 weathers].
// ----------------------------------------------------------------------
const TcCol kTimecycCols[52] = {
    { 0x523A4C0, K_U8 }, { 0x523A640, K_U8 }, { 0x523A580, K_U8 },   // ambient
    { 0x523A280, K_U8 }, { 0x523A1C0, K_U8 }, { 0x523A400, K_U8 },   // ambient obj
    { 0, K_SKIP }, { 0, K_SKIP }, { 0, K_SKIP },                     // directional (unused by the game)
    { 0x523A340, K_U8 }, { 0x5239F80, K_U8 }, { 0x5239EC0, K_U8 },   // sky top
    { 0x523A100, K_U8 }, { 0x523A040, K_U8 }, { 0x5239C80, K_U8 },   // sky bottom
    { 0x5239BC0, K_U8 }, { 0x5239E00, K_U8 }, { 0x5239D40, K_U8 },   // sun core
    { 0x5239980, K_U8 }, { 0x52398C0, K_U8 }, { 0x5239B00, K_U8 },   // sun corona
    { 0x5239A40, K_X10 }, { 0x5239680, K_X10 }, { 0x52395C0, K_X10 }, // sun size, sprite size, sprite brightness
    { 0x5239800, K_U8 }, { 0x5239740, K_U8 }, { 0x52392D0, K_U8 },   // shadow, light shadow, pole shadow
    { 0x5239160, K_I16 }, { 0x5239450, K_I16 }, { 0x5239390, K_X10 }, // far clip, fog start, light on ground
    { 0x523B5A0, K_U8 }, { 0x523B4E0, K_U8 }, { 0x523B720, K_U8 },   // low clouds
    { 0x523B660, K_U8 }, { 0x523B2A0, K_U8 }, { 0x523B1E0, K_U8 },   // bottom clouds
    { 0x523B420, K_U8 }, { 0x523B360, K_U8 }, { 0x523AF80, K_U8 }, { 0x523AEC0, K_U8 }, // water rgba
    { 0x523A940, K_U8 }, { 0x523B120, K_U8 }, { 0x523B040, K_U8 }, { 0x523AC60, K_U8 }, // postfx1 a,r,g,b
    { 0x523A880, K_U8 }, { 0x523AB80, K_U8 }, { 0x523AE00, K_U8 }, { 0x523AD30, K_U8 }, // postfx2 a,r,g,b
    { 0x523AAC0, K_U8 }, { 0x523AA00, K_U8 }, { 0x523A7C0, K_U8 },   // cloud alpha, highlight min, water fog alpha
    { 0, K_SKIP },                                                    // directional mult (DE forces 1.28)
};

// Every RVA above must be read by DE's CColourSet constructor, otherwise the table belongs to another build.
static bool VerifyTimecycTable(const uint8_t* ctor, size_t len) {
    for (const TcCol& c : kTimecycCols) {
        if (c.kind == K_SKIP) continue;
        bool seen = false;
        for (size_t i = 0; i + 4 <= len && !seen; ++i) seen = *(const uint32_t*)(ctor + i) == c.rva;
        if (!seen) return false;
    }
    return true;
}

// Loads a timecyc.dat (same format as the game's) into DE's tables. 8-hour files map 1:1; 24-hour files
// (23 weathers x 24 lines) contribute the hours of the 8 original slots.
static const int kSlotHours[kHours] = { 0, 5, 6, 7, 12, 19, 20, 22 };
bool LoadTimecycFile(const char* path) {
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
                const TcCol& col = kTimecycCols[c];
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

// skygfx CPostEffects::ColourFilter_PS2: the frame is drawn with postfx1 (MODULATE2X, replace), then
// postfx2 is added with its alpha (also MODULATE2X): gain = 2*c1 + 2*c2 * min(1, 2*a2), per channel.
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
    for (int i = 0; i < 3; ++i) {
        const float lin = powf(Clamp(g[i] / norm, 0.0f, 4.0f), g_cfg.gamma);
        gain[i] = Clamp(1.0f + (lin - 1.0f) * strength, 0.0f, 4.0f);
    }
}

// ----------------------------------------------------------------------
// Post-process volumes seen in DE's UpdateColorOptions
// ----------------------------------------------------------------------
struct Volume {
    uint8_t* obj; int32_t index, serial;
    float deGain[4], deSat[4], deContrast[4]; // DE's own values (brightness/contrast options), after UpdateColorOptions
    uint8_t deOverride0;                       // DE's override bits in byte 0
    bool origIndirectOverride; float origIndirect;
};
static Volume g_volumes[64];
static int g_volumeCount = 0;
static SRWLOCK g_lock = SRWLOCK_INIT;
static float g_gain[3] = { 1, 1, 1 };
static float g_indirect = 1.0f;

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

    float* indirect = (float*)(s + PP::IndirectLightingIntensity);
    if (on && g_cfg.shadowDarkness > 0.0f && !v.obj[PP::IsInterior]) {
        s[PP::OverrideByte16] |= PP::IndirectBit;
        *indirect = (v.origIndirectOverride ? v.origIndirect : 1.0f) * g_indirect;
    } else {
        if (v.origIndirectOverride) s[PP::OverrideByte16] |= PP::IndirectBit;
        else s[PP::OverrideByte16] &= (uint8_t)~PP::IndirectBit;
        *indirect = v.origIndirect;
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
    v.origIndirectOverride = (s[PP::OverrideByte16] & PP::IndirectBit) != 0;
    v.origIndirect = v.origIndirectOverride ? *(const float*)(s + PP::IndirectLightingIntensity) : 1.0f;
    Log(1, "post-process volume %p tracked (%d total, interior=%d)", obj, g_volumeCount, obj[PP::IsInterior]);
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
// Height fog, AGTAHeightFog::UpdateColors. Two modes:
// - GTA fog (default): DE's own timecyc fog path, the one Classic Atmosphere uses. ShouldUseGTAFog (0x140B65820)
//   is true when bUseGTAValues is set, so the modern lighting stays. Density comes from the timecyc fog start /
//   far clip (CTimeCycle::Update copies them to the time-of-day actor, PerFrame scales them by FogDistance). That
//   path turns volumetric fog off unless gta.ShowVolumeFogInClassic is set; the byte is set for the call only.
// - Modern fog: FogDensity from the sky set plus a fixed 0.02 ground layer, scaled by Haze / GroundHaze after DE
//   writes them (only when DE changed them, so the render state is marked dirty at most once per change).
// ----------------------------------------------------------------------
typedef uintptr_t (*FogUpdate_Fn)(void* fogActor, float dt);
typedef uintptr_t (*MarkDirty_Fn)(void* component);
static FogUpdate_Fn o_FogUpdateColors = nullptr;
static MarkDirty_Fn g_MarkRenderStateDirty = nullptr;
uint8_t* g_volFogInClassic = nullptr; // gta.ShowVolumeFogInClassic value, read only by UpdateColors

static bool GtaFogOn() { return g_active && g_cfg.gtaFog && !(g_classicFlag && *g_classicFlag); }

static uintptr_t Hooked_FogUpdateColors(void* fogActor, float dt) {
    // DE compares its new value with the component before writing; restore DE's values first so its
    // change detection keeps working, then apply ours.
    static float lastDeMain = -1.0f, lastDeSecond = -1.0f, lastMain = -1.0f, lastSecond = -1.0f;
    static float lastDeCol[4] = { -1, -1, -1, -1 }, lastCol[4] = { -1, -1, -1, -1 };
    static uint8_t* deActor = nullptr;
    static uint8_t deUseGta = 0;
    const bool gtaFog = GtaFogOn();
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
    const uintptr_t r = o_FogUpdateColors(fogActor, dt);
    if (g_volFogInClassic) *g_volFogInClassic = volSaved;
    __try {
        if (!comp) return r;
        float* density = (float*)(comp + FOG::Density);
        float* second = (float*)(comp + FOG::SecondDensity);
        const bool classic = g_classicFlag && *g_classicFlag;
        lastDeMain = *density;
        lastDeSecond = *second;
        g_look.fogDensityDE = *density;
        g_look.secondFogDE = *second;
        // GTA fog: DE's density follows a per-weather value more than the timecyc distances (too thick, deaf to
        // FogDistance), so it is replaced. Clear up to StartDistance (half the scaled far clip, set in PerFrame), then
        // FogOpacity reached at the scaled far clip. UE 4.26 height fog on a level ray: opacity = 1 - exp(-(ln 2)^2 *
        // FogDensity / 1000 * cm). ponytail: level ray at fog height; the camera's height above the fog actor thins it.
        const float rangeCm = (g_look.farClip - g_look.fogStart) * 100.0f;
        const bool gtaOn = g_active && !classic && gtaFog && rangeCm > 0.0f;
        const bool modernOn = g_active && !classic && !gtaFog;
        const float wantMain = gtaOn ? -logf(1.0f - g_cfg.fogOpacity) * 1000.0f / (0.480453f * rangeCm)
                             : modernOn ? *density * g_cfg.haze : *density;
        const float wantSecond = gtaOn || modernOn ? *second * g_cfg.groundHaze : *second;
        g_look.fogDensityApplied = wantMain;
        g_look.gtaFog = gtaFog;
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
static void ApplyColours(uint8_t* tod, const uint8_t* cc) {
    const float* src = (const float*)(cc + CS::SkyColorSet);
    float amb[3], top[3], bottom[3], sun[3], clouds[3];
    for (int i = 0; i < 3; ++i) amb[i] = ((const float*)(cc + CS::Ambient))[i];
    ReadU16Rgb(cc + CS::SkyTop, top);
    ReadU16Rgb(cc + CS::SkyBottom, bottom);
    ReadU16Rgb(cc + CS::SunCore, sun);
    ReadU16Rgb(cc + CS::LowClouds, clouds);
    struct Field { size_t off; const float* colour; float strength; } fields[] = {
        { SCS::SkyUpper, top, g_cfg.skyStrength },
        { SCS::SkyLower, bottom, g_cfg.skyStrength },
        { SCS::Fog, bottom, g_cfg.fogStrength },
        { SCS::Skylight, amb, g_cfg.ambientStrength },
        { SCS::Sun, sun, g_cfg.sunTint },
        { SCS::VolumetricCloud, clouds, g_cfg.cloudTint },
    };
    for (const Field& f : fields) {
        float c[4];
        memcpy(c, (const uint8_t*)src + f.off, sizeof(c));
        TransferHue(c, f.colour, f.strength);
        memcpy(tod + TOD::TargetColors + f.off, c, sizeof(c));
        memcpy(tod + TOD::LiveColors + f.off, c, sizeof(c));
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

    const bool classic = g_classicFlag && *g_classicFlag;
    uint8_t* tod = *g_singleton ? *(uint8_t**)(*g_singleton + TOD::OfSingleton) : nullptr;
    if (g_active && tod && !classic) ApplyColours(tod, cc);
    // GTA fog: fog starts at half the scaled timecyc far clip. The original's fog started near 0 m (timecyc fog start
    // median 10 m), too thick for DE's full-distance world. DE copies StartDistance every frame from its fog override
    // data (class default object + 0x220), so the value is written at that source; restored when GTA fog is off.
    g_look.farClip = *(const float*)(cc + CS::FarClip) * g_cfg.fogDistance;
    g_look.fogStart = g_look.farClip * 0.5f;
    static float* startSrc = nullptr;
    static float startDE = 0.0f;
    uint8_t* vgdClass = tod ? *(uint8_t**)(tod + TOD::VgdOverrideClass) : nullptr;
    float* src = vgdClass && *(uint8_t**)(vgdClass + UCLASS_CDO) ? (float*)(*(uint8_t**)(vgdClass + UCLASS_CDO) + VGD_FogStartDistance) : nullptr;
    if (src != startSrc) {
        if (startSrc) *startSrc = startDE;
        startSrc = src;
        if (src) startDE = *src;
    }
    if (startSrc) *startSrc = GtaFogOn() ? g_look.fogStart * 100.0f : startDE;

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
               "gain=(%.3f %.3f %.3f) shadow=%.2f indirect=%.2f fog=%.5f->%.5f groundFog=%.4f gtaFog=%d fogStart=%.0fm farClip=%.0fm",
            g_look.frames, g_active, classic, volumes, p1[0], p1[1], p1[2], p1[3], p2[0], p2[1], p2[2], p2[3],
            g_gain[0], g_gain[1], g_gain[2], shadow, g_indirect, g_look.fogDensityDE, g_look.fogDensityApplied, g_look.secondFogDE,
            g_look.gtaFog, g_look.fogStart, g_look.farClip);
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
            *fogUpdate = nullptr, *markDirty = nullptr;
    const Sig sigs[] = {
        { "CTimeCycle::Update", "4C 8B DC 55 56 49 8D 6B A1 48 81 EC C8 00 00 00 45 0F 29 4B A8 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 D7", &update },
        { "CTimeCycle::Initialise", "48 8B C4 88 48 08 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 80 48 81 EC B0 02 00 00 F3 0F 10 05 ?? ?? ?? ?? 48 8D 1D", &init },
        { "SetClassicAtmosphere", "4C 8B DC 55 57 49 8D 6B D8 48 81 EC 18 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 E0 38 15 ?? ?? ?? ?? 48 8B F9 0F 84", &setClassic },
        { "CColourSet::CColourSet", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 48 8B D9 49 63 F8 48 81 C1 AC 00 00 00 E8", &ctor },
        { "AGTAPostProcessVolume::UpdateColorOptions", "48 8B C4 55 41 56 48 8D 68 A1 48 81 EC E8 00 00 00 48 83 3D ?? ?? ?? ?? 00 4C 8B F1 0F 84 ?? ?? ?? ?? 0F 29 78 A8", &colorOptions },
        { "GObjects ref", "48 8D 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? E8 03 00 00 48 8D 0D ?? ?? ?? ??", &gobjRef },
        { "AGTAHeightFog::UpdateColors", "48 8B C4 55 57 48 8D 68 A1 48 81 EC D8 00 00 00 48 83 B9 B0 02 00 00 00 48 8B F9", &fogUpdate },
        { "UActorComponent::MarkRenderStateDirty", "40 53 48 83 EC 20 48 8B D9 0F B6 89 88 00 00 00 0F B6 C1 24 03 3C 03 0F 85", &markDirty },
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
    g_timecycTableOk = VerifyTimecycTable(ctor, 1100);
    Log(1, "timecyc table %s", g_timecycTableOk ? "verified" : "NOT verified: Timecyc.File ignored");
    g_MarkRenderStateDirty = (MarkDirty_Fn)markDirty;
    static const uint8_t cmpByte[] = { 0x80, 0x3D }; // UpdateColors+0x84: cmp cs:gta.ShowVolumeFogInClassic, 0
    g_volFogInClassic = RipAt(fogUpdate + 0x84, cmpByte, 2, 7);
    if (!g_volFogInClassic) Log(1, "gta.ShowVolumeFogInClassic not found: GTA fog turns volumetric fog off");

    if (MH_Initialize() != MH_OK) return false;
    ok = MH_CreateHook(update, (void*)&Hooked_TimeCycleUpdate, (void**)&o_TimeCycleUpdate) == MH_OK &&
         MH_CreateHook(init, (void*)&Hooked_TimeCycleInit, (void**)&o_TimeCycleInit) == MH_OK &&
         MH_CreateHook(colorOptions, (void*)&Hooked_UpdateColorOptions, (void**)&o_UpdateColorOptions) == MH_OK &&
         MH_CreateHook(fogUpdate, (void*)&Hooked_FogUpdateColors, (void**)&o_FogUpdateColors) == MH_OK;
    if (!ok) { Log(1, "hook creation failed"); MH_Uninitialize(); return false; }
    if (!InstallTools()) Log(1, "debug tools unavailable (see above); the look still works");
    InstallPeds();
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

void LookPanel() {
    ImGui::Checkbox("Effect on", &g_active);
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", g_cfg.keyToggle.text);
    if (g_look.classic) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Classic Atmosphere is ON: only filter/grade/shadows apply");

    if (ImGui::CollapsingHeader("PS2 colour filter", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("PS2 filter", &g_cfg.filter);
        if (g_customTimecyc) {
            ImGui::SliderFloat("Strength", &g_cfg.filterStrength, 0.0f, 2.0f);
            ImGui::SliderFloat("Keep brightness", &g_cfg.keepBrightness, 0.0f, 1.0f);
        } else {
            ImGui::SliderFloat("Strength (game timecyc)", &g_cfg.gameTimecycStrength, 0.0f, 2.0f);
        }
        ImGui::SliderFloat("Gamma", &g_cfg.gamma, 1.0f, 3.0f);
        ImGui::Text("gain  %.3f %.3f %.3f", g_look.gain[0], g_look.gain[1], g_look.gain[2]);
    }
    if (ImGui::CollapsingHeader("Grade", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Saturation", &g_cfg.saturation, 0.0f, 2.0f);
        ImGui::SliderFloat("Contrast", &g_cfg.contrast, 0.5f, 2.0f);
    }
    if (ImGui::CollapsingHeader("Atmosphere", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("GTA fog (timecyc fog start / far clip)", &g_cfg.gtaFog);
        if (g_cfg.gtaFog) {
            ImGui::SliderFloat("Fog distance", &g_cfg.fogDistance, 0.5f, 5.0f, "x%.2f");
            ImGui::SliderFloat("Fog at far clip", &g_cfg.fogOpacity, 0.05f, 0.95f, "%.2f");
            ImGui::Text("clear to %.0f m, %.0f%% at %.0f m   density %.5f", g_look.fogStart, g_cfg.fogOpacity * 100.0f,
                        g_look.farClip, g_look.fogDensityApplied);
        } else {
            ImGui::SliderFloat("Haze (height fog)", &g_cfg.haze, 0.0f, 2.0f);
            ImGui::SliderFloat("Ground haze", &g_cfg.groundHaze, 0.0f, 2.0f);
            ImGui::Text("fog density DE %.5f -> %.5f   ground layer DE %.4f", g_look.fogDensityDE, g_look.fogDensityApplied, g_look.secondFogDE);
        }
    }
    if (ImGui::CollapsingHeader("Timecyc colours", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Sky", &g_cfg.skyStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("Fog colour", &g_cfg.fogStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("Ambient", &g_cfg.ambientStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("Sun", &g_cfg.sunTint, 0.0f, 1.0f);
        ImGui::SliderFloat("Clouds", &g_cfg.cloudTint, 0.0f, 1.0f);
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
        ImGui::SliderFloat("Darkness", &g_cfg.shadowDarkness, 0.0f, 0.9f);
        ImGui::Text("indirect light x%.2f  (%d post-process volumes)", g_look.indirect, g_look.volumes);
    }
    if (ImGui::CollapsingHeader("Characters", ImGuiTreeNodeFlags_DefaultOpen)) PedsPanel();
    ImGui::Separator();
    if (ImGui::Button("Save to ini")) SaveIni();
    ImGui::SameLine();
    if (ImGui::Button("Reload ini")) { ReadIni(); ApplyTimecycFile(); }
    ImGui::SameLine();
    ImGui::TextDisabled("timecyc: %s", g_customTimecyc ? g_cfg.timecycFile : "game's own");
}
