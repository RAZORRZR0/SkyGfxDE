// Unreal features DE ships but doesn't use (SSGI, GTAO, SSR, volumetric fog, lens flare, sun
// strength): each is forced by its own [ModernLook] toggle, independent of the others, and restored when off.
// Post-process volume values (AO, SSR, lens flare) are written into APostProcessVolume settings.
#include "skygfx.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include "../imgui/imgui.h"

namespace {

struct CVar {
    const wchar_t* name;
    int32_t forced;
    bool Config::* enable;
    int Config::* level; // quality level read from the config every frame (1..4); null: `forced`
    int32_t** holder;  // TAutoConsoleVariable<int32>: static holding TConsoleVariableData<int32>* (values [0] game, [1] render)
    int32_t* ref;      // FAutoConsoleVariableRef: plain int global, used when holder is null
    int32_t de[2];     // DE's values, saved on first force
    bool saved;
};

CVar g_cvars[] = {
    {L"r.SSGI.Enable", 1, &Config::modernSsgi, nullptr, nullptr, nullptr, {}, false},
    {L"r.SSGI.Quality", 4, &Config::modernSsgi, &Config::modernSsgiQuality, nullptr, nullptr, {}, false},
    {L"r.AmbientOcclusion.Method", 1, &Config::modernAo, nullptr, nullptr, nullptr, {}, false},
    {L"r.SSR.Quality", 4, &Config::modernSsr, &Config::modernSsrQuality, nullptr, nullptr, {}, false},
    {L"r.VolumetricFog", 1, &Config::modernVolFog, nullptr, nullptr, nullptr, {}, false},
};

// Address of the NUL-terminated wide string `name` (preceded by a NUL wchar) anywhere in the image.
const uint8_t* FindWideName(const wchar_t* name) {
    auto* base = (uint8_t*)g_moduleBase;
    auto* nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    const size_t len = (wcslen(name) + 1) * sizeof(wchar_t);
    const uint8_t* end = base + nt->OptionalHeader.SizeOfImage - len;
    for (const uint8_t* p = base + 2; p <= end; ++p)
        if (p[-1] == 0 && p[-2] == 0 && memcmp(p, name, len) == 0) return p;
    return nullptr;
}

// Plain-int form: lea r8, [global]; lea rdx, [name]; ...; call [rax+38h] (RegisterConsoleVariableRef).
// ponytail: scans the whole image, not just .text; runs once at install.
int32_t* FindRefCVar(const wchar_t* name) {
    const uint8_t* str = FindWideName(name);
    if (!str) return nullptr;
    static const uint8_t leaRdx[] = {0x48, 0x8D, 0x15};
    static const uint8_t leaR8[] = {0x4C, 0x8D, 0x05};
    auto* base = (uint8_t*)g_moduleBase;
    auto* nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    uint8_t* end = base + nt->OptionalHeader.SizeOfImage - 7;
    for (uint8_t* cur = base + 0x10; cur <= end; ++cur) {
        if (RipAt(cur, leaRdx, 3, 7) != str) continue;
        if (auto* g = RipAt(cur - 0xF, leaR8, 3, 7)) return (int32_t*)g;
    }
    return nullptr;
}

// Writes the forced value (or restores DE's) to every int the cvar exposes: both thread copies for TConsoleVariableData.
void ApplyCVar(CVar& c, bool on) {
    int32_t* v = c.holder ? *c.holder : c.ref;
    if (!v) return;
    const int n = c.holder ? 2 : 1;
    // Quality levels are read from the config each frame, so slider changes reach the cvar at once.
    const int32_t want = c.level ? std::clamp(g_cfg.*c.level, 1, 4) : c.forced;
    if (on) {
        if (!c.saved) {
            for (int i = 0; i < n; ++i) c.de[i] = v[i];
            c.saved = true;
        }
        for (int i = 0; i < n; ++i) v[i] = want;
    } else if (c.saved) {
        for (int i = 0; i < n; ++i) v[i] = c.de[i];
        c.saved = false;
    }
}

// FPostProcessSettings offsets and override bits (Engine_structs.hpp, struct FPostProcessSettings).
struct VolumeField {
    uint32_t value;   // float offset
    uint32_t ovByte;  // bOverride_* byte offset
    uint8_t bit;      // bOverride_* bit mask
    bool Config::* enable; // null: the lens flare multiplier (active when != 1)
};
const VolumeField kFields[] = {
    {0x040C, 0x0C, 1 << 5, &Config::modernAo},   // AmbientOcclusionIntensity
    {0x0414, 0x0C, 1 << 7, &Config::modernAo},   // AmbientOcclusionRadius
    {0x04FC, 0x19, 1 << 1, &Config::modernSsr},  // ScreenSpaceReflectionIntensity
    {0x0500, 0x19, 1 << 2, &Config::modernSsr},  // ScreenSpaceReflectionQuality
    {0x0504, 0x19, 1 << 3, &Config::modernSsr},  // ScreenSpaceReflectionMaxRoughness
    {0x035C, 0x0B, 1 << 4, nullptr},             // LensFlareIntensity
};

}  // namespace

void ModernInstall() {
    for (CVar& c : g_cvars) {
        c.holder = FindIntCVar(c.name);
        if (!c.holder) c.ref = FindRefCVar(c.name);
        Log(1, c.holder || c.ref ? "modern: %ls found" : "modern: %ls NOT found", c.name);
    }
}

void ModernFrame(bool on) {
    for (CVar& c : g_cvars) ApplyCVar(c, on && g_cfg.*c.enable);
}

// Settings (Lighting tab): feature toggles with their quality levels, sun strength, lens flare.
void ModernPanel() {
    ImGui::Checkbox("SSGI", &g_cfg.modernSsgi);
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_cfg.modernSsgi);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    ImGui::SliderInt("##ssgiq", &g_cfg.modernSsgiQuality, 1, 4, "quality %d");
    ImGui::EndDisabled();
    ImGui::Checkbox("GTAO", &g_cfg.modernAo);
    ImGui::Checkbox("SSR", &g_cfg.modernSsr);
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_cfg.modernSsr);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    ImGui::SliderInt("##ssrq", &g_cfg.modernSsrQuality, 1, 4, "quality %d");
    ImGui::EndDisabled();
    ImGui::Checkbox("Volumetric fog", &g_cfg.modernVolFog);
    SliderBox("Sun strength", &g_cfg.modernSun, 0.0f, 3.0f, 0.0f, 10.0f, "x%.2f");
    if (g_cfg.modernSun > 1.0f && g_cfg.filter) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Tip: with a stronger sun, turn the PS2 filter off.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Turn it off")) g_cfg.filter = false;
        ImGui::TextDisabled("The filter's tint was made for DE's sun, so it over-tints the brighter light.");
    }
    SliderBox("Lens flare", &g_cfg.modernLensFlare, 0.0f, 2.0f, 0.0f, 10.0f, "x%.2f");
}

// Live readback (Info tab): what the renderer reads (render-thread copy for TConsoleVariableData) and DE's own value.
void ModernInfo() {
    for (const CVar& c : g_cvars) {
        const int32_t* v = c.holder ? *c.holder : c.ref;
        if (!v) { ImGui::TextDisabled("%ls: not found", c.name); continue; }
        const int32_t live = v[c.holder ? 1 : 0];
        if (c.saved) ImGui::TextDisabled("%ls = %d (DE %d)", c.name, live, c.de[c.holder ? 1 : 0]);
        else ImGui::TextDisabled("%ls = %d (DE's)", c.name, live);
    }
}

void ModernVolume(uint8_t* settings, const uint8_t* deSnapshot, bool on) {
    // UE units: AO intensity 0..1, SSR intensity/quality are percentages. Lens flare scales DE's own value.
    // ponytail: AO intensity/radius and SSR max roughness are fixed; add sliders if they need tuning.
    float deLens;
    memcpy(&deLens, deSnapshot + 0x035C, sizeof(float));
    const float vals[] = { 0.6f, 200.0f, 100.0f, 25.0f * g_cfg.modernSsrQuality, 0.6f, deLens * g_cfg.modernLensFlare };
    for (size_t i = 0; i < sizeof(kFields) / sizeof(kFields[0]); ++i) {
        const VolumeField& fld = kFields[i];
        if (on && (fld.enable ? g_cfg.*fld.enable : g_cfg.modernLensFlare != 1.0f)) {
            memcpy(settings + fld.value, &vals[i], sizeof(float));
            settings[fld.ovByte] |= fld.bit;
        } else {
            memcpy(settings + fld.value, deSnapshot + fld.value, sizeof(float));
            settings[fld.ovByte] = (uint8_t)((settings[fld.ovByte] & ~fld.bit) | (deSnapshot[fld.ovByte] & fld.bit));
        }
    }
}
