// Distant lamp coronas, after Project2DFX SALodLights (LamppostInfo.ixx LoadDatFile, LODLights.ixx RegisterLODLights).
// SALodLights.dat lists corona offsets per model. Every IPL instance of those models that CFileLoader::
// LoadObjectInstance builds while the level loads becomes a lamp (instance position + offset turned by the heading).
// Each frame, lamps past the range of the model's own corona and inside the far clip get a corona in DE's
// UGTACoronaComponent: AGTAWorldSettings::GetCoronaComponent(bDynamic), the one AGTALightBase::SetFarCorona uses,
// fed through ProcessEvent AddCorona / UpdateCorona / RemoveCorona. A corona there is 4 vertices of the component's
// procedural mesh (normal = corner offset x Scale, intensity), drawn by UE with scene depth, so terrain and
// buildings hide it. The component rebuilds its mesh once per tick when anything changed. Game thread only.
#include "skygfx.h"
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <unordered_map>
#include <vector>
#include "../minhook/MinHook.h"
#include "../imgui/imgui.h"

namespace MDL { constexpr size_t Key = 0x08, DrawDistance = 0x38; }     // CBaseModelInfo (x64)
namespace INST { constexpr size_t Model = 0x1C, Area = 0x20; }          // CFileObjectInstance: pos, quaternion xyzw at 0xC
namespace WS { constexpr size_t StaticCoronas = 0x3B0; }                // AGTAWorldSettings::StaticCoronaComponent
constexpr uint32_t RF_ClassDefaultObject = 0x10, RF_ArchetypeObject = 0x20;

struct Light { float off[3]; uint8_t r, g, b, a; float size, drawDist; int blink, noDist; };
struct Lamp { float pos[3]; uint8_t r, g, b, a; float size, drawDist; int blink, noDist; int32_t corona; float scale, intensity; };

static std::unordered_map<uint32_t, std::vector<Light>> g_dat; // by model key, until the level has loaded
static std::vector<Lamp> g_lamps;
static volatile bool g_catching = false;  // LoadObjectInstance adds lamps until the first game frame
static uint8_t** g_modelInfos = nullptr;  // CModelInfo::ms_modelInfoPtrs
static bool g_coronasOk = false;
static uint8_t* g_settings = nullptr;     // AGTAWorldSettings of the running world
static int32_t g_settingsIndex = -1;
static uint8_t* g_component = nullptr;    // its dynamic UGTACoronaComponent
static void *g_fnAdd = nullptr, *g_fnUpdate = nullptr, *g_fnRemove = nullptr, *g_fnGetComponent = nullptr;
static volatile int g_shown = 0;          // coronas in the component (panel)

// CKeyGen::GetUppercaseKey (0x14114FA90): CRC-32 table, seed ~0, no final xor, upper-cased; = CBaseModelInfo key.
static uint32_t UppercaseKey(const char* s) {
    uint32_t k = 0xFFFFFFFF;
    for (; *s; ++s) {
        uint32_t c = (k ^ (uint32_t)toupper((uint8_t)*s)) & 0xFF;
        for (int i = 0; i < 8; ++i) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        k = c ^ (k >> 8);
    }
    return k;
}

// SALodLights.dat: "%model" sections, then "R G B A offX offY offZ CustomSize DrawDistance Blink NoDistance
// Searchlight" (legacy lines lack DrawDistance). "%additional_coronas" offsets are world positions.
static int LoadDat() {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sSALodLights.dat", g_dir);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    std::vector<Light>* section = nullptr;
    bool world = false;
    int n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        const char* p = line + strspn(line, " \t");
        if (*p == '%') {
            char name[64] = "";
            sscanf(p + 1, "%63s", name);
            world = strcmp(name, "additional_coronas") == 0;
            section = world ? nullptr : &g_dat[UppercaseKey(name)];
            continue;
        }
        Light l{};
        unsigned r, g, b, a, search;
        const int got = sscanf(p, "%u %u %u %u %f %f %f %f %f %d %d %u", &r, &g, &b, &a, &l.off[0], &l.off[1], &l.off[2],
                               &l.size, &l.drawDist, &l.blink, &l.noDist, &search);
        if (got == 11) { l.noDist = l.blink; l.blink = (int)l.drawDist; l.drawDist = 0.0f; }
        else if (got != 12) continue; // comment, blank
        // ponytail: traffic lights (CustomSize 0.45) skipped; they need CTrafficLights phases and each light's road axis.
        if (l.size == 0.45f) continue;
        l.r = (uint8_t)r; l.g = (uint8_t)g; l.b = (uint8_t)b; l.a = (uint8_t)a;
        if (world) g_lamps.push_back({ { l.off[0], l.off[1], l.off[2] }, l.r, l.g, l.b, l.a, l.size, 0.0f, l.blink, l.noDist, -1, 0, 0 });
        else if (section) section->push_back(l);
        ++n;
    }
    fclose(f);
    return n;
}

// One IPL instance: lamps for every dat light of its model. Heading as LoadObjectInstance sets it for an untilted
// instance: acos(qw) x (qz < 0 ? 2 : -2).
static void Catch(const uint8_t* inst) {
    const uint8_t* mi = g_modelInfos[*(const uint16_t*)(inst + INST::Model)];
    if (!mi || inst[INST::Area] != 0) return; // interiors never show outside
    auto it = g_dat.find(*(const uint32_t*)(mi + MDL::Key));
    if (it == g_dat.end()) return;
    const float* q = (const float*)inst;
    if (q[0] == 0.0f && q[1] == 0.0f) return;
    const float h = acosf(fminf(fmaxf(q[6], -1.0f), 1.0f)) * (q[5] < 0.0f ? 2.0f : -2.0f);
    const float c = cosf(h), s = sinf(h), dd = *(const float*)(mi + MDL::DrawDistance);
    for (const Light& l : it->second)
        g_lamps.push_back({ { q[0] + c * l.off[0] - s * l.off[1], q[1] + s * l.off[0] + c * l.off[1], q[2] + l.off[2] },
                            l.r, l.g, l.b, l.a, l.size, fminf(l.drawDist, dd), l.blink, l.noDist, -1, 0, 0 });
}

typedef uint8_t* (*LoadInstance_Fn)(const uint8_t* inst, const char* name, uint8_t fromIpl);
static LoadInstance_Fn o_LoadInstance = nullptr;
static uint8_t* Hooked_LoadInstance(const uint8_t* inst, const char* name, uint8_t fromIpl) {
    uint8_t* e = o_LoadInstance(inst, name, fromIpl);
    if (e && g_catching) Catch(inst);
    return e;
}

// ---------------------------------------------------------------- UE side
static void Call(uint8_t* obj, void* fn, void* parms) {
    ((void (*)(void*, void*, void*))(*(void***)obj)[UO::ProcessEventSlot])(obj, fn, parms);
}

// GObjects pass: the four UFunctions, and the AGTAWorldSettings whose static coronas DE's lights already use.
static bool Resolve() {
    static int32_t nFunction = -1, nSettings = -1, nComponent = -1, nAdd = -1, nUpdate = -1, nRemove = -1, nGet = -1;
    int32_t* const ids[] = { &nFunction, &nSettings, &nComponent, &nAdd, &nUpdate, &nRemove, &nGet };
    const char* const names[] = { "Function", "GTAWorldSettings", "GTACoronaComponent", "AddCorona", "UpdateCorona",
                                  "RemoveCorona", "GetCoronaComponent" };
    for (int i = 0; i < 7; ++i)
        if (*ids[i] < 0 && (*ids[i] = FindName(names[i])) < 0) return false;
    uint8_t* best = nullptr;
    int candidates = 0;
    const int32_t count = *(int32_t*)(g_objects + 0x14);
    for (int32_t i = 0; i < count; ++i) {
        uint8_t* item = ObjectItem(i);
        uint8_t* obj = item ? *(uint8_t**)item : nullptr;
        if (!obj || (*(int32_t*)(item + 8) & ((1 << 29) | (1 << 28)))) continue;
        const int32_t cls = ClassOf(obj), name = NameOf(obj);
        if (cls == nFunction) {
            const int32_t outer = NameOf(*(uint8_t**)(obj + UO::Outer));
            if (outer == nComponent) {
                if (name == nAdd) g_fnAdd = obj;
                else if (name == nUpdate) g_fnUpdate = obj;
                else if (name == nRemove) g_fnRemove = obj;
            } else if (outer == nSettings && name == nGet) g_fnGetComponent = obj;
        } else if (cls == nSettings && !(*(uint32_t*)(obj + UO::Flags) & (RF_ClassDefaultObject | RF_ArchetypeObject))) {
            ++candidates;
            if (!best || *(uint8_t**)(obj + WS::StaticCoronas)) { best = obj; g_settingsIndex = i; }
        }
    }
    if (!best || !g_fnAdd || !g_fnUpdate || !g_fnRemove || !g_fnGetComponent) return false;
    struct { bool dynamic, upClose; uint8_t pad[6]; uint8_t* ret; } get = { true, false, {}, nullptr };
    Call(best, g_fnGetComponent, &get);
    Log(1, "coronas: GTAWorldSettings %p (%d candidates), corona component %p", best, candidates, get.ret);
    g_settings = best;
    g_component = get.ret;
    return g_component != nullptr;
}

static void Show(Lamp& l, float scale, float intensity) {
    const float pos[3] = { l.pos[0] * 100.0f, -l.pos[1] * 100.0f, l.pos[2] * 100.0f }; // GTA m -> UE cm, Y flipped
    const uint32_t color = 0xFF000000u | l.r << 16 | l.g << 8 | l.b; // FColor BGRA
    if (l.corona < 0) {
        struct { float pos[3], intensity, scale; uint32_t color; int32_t ret; } p = { { pos[0], pos[1], pos[2] }, intensity, scale, color, -1 };
        Call(g_component, g_fnAdd, &p);
        l.corona = p.ret;
        ++g_shown;
    } else {
        struct { int32_t index; float pos[3], scale; uint32_t color; float intensity; } p = { l.corona, { pos[0], pos[1], pos[2] }, scale, color, intensity };
        Call(g_component, g_fnUpdate, &p);
    }
    l.scale = scale;
    l.intensity = intensity;
}

static void Hide(Lamp& l) {
    int32_t index = l.corona;
    Call(g_component, g_fnRemove, &index);
    l.corona = -1;
    --g_shown;
}

// p2dfx smooth blink: on/off ms, edges eased over up to 500 ms, one phase for all lamps of a mode.
static float Pulse(uint32_t t, uint32_t on, uint32_t off) {
    const uint32_t phase = t % (on + off);
    if (phase >= on) return 0.0f;
    const float x = fminf(fminf((float)phase, (float)(on - phase)) / fminf(500.0f, on * 0.5f), 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// RegisterLODLights for one lamp: alpha 0..1 (0 = no corona) and radius in m. night = 0..255 time-of-day alpha.
static float LampAlpha(const Lamp& l, const float* cam, float farClip, float night, uint32_t ms, float* radius) {
    if (l.pos[2] < -15.0f || l.pos[2] > 1030.0f) return 0.0f;
    const float dx = cam[0] - l.pos[0], dy = cam[1] - l.pos[1], dz = cam[2] - l.pos[2];
    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
    const float nearEdge = l.drawDist - 30.0f; // the model's own corona covers the range below
    if (!l.noDist && (d <= nearEdge || d >= farClip)) return 0.0f;
    float r = 1.75f, a = 1.0f;
    if (!l.noDist) {
        r = fminf(1.75f * (d - nearEdge) / 30.0f, 1.75f);
        if (d < l.drawDist) a = (d - nearEdge) / 30.0f;
        else if (d > farClip - 100.0f) a = fminf(fmaxf((farClip - d) / 100.0f, 0.0f), 1.0f);
    }
    r *= fminf(1.0f + 3.0f * (d - nearEdge) / (1000.0f - nearEdge), 4.0f); // SlightlyIncreaseRadiusWithDistance
    // CoronaAlphaNearMinMult 0.5 up to 1 over CoronaAlphaReachOneAt 150 m, then x4 (FarBoostMax) over 900 m.
    const float past = d - nearEdge;
    a *= past > 150.0f ? 1.0f + 3.0f * fminf((past - 150.0f) / 900.0f, 1.0f) : 0.5f + 0.5f * fmaxf(past, 0.0f) / 150.0f;
    a *= night / 255.0f * l.a / 255.0f;
    r *= l.size * g_cfg.coronaSize;
    if (r > 1.0f) a *= fminf(fmaxf(1.0f / (0.75f * r + 0.25f), 0.3f), 1.0f);
    if (l.blink == 1) a *= Pulse(ms, 500, 500);
    else if (l.blink >= 2 && l.blink <= 7) a *= Pulse(ms, (l.blink - 1) * 1000, l.blink == 7 ? 4000 : (l.blink - 1) * 1000);
    *radius = r;
    return fminf(a, 1.0f);
}

// p2dfx night alpha: 20:00-07:00, 30 -> 255 over 20:00-21:00 ... 255 until 03:00 -> 30 at 07:00.
static float NightAlpha() {
    const int t = *g_hours * 60 + *g_minutes;
    if (t >= 20 * 60) return fminf(15.0f / 16.0f * t - 1095.0f, 255.0f);
    if (t < 3 * 60) return 255.0f;
    if (t < 7 * 60) return -15.0f / 16.0f * t + 424.0f;
    return 0.0f;
}

void CoronasFrame() {
    if (g_catching) {
        g_catching = false;
        g_dat.clear();
        const Lamp* l = g_lamps.empty() ? nullptr : &g_lamps.back();
        Log(1, "coronas: %d lamps after level load (last at %.1f %.1f %.1f, draw distance %.0f)", (int)g_lamps.size(),
            l ? l->pos[0] : 0.0f, l ? l->pos[1] : 0.0f, l ? l->pos[2] : 0.0f, l ? l->drawDist : 0.0f);
    }
    if (!g_coronasOk || g_lamps.empty() || !OnGameThread()) return;
    static unsigned frame = 0;
    ++frame;
    if (g_component && !LiveAt(g_settingsIndex, g_settings)) { // world unloaded, component and coronas with it
        Log(1, "coronas: world settings gone (%d coronas dropped)", g_shown);
        g_component = nullptr;
        for (Lamp& l : g_lamps) l.corona = -1;
        g_shown = 0;
    }
    const float night = NightAlpha();
    const bool on = g_active && g_cfg.coronas && *g_currArea == 0 && night > 0.0f;
    if (!g_component) {
        if (!on || frame % 120 != 1) return;
        if (!Resolve()) {
            static bool logged = false;
            if (!logged) { logged = true; Log(1, "coronas: GTAWorldSettings / corona UFunctions not found yet"); }
            return;
        }
    }
    if (!on && !g_shown) return;
    const float farClip = g_cfg.coronaFarClip > 0.0f ? g_cfg.coronaFarClip : g_look.farClip > 0.0f ? g_look.farClip : 1500.0f;
    const uint32_t ms = *g_timeMs;
    for (Lamp& l : g_lamps) {
        float r = 0.0f;
        const float a = on ? LampAlpha(l, g_fx.cam + 12, farClip, night, ms, &r) : 0.0f;
        if (a < 1.0f / 255.0f) { if (l.corona >= 0) Hide(l); continue; }
        const float scale = r * 100.0f, intensity = a * g_cfg.coronaIntensity;
        if (l.corona < 0 || fabsf(scale - l.scale) > 0.03f * scale || fabsf(intensity - l.intensity) > 0.03f * fmaxf(intensity, l.intensity))
            Show(l, scale, intensity);
    }
}

bool InstallCoronas() {
    // CFileLoader::LoadObjectInstance(CFileObjectInstance*, name, fromIpl), the entity builder for text and binary IPL
    // lines (0x141140270); lea r15, CModelInfo::ms_modelInfoPtrs at +0x15.
    uint8_t* fn = FindUnique("48 89 6C 24 20 57 41 56 41 57 48 81 EC A0 00 00 00 0F B7 41 1C 4C 8D 3D ?? ?? ?? ?? 41 0F B6 E8 48 8B F9 4D 8B 34 C7");
    static const uint8_t leaR15[] = { 0x4C, 0x8D, 0x3D };
    g_modelInfos = fn ? (uint8_t**)RipAt(fn + 0x15, leaR15, 3, 7) : nullptr;
    if (!g_modelInfos || !g_namePool || !g_objects || !g_hours || !g_minutes || !g_timeMs || !g_currArea) {
        Log(1, "coronas: LoadObjectInstance %p, model infos %p, name pool %p, clock %p: distant lamp coronas unavailable",
            fn, g_modelInfos, g_namePool, g_hours);
        return false;
    }
    const int lights = LoadDat();
    if (lights <= 0) { Log(1, "coronas: %sSALodLights.dat missing or empty: distant lamp coronas off", g_dir); return false; }
    if (MH_CreateHook(fn, (void*)&Hooked_LoadInstance, (void**)&o_LoadInstance) != MH_OK) { Log(1, "coronas: hook failed"); return false; }
    Log(1, "coronas: %d lights for %d models from SALodLights.dat", lights, (int)g_dat.size());
    g_catching = true;
    return g_coronasOk = true;
}

void CoronasPanel() {
    if (!g_coronasOk) { ImGui::TextDisabled("unavailable (see SkyGfxDE.log)"); return; }
    ImGui::Checkbox("Distant lamp coronas (Project2DFX)", &g_cfg.coronas);
    SliderBox("Size", &g_cfg.coronaSize, 0.1f, 2.0f, 0.0f, 20.0f);
    SliderBox("Intensity", &g_cfg.coronaIntensity, 0.0f, 4.0f, 0.0f, 100.0f);
    SliderBox("Far clip (0 = auto)", &g_cfg.coronaFarClip, 0.0f, 5000.0f, 0.0f, 20000.0f, "%.0f m");
    ImGui::Text("%d lamps, %d coronas now", (int)g_lamps.size(), g_shown);
}
