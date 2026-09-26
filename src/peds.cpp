// Matte characters. DE's ped materials are glossy PBR: skin (M_Character_VGD) GlobalRoughness 0.5, clothes
// (M_Character_Clothes_VGD) Roughness 0.9 x texture with Specular 0.5, hair (M_Character_Hair_VGD) Roughness 0.3 with
// Specular 1.0. The originals were matte. Every loaded material instance under those masters gets its roughness pulled
// towards 1 and its specular scaled down by [Characters] Matte. Values go through UMaterialInstanceDynamic::
// SetScalarParameterValue called with ProcessEvent: its native body (SetScalarParameterValueInternal at 0x1433AADD0,
// then GameThread_UpdateMIParameter) is plain UMaterialInstance code, so it also works on the cooked constant
// instances. Game thread only (the instance arrays are game-thread state).
#include "skygfx.h"
#include <math.h>
#include <string.h>
#include <unordered_map>
#include "../imgui/imgui.h"

uint8_t* g_namePool = nullptr; // FNamePool: Blocks[] at +0x10, CurrentBlock +8, CurrentByteCursor +0xC

namespace UO { constexpr size_t Class = 0x10, Name = 0x18, Outer = 0x20, Index = 0x0C; constexpr int ProcessEventSlot = 0x43; }
namespace MI { constexpr size_t Parent = 0xD0, Scalars = 0xE0, ScalarStride = 0x24, ScalarValue = 0x10; } // UMaterialInstance
constexpr uint8_t GlobalParameter = 2; // EMaterialParameterAssociation

enum Param { P_GLOBALROUGH, P_ROUGH, P_SPEC, P_COUNT };
static const char* const kParamNames[P_COUNT] = { "GlobalRoughness", "Roughness", "Specular" };
static const bool kRoughness[P_COUNT] = { true, true, false };

// Defaults from each master's cooked CachedExpressionData; NAN = the master has no such parameter.
struct Master { const char* name; float def[P_COUNT]; };
static Master g_masters[] = {
    { "M_Character_VGD",         { 0.5f, NAN, 0.02f } },
    { "M_Character_Clothes_VGD", { NAN, 0.9f, 0.5f } },
    { "M_Character_Clothes",     { NAN, 0.9f, 0.5f } },
    { "M_Character_Hair_VGD",    { NAN, 0.3f, 1.0f } },
};

struct Tracked { uint8_t* obj; Master* master; float orig[P_COUNT]; float applied[P_COUNT]; };
static std::unordered_map<int32_t, Tracked> g_tracked; // by GObjects index, game thread only
static int32_t g_param[P_COUNT] = { -1, -1, -1 };
static int32_t g_clsMaterial = -1, g_clsMIC = -1, g_clsMID = -1, g_clsFunction = -1, g_nameSetScalar = -1;
static void* g_setScalar = nullptr; // UFunction MaterialInstanceDynamic:SetScalarParameterValue
static bool g_pedsOk = false;
static float g_appliedMatte = -1.0f;
static int g_pedSets = 0;

// ---------------------------------------------------------------- FName
static const uint8_t* NameEntry(int32_t idx) {
    const uint32_t block = (uint32_t)idx >> 16;
    if (idx < 0 || block > *(uint32_t*)(g_namePool + 8)) return nullptr;
    const uint8_t* b = ((uint8_t**)(g_namePool + 0x10))[block];
    return b ? b + 2 * (idx & 0xFFFF) : nullptr;
}

static bool NameIs(int32_t idx, const char* s) {
    const uint8_t* e = NameEntry(idx);
    if (!e) return false;
    const uint16_t h = *(const uint16_t*)e; // bIsWide:1, probe hash:5, Len:10
    const size_t len = h >> 6;
    return !(h & 1) && len == strlen(s) && memcmp(e + 2, s, len) == 0;
}

// Comparison index of an ANSI name already in the pool, or -1.
static int32_t FindName(const char* s) {
    const size_t n = strlen(s);
    const uint32_t cur = *(uint32_t*)(g_namePool + 8), cursor = *(uint32_t*)(g_namePool + 0xC);
    for (uint32_t b = 0; b <= cur; ++b) {
        const uint8_t* base = ((uint8_t**)(g_namePool + 0x10))[b];
        const uint32_t end = b == cur ? cursor : 0x20000;
        for (uint32_t off = 0; base && off + 2 <= end;) {
            const uint16_t h = *(const uint16_t*)(base + off);
            const uint32_t len = h >> 6;
            if (!len) break;
            if (!(h & 1) && len == n && memcmp(base + off + 2, s, n) == 0) return (int32_t)((b << 16) | (off >> 1));
            off += (2 + len * ((h & 1) ? 2 : 1) + 1) & ~1u;
        }
    }
    return -1;
}

static int32_t NameOf(const uint8_t* obj) { return obj ? *(const int32_t*)(obj + UO::Name) : -1; }
static int32_t ClassOf(const uint8_t* obj) { return NameOf(*(uint8_t* const*)(obj + UO::Class)); }

// ---------------------------------------------------------------- material instances
static bool IsInstance(const uint8_t* obj) { const int32_t c = ClassOf(obj); return c == g_clsMIC || c == g_clsMID; }

static bool OwnScalar(const uint8_t* mi, int32_t name, float* out) {
    const uint8_t* data = *(uint8_t* const*)(mi + MI::Scalars);
    const int32_t n = *(const int32_t*)(mi + MI::Scalars + 8);
    for (int32_t i = 0; i < n; ++i) {
        const uint8_t* e = data + i * MI::ScalarStride;
        if (*(const int32_t*)e == name && *(const int32_t*)(e + 4) == 0 && e[8] == GlobalParameter) {
            *out = *(const float*)(e + MI::ScalarValue);
            return true;
        }
    }
    return false;
}

static Tracked* TrackedOf(uint8_t* obj) {
    auto it = g_tracked.find(*(int32_t*)(obj + UO::Index));
    return it != g_tracked.end() && it->second.obj == obj ? &it->second : nullptr;
}

// Root master of an instance chain, or nullptr when it is not one of ours.
static Master* MasterOf(const uint8_t* obj) {
    for (int depth = 0; obj && depth < 16; ++depth) {
        const int32_t c = ClassOf(obj);
        if (c == g_clsMaterial) {
            for (Master& m : g_masters)
                if (NameIs(NameOf(obj), m.name)) return &m;
            return nullptr;
        }
        if (c != g_clsMIC && c != g_clsMID) return nullptr;
        obj = *(uint8_t* const*)(obj + MI::Parent);
    }
    return nullptr;
}

// Value the game gives the parameter: first override up the chain (our tracked originals for instances we changed),
// else the master default.
static float GameValue(uint8_t* obj, int p, const Master* m) {
    for (int depth = 0; obj && depth < 16 && IsInstance(obj); ++depth) {
        if (Tracked* t = TrackedOf(obj)) return t->orig[p];
        float v;
        if (OwnScalar(obj, g_param[p], &v)) return v;
        obj = *(uint8_t**)(obj + MI::Parent);
    }
    return m->def[p];
}

static float Target(float orig, int p, float matte) {
    return kRoughness[p] ? orig + (fmaxf(orig, 1.0f) - orig) * matte : orig * (1.0f - matte);
}

static void SetScalar(uint8_t* obj, int p, float v) {
    struct { int32_t index, number; float value; int32_t pad; } parms = { g_param[p], 0, v, 0 };
    ((void (*)(void*, void*, void*))(*(void***)obj)[UO::ProcessEventSlot])(obj, g_setScalar, &parms);
    ++g_pedSets;
}

static void Apply(Tracked& t, float matte) {
    for (int p = 0; p < P_COUNT; ++p) {
        if (isnan(t.master->def[p])) continue;
        const float v = Target(t.orig[p], p, matte);
        if (v != t.applied[p]) { SetScalar(t.obj, p, v); t.applied[p] = v; }
    }
}

// ---------------------------------------------------------------- per frame
static bool OnGameThread() {
    static DWORD tid = 0;
    if (!tid) {
        DWORD pid = 0;
        const HWND w = FindWindowA("UnrealWindow", nullptr);
        const DWORD t = w ? GetWindowThreadProcessId(w, &pid) : 0;
        if (pid != GetCurrentProcessId()) return false;
        tid = t;
        Log(1, "characters: game thread %lu, look hook thread %lu", tid, GetCurrentThreadId());
    }
    return GetCurrentThreadId() == tid;
}

// Names the scan needs; masters are matched by string, so a master that never loads costs nothing.
static bool ResolveNames() {
    int32_t* const ids[] = { &g_clsMaterial, &g_clsMIC, &g_clsMID, &g_clsFunction, &g_nameSetScalar,
                             &g_param[P_GLOBALROUGH], &g_param[P_ROUGH], &g_param[P_SPEC] };
    const char* const names[] = { "Material", "MaterialInstanceConstant", "MaterialInstanceDynamic", "Function",
                                  "SetScalarParameterValue", kParamNames[P_GLOBALROUGH], kParamNames[P_ROUGH], kParamNames[P_SPEC] };
    bool ok = true;
    for (int i = 0; i < 8; ++i) {
        if (*ids[i] < 0) *ids[i] = FindName(names[i]); // GlobalRoughness appears once the skin master loads
        ok &= *ids[i] >= 0;
    }
    return ok;
}

// Tracked instance still in its GObjects slot and not PendingKill/Unreachable.
static bool LiveAt(int32_t i, const uint8_t* obj) {
    uint8_t* item = ObjectItem(i);
    return item && *(uint8_t**)item == obj && !(*(int32_t*)(item + 8) & ((1 << 29) | (1 << 28)));
}

// One GObjects slot: drop a dead entry, track a new character instance, catch values the game changed.
static void Visit(int32_t i, float matte) {
    auto it = g_tracked.find(i);
    if (it != g_tracked.end() && !LiveAt(i, it->second.obj)) { g_tracked.erase(it); it = g_tracked.end(); }
    uint8_t* item = ObjectItem(i);
    uint8_t* obj = item ? *(uint8_t**)item : nullptr;
    if (!obj || (*(int32_t*)(item + 8) & ((1 << 29) | (1 << 28)))) return;
    const int32_t cls = ClassOf(obj);
    if (!g_setScalar && cls == g_clsFunction && NameOf(obj) == g_nameSetScalar &&
        NameIs(NameOf(*(uint8_t**)(obj + UO::Outer)), "MaterialInstanceDynamic"))
        g_setScalar = obj;
    if (cls != g_clsMIC && cls != g_clsMID) return;
    if (it == g_tracked.end()) {
        Master* m = MasterOf(obj);
        if (!m) return;
        Tracked t{ obj, m, {}, { NAN, NAN, NAN } };
        for (int p = 0; p < P_COUNT; ++p) t.orig[p] = isnan(m->def[p]) ? NAN : GameValue(obj, p, m);
        it = g_tracked.emplace(i, t).first;
    } else {
        // Our value gone (instance reloaded, or the game set it): take the game's value as the new original.
        Tracked& t = it->second;
        for (int p = 0; p < P_COUNT; ++p) {
            float v;
            if (!isnan(t.applied[p]) && (!OwnScalar(obj, g_param[p], &v) || v != t.applied[p])) {
                t.orig[p] = OwnScalar(obj, g_param[p], &v) ? v : GameValue(*(uint8_t**)(obj + MI::Parent), p, t.master);
                t.applied[p] = NAN;
            }
        }
    }
    if (g_setScalar) Apply(it->second, matte);
}

static volatile int g_trackedCount = 0; // for the panel (render thread)

void PedsFrame() {
    if (!g_pedsOk) return;
    static bool disabled = false;
    static int32_t cursor = 0, frame = 0;
    if (disabled) return;
    if (!OnGameThread()) {
        disabled = true;
        Log(1, "characters: look hook is not on the game thread, matte characters disabled");
        return;
    }
    if (g_param[P_GLOBALROUGH] < 0 && ++frame % 120 != 0) return; // name pool walk only every 120 frames until loaded
    if (!ResolveNames()) return;
    const float matte = g_active ? g_cfg.pedMatte : 0.0f;
    if (matte != g_appliedMatte && g_setScalar) {
        const int before = g_pedSets;
        for (auto& kv : g_tracked)
            if (LiveAt(kv.first, kv.second.obj)) Apply(kv.second, matte);
        Log(1, "characters: matte %.2f, %d instances, %d parameter sets", matte, (int)g_tracked.size(), g_pedSets - before);
        g_appliedMatte = matte;
    }
    // ponytail: fixed 4096 GObjects slots per frame (full pass ~100 frames); budget by time if the table grows a lot.
    const int32_t count = *(int32_t*)(g_objects + 0x14);
    for (int n = 0; n < 4096 && count > 0; ++n) {
        if (cursor >= count) cursor = 0;
        Visit(cursor++, matte);
    }
    g_trackedCount = (int)g_tracked.size();
}

bool InstallPeds() {
    // FNamePool::Resolve-style accessor: lea rdx, NamePoolData at +0x1F.
    uint8_t* fn = FindUnique("40 53 48 83 EC 20 8B D9 0F B7 C1 C1 EB 10 80 3D ?? ?? ?? ?? 00 89 5C 24 38 89 44 24 3C 74 ?? 48 8D 15");
    static const uint8_t leaRdx[] = { 0x48, 0x8D, 0x15 };
    g_namePool = fn ? RipAt(fn + 0x1F, leaRdx, 3, 7) : nullptr;
    g_pedsOk = g_namePool && g_objects;
    Log(1, g_pedsOk ? "characters: FNamePool %p" : "characters: FNamePool not found, matte characters unavailable", g_namePool);
    return g_pedsOk;
}

void PedsPanel() {
    if (!g_pedsOk) { ImGui::TextDisabled("unavailable on this build"); return; }
    ImGui::SliderFloat("Matte", &g_cfg.pedMatte, 0.0f, 1.0f);
    ImGui::TextDisabled("roughness -> 1 and specular x(1 - matte) on skin, clothes and hair");
    ImGui::Text("%d character material instances", g_trackedCount);
}
