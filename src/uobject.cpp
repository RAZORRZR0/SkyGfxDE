// FName and UObject helpers: FNamePool lookups, class and name of a GObjects object, liveness of a GObjects slot and
// the game-thread check. Used by coronas.cpp and streetlights.cpp. The pool and GObjects walks are unsynchronised,
// so call them from the game thread.
#include "skygfx.h"
#include <string.h>

uint8_t* g_namePool = nullptr; // FNamePool: Blocks[] at +0x10, CurrentBlock +8, CurrentByteCursor +0xC

// ---------------------------------------------------------------- FName
static const uint8_t* NameEntry(int32_t idx) {
    const uint32_t block = (uint32_t)idx >> 16;
    if (idx < 0 || block > *(uint32_t*)(g_namePool + 8)) return nullptr;
    const uint8_t* b = ((uint8_t**)(g_namePool + 0x10))[block];
    return b ? b + 2 * (idx & 0xFFFF) : nullptr;
}

bool NameIs(int32_t idx, const char* s) {
    const uint8_t* e = NameEntry(idx);
    if (!e) return false;
    const uint16_t h = *(const uint16_t*)e; // bIsWide:1, probe hash:5, Len:10
    const size_t len = h >> 6;
    return !(h & 1) && len == strlen(s) && memcmp(e + 2, s, len) == 0;
}

// Comparison index of an ANSI name already in the pool, or -1.
int32_t FindName(const char* s) {
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

int32_t NameOf(const uint8_t* obj) { return obj ? *(const int32_t*)(obj + UO::Name) : -1; }
int32_t ClassOf(const uint8_t* obj) { return NameOf(*(uint8_t* const*)(obj + UO::Class)); }

// ---------------------------------------------------------------- per frame
// UE's game thread owns the game window. FindWindow("UnrealWindow") missed DE's window, so take this process's
// visible top-level window instead.
bool OnGameThread() {
    static DWORD tid = 0;
    if (!tid) {
        HWND w = nullptr;
        EnumWindows([](HWND h, LPARAM p) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
            *(HWND*)p = h;
            return FALSE;
        }, (LPARAM)&w);
        if (!w) return false;
        tid = GetWindowThreadProcessId(w, nullptr);
        Log(1, "game thread %lu, this thread %lu", tid, GetCurrentThreadId());
    }
    return GetCurrentThreadId() == tid;
}

// GObjects slot i still holds obj, not PendingKill/Unreachable.
bool LiveAt(int32_t i, const uint8_t* obj) {
    uint8_t* item = ObjectItem(i);
    return item && *(uint8_t**)item == obj && !(*(int32_t*)(item + 8) & ((1 << 29) | (1 << 28)));
}

bool InstallUObject() {
    // FNamePool::Resolve-style accessor: lea rdx, NamePoolData at +0x1F.
    uint8_t* fn = FindUnique("40 53 48 83 EC 20 8B D9 0F B7 C1 C1 EB 10 80 3D ?? ?? ?? ?? 00 89 5C 24 38 89 44 24 3C 74 ?? 48 8D 15");
    static const uint8_t leaRdx[] = { 0x48, 0x8D, 0x15 };
    g_namePool = fn ? RipAt(fn + 0x1F, leaRdx, 3, 7) : nullptr;
    const bool ok = g_namePool && g_objects;
    Log(1, ok ? "uobject: FNamePool %p" : "uobject: FNamePool not found", g_namePool);
    return ok;
}
