// Real-binary check for SkyGfxDE: maps SanAndreas.exe, runs the plugin's own Install() (all look and tool
// signatures, code-layout checks, timecyc table verification, MinHook installation) and a timecyc.dat load
// into the mapped tables. Linked with core.cpp + tools.cpp (not dllmain.cpp / overlay.cpp).
// Usage: offline_check.exe <SanAndreas.exe> <timecyc.dat> <result file>
// The result file lists every resolved address and a table checksum; two runs on the same inputs give the same file.
#include "../src/skygfx.h"
#include <vector>
#include "../minhook/MinHook.h"

volatile bool g_menuOpen = false; // overlay.cpp is not linked

static FILE* g_out = nullptr;
static int g_failures = 0;
static void Check(bool ok, const char* what) {
    fprintf(g_out, "%s %s\n", ok ? "ok  " : "FAIL", what);
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

static uint8_t* MapImage(const char* path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return nullptr;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> file(size);
    fread(file.data(), 1, size, f);
    fclose(f);
    auto* nt = (IMAGE_NT_HEADERS64*)(file.data() + ((IMAGE_DOS_HEADER*)file.data())->e_lfanew);
    auto* img = (uint8_t*)VirtualAlloc(nullptr, nt->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!img) return nullptr;
    memcpy(img, file.data(), nt->OptionalHeader.SizeOfHeaders);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        memcpy(img + sec->VirtualAddress, file.data() + sec->PointerToRawData,
               min(sec->SizeOfRawData, sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData));
    return img;
}

static uint32_t TableChecksum(const uint8_t* img) {
    uint32_t h = 2166136261u;
    for (const TcCol& c : g_timecycCols) {
        if (c.kind == K_SKIP) continue;
        const size_t n = kHours * kWeathers * (c.kind == K_I16 ? 2 : 1);
        for (size_t i = 0; i < n; ++i) h = (h ^ img[c.rva + i]) * 16777619u;
    }
    return h;
}

// Expected tool addresses (IDA of the current build), checked against what InstallTools resolved.
static const struct { const char* name; uint32_t rva; } kExpected[] = {
    { "CWeather::Update", 0x12903B0 }, { "CClock::SetGameClock", 0x112B980 }, { "CWeather::FindWeatherTypesList", 0x1291330 },
    { "FindPlayerEntity", 0x116EE70 }, { "CCamera::Process", 0x111B2E0 }, { "CClock::Update tick", 0x1146764 },
    { "CTimeCycle::Update", 0x1182230 }, { "Minutes", 0x521270F }, { "Seconds", 0x522A584 }, { "NewWeatherType", 0x52FFFF0 },
    { "ForcedWeatherType", 0x5300018 }, { "OldWeatherType", 0x5300000 }, { "InterpolationValue", 0x52FFFE8 },
    { "TimeInMilliseconds", 0x52397F8 }, { "Hours", 0x521270B }, { "LastClockTick", 0x522A58C }, { "WeatherRegion", 0x5300048 },
    { "MsPerGameMinute", 0x522AD00 }, { "TheCamera.m_matrix", 0x53E13F8 },
    { "CWorld::PlayerInFocus", 0x521E79A }, { "CWorld::Players", 0x53EA730 },
    { "TimeCycle fog reduction", 0x11839DA }, { "CCutsceneMgr::ms_running", 0x5729449 },
    { "CWeather::Rain", 0x531A1DC }, { "CWeather::UnderWaterness", 0x572AEE0 }, { "CGame::currArea", 0x572A450 },
    { "CCullZones flags (player)", 0x5313544 }, { "CCullZones flags (camera)", 0x5313548 },
    { "water_splash_big FX", 0xAE7C60 }, { "water_splash FX", 0xAE7E60 }, { "water_splsh_sml FX", 0xAE8020 },
    { "EntryExit state reset", 0x1085284 }, { "CEntryExitManager::ms_exitEnterState", 0x51B4A28 },
    { "hydrant/fountain audio event", 0x1005E30 }, { "FxSystem_c::AddParticle", 0xAEFA60 },
    { "Fx_c::prt_boatsplash", 0x5379778 }, { "Fx_c::prt_wake", 0x53797F8 }, { "Fx_c::prt_watersplash", 0x5379800 },
    { "CCamera LOD multiplier store", 0x111CC5D }, { "TheCamera.m_fLODDistMultiplier", 0x53E14E4 },
};

int main(int argc, char** argv) {
    if (argc < 4) { printf("usage: offline_check <SanAndreas.exe> <timecyc.dat> <result file>\n"); return 2; }
    if (fopen_s(&g_out, argv[3], "w") != 0 || !g_out) return 2;
    g_cfg.logLevel = 2;
    g_logFile = stdout;
    uint8_t* img = MapImage(argv[1]);
    Check(img != nullptr, "exe mapped");
    if (!img) return 1;
    g_moduleBase = img;
    Check(Install(), "Install(): look + tools signatures, code layout, hooks");
    fprintf(g_out, "m_CurrentColours rva=0x%llX\n", (unsigned long long)(g_curColours - img));
    fprintf(g_out, "engine singleton rva=0x%llX\n", (unsigned long long)((uint8_t*)g_singleton - img));
    fprintf(g_out, "classic flag rva=0x%llX\n", (unsigned long long)(g_classicFlag - img));
    fprintf(g_out, "GObjects rva=0x%llX\n", (unsigned long long)(g_objects - img));
    Check(g_curColours == img + 0x5067020, "m_CurrentColours = CTimeCycle::Update's lea rdx target (0x145067020)");
    Check((uint8_t*)g_singleton == img + 0x5724750, "engine singleton = qword_145724750");
    Check(g_classicFlag == img + 0x5024151, "Classic Atmosphere flag = byte_145024151");
    Check(g_objects == img + 0x5086380, "GObjects = 0x145086380");
    Check(g_timecycTableOk, "timecyc table RVAs all read by CColourSet::CColourSet");
    fprintf(g_out, "FNamePool rva=0x%llX\n", (unsigned long long)(g_namePool - img));
    Check(g_namePool == img + 0x570CDC0, "FNamePool = stru_14570CDC0 (lea rdx in the name-entry accessor)");
    extern int32_t** g_cloudCVar;
    Check((const uint8_t*)g_cloudCVar == img + 0x56D90A8, "r.VolumetricCloud data = qword_1456D90A8 (its TAutoConsoleVariable registration)");
    extern int32_t *g_drawVar, *g_shadowVar;
    Check((const uint8_t*)g_drawVar == img + 0x50242A8, "gta.streetlightdistance = dword_1450242A8 (its RegisterConsoleVariableRef)");
    Check((const uint8_t*)g_shadowVar == img + 0x5725260, "gta.streetlight.shadowdistance = dword_145725260");
    extern void (*g_SkyEval)(void*, float, float, void*);
    extern const int32_t* g_skyRemap;
    Check((const uint8_t*)g_SkyEval == img + 0xBAE000, "sky curve evaluator = sub_140BAE000 (CColourSet::CColourSet+0x41D)");
    Check((const uint8_t*)g_skyRemap == img + 0x4222DC0, "weather -> sky curve set = dword_144222DC0 (CColourSet::CColourSet+0x3EF)");

    // Tool addresses: every one resolved and equal to the IDA value.
    char buf[8192] = {};
    FILE* mem = nullptr;
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    strcat_s(tmp, "skygfx_anchors.txt");
    if (fopen_s(&mem, tmp, "w+") == 0 && mem) {
        ToolsWriteAnchors(mem);
        rewind(mem);
        buf[fread(buf, 1, sizeof(buf) - 1, mem)] = '\0';
        fclose(mem);
        DeleteFileA(tmp);
    }
    fputs(buf, g_out);
    for (const auto& e : kExpected) {
        char want[96], msg[128];
        snprintf(want, sizeof(want), "%-32s rva=0x%X\n", e.name, e.rva);
        snprintf(msg, sizeof(msg), "tool anchor %s at 0x%X", e.name, e.rva);
        Check(strstr(buf, want) != nullptr, msg);
    }

    // Load a timecyc.dat into the mapped tables and spot-check against the file's first data line (weather 0, hour 0).
    Check(LoadTimecycFile(argv[2]), "timecyc.dat loaded into the tables");
    fprintf(g_out, "table checksum=0x%08X\n", TableChecksum(img));
    FILE* f = nullptr;
    char line[1024] = {};
    if (fopen_s(&f, argv[2], "rb") == 0 && f) {
        while (fgets(line, sizeof(line), f)) {
            const char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p && *p != '/' && *p != '\r' && *p != '\n') break;
        }
        fclose(f);
        int v[21] = {};
        sscanf_s(line, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4],
                 &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16], &v[17], &v[18], &v[19], &v[20]);
        Check(img[0x523A4C0] == (uint8_t)v[0] && img[0x523A640] == (uint8_t)v[1] && img[0x523A580] == (uint8_t)v[2],
              "midnight/weather 0 ambient matches the file");
        Check(img[0x523A340] == (uint8_t)v[9] && img[0x523A100] == (uint8_t)v[12], "sky top/bottom red match the file");
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    fprintf(g_out, g_failures ? "%d FAILED\n" : "ALL PASSED\n", g_failures);
    fclose(g_out);
    printf(g_failures ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
