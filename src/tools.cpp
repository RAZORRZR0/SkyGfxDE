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
uint8_t*  g_hours = nullptr;               // CClock::ms_nGameClockHours
uint8_t*  g_minutes = nullptr;             // CClock::ms_nGameClockMinutes
static uint16_t* g_seconds = nullptr;      // CClock::ms_nGameClockSeconds
static uint32_t* g_lastTick = nullptr;     // CClock::ms_nLastClockTick
static uint32_t* g_msPerMinute = nullptr;  // CClock::ms_nMillisecondsPerGameMinute
uint32_t* g_timeMs = nullptr;              // CTimer::m_snTimeInMilliseconds
static uint8_t** g_camMatrix = nullptr;    // TheCamera.m_matrix (CMatrix*)
static uint8_t** g_players = nullptr;      // CWorld::Players[].m_pPed, stride 0x1C0
static uint8_t*  g_playerInFocus = nullptr; // CWorld::PlayerInFocus
volatile int g_speedFxRow = -1;           // row of kSpeedFx for this frame, -1 = off (read by the overlay)
static float*    g_rain = nullptr;         // CWeather::Rain
static float*    g_underWater = nullptr;   // CWeather::UnderWaterness
int32_t*  g_currArea = nullptr;            // CGame::currArea (0 = outside)
static uint32_t* g_zoneFlagsA = nullptr;   // CCullZones current flags (player / camera, as tested by CTimeCycle;
static uint32_t* g_zoneFlagsB = nullptr;   //  both are only ever ORed): bit 3 = no rain
FxState g_fx{};
volatile int g_splash = -1;
volatile LONG g_dropFill = 0;               // float bits: WaterDrops::FillScreenMoving amount queued for the render thread
static int32_t*  g_exitEnterState = nullptr; // CEntryExitManager::ms_exitEnterState (0 = no interior transition)

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
static volatile bool g_timelapse = false;       // runs the clock at g_timelapseSpeed (freeze off) until unticked
static float g_timelapseSpeed = 240.0f;         // 240x: a game day in 6 real seconds
static uint32_t g_baseMsPerMinute = 0;

// ---------------------------------------------------------------- freecam / noclip state (game thread)
static volatile bool g_freecam = false, g_noclip = false;
static volatile LONG g_wheel = 0;
static float g_camPos[3], g_freecamMul = 1.0f;
static bool g_freecamInit = false;
static uint8_t* g_noclipEntity = nullptr;
static bool g_noclipHadCollision = false;
static volatile bool g_teleportToCam = false;
static float g_lastPlayerPos[3], g_lastCamPos[3];
static LARGE_INTEGER g_qpf, g_lastQpc;

void ToolsToggleFreecam() { g_freecam = !g_freecam; g_freecamInit = false; Log(1, "freecam %s", g_freecam ? "on" : "off"); }
void ToolsToggleNoclip() { g_noclip = !g_noclip; Log(1, "noclip %s", g_noclip ? "on" : "off"); }
bool ToolsWantCapture() { return g_freecam || g_noclip; }
bool WeatherBlend(int* oldW, int* newW, float* t) {
    if (!g_oldWeather || !g_newWeather || !g_interp) return false;
    *oldW = *g_oldWeather; *newW = *g_newWeather; *t = *g_interp;
    return *oldW >= 0 && *oldW < kWeathers && *newW >= 0 && *newW < kWeathers;
}
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
        const float want = g_timelapse ? g_timelapseSpeed : g_timeSpeed;
        const float speed = want < 0.05f ? 0.05f : want;
        *g_msPerMinute = (uint32_t)(g_baseMsPerMinute / speed + 0.5f);
    }
    if (g_freezeTime && !g_timelapse) { *g_lastTick = *g_timeMs; *g_seconds = 0; }
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
    // The mouse keeps reaching the game, so TheCamera's own matrix (computed from the mouse this frame, before this
    // overwrite) gives the look direction; only the position is the freecam's.
    float yaw, pitch;
    CameraAngles(cm, yaw, pitch);
    if (!g_freecamInit) {
        memcpy(g_camPos, cm + 12, 12);
        InterlockedExchange(&g_wheel, 0);
        g_freecamInit = true;
    }
    const LONG wheel = InterlockedExchange(&g_wheel, 0);
    if (wheel) g_freecamMul = fminf(20.0f, fmaxf(0.05f, g_freecamMul * powf(1.25f, (float)wheel)));
    float v[3];
    if (MoveInput(yaw, pitch, true, v)) {
        const float s = g_cfg.freecamSpeed * g_freecamMul * SpeedMul() * dt;
        for (int i = 0; i < 3; ++i) g_camPos[i] += v[i] * s;
    }
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
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

// SpeedFX, CPostEffects::Render (gta-reversed PostEffects.cpp) with DE offsets:
// - player vehicle, not heli/plane/boat/train: IsSubHeli/IsSubPlane/... test m_nVehicleSubType (+0x894: 3/4/5/6).
//   +0x890 is the base type (m_nVehicleType), which is 0 (automobile) for planes and helis too: DE compares it only
//   with 0/5/6/9, the subtype with 0..10;
// - NOS: automobile (type 0) with handlingFlags.bNosInst (+0x5C0 & 0x80000) and m_fTireTemperature (+0xC0C) < 0
//   (nitro burning, CAutomobile::NitrousControl 0x14138F290): dir = moveSpeed . forward, if > 0.2 the input is
//   clamp(2 * dir * (m_GasPedal (+0x710) + 1), 0, 1), drawn even in cutscenes as in the original;
// - else, if !CCutsceneMgr::ms_running, the input is |moveSpeed| (units per frame).
// The input picks the last kSpeedFx row it reaches. SpeedFX (0x7030A0) then reads TheCamera.m_aCams[m_nActiveCam].
// m_nDirectionWasLooking: DE TheCamera + 0x5B (active cam), cams of 0x1B8 bytes, field at +0x1C4 (1 behind, 2 side).
uint8_t* g_cutsceneRunning = nullptr; // CCutsceneMgr::ms_running (InstallTools)
static int SpeedFxInputRow(float input) {
    for (int i = 6; i >= 0; --i)
        if (input >= kSpeedFx[i].speed) return i;
    return -1;
}

static void SpeedFxRowUpdate() {
    int row = -1;
    const uint8_t* ped = g_players[0x1C0 / 8 * *g_playerInFocus];
    const uint8_t* veh = ped && (*(const uint32_t*)(ped + 0x634) & 0x100) ? *(uint8_t* const*)(ped + 0x7C8) : nullptr;
    const uint32_t type = veh ? *(const uint32_t*)(veh + 0x894) : 0; // m_nVehicleSubType
    const bool cutscene = g_cutsceneRunning && *g_cutsceneRunning;
    if (g_cfg.speedFxTestMode && g_cfg.speedFx && g_active) {
        row = SpeedFxInputRow(1.0f); // m_bSpeedFXTestMode: SetSpeedFXManualSpeedCurrentFrame(1.0f), no vehicle needed
    } else if (veh && g_cfg.speedFx && g_active && (type < 3 || type > 6)) {
        const float* v = (const float*)(veh + ENT::MoveSpeed);
        bool nos = false;
        const float* m = *(const float* const*)(veh + ENT::Matrix);
        if (type == 0 && (*(const uint32_t*)(veh + 0x5C0) & 0x80000) && *(const float*)(veh + 0xC0C) < 0.0f && m) {
            const float dir = v[0] * m[4] + v[1] * m[5] + v[2] * m[6]; // GetMoveSpeed().Dot(GetForward())
            if (dir > 0.2f) {
                row = SpeedFxInputRow(fminf(fmaxf(2.0f * dir * (*(const float*)(veh + 0x710) + 1.0f), 0.0f), 1.0f));
                nos = true;
            }
        }
        if (!nos && !cutscene)
            row = SpeedFxInputRow(sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
    }
    const uint8_t* cam = (const uint8_t*)g_camMatrix - 0x18; // TheCamera
    const uint8_t active = cam[0x5B];
    const uint32_t look = *(const uint32_t*)(cam + 0x1C4 + 0x1B8 * active);
    const uint16_t mode = *(const uint16_t*)(cam + 0x1B0 + 0x1B8 * active); // m_aCams[m_nActiveCam].m_nMode
    static bool logged = false;
    if (row >= 0 && !logged) { logged = true; Log(1, "speedfx: first active (row %d, looking %d)", row, look); }
    g_speedFxRow = row < 0 ? -1 : row | (look == 1 || look == 2 ? look : 0) << 8;

    // Post-effect inputs (postfx.cpp). Rain grain: CPostEffects::Render moves s_CurrentStrength one step per frame
    // towards 128 * Rain and draws Grain(strength / 4) outside no-rain zones, above water, outside and below 900 m.
    if (const float* m = (const float*)*g_camMatrix) memcpy(g_fx.cam, m, sizeof(g_fx.cam));
    const float rain = *g_rain, under = *g_underWater;
    const bool noRainZone = ((*g_zoneFlagsA | *g_zoneFlagsB) & 8) != 0;
    static int strength = 0;
    int grain = 0;
    if (rain > 0.0f || strength != 0) {
        if ((float)strength < 128.0f * rain) ++strength;
        else if ((float)strength > 128.0f * rain) --strength;
        if (strength < 0) strength = 0;
        if (!noRainZone && under <= 0.0f && *g_currArea == 0 && g_fx.cam[14] <= 900.0f) grain = strength / 4;
    }
    g_fx.rain = rain;
    g_fx.grain = grain;
    // Water drops (skygfx WaterDrops::NoDrops / NoRain / CalculateMovement / Render); modes 1 and 54 are top-down,
    // 16 is MODE_1STPERSON; looking behind/left/right from a 1st-person car (direction != 3, forward) stops them.
    // NoDrops also covers the entry/exit fade (ms_exitEnterState != 0): the drops are cleared.
    g_fx.noDrops = under > 0.339731634f || *g_exitEnterState != 0;
    g_fx.noRain = noRainZone || *g_currArea != 0 || g_fx.noDrops;
    g_fx.firstPerson = mode == 16;
    g_fx.dropsEnabled = mode != 1 && mode != 54 && !(mode == 16 && veh && look != 3);
    g_fx.hideDrops = cutscene || (mode == 16 && !veh);
}

// DE's water splash FX (Fx_c-style wrappers of "water_splash_big", "water_splash", "water_splsh_sml"; pos in metres):
// skygfx hooks their CreateFxSystem calls with WaterDrops::RegisterSplash(point, 10.0f, 1).
typedef void* (*SplashBig_Fn)(void*, const float*);
typedef void* (*Splash_Fn)(void*, const float*, float);
typedef void* (*SplashSmall_Fn)(void*, const float*, char);
static SplashBig_Fn o_SplashBig = nullptr;
static Splash_Fn o_Splash = nullptr;
static SplashSmall_Fn o_SplashSmall = nullptr;
static float CamDist(const float* p) {
    const float* c = g_fx.cam + 12;
    const float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
static void RegisterSplash(const float* p, int duration) { // skygfx: within 10 m of the camera
    if (g_cfg.waterDrops && p && CamDist(p) <= 10.0f) g_splash = duration;
}
static void* Hooked_SplashBig(void* a, const float* p) { RegisterSplash(p, 1); return o_SplashBig(a, p); }
static void* Hooked_Splash(void* a, const float* p, float s) { RegisterSplash(p, 1); return o_Splash(a, p, s); }
static void* Hooked_SplashSmall(void* a, const float* p, char s) { RegisterSplash(p, 1); return o_SplashSmall(a, p, s); }

// Hydrants and fountains: DE's particle-audio callback (FX_water_hydrant / FX_water_fountain / FX_water_fnt_tme) is
// the only caller of this audio-event function (event 137, pos in metres), as skygfx's hooked CAEFireAudioEntity
// calls: RegisterSplash(point, 20.0f, 20) when within 10 m.
typedef void* (*WaterAudio_Fn)(void*, int, const float*, uintptr_t);
static WaterAudio_Fn o_WaterAudio = nullptr;
static void* Hooked_WaterAudio(void* a, int ev, const float* p, uintptr_t r9) {
    RegisterSplash(p, 20);
    return o_WaterAudio(a, ev, p, r9);
}

// Boat splash, wake and water splash particles (FxSystem_c::AddParticle, this = Fx_c's prt_* system, pos in
// metres): skygfx fills the screen with 1 / (dist / 2) when within 40 / 10 / 30 m. Blood is not ported.
typedef void* (*AddParticle_Fn)(void*, const float*, void*, float, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static AddParticle_Fn o_AddParticle = nullptr;
static void** g_prtBoatSplash = nullptr;  // Fx_c::prt_boatsplash
static void** g_prtWake = nullptr;        // Fx_c::prt_wake
static void** g_prtWaterSplash = nullptr; // Fx_c::prt_watersplash
static void* Hooked_AddParticle(void* sys, const float* p, void* v, float f, uintptr_t a, uintptr_t b, uintptr_t c,
                                uintptr_t d, uintptr_t e, uintptr_t g) {
    const float range = sys == *g_prtBoatSplash ? 40.0f : sys == *g_prtWake ? 10.0f : sys == *g_prtWaterSplash ? 30.0f : 0.0f;
    if (range > 0.0f && g_cfg.waterDrops && p) {
        const float dist = fmaxf(CamDist(p), 0.1f);
        if (dist <= range) {
            LONG o, n;
            do { // g_dropFill += 2 / dist (float bits)
                o = g_dropFill;
                float amount; memcpy(&amount, &o, 4);
                amount += 2.0f / dist;
                memcpy(&n, &amount, 4);
            } while (InterlockedCompareExchange(&g_dropFill, n, o) != o);
        }
    }
    return o_AddParticle(sys, p, v, f, a, b, c, d, e, g);
}

// DE's CCamera::Process sets m_fLODDistMultiplier = 70 / FOV (the original: 70 / FOV * CRenderer::ms_lodDistScale, the
// PC draw distance slider, 1.2 default, 0.925-1.8). It scales every model's draw distance for rendering and
// streaming, so it's multiplied by [World] LodDistance here, before the frame's render list is built. Cutscenes keep
// DE's fixed value, as in the original. DE caps visibility at bound radius + 700 m regardless.
// ponytail: m_fGenerationDistMultiplier (+4, car/ped spawning) is left alone; scale it too if traffic pops in.
static float* g_lodMult = nullptr; // TheCamera.m_fLODDistMultiplier
static uintptr_t Hooked_CameraProcess(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    const uintptr_t r = o_CameraProcess(a, b, c, d);
    const float dt = FrameDt();
    __try {
        if (g_lodMult && !(g_cutsceneRunning && *g_cutsceneRunning)) *g_lodMult *= g_cfg.lodDistance;
        AfterCamera(dt);
        SpeedFxRowUpdate();
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
            if (SliderBox("Blend", &b, 0.0f, 1.0f, 0.0f, 1.0f)) g_blend = b;
        }
        ImGui::TextDisabled("Force holds the weather; Game releases it back to the region cycle.");
    }
    if (ImGui::CollapsingHeader("Time", ImGuiTreeNodeFlags_DefaultOpen)) {
        static int h = 12, m = 0;
        SliderBoxInt("Hour", &h, 0, 23, 0, 23);
        SliderBoxInt("Minute", &m, 0, 59, 0, 59);
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
        if (SliderBox("Time speed", &speed, 0.1f, 60.0f, 0.05f, 1000.0f, "%.1fx", ImGuiSliderFlags_Logarithmic)) g_timeSpeed = speed;
        ImGui::SameLine();
        if (ImGui::SmallButton("1x")) g_timeSpeed = 1.0f;
        bool lapse = g_timelapse;
        if (ImGui::Checkbox("Timelapse", &lapse)) g_timelapse = lapse;
        ImGui::SameLine();
        ImGui::TextDisabled("(a game day in %.0f s)", 1440.0f * (g_baseMsPerMinute ? g_baseMsPerMinute : 1000) / 1000.0f / g_timelapseSpeed);
        SliderBox("Timelapse speed", &g_timelapseSpeed, 30.0f, 1000.0f, 1.0f, 1000.0f, "%.0fx", ImGuiSliderFlags_Logarithmic);
    }
    if (ImGui::CollapsingHeader("Draw distance", ImGuiTreeNodeFlags_DefaultOpen)) {
        SliderBox("LOD distance", &g_cfg.lodDistance, 0.5f, 4.0f, 0.1f, 10.0f, "x%.2f");
        ImGui::TextDisabled(g_lodMult ? "multiplier now %.2f (DE: 70/FOV = 1.0; PC slider max 1.8)" : "not available on this build",
                            g_lodMult ? *g_lodMult : 0.0f);
    }
    if (ImGui::CollapsingHeader("Camera / player", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool fc = g_freecam, nc = g_noclip;
        if (ImGui::Checkbox("Freecam", &fc)) { if (fc != g_freecam) ToolsToggleFreecam(); }
        ImGui::SameLine(); ImGui::TextDisabled("(%s)", g_cfg.keyFreecam.text);
        ImGui::SameLine(0, 30);
        if (ImGui::Checkbox("Noclip", &nc)) { if (nc != g_noclip) ToolsToggleNoclip(); }
        ImGui::SameLine(); ImGui::TextDisabled("(%s)", g_cfg.keyNoclip.text);
        SliderBox("Freecam speed (m/s)", &g_cfg.freecamSpeed, 1.0f, 200.0f, 0.1f, 1000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        SliderBox("Noclip speed (m/s)", &g_cfg.noclipSpeed, 1.0f, 200.0f, 0.1f, 1000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
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
static struct { const char* name; const void* addr; } g_resolved[64];
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
    // CTimeCycle::CalcColoursForPoint fog reduction: !bScriptsForceRain && (PlayerNoRain || CamNoRain || ms_running)
    uint8_t* fr = FindUnique("80 3D ?? ?? ?? ?? 00 75 ?? F6 05 ?? ?? ?? ?? 08 75 ?? F6 05 ?? ?? ?? ?? 08 75 ?? 80 3D ?? ?? ?? ?? 00");
    // CEntryExitManager::Update: ms_exitEnterState = ms_exitEnterState != 3 ? 0 : 4
    uint8_t* ee = FindUnique("83 3D ?? ?? ?? ?? 03 B8 04 00 00 00 44 0F 44 F8 44 89 3D");
    const char* names[] = { "CWeather::Update", "CClock::SetGameClock", "CWeather::FindWeatherTypesList", "FindPlayerEntity",
                            "CCamera::Process", "CClock::Update tick", "CTimeCycle::Update", "TimeCycle fog reduction",
                            "EntryExit state reset" };
    uint8_t* found[] = { wu, sc, fl, fpe, cp, ct, tu, fr, ee };
    bool ok = true;
    for (int i = 0; i < 9; ++i) {
        Log(found[i] ? 2 : 1, "tools sig %-32s %p%s", names[i], found[i], found[i] ? "" : "  <-- NOT FOUND");
        ok &= found[i] != nullptr;
        if (found[i]) Resolved(names[i], found[i]);
    }
    if (!ok) return false;

    static const uint8_t movzxEax[] = { 0x0F, 0xB6, 0x05 }, movzxEcx[] = { 0x0F, 0xB6, 0x0D }, movzxwEdx[] = { 0x0F, 0xB7, 0x15 },
                         movzxwEax[] = { 0x0F, 0xB7, 0x05 }, movWordDx[] = { 0x66, 0x89, 0x15 }, movssXmm7[] = { 0xF3, 0x0F, 0x11, 0x3D },
                         movEdi[] = { 0x8B, 0x3D }, movzxEsi[] = { 0x0F, 0xB6, 0x35 }, movMemEax[] = { 0x89, 0x05 },
                         movsxEcx[] = { 0x0F, 0xBF, 0x0D }, movR8d[] = { 0x44, 0x8B, 0x05 }, movRax[] = { 0x48, 0x8B, 0x05 },
                         leaRcx[] = { 0x48, 0x8D, 0x0D }, cmpByte[] = { 0x80, 0x3D }, movssStXmm9[] = { 0xF3, 0x44, 0x0F, 0x11, 0x0D },
                         movssXmm0[] = { 0xF3, 0x0F, 0x10, 0x05 }, cmpDword[] = { 0x83, 0x3D }, testByte[] = { 0xF6, 0x05 };
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
        { "CWorld::PlayerInFocus", fpe, 0x0, movzxEax, 3, 7, &g_playerInFocus },
        { "CWorld::Players", fpe, 0x7, leaRcx, 3, 7, &g_players },
        { "CCutsceneMgr::ms_running", fr, 0x1B, cmpByte, 2, 7, &g_cutsceneRunning },
        { "CWeather::Rain", wu, 0x451, movssStXmm9, 5, 9, &g_rain },           // rain from RAINY_SF / RAINY_COUNTRYSIDE
        { "CWeather::UnderWaterness", wu, 0x1AE, movssXmm0, 4, 8, &g_underWater }, // lightning: ... || UnderWaterness > 0
        { "CGame::currArea", wu, 0x1BF, cmpDword, 2, 7, &g_currArea },          //  ... || currArea != 0
        { "CCullZones flags (player)", fr, 0x9, testByte, 2, 7, &g_zoneFlagsA },
        { "CCullZones flags (camera)", fr, 0x12, testByte, 2, 7, &g_zoneFlagsB },
        { "CEntryExitManager::ms_exitEnterState", ee, 0x0, cmpDword, 2, 7, &g_exitEnterState },
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
    // CCamera::Process: movss xmm0, 70.0; divss xmm0, FOV; movss [rbx+104h], xmm0; movss [rbx+108h], xmm0
    // (rbx = TheCamera); the store's disp32 gives the field offset.
    uint8_t* ld = FindUnique("F3 0F 10 05 ?? ?? ?? ?? F3 0F 5E 05 ?? ?? ?? ?? F3 0F 11 83 ?? ?? ?? ?? F3 0F 11 83");
    if (ld && *(const float*)(ld + 8 + *(const int32_t*)(ld + 4)) == 70.0f) {
        g_lodMult = (float*)((uint8_t*)g_camMatrix - 0x18 + *(const int32_t*)(ld + 20));
        Resolved("CCamera LOD multiplier store", ld);
        Resolved("TheCamera.m_fLODDistMultiplier", g_lodMult);
    }
    Log(1, "lod distance: %s", g_lodMult ? "ready" : "NOT found (DE's own LOD distance kept)");
    // Water splash FX for the lens drops: optional, rain drops work without them.
    uint8_t* sb = FindUnique("48 8B C4 57 48 83 EC 70 F3 0F 10 19 48 8B FA F3 0F 59 1D ?? ?? ?? ?? F3 0F 10 51 04 F3 0F 59 15");
    uint8_t* sp = FindUnique("4C 8B DC 49 89 4B 08 53 48 81 EC 80 00 00 00 48 8B 05 ?? ?? ?? ?? 49 89 7B 10 48 8B FA 45 0F 29 4B B8 44 0F 28 CA");
    uint8_t* ss = FindUnique("48 8B C4 48 89 48 08 57 48 83 EC 70 F3 0F 10 1D ?? ?? ?? ?? 48 8B FA F3 0F 59 1D ?? ?? ?? ?? F3 0F 10 15");
    if (sb) Resolved("water_splash_big FX", sb);
    if (sp) Resolved("water_splash FX", sp);
    if (ss) Resolved("water_splsh_sml FX", ss);
    const bool splash = sb && sp && ss && MH_CreateHook(sb, (void*)&Hooked_SplashBig, (void**)&o_SplashBig) == MH_OK &&
                        MH_CreateHook(sp, (void*)&Hooked_Splash, (void**)&o_Splash) == MH_OK &&
                        MH_CreateHook(ss, (void*)&Hooked_SplashSmall, (void**)&o_SplashSmall) == MH_OK;
    Log(1, "water drops: splash FX hooks %s", splash ? "ready" : "NOT found (rain drops only)");
    uint8_t* wa = FindUnique("48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC E0 00 00 00 F3 0F 10 05 ?? ?? ?? ?? 45 33 F6 0F 29 70 C8 49 8B D8");
    if (wa) Resolved("hydrant/fountain audio event", wa);
    const bool hydrant = wa && MH_CreateHook(wa, (void*)&Hooked_WaterAudio, (void**)&o_WaterAudio) == MH_OK;
    Log(1, "water drops: hydrant/fountain hook %s", hydrant ? "ready" : "NOT found");
    uint8_t* ap = FindUnique("48 8B C4 4C 89 40 18 55 53 57 41 54 41 55 48 8D 68 C9 48 81 EC F0 00 00 00 F3 0F 10 02 49 8B F8");
    uint8_t* fr2 = FindUnique("40 53 48 83 EC 30 48 89 74 24 48 48 8B 35 ?? ?? ?? ?? 4C 89 64 24 58 45 33 E4 48 85 F6 0F 84");
    static const uint8_t movRdx[] = { 0x48, 0x8B, 0x15 };
    if (fr2) { // Fx_c system release: mov rdx, cs:prt_* in declaration order
        g_prtBoatSplash = (void**)RipAt(fr2 + 0x18C, movRdx, 3, 7);
        g_prtWake = (void**)RipAt(fr2 + 0x24C, movRdx, 3, 7);
        g_prtWaterSplash = (void**)RipAt(fr2 + 0x258, movRdx, 3, 7);
    }
    if (ap) Resolved("FxSystem_c::AddParticle", ap);
    if (g_prtBoatSplash) Resolved("Fx_c::prt_boatsplash", g_prtBoatSplash);
    if (g_prtWake) Resolved("Fx_c::prt_wake", g_prtWake);
    if (g_prtWaterSplash) Resolved("Fx_c::prt_watersplash", g_prtWaterSplash);
    const bool particles = ap && g_prtBoatSplash && g_prtWake && g_prtWaterSplash &&
                           MH_CreateHook(ap, (void*)&Hooked_AddParticle, (void**)&o_AddParticle) == MH_OK;
    Log(1, "water drops: boat splash / wake particle hook %s", particles ? "ready" : "NOT found");
    g_toolsOk = ok;
    Log(1, "debug tools %s", ok ? "ready" : "hook creation FAILED");
    return ok;
}
