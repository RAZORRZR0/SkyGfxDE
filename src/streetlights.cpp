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
#include <unordered_set>
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

// [StreetLights] OtherLightShadows: every world light casts shadows. Each point/spot light component whose owner is
// not a vehicle, pickup, explosion or other moving/effect actor (kSkipOwners: DE's 158 light-owning blueprints sorted
// by owner, the 105 fixtures kept) gets UE's CastShadows and CastDynamicShadows (+0x214 bits 1 and 3). Street lamps
// (AStreetLightMapActor) with bNeverCastShadows (+0x300 bit 7, e.g. the park bollards) also have it cleared: DE's lamp
// update (0x140C206C0) rewrites CastShadows from it (= !bNeverCastShadows && the Streetlights option's spot/point
// shadow switch) whenever it runs. CastDynamicShadows has no setter: the bit is set first, then
// ULightComponentBase::SetCastShadows(false/true) (ProcessEvent, game thread: AGTATimeOfDay::Tick) re-registers the
// light with both. Park bollards and globe lamps also get a brightness multiplier (ULightComponent::SetIntensity).
// A shadowed light sits inside its own fixture (lantern heads such as SF Chinatown's chinalamp_sf), which then blacks
// it out up close (within the shadow distance) while the far, unshadowed lamps still light the street. So the
// owner's small meshes (bounds radius < kFixtureRadius) stop casting shadows (UPrimitiveComponent::SetCastShadow).
// ponytail: those fixtures cast no sun shadow while this is on; a per-light exclusion needs custom lighting channels.
// Everything is put back when the look is off or the setting is 0 / 1.
constexpr size_t kCastShadows = 0x214;    // ULightComponentBase bitfield byte: CastShadows = bit 1, CastDynamicShadows = bit 3
constexpr size_t kIntensity = 0x20C;      // ULightComponentBase::Intensity
constexpr size_t kLampFlags = 0x300;      // AStreetLightMapActor bitfield byte, bNeverCastShadows = bit 7
constexpr size_t kSuperStruct = 0x40;     // UStruct::SuperStruct
constexpr size_t kMeshCastShadow = 0x213; // UPrimitiveComponent bitfield byte: CastShadow = bit 5
constexpr size_t kBoundsRadius = 0x118;   // USceneComponent::Bounds.SphereRadius (FBoxSphereBounds at +0x100)
constexpr float kFixtureRadius = 600.0f;  // cm: a lamp post, not a building
static const char* const kSkipOwners[] = { // moving / effect light owners (and their subclasses)
    "BP_Vehicle_Base_C", "BP_Vehicle_Cinematic_Base_C", "BP_GTASA_UpgradePart_C", "BP_VehicleLight_Blinking_C",
    "BP_VehicleLight_Dash_C", "BP_VehicleLight_Dome_C", "BP_HelicopterSpotLight_C", "BP_RotatingColoredSpotLight_C",
    "BP_Pickup_Base_C", "BP_Pickup_Camera_C", "BP_Explosion_Base_C", "BP_SmokeFireLick_C", "BP_Marker_Cylinder_C",
    "BP_RailTriggerLight_SnailTrail_C", "BP_Singleton_C", "BP_SanAndreasInterface_C" };
struct Light { uint8_t* comp; uint8_t* owner; float intensity; bool neverWas, castWasOff, dynamicWasOff; int kind; };
static uint8_t *g_fnSetCastShadows = nullptr, *g_fnSetIntensity = nullptr, *g_fnSetMeshShadow = nullptr;
static std::unordered_map<int32_t, Light> g_lights;      // GObjects index of the light component
static std::unordered_map<int32_t, uint8_t*> g_meshes;   // fixture meshes whose shadow was turned off
static std::unordered_set<uint8_t*> g_owners;            // owners of shadowed lights
static volatile int g_shadowedCount = 0, g_meshCount = 0;

static void Call(uint8_t* obj, uint8_t* fn, void* parm) {
    ((void (*)(void*, void*, void*))(*(void***)obj)[UO::ProcessEventSlot])(obj, fn, parm);
}
static void SetCastShadows(uint8_t* comp, bool on) { Call(comp, g_fnSetCastShadows, &on); }
static void SetIntensity(uint8_t* comp, float v) { Call(comp, g_fnSetIntensity, &v); }
static void SetMeshShadow(uint8_t* comp, bool on) { Call(comp, g_fnSetMeshShadow, &on); }

static bool IsA(const uint8_t* obj, int32_t className) {
    for (const uint8_t* c = *(uint8_t* const*)(obj + UO::Class); c; c = *(uint8_t* const*)(c + kSuperStruct))
        if (NameOf(c) == className) return true;
    return false;
}
static bool OwnerIs(const uint8_t* owner, const char* const* names, size_t n) { // class or a superclass, by name
    for (const uint8_t* c = *(uint8_t* const*)(owner + UO::Class); c; c = *(uint8_t* const*)(c + kSuperStruct))
        for (size_t k = 0; k < n; ++k)
            if (NameIs(NameOf(c), names[k])) return true;
    return false;
}
static bool Skipped(const uint8_t* owner) { return OwnerIs(owner, kSkipOwners, sizeof(kSkipOwners) / sizeof(*kSkipOwners)); }

// Light multiplier by owner: park bollards (DE's are very bright next to the lampposts) and the SF park globe lamps
// (BP_Streetlamp1/2: an 8500 point light, about a tenth of a lamppost's spot, so no pool of light under them even
// unshadowed). 0 = untouched.
enum { kPlain, kBollard, kGlobe };
static float Scale(int kind) {
    return !g_active ? 1.0f : kind == kBollard ? g_cfg.bollardBrightness : kind == kGlobe ? g_cfg.globeLampBrightness : 1.0f;
}

static void Restore(Light& l) {
    if (l.dynamicWasOff) l.comp[kCastShadows] &= ~8;
    if (l.neverWas) l.owner[kLampFlags] |= 0x80;
    if (l.castWasOff || l.dynamicWasOff) { SetCastShadows(l.comp, l.castWasOff); SetCastShadows(l.comp, !l.castWasOff); }
    if (l.kind != kPlain) SetIntensity(l.comp, l.intensity);
}

void OtherLightShadowsFrame() {
    static int32_t nFunction = -1, nSet = -1, nSetI = -1, nSetM = -1, nBase = -1, nLight = -1, nPrim = -1, nPoint = -1,
                   nSpot = -1, nStreet = -1, cursor = 0;
    static bool off = false;
    if (off) return;
    if (!OnGameThread()) { off = true; Log(1, "light shadows: not on the game thread, disabled"); return; }
    if (nStreet < 0) {
        static int frame = 0;
        if (++frame % 120) return; // name pool walk only every 120 frames until the classes load
        nFunction = FindName("Function"); nSet = FindName("SetCastShadows"); nSetI = FindName("SetIntensity");
        nSetM = FindName("SetCastShadow"); nBase = FindName("LightComponentBase"); nLight = FindName("LightComponent");
        nPrim = FindName("PrimitiveComponent"); nPoint = FindName("PointLightComponent"); nSpot = FindName("SpotLightComponent");
        nStreet = FindName("StreetLightMapActor");
        if (nFunction < 0 || nSet < 0 || nSetI < 0 || nSetM < 0 || nBase < 0 || nLight < 0 || nPrim < 0 || nPoint < 0 || nSpot < 0) {
            nStreet = -1; return;
        }
    }
    const bool shadows = g_active && g_cfg.otherLightShadows;
    const float scale[3] = { 1.0f, Scale(kBollard), Scale(kGlobe) };
    const bool unscaled = scale[kBollard] == 1.0f && scale[kGlobe] == 1.0f;
    static float applied[3] = { 1.0f, 1.0f, 1.0f };
    static bool appliedShadows = false;
    if (shadows != appliedShadows || (!shadows && unscaled)) { // put everything back; rescanned below if wanted
        for (auto& kv : g_lights)
            if (LiveAt(kv.first, kv.second.comp)) Restore(kv.second);
        for (auto& kv : g_meshes)
            if (LiveAt(kv.first, kv.second)) SetMeshShadow(kv.second, true);
        g_lights.clear(); g_meshes.clear(); g_owners.clear();
        g_shadowedCount = g_meshCount = 0;
        applied[kBollard] = applied[kGlobe] = 1.0f;
        appliedShadows = shadows;
        if (!shadows && unscaled) return;
    }
    if (scale[kBollard] != applied[kBollard] || scale[kGlobe] != applied[kGlobe]) { // slider moved: rescale those seen
        for (auto& kv : g_lights)
            if (kv.second.kind != kPlain && LiveAt(kv.first, kv.second.comp))
                SetIntensity(kv.second.comp, kv.second.intensity * scale[kv.second.kind]);
        applied[kBollard] = scale[kBollard]; applied[kGlobe] = scale[kGlobe];
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
            else if (name == nSetM && outer == nPrim) g_fnSetMeshShadow = obj;
            continue;
        }
        if (!g_fnSetCastShadows || !g_fnSetIntensity || !g_fnSetMeshShadow) continue;
        if ((*(uint32_t*)(obj + UO::Flags) & 0x30) || g_lights.count(i) || g_meshes.count(i)) continue; // CDO / archetype, seen
        uint8_t* owner = *(uint8_t**)(obj + UO::Outer);
        if (!owner) continue;
        if (cls != nPoint && cls != nSpot) { // a fixture's own mesh: no shadow, or it blacks out its light
            if (shadows && g_owners.count(owner) && (obj[kMeshCastShadow] & 0x20) && IsA(obj, nPrim)
                && *(float*)(obj + kBoundsRadius) < kFixtureRadius) {
                SetMeshShadow(obj, false);
                g_meshes.emplace(i, obj);
            }
            continue;
        }
        if (Skipped(owner)) continue;
        const bool street = nStreet >= 0 && IsA(owner, nStreet);
        static const char* const kBollards[] = { "BP_Streetlight_BollardPark_C" }, *const kGlobes[] = { "BP_Streetlamp1_C", "BP_Streetlamp2_C" };
        const int kind = OwnerIs(owner, kBollards, 1) ? kBollard : OwnerIs(owner, kGlobes, 2) ? kGlobe : kPlain;
        Light l{ obj, owner, *(float*)(obj + kIntensity), street && (owner[kLampFlags] & 0x80), !(obj[kCastShadows] & 2),
                 !(obj[kCastShadows] & 8), kind };
        if (shadows) {
            if (l.neverWas) owner[kLampFlags] &= ~0x80;
            if (l.castWasOff || l.dynamicWasOff) {
                obj[kCastShadows] |= 8;                                    // no UFunction sets it; read at re-register
                SetCastShadows(obj, false); SetCastShadows(obj, true);    // force the re-register (setter skips no-ops)
            }
            g_owners.insert(owner);
        } else l.neverWas = l.castWasOff = l.dynamicWasOff = false;
        if (scale[kind] != 1.0f) SetIntensity(obj, l.intensity * scale[kind]);
        g_lights.emplace(i, l);
    }
    g_shadowedCount = (int)g_lights.size();
    g_meshCount = (int)g_meshes.size();
    static int logged = -1;
    if (cursor < 4096 && logged != g_shadowedCount) {
        logged = g_shadowedCount;
        Log(1, "light shadows: %d lights, %d fixture meshes without shadow", logged, (int)g_meshCount);
    }
}

void StreetLightsPanel() {
    if (!g_drawVar) { ImGui::TextDisabled("unavailable (see SkyGfxDE.log)"); return; }
    SliderBox("Light distance", &g_cfg.lampDrawDistance, 0.0f, 500.0f, 0.0f, 5000.0f, "%.0f m");
    SliderBox("Shadow distance", &g_cfg.lampShadowDistance, 0.0f, 150.0f, 0.0f, 1000.0f, "%.0f m");
    ImGui::TextDisabled("DE's: %d / %d cm (0 = DE's); applies to lamps as they stream in", g_deDraw, g_deShadow);
    ImGui::Checkbox("Shadows for all lights", &g_cfg.otherLightShadows);
    SliderBox("Park bollard brightness", &g_cfg.bollardBrightness, 0.0f, 1.0f, 0.0f, 4.0f);
    SliderBox("Park globe lamp brightness", &g_cfg.globeLampBrightness, 1.0f, 16.0f, 0.0f, 50.0f);
    ImGui::TextDisabled("%d lights, %d fixture meshes without shadow", g_shadowedCount, g_meshCount);
}
