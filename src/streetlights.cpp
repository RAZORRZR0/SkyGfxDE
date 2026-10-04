// Street lights. DE's lamps are AStreetLightMapActor actors with spot/point light components. DE's lamp update
// (0x140C206C0, a virtual of the lamp actor) gives each light MaxDrawDistance = gta.streetlightdistance (cm, int) and
// its engine's per-light shadow distance (+0x324) = gta.streetlight.shadowdistance (cm, int; 0 = the Streetlights
// option's Spot/PointLightShadowDistance). In game both read 4 (cm), so UE culls every lamp light (no pool of light,
// only the glowing lamp heads) and none casts a shadow. Both cvars are set to [StreetLights] DrawDistance /
// ShadowDistance every frame, before DE's update runs for the lamps that stream in, so DE applies them through its own
// path. Writing the light components directly crashed the game (heap corruption at load), so nothing else is touched.
#include "skygfx.h"
#include <string.h>
#include <unordered_map>
#include "../imgui/imgui.h"

int32_t *g_drawVar = nullptr, *g_shadowVar = nullptr; // the cvars' int storage (offline check asserts them)
static int32_t g_deDraw = -1, g_deShadow = -1;                 // DE's values, restored when the look is off

// Storage of an int cvar registered by reference: lea r9, help; lea r8, <var>; mov [rsp+20h], 0; lea rdx, L"name";
// call [rax+38h] (IConsoleManager::RegisterConsoleVariableRef).
static int32_t* FindRefCVar(const wchar_t* name) {
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
    static const uint8_t leaRdx[] = { 0x48, 0x8D, 0x15 }, leaR8[] = { 0x4C, 0x8D, 0x05 };
    sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5)) continue;
        uint8_t* b = g_moduleBase + sec->VirtualAddress;
        for (uint8_t* cur = b + 0x10; cur + 7 < b + sec->Misc.VirtualSize; ++cur)
            if (RipAt(cur, leaRdx, 3, 7) == str) return (int32_t*)RipAt(cur - 0xF, leaR8, 3, 7); // lea r8 (7) + mov (8)
    }
    return nullptr;
}

bool InstallStreetLights() {
    g_drawVar = FindRefCVar(L"gta.streetlightdistance");
    g_shadowVar = FindRefCVar(L"gta.streetlight.shadowdistance");
    Log(1, g_drawVar && g_shadowVar ? "street lights: cvars found" : "street lights: cvars NOT found, lamps unchanged");
    return g_drawVar && g_shadowVar;
}

void StreetLightsFrame() {
    if (!g_drawVar || !g_shadowVar) return;
    const int32_t want[2] = { (int32_t)(g_cfg.lampDrawDistance * 100.0f), (int32_t)(g_cfg.lampShadowDistance * 100.0f) };
    int32_t* var[2] = { g_drawVar, g_shadowVar };
    int32_t* de[2] = { &g_deDraw, &g_deShadow };
    for (int i = 0; i < 2; ++i) {
        if (*de[i] < 0) {
            *de[i] = *var[i];
            Log(1, "street lights: DE's %s = %d cm", i ? "gta.streetlight.shadowdistance" : "gta.streetlightdistance", *de[i]);
        }
        const int32_t v = g_active && want[i] > 0 ? want[i] : *de[i];
        if (*var[i] != v) *var[i] = v;
    }
}

// The park bollards (BP_Streetlight_BollardPark_C) are lamp actors with AStreetLightMapActor::bNeverCastShadows
// (+0x300 bit 7) set, and DE's lamp update (0x140C206C0) sets every lamp light's CastShadows from it
// (CastShadows = !bNeverCastShadows && the Streetlights option's spot/point shadow switch) whenever it runs: a
// SetCastShadows from outside is undone at the next lamp update. So the actor's bit is cleared (a plain flag only
// that update reads) and its light components get UE's own ULightComponentBase::SetCastShadows(true) (ProcessEvent,
// game thread: called from AGTATimeOfDay::Tick); later updates then keep them on, with ShadowDistance applied.
// The bollard blueprint also has CastDynamicShadows (+0x214 bit 3) off, which UE needs for movable lights to cast any
// shadow, and has no setter: the bit is set first, then SetCastShadows(false/true) re-registers the light with both.
// The bollards' lights are also scaled by [StreetLights] BollardBrightness through ULightComponent::SetIntensity.
// Everything is put back when the look is off or the setting is 0 / 1.
constexpr size_t kCastShadows = 0x214;    // ULightComponentBase bitfield byte: CastShadows = bit 1, CastDynamicShadows = bit 3
constexpr size_t kIntensity = 0x20C;      // ULightComponentBase::Intensity
constexpr size_t kLampFlags = 0x300;      // AStreetLightMapActor bitfield byte, bNeverCastShadows = bit 7
constexpr size_t kSuperStruct = 0x40;     // UStruct::SuperStruct
struct Bollard { uint8_t* comp; uint8_t* owner; float intensity; bool shadowsWereOff, dynamicWasOff; };
static uint8_t *g_fnSetCastShadows = nullptr, *g_fnSetIntensity = nullptr;
static std::unordered_map<int32_t, Bollard> g_bollards; // GObjects index of the light component
static volatile int g_shadowedCount = 0;

static void Call(uint8_t* obj, uint8_t* fn, void* parm) {
    ((void (*)(void*, void*, void*))(*(void***)obj)[UO::ProcessEventSlot])(obj, fn, parm);
}
static void SetCastShadows(uint8_t* comp, bool on) { Call(comp, g_fnSetCastShadows, &on); }
static void SetIntensity(uint8_t* comp, float v) { Call(comp, g_fnSetIntensity, &v); }

static bool IsA(const uint8_t* obj, int32_t className) {
    for (const uint8_t* c = *(uint8_t* const*)(obj + UO::Class); c; c = *(uint8_t* const*)(c + kSuperStruct))
        if (NameOf(c) == className) return true;
    return false;
}

static void Restore(Bollard& b) {
    if (b.dynamicWasOff) b.comp[kCastShadows] &= ~8;
    if (b.shadowsWereOff) b.owner[kLampFlags] |= 0x80;
    if (b.shadowsWereOff || b.dynamicWasOff) { SetCastShadows(b.comp, !b.shadowsWereOff); }
    SetIntensity(b.comp, b.intensity);
}

void OtherLightShadowsFrame() {
    static int32_t nFunction = -1, nSet = -1, nSetI = -1, nBase = -1, nLight = -1, nPoint = -1, nSpot = -1, nBollard = -1, cursor = 0;
    static float applied = 1.0f;
    static bool off = false;
    if (off) return;
    if (!OnGameThread()) { off = true; Log(1, "bollards: not on the game thread, disabled"); return; }
    if (nBollard < 0) {
        static int frame = 0;
        if (++frame % 120) return; // name pool walk only every 120 frames until the classes load
        nFunction = FindName("Function"); nSet = FindName("SetCastShadows"); nSetI = FindName("SetIntensity");
        nBase = FindName("LightComponentBase"); nLight = FindName("LightComponent");
        nPoint = FindName("PointLightComponent"); nSpot = FindName("SpotLightComponent");
        nBollard = FindName("BP_Streetlight_BollardPark_C");
        if (nFunction < 0 || nSet < 0 || nSetI < 0 || nBase < 0 || nLight < 0 || nPoint < 0 || nSpot < 0) { nBollard = -1; return; }
    }
    const bool shadows = g_active && g_cfg.otherLightShadows;
    const float scale = g_active ? g_cfg.bollardBrightness : 1.0f;
    static bool appliedShadows = false;
    if (shadows != appliedShadows || (!shadows && scale == 1.0f)) { // put everything back; rescanned below if wanted
        for (auto& kv : g_bollards)
            if (LiveAt(kv.first, kv.second.comp)) Restore(kv.second);
        g_bollards.clear();
        g_shadowedCount = 0;
        applied = 1.0f;
        appliedShadows = shadows;
        if (!shadows && scale == 1.0f) return;
    }
    if (scale != applied) { // slider moved: rescale the ones already seen
        for (auto& kv : g_bollards)
            if (LiveAt(kv.first, kv.second.comp)) SetIntensity(kv.second.comp, kv.second.intensity * scale);
        applied = scale;
    }
    // ponytail: 4096 GObjects slots per frame (full pass ~100 frames), as peds.cpp; lamps stream in slower than that.
    const int32_t count = *(int32_t*)(g_objects + 0x14);
    for (int n = 0; n < 4096 && count > 0; ++n) {
        if (cursor >= count) cursor = 0;
        const int32_t i = cursor++;
        uint8_t* item = ObjectItem(i);
        uint8_t* obj = item ? *(uint8_t**)item : nullptr;
        if (!obj || (*(int32_t*)(item + 8) & ((1 << 29) | (1 << 28)))) continue;
        const int32_t cls = ClassOf(obj);
        if (cls == nFunction) {
            const int32_t name = NameOf(obj), outer = NameOf(*(uint8_t**)(obj + UO::Outer));
            if (name == nSet && outer == nBase) g_fnSetCastShadows = obj;
            else if (name == nSetI && outer == nLight) g_fnSetIntensity = obj;
            continue;
        }
        if ((cls != nPoint && cls != nSpot) || !g_fnSetCastShadows || !g_fnSetIntensity) continue;
        if ((*(uint32_t*)(obj + UO::Flags) & 0x30) || g_bollards.count(i)) continue; // CDO / archetype, seen
        uint8_t* owner = *(uint8_t**)(obj + UO::Outer);
        if (!owner || !IsA(owner, nBollard)) continue;
        Bollard b{ obj, owner, *(float*)(obj + kIntensity), (owner[kLampFlags] & 0x80) != 0, !(obj[kCastShadows] & 8) };
        if (shadows && (b.shadowsWereOff || b.dynamicWasOff)) {
            owner[kLampFlags] &= ~0x80;
            obj[kCastShadows] |= 8;                                    // no UFunction sets it; read at re-register
            SetCastShadows(obj, false); SetCastShadows(obj, true);    // force the re-register (setter skips no-ops)
        } else b.shadowsWereOff = b.dynamicWasOff = false;
        if (scale != 1.0f) SetIntensity(obj, b.intensity * scale);
        g_bollards.emplace(i, b);
    }
    g_shadowedCount = (int)g_bollards.size();
    static int logged = -1;
    if (cursor < 4096 && logged != g_shadowedCount) { logged = g_shadowedCount; Log(1, "bollards: %d lights adjusted", logged); }
}

void StreetLightsPanel() {
    if (!g_drawVar) { ImGui::TextDisabled("unavailable (see SkyGfxDE.log)"); return; }
    SliderBox("Light distance", &g_cfg.lampDrawDistance, 0.0f, 500.0f, 0.0f, 5000.0f, "%.0f m");
    SliderBox("Shadow distance", &g_cfg.lampShadowDistance, 0.0f, 150.0f, 0.0f, 1000.0f, "%.0f m");
    ImGui::TextDisabled("DE's: %d / %d cm (0 = DE's); applies to lamps as they stream in", g_deDraw, g_deShadow);
    ImGui::Checkbox("Park bollard shadows", &g_cfg.otherLightShadows);
    SliderBox("Park bollard brightness", &g_cfg.bollardBrightness, 0.0f, 1.0f, 0.0f, 4.0f);
    ImGui::TextDisabled("%d bollard lights adjusted", g_shadowedCount);
}
