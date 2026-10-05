// SkyGfxDE entry point. Everything else: core.cpp (look), overlay.cpp (ImGui), tools.cpp (debug tools).
#include "skygfx.h"
#include <stdio.h>
#include <string.h>
#include "../minhook/MinHook.h"

static DWORD WINAPI OverlayThread(LPVOID) {
    InstallOverlay();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        GetModuleFileNameA(hModule, g_dir, MAX_PATH);
        if (char* slash = strrchr(g_dir, '\\')) slash[1] = '\0';
        snprintf(g_iniPath, sizeof(g_iniPath), "%sSkyGfxDE.ini", g_dir);
        ReadIni();
        OpenLog();
        char exe[MAX_PATH] = "?";
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        Log(1, "SkyGfxDE 1.2.4 | exe %s | ini %s", exe, g_iniPath);
        Log(1, "keys: menu=%s look=%s reload=%s freecam=%s noclip=%s", g_cfg.keyMenu.text, g_cfg.keyToggle.text,
            g_cfg.keyReload.text, g_cfg.keyFreecam.text, g_cfg.keyNoclip.text);
        if (!g_cfg.enabled) { Log(1, "disabled in ini"); return TRUE; }
        if (Install()) {
            ApplyTimecycFile(); // in case the game already loaded its timecyc
            if (HANDLE h = CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr)) CloseHandle(h);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    return TRUE;
}
