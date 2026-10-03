// Street lights. DE's lamps are AStreetLightMapActor actors with spot/point light components. DE's lamp update
// (0x140C206C0, a virtual of the lamp actor) gives each light MaxDrawDistance = gta.streetlightdistance (cm, int) and
// its engine's per-light shadow distance (+0x324) = gta.streetlight.shadowdistance (cm, int; 0 = the Streetlights
// option's Spot/PointLightShadowDistance). In game both read 4 (cm), so UE culls every lamp light (no pool of light,
// only the glowing lamp heads) and none casts a shadow. Both cvars are set to [StreetLights] DrawDistance /
// ShadowDistance every frame, before DE's update runs for the lamps that stream in, so DE applies them through its own
// path. Writing the light components directly crashed the game (heap corruption at load), so nothing else is touched.
#include "skygfx.h"
#include <string.h>
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

void StreetLightsPanel() {
    if (!g_drawVar) { ImGui::TextDisabled("unavailable (see SkyGfxDE.log)"); return; }
    SliderBox("Light distance", &g_cfg.lampDrawDistance, 0.0f, 500.0f, 0.0f, 5000.0f, "%.0f m");
    SliderBox("Shadow distance", &g_cfg.lampShadowDistance, 0.0f, 150.0f, 0.0f, 1000.0f, "%.0f m");
    ImGui::TextDisabled("DE's: %d / %d cm (0 = DE's); applies to lamps as they stream in", g_deDraw, g_deShadow);
}
