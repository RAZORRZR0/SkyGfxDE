// Real-binary check for SkyGfxDE: maps SanAndreas.exe, runs the plugin's Install() (all signatures, code-layout
// checks, timecyc table verification, MinHook installation) and a timecyc.dat load into the mapped tables.
// Usage: offline_check.exe <SanAndreas.exe> <timecyc.dat> <result file>
// The result file lists every resolved address and table checksum; two runs on the same inputs give the same file.
#define SKYGFX_TEST
#include "../src/dllmain.cpp"
#include <vector>

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
    for (const TcCol& c : kTimecycCols) {
        if (c.kind == K_SKIP) continue;
        const size_t n = kHours * kWeathers * (c.kind == K_I16 ? 2 : 1);
        for (size_t i = 0; i < n; ++i) h = (h ^ img[c.rva + i]) * 16777619u;
    }
    return h;
}

int main(int argc, char** argv) {
    if (argc < 4) { printf("usage: offline_check <SanAndreas.exe> <timecyc.dat> <result file>\n"); return 2; }
    if (fopen_s(&g_out, argv[3], "w") != 0 || !g_out) return 2;
    g_cfg.logLevel = 2;
    g_logFile = stdout;
    uint8_t* img = MapImage(argv[1]);
    Check(img != nullptr, "exe mapped");
    if (!img) return 1;
    g_moduleBase = img;
    Check(Install(), "Install(): signatures, code layout, hooks");
    fprintf(g_out, "m_CurrentColours rva=0x%llX\n", (unsigned long long)(g_curColours - img));
    fprintf(g_out, "engine singleton rva=0x%llX\n", (unsigned long long)((uint8_t*)g_singleton - img));
    fprintf(g_out, "classic flag rva=0x%llX\n", (unsigned long long)(g_classicFlag - img));
    fprintf(g_out, "GObjects rva=0x%llX\n", (unsigned long long)(g_objects - img));
    Check(g_curColours == img + 0x5067020, "m_CurrentColours = CTimeCycle::Update's lea rdx target (0x145067020)");
    Check((uint8_t*)g_singleton == img + 0x5724750, "engine singleton = qword_145724750");
    Check(g_classicFlag == img + 0x5024151, "Classic Atmosphere flag = byte_145024151");
    Check(g_objects == img + 0x5086380, "GObjects = 0x145086380");
    Check(g_timecycTableOk, "timecyc table RVAs all read by CColourSet::CColourSet");

    // Load a timecyc.dat into the mapped tables and spot-check against the file's first line (weather 0, hour 0).
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
