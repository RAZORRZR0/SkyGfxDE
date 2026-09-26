// SkyGfxDE debug tools: weather and time control, noclip and freecam.
// Game-side work runs on the game thread inside two hooks (CGame::Process order: clock tick ->
// CWeather::Update -> ... -> CCamera::Process). The ImGui panel (render thread) only posts requests.
#include "skygfx.h"
#include <math.h>
#include <string.h>
#include "../minhook/MinHook.h"
#include "../imgui/imgui.h"

bool GameWindowFocused();

const char* const kWeatherNames[kWeathers] = {
    "EXTRASUNNY_LA", "SUNNY_LA", "EXTRASUNNY_SMOG_LA", "SUNNY_SMOG_LA", "CLOUDY_LA",
    "SUNNY_SF", "EXTRASUNNY_SF", "CLOUDY_SF", "RAINY_SF", "FOGGY_SF",
    "SUNNY_VEGAS", "EXTRASUNNY_VEGAS", "CLOUDY_VEGAS",
    "EXTRASUNNY_COUNTRYSIDE", "SUNNY_COUNTRYSIDE", "CLOUDY_COUNTRYSIDE", "RAINY_COUNTRYSIDE",
    "EXTRASUNNY_DESERT", "SUNNY_DESERT", "SANDSTORM_DESERT", "UNDERWATER", "EXTRACOLOURS_1", "EXTRACOLOURS_2",
};
static const char* const kRegionNames[5] = { "Countryside (default)", "Los Santos", "San Fierro", "Las Venturas", "Desert" };

// ---------------------------------------------------------------- game state (resolved in InstallTools)
static int16_t*  g_oldWeather = nullptr;   // CWeather::OldWeatherType
static int16_t*  g_newWeather = nullptr;   // CWeather::NewWeatherType
static int16_t*  g_forcedWeather = nullptr;// CWeather::ForcedWeatherType (-1 = none)
static float*    g_interp = nullptr;       // CWeather::InterpolationValue
static int16_t*  g_region = nullptr;       // CWeather::WeatherRegion
static uint8_t*  g_hours = nullptr;        // CClock::ms_nGameClockHours
static uint8_t*  g_minutes = nullptr;      // CClock::ms_nGameClockMinutes
static uint16_t* g_seconds = nullptr;      // CClock::ms_nGameClockSeconds
static uint32_t* g_lastTick = nullptr;     // CClock::ms_nLastClockTick
static uint32_t* g_msPerMinute = nullptr;  // CClock::ms_nMillisecondsPerGameMinute
static uint32_t* g_timeMs = nullptr;       // CTimer::m_snTimeInMilliseconds
static uint8_t** g_camMatrix = nullptr;    // TheCamera.m_matrix (CMatrix*)

typedef uintptr_t (*Void_Fn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*SetGameClock_Fn)(uint8_t hours, uint8_t minutes, uint8_t day);
typedef uint8_t* (*FindPlayerEntity_Fn)();
static Void_Fn o_WeatherUpdate = nullptr;
static Void_Fn o_CameraProcess = nullptr;
static SetGameClock_Fn g_SetGameClock = nullptr;
static FindPlayerEntity_Fn g_FindPlayerEntity = nullptr;
static bool g_toolsOk = false;

namespace ENT { // CEntity / CPhysical (x64 DE)
constexpr size_t Matrix = 0x18;       // CMatrix*: right, forward, up, pos (16-byte rows)
constexpr size_t Flags = 0x30;        // bit 0 bUsesCollision
constexpr uint32_t UsesCollision = 0x01;
constexpr size_t MoveSpeed = 0x7C;    // CVector
constexpr size_t TurnSpeed = 0x88;    // CVector
}

// ---------------------------------------------------------------- requests from the panel
enum WeatherMode { WM_GAME, WM_FORCE, WM_BLEND };
static volatile int  g_weatherMode = WM_GAME;
static volatile int  g_weatherA = 0, g_weatherB = 4;
static volatile float g_blend = 0.0f;
static volatile bool g_releaseWeather = false;
static volatile int  g_setHour = -1, g_setMinute = 0;
static volatile bool g_freezeTime = false;
static volatile float g_timeSpeed = 1.0f;
static uint32_t g_baseMsPerMinute = 0;

// ---------------------------------------------------------------- freecam / noclip state (game thread)
static volatile bool g_freecam = false, g_noclip = false;
static volatile LONG g_mouseDx = 0, g_mouseDy = 0, g_wheel = 0;
static float g_camPos[3], g_yaw = 0.0f, g_pitch = 0.0f, g_freecamMul = 1.0f;
static bool g_freecamInit = false;
static uint8_t* g_noclipEntity = nullptr;
static bool g_noclipHadCollision = false;
static volatile bool g_teleportToCam = false;
static float g_lastPlayerPos[3], g_lastCamPos[3];
static LARGE_INTEGER g_qpf, g_lastQpc;

void ToolsToggleFreecam() { g_freecam = !g_freecam; g_freecamInit = false; Log(1, "freecam %s", g_freecam ? "on" : "off"); }
void ToolsToggleNoclip() { g_noclip = !g_noclip; Log(1, "noclip %s", g_noclip ? "on" : "off"); }
bool ToolsWantCapture() { return g_freecam || g_noclip; }
void ToolsAddMouseDelta(long dx, long dy) { InterlockedAdd(&g_mouseDx, dx); InterlockedAdd(&g_mouseDy, dy); }
void ToolsAddWheel(int notches) { InterlockedAdd(&g_wheel, notches); }

static bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// WASD + Space/C relative to (yaw, pitch); returns false when there is no input.
static bool MoveInput(float yaw, float pitch, bool flyPitch, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (g_menuOpen || !GameWindowFocused()) return false;
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    const float fwd[3] = { flyPitch ? cp * cy : cy, flyPitch ? cp * sy : sy, flyPitch ? sp : 0.0f };
    const float right[3] = { sy, -cy, 0.0f };
    float f = 0, r = 0, u = 0;
    if (Key('W')) f += 1; if (Key('S')) f -= 1;
    if (Key('D')) r += 1; if (Key('A')) r -= 1;
    if (Key(VK_SPACE)) u += 1; if (Key('C')) u -= 1;
    for (int i = 0; i < 3; ++i) out[i] = fwd[i] * f + right[i] * r;
    out[2] += u;
    return f || r || u;
}

static float SpeedMul() { return Key(VK_SHIFT) ? 5.0f : Key(VK_MENU) ? 0.2f : 1.0f; }

static float FrameDt() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = g_lastQpc.QuadPart ? (float)(now.QuadPart - g_lastQpc.QuadPart) / (float)g_qpf.QuadPart : 0.0f;
    g_lastQpc = now;
    return dt > 0.1f ? 0.1f : dt;
}

// ---------------------------------------------------------------- CWeather::Update hook (game thread)
static void BeforeWeather() {
    if (g_setHour >= 0) {
        g_SetGameClock((uint8_t)g_setHour, (uint8_t)g_setMinute, 0);
        Log(1, "time set to %02d:%02d", g_setHour, g_setMinute);
        g_setHour = -1;
    }
    if (!g_baseMsPerMinute && *g_msPerMinute) g_baseMsPerMinute = *g_msPerMinute;
    if (g_baseMsPerMinute) {
        const float speed = g_timeSpeed < 0.05f ? 0.05f : g_timeSpeed;
        *g_msPerMinute = (uint32_t)(g_baseMsPerMinute / speed + 0.5f);
    }
    if (g_freezeTime) { *g_lastTick = *g_timeMs; *g_seconds = 0; }
}

static void AfterWeather() {
    if (g_releaseWeather) { *g_forcedWeather = -1; g_releaseWeather = false; Log(1, "weather released"); }
    switch (g_weatherMode) {
    case WM_FORCE:
        *g_oldWeather = *g_newWeather = *g_forcedWeather = (int16_t)g_weatherA;
        break;
    case WM_BLEND:
        *g_oldWeather = (int16_t)g_weatherA;
        *g_newWeather = (int16_t)g_weatherB;
        *g_forcedWeather = (int16_t)g_weatherB;
        *g_interp = g_blend;
        break;
    default: break;
    }
}

static uintptr_t Hooked_WeatherUpdate(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    __try { BeforeWeather(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const uintptr_t r = o_WeatherUpdate(a, b, c, d);
    __try { AfterWeather(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

// ---------------------------------------------------------------- CCamera::Process hook (game thread)
static void RestoreNoclipEntity() {
    if (!g_noclipEntity) return;
    __try {
        uint32_t* flags = (uint32_t*)(g_noclipEntity + ENT::Flags);
        if (g_noclipHadCollision) *flags |= ENT::UsesCollision;
        memset(g_noclipEntity + ENT::MoveSpeed, 0, 12);
        memset(g_noclipEntity + ENT::TurnSpeed, 0, 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_noclipEntity = nullptr;
}

static void CameraAngles(const float* m, float& yaw, float& pitch) {
    const float* fwd = m + 4; // forward row
    yaw = atan2f(fwd[1], fwd[0]);
    pitch = asinf(fwd[2] < -1 ? -1 : fwd[2] > 1 ? 1 : fwd[2]);
}

static void AfterCamera(float dt) {
    float* cm = *g_camMatrix ? (float*)*g_camMatrix : nullptr;
    uint8_t* ent = g_FindPlayerEntity();
    if (ent) {
        if (const float* em = *(const float**)(ent + ENT::Matrix)) memcpy(g_lastPlayerPos, em + 12, 12);
    }

    // noclip: the player (or the player's vehicle) flies along the camera heading, no collision
    if (g_noclip && ent && !g_freecam) {
        if (ent != g_noclipEntity) {
            RestoreNoclipEntity();
            g_noclipEntity = ent;
            g_noclipHadCollision = (*(uint32_t*)(ent + ENT::Flags) & ENT::UsesCollision) != 0;
        }
        *(uint32_t*)(ent + ENT::Flags) &= ~ENT::UsesCollision;
        memset(ent + ENT::MoveSpeed, 0, 12);
        memset(ent + ENT::TurnSpeed, 0, 12);
        float yaw = 0, pitch = 0, v[3];
        if (cm) CameraAngles(cm, yaw, pitch);
        if (float* em = *(float**)(ent + ENT::Matrix)) {
            if (MoveInput(yaw, pitch, false, v)) {
                const float s = g_cfg.noclipSpeed * SpeedMul() * dt;
                for (int i = 0; i < 3; ++i) em[12 + i] += v[i] * s;
            }
        }
    } else if (g_noclipEntity) {
        RestoreNoclipEntity();
    }

    // freecam: overwrite TheCamera's final matrix
    if (!g_freecam || !cm) { g_freecamInit = false; if (cm) memcpy(g_lastCamPos, cm + 12, 12); return; }
    if (!g_freecamInit) {
        memcpy(g_camPos, cm + 12, 12);
        CameraAngles(cm, g_yaw, g_pitch);
        InterlockedExchange(&g_mouseDx, 0); InterlockedExchange(&g_mouseDy, 0); InterlockedExchange(&g_wheel, 0);
        g_freecamInit = true;
    }
    const float sens = g_cfg.freecamSensitivity * 3.14159265f / 180.0f;
    g_yaw -= InterlockedExchange(&g_mouseDx, 0) * sens;
    g_pitch -= InterlockedExchange(&g_mouseDy, 0) * sens;
    if (g_pitch > 1.55f) g_pitch = 1.55f;
    if (g_pitch < -1.55f) g_pitch = -1.55f;
    const LONG wheel = InterlockedExchange(&g_wheel, 0);
    if (wheel) g_freecamMul = fminf(20.0f, fmaxf(0.05f, g_freecamMul * powf(1.25f, (float)wheel)));
    float v[3];
    if (MoveInput(g_yaw, g_pitch, true, v)) {
        const float s = g_cfg.freecamSpeed * g_freecamMul * SpeedMul() * dt;
        for (int i = 0; i < 3; ++i) g_camPos[i] += v[i] * s;
    }
    const float cy = cosf(g_yaw), sy = sinf(g_yaw), cp = cosf(g_pitch), sp = sinf(g_pitch);
    const float fwd[3] = { cp * cy, cp * sy, sp };
    const float up[3] = { -sp * cy, -sp * sy, cp };
    const float right[3] = { up[1] * fwd[2] - up[2] * fwd[1], up[2] * fwd[0] - up[0] * fwd[2], up[0] * fwd[1] - up[1] * fwd[0] };
    memcpy(cm + 0, right, 12);
    memcpy(cm + 4, fwd, 12);
    memcpy(cm + 8, up, 12);
    memcpy(cm + 12, g_camPos, 12);
    memcpy(g_lastCamPos, g_camPos, 12);

    if (g_teleportToCam && ent) {
        if (float* em = *(float**)(ent + ENT::Matrix)) {
            memcpy(em + 12, g_camPos, 12);
            memset(ent + ENT::MoveSpeed, 0, 12);
            Log(1, "player moved to %.1f %.1f %.1f", g_camPos[0], g_camPos[1], g_camPos[2]);
        }
    }
    g_teleportToCam = false;
}

static uintptr_t Hooked_CameraProcess(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    const uintptr_t r = o_CameraProcess(a, b, c, d);
    const float dt = FrameDt();
    __try {
        AfterCamera(dt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int logged = 0;
        if (logged++ < 5) Log(1, "exception 0x%08lX in camera tools", GetExceptionCode());
    }
    return r;
}

// ---------------------------------------------------------------- panel (render thread)
static void WeatherCombo(const char* label, volatile int& v) {
    int cur = v;
    if (ImGui::BeginCombo(label, kWeatherNames[cur])) {
        for (int i = 0; i < kWeathers; ++i)
            if (ImGui::Selectable(kWeatherNames[i], i == cur)) v = i;
        ImGui::EndCombo();
    }
}

void ToolsPanel() {
    if (!g_toolsOk) { ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "World tools unavailable on this build (see SkyGfxDE.log)"); return; }
    const int region = *g_region;
    ImGui::Text("Time %02u:%02u   weather %s -> %s (%.2f)   forced %d", *g_hours, *g_minutes,
                *g_oldWeather >= 0 && *g_oldWeather < kWeathers ? kWeatherNames[*g_oldWeather] : "?",
                *g_newWeather >= 0 && *g_newWeather < kWeathers ? kWeatherNames[*g_newWeather] : "?", *g_interp, *g_forcedWeather);
    ImGui::Text("Region: %s", region >= 0 && region < 5 ? kRegionNames[region] : "?");

    if (ImGui::CollapsingHeader("Weather", ImGuiTreeNodeFlags_DefaultOpen)) {
        int mode = g_weatherMode;
        ImGui::RadioButton("Game", &mode, WM_GAME); ImGui::SameLine();
        ImGui::RadioButton("Force", &mode, WM_FORCE); ImGui::SameLine();
        ImGui::RadioButton("Blend A -> B", &mode, WM_BLEND);
        if (mode != g_weatherMode) {
            if (mode == WM_GAME) g_releaseWeather = true;
            g_weatherMode = mode;
        }
        WeatherCombo("A", g_weatherA);
        if (g_weatherMode == WM_BLEND) {
            WeatherCombo("B", g_weatherB);
            float b = g_blend;
            if (ImGui::SliderFloat("Blend", &b, 0.0f, 1.0f)) g_blend = b;
        }
        ImGui::TextDisabled("Force holds the weather; Game releases it back to the region cycle.");
    }
    if (ImGui::CollapsingHeader("Time", ImGuiTreeNodeFlags_DefaultOpen)) {
        static int h = 12, m = 0;
        ImGui::SliderInt("Hour", &h, 0, 23);
        ImGui::SliderInt("Minute", &m, 0, 59);
        if (ImGui::Button("Set time")) { g_setMinute = m; g_setHour = h; }
        ImGui::SameLine();
        if (ImGui::Button("Now")) { h = *g_hours; m = *g_minutes; }
        static const int kSlots[8] = { 0, 5, 6, 7, 12, 19, 20, 22 };
        ImGui::TextDisabled("timecyc keyframes:");
        for (int i = 0; i < 8; ++i) {
            ImGui::SameLine();
            char lbl[8];
            snprintf(lbl, sizeof(lbl), "%02d", kSlots[i]);
            if (ImGui::SmallButton(lbl)) { h = kSlots[i]; m = 0; g_setMinute = 0; g_setHour = kSlots[i]; }
        }
        bool freeze = g_freezeTime;
        if (ImGui::Checkbox("Freeze time", &freeze)) g_freezeTime = freeze;
        float speed = g_timeSpeed;
        if (ImGui::SliderFloat("Time speed", &speed, 0.1f, 60.0f, "%.1fx", ImGuiSliderFlags_Logarithmic)) g_timeSpeed = speed;
        ImGui::SameLine();
        if (ImGui::SmallButton("1x")) g_timeSpeed = 1.0f;
    }
    if (ImGui::CollapsingHeader("Camera / player", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool fc = g_freecam, nc = g_noclip;
        if (ImGui::Checkbox("Freecam", &fc)) { if (fc != g_freecam) ToolsToggleFreecam(); }
        ImGui::SameLine(); ImGui::TextDisabled("(%s)", g_cfg.keyFreecam.text);
        ImGui::SameLine(0, 30);
        if (ImGui::Checkbox("Noclip", &nc)) { if (nc != g_noclip) ToolsToggleNoclip(); }
        ImGui::SameLine(); ImGui::TextDisabled("(%s)", g_cfg.keyNoclip.text);
        ImGui::SliderFloat("Freecam speed (m/s)", &g_cfg.freecamSpeed, 1.0f, 200.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Mouse sensitivity", &g_cfg.freecamSensitivity, 0.01f, 1.0f);
        ImGui::SliderFloat("Noclip speed (m/s)", &g_cfg.noclipSpeed, 1.0f, 200.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::Text("camera %.1f %.1f %.1f (height %.0f m)", g_lastCamPos[0], g_lastCamPos[1], g_lastCamPos[2], g_lastCamPos[2]);
        ImGui::Text("player %.1f %.1f %.1f", g_lastPlayerPos[0], g_lastPlayerPos[1], g_lastPlayerPos[2]);
        ImGui::BeginDisabled(!g_freecam);
        if (ImGui::Button("Move player to camera")) g_teleportToCam = true;
        ImGui::EndDisabled();
        ImGui::TextDisabled("Move: WASD, Space up, C down, mouse look, Shift x5, Alt x0.2, wheel = freecam speed.\n"
                            "The world streams around the player: use 'Move player to camera' before flying far.");
    }
}

// ---------------------------------------------------------------- install
struct Anchor { const char* name; uint8_t* base; size_t off; const uint8_t* op; size_t opLen, len; void* out; };
static struct { const char* name; const void* addr; } g_resolved[24];
static int g_resolvedCount = 0;
static void Resolved(const char* name, const void* addr) {
    if (g_resolvedCount < (int)(sizeof(g_resolved) / sizeof(g_resolved[0]))) g_resolved[g_resolvedCount++] = { name, addr };
}

// "name rva" per line for everything InstallTools resolved (the offline check writes it to its artifact).
void ToolsWriteAnchors(FILE* f) {
    for (int i = 0; i < g_resolvedCount; ++i)
        fprintf(f, "%-32s rva=0x%llX\n", g_resolved[i].name, (unsigned long long)((const uint8_t*)g_resolved[i].addr - g_moduleBase));
}

bool InstallTools() {
    QueryPerformanceFrequency(&g_qpf);
    uint8_t* wu = FindUnique("48 8B C4 48 81 EC D8 00 00 00 F6 05 ?? ?? ?? ?? 0F 44 0F 29 64 24 50 48 89 58 10 48 89 70 18 48 89 78 F8 4C 89 78 E8");
    uint8_t* sc = FindUnique("8B 05 ?? ?? ?? ?? 44 0F B6 15 ?? ?? ?? ?? 44 0F B6 CA 44 88 0D ?? ?? ?? ?? 89 05 ?? ?? ?? ?? 88 0D ?? ?? ?? ?? 45 84 C0");
    uint8_t* fl = FindUnique("0F BF 0D ?? ?? ?? ?? 85 C9 74 34 83 E9 01 74 27 83 E9 01 74 1A 83 E9 01 74 0D 83 F9 01 75 20");
    uint8_t* fpe = FindUnique("0F B6 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 69 C0 C0 01 00 00 48 8B 0C 08 F7 81 34 06 00 00 00 01 00 00 74 0C");
    uint8_t* cp = FindUnique("48 8B C4 48 89 58 18 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 D8 FE FF FF 48 81 EC F0 01 00 00 0F 29 70 B8 0F 29 78 A8 44 0F 29 40 98 44 0F 29 48 88");
    uint8_t* ct = FindUnique("8B 0D ?? ?? ?? ?? 2B C1 44 8B 05 ?? ?? ?? ?? F3 0F 10 35 ?? ?? ?? ?? 44 0F B6 0D");
    uint8_t* tu = FindUnique("4C 8B DC 55 56 49 8D 6B A1 48 81 EC C8 00 00 00 45 0F 29 4B A8 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 D7");
    const char* names[] = { "CWeather::Update", "CClock::SetGameClock", "CWeather::FindWeatherTypesList", "FindPlayerEntity",
                            "CCamera::Process", "CClock::Update tick", "CTimeCycle::Update" };
    uint8_t* found[] = { wu, sc, fl, fpe, cp, ct, tu };
    bool ok = true;
    for (int i = 0; i < 7; ++i) {
        Log(found[i] ? 2 : 1, "tools sig %-32s %p%s", names[i], found[i], found[i] ? "" : "  <-- NOT FOUND");
        ok &= found[i] != nullptr;
        if (found[i]) Resolved(names[i], found[i]);
    }
    if (!ok) return false;

    static const uint8_t movzxEax[] = { 0x0F, 0xB6, 0x05 }, movzxEcx[] = { 0x0F, 0xB6, 0x0D }, movzxwEdx[] = { 0x0F, 0xB7, 0x15 },
                         movzxwEax[] = { 0x0F, 0xB7, 0x05 }, movWordDx[] = { 0x66, 0x89, 0x15 }, movssXmm7[] = { 0xF3, 0x0F, 0x11, 0x3D },
                         movEdi[] = { 0x8B, 0x3D }, movzxEsi[] = { 0x0F, 0xB6, 0x35 }, movMemEax[] = { 0x89, 0x05 },
                         movsxEcx[] = { 0x0F, 0xBF, 0x0D }, movR8d[] = { 0x44, 0x8B, 0x05 }, movRax[] = { 0x48, 0x8B, 0x05 };
    const Anchor anchors[] = {
        { "Minutes", wu, 0x5E, movzxEax, 3, 7, &g_minutes },
        { "Seconds", wu, 0x65, movzxEcx, 3, 7, &g_seconds },
        { "NewWeatherType", wu, 0xAF, movzxwEdx, 3, 7, &g_newWeather },
        { "ForcedWeatherType", wu, 0xB6, movzxwEax, 3, 7, &g_forcedWeather },
        { "OldWeatherType", wu, 0xC1, movWordDx, 3, 7, &g_oldWeather },
        { "InterpolationValue", wu, 0x169, movssXmm7, 4, 8, &g_interp },
        { "TimeInMilliseconds", wu, 0x1DF, movEdi, 2, 6, &g_timeMs },
        { "Hours", wu, 0x5D3, movzxEsi, 3, 7, &g_hours },
        { "LastClockTick", sc, 0x19, movMemEax, 2, 6, &g_lastTick },
        { "WeatherRegion", fl, 0x0, movsxEcx, 3, 7, &g_region },
        { "MsPerGameMinute", ct, 0x8, movR8d, 3, 7, &g_msPerMinute },
        { "TheCamera.m_matrix", tu, 0x23, movRax, 3, 7, &g_camMatrix },
    };
    for (const Anchor& a : anchors) {
        uint8_t* p = RipAt(a.base + a.off, a.op, a.opLen, a.len);
        *(void**)a.out = p;
        Log(p ? 2 : 1, "tools global %-20s %p%s", a.name, p, p ? "" : "  <-- unexpected code");
        ok &= p != nullptr;
        if (p) Resolved(a.name, p);
    }
    if (!ok) return false;
    g_SetGameClock = (SetGameClock_Fn)sc;
    g_FindPlayerEntity = (FindPlayerEntity_Fn)fpe;
    ok = MH_CreateHook(wu, (void*)&Hooked_WeatherUpdate, (void**)&o_WeatherUpdate) == MH_OK &&
         MH_CreateHook(cp, (void*)&Hooked_CameraProcess, (void**)&o_CameraProcess) == MH_OK;
    g_toolsOk = ok;
    Log(1, "debug tools %s", ok ? "ready" : "hook creation FAILED");
    return ok;
}
