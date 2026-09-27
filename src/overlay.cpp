// ImGui overlay for DE's D3D11 renderer (the game only loads d3d11/dxgi).
// Present runs on UE's render thread, the window procedure on the game (main) thread: ImGui state is
// shared under g_imguiLock. While the menu or freecam/noclip is active, key-down/char/mouse messages and
// raw mouse input are kept from the game; key-up and button-up always reach it, so no key stays stuck.
#include "skygfx.h"
#include <d3d11.h>
#include <dxgi.h>
#include "../imgui/imgui.h"
#include "../imgui/imgui_impl_dx11.h"
#include "../imgui/imgui_impl_win32.h"
#include "../minhook/MinHook.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

volatile bool g_menuOpen = false;

typedef HRESULT(STDMETHODCALLTYPE* Present_Fn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffers_Fn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef BOOL(WINAPI* SetCursorPos_Fn)(int, int);
typedef BOOL(WINAPI* GetCursorPos_Fn)(POINT*);
typedef BOOL(WINAPI* ClipCursor_Fn)(const RECT*);
typedef BOOL(WINAPI* ReleaseCapture_Fn)();
typedef SHORT(WINAPI* GetAsyncKeyState_Fn)(int);

static Present_Fn o_Present = nullptr;
static ResizeBuffers_Fn o_ResizeBuffers = nullptr;
static SetCursorPos_Fn o_SetCursorPos = nullptr;
static GetCursorPos_Fn o_GetCursorPos = nullptr;
static ClipCursor_Fn o_ClipCursor = nullptr;
static ReleaseCapture_Fn o_ReleaseCapture = nullptr;
static GetAsyncKeyState_Fn o_GetAsyncKeyState = nullptr;

// While the menu is open the game sees a frozen cursor: GetCursorPos returns g_gameCursor, its SetCursorPos
// recentering only updates g_gameCursor, its ClipCursor rect is remembered in g_gameClip. Closing the menu puts
// the real cursor and clip back, so the game resumes with no delta. ImGui (render thread, and the WndProc handler
// at t_imguiDepth > 0) always uses the real functions.
static DWORD g_renderThread = 0;
static thread_local int t_imguiDepth = 0;
static POINT g_gameCursor{};
static RECT g_gameClip{};
static volatile bool g_gameClipped = false;
static bool g_imguiOwnsCapture = false; // game thread only

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static HWND g_hwnd = nullptr;
static WNDPROC o_WndProc = nullptr;
static bool g_ready = false;
static bool g_notD3D11 = false;
static SRWLOCK g_imguiLock = SRWLOCK_INIT;
static char g_imguiIni[MAX_PATH];

static void CreateRtv() {
    ID3D11Texture2D* back = nullptr;
    if (g_swapChain && SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        if (FAILED(g_device->CreateRenderTargetView(back, nullptr, &g_rtv))) g_rtv = nullptr;
        back->Release();
    }
}

static void ReleaseRtv() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

// ---------------------------------------------------------------- input
static LRESULT CALLBACK Hooked_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const bool menu = g_menuOpen;
    const bool tool = ToolsWantCapture();
    const bool capture = menu || tool;
    if (msg == WM_INPUT && capture) {
        if (!menu && tool) {
            RAWINPUT ri;
            UINT size = sizeof(ri);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                ToolsAddMouseDelta(ri.data.mouse.lLastX, ri.data.mouse.lLastY);
                if (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) ToolsAddWheel((short)ri.data.mouse.usButtonData / WHEEL_DELTA);
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp); // keeps raw input from the game, cleans up the buffer
    }
    if (menu && g_ready) {
        // ImGui's handler re-enters this procedure synchronously (ReleaseCapture sends WM_CAPTURECHANGED) and
        // SRW locks are not recursive: nested calls on this thread reuse the lock it already holds.
        if (t_imguiDepth++ == 0) AcquireSRWLockExclusive(&g_imguiLock);
        const HWND captureBefore = GetCapture();
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        if (!captureBefore && GetCapture() == hwnd) g_imguiOwnsCapture = true; // ImGui took a free capture
        if (--t_imguiDepth == 0) ReleaseSRWLockExclusive(&g_imguiLock);
    }
    // Button-ups are swallowed only when their down was: an unmatched up reaching UE ends a capture that UE's viewport
    // never started and spins the camera; ups of buttons held before the menu opened still reach the game.
    static unsigned swallowedDown = 0; // bit per button (game thread)
    auto button = [](UINT m, WPARAM w) -> unsigned {
        switch (m) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP: return 1;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP: return 2;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_MBUTTONUP: return 4;
        case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP: return GET_XBUTTON_WPARAM(w) == XBUTTON1 ? 8 : 16;
        default: return 0;
        }
    };
    if (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP || msg == WM_XBUTTONUP) {
        const unsigned b = button(msg, wp);
        if (swallowedDown & b) { swallowedDown &= ~b; return 0; }
    }
    if (capture) {
        switch (msg) {
        case WM_SYSKEYDOWN:
            if (wp == VK_F4) break; // Alt+F4 still works
            return 0;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
            swallowedDown |= button(msg, wp);
            return 0;
        case WM_KEYDOWN: case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR:
        case WM_MOUSEMOVE: case WM_MOUSEHWHEEL:
            return 0;
        case WM_MOUSEWHEEL:
            if (!menu) ToolsAddWheel(GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
            return 0;
        case WM_SETCURSOR:
            if (menu) { SetCursor(nullptr); return TRUE; } // ImGui draws its own cursor
            break;
        default: break;
        }
    }
    return CallWindowProcW(o_WndProc, hwnd, msg, wp, lp);
}

static bool OurCall() { return t_imguiDepth > 0 || GetCurrentThreadId() == g_renderThread; }

static BOOL WINAPI Hooked_SetCursorPos(int x, int y) {
    if (g_menuOpen && !OurCall()) { g_gameCursor = POINT{ x, y }; return TRUE; }
    return o_SetCursorPos(x, y);
}

static BOOL WINAPI Hooked_GetCursorPos(POINT* p) {
    if (g_menuOpen && p && !OurCall()) { *p = g_gameCursor; return TRUE; }
    return o_GetCursorPos(p);
}

static BOOL WINAPI Hooked_ClipCursor(const RECT* r) {
    if (!OurCall()) {
        if (r) g_gameClip = *r;
        g_gameClipped = r != nullptr;
    }
    if (g_menuOpen) return o_ClipCursor(nullptr);
    return o_ClipCursor(r);
}

// ImGui releases the capture on mouse-up even when it was the game's (UE captures the viewport): keep it.
static BOOL WINAPI Hooked_ReleaseCapture() {
    if (t_imguiDepth > 0) {
        if (!g_imguiOwnsCapture) return TRUE;
        g_imguiOwnsCapture = false;
    }
    return o_ReleaseCapture();
}

static SHORT WINAPI Hooked_GetAsyncKeyState(int vk) {
    const bool mouseButton = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
    if (mouseButton && g_menuOpen && !OurCall()) return 0;
    return o_GetAsyncKeyState(vk);
}

// Render thread, under g_imguiLock.
static void SetMenuOpen(bool open) {
    if (open == g_menuOpen) return;
    if (open) {
        o_GetCursorPos(&g_gameCursor); // what the game sees until the menu closes
        g_menuOpen = true;
        o_ClipCursor(nullptr);
    } else {
        g_menuOpen = false;
        if (g_gameClipped) { const RECT r = g_gameClip; o_ClipCursor(&r); }
        o_SetCursorPos(g_gameCursor.x, g_gameCursor.y);
    }
}

// ---------------------------------------------------------------- frame
static void InitImGui(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_device))) {
        g_notD3D11 = true;
        Log(1, "overlay: swap chain is not D3D11, overlay disabled");
        return;
    }
    g_device->GetImmediateContext(&g_context);
    g_swapChain = sc;
    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    snprintf(g_imguiIni, sizeof(g_imguiIni), "%sSkyGfxDE_imgui.ini", g_dir); // not the shared imgui.ini
    io.IniFilename = g_imguiIni;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f; st.FrameRounding = 4.0f; st.GrabRounding = 4.0f;
    const float scale = desc.BufferDesc.Height >= 1400 ? 1.5f : desc.BufferDesc.Height >= 1000 ? 1.25f : 1.0f;
    st.ScaleAllSizes(scale);
    io.FontGlobalScale = scale;
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    CreateRtv();
    o_WndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)&Hooked_WndProc);
    g_ready = true;
    Log(1, "overlay: ImGui ready (window %p, %ux%u, format %d)", g_hwnd, desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format);
}

static void DrawStatus() {
    if (!ToolsWantCapture() || g_menuOpen) return;
    ImGui::GetForegroundDrawList()->AddText(ImVec2(20, 20), IM_COL32(255, 220, 80, 255),
        "SkyGfxDE: WASD move, Space/C up/down, mouse look, Shift fast, Alt slow, wheel speed");
}

static void DrawMenu() {
    ImGui::SetNextWindowSize(ImVec2(560, 680), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("SkyGfxDE", &open)) {
        ImGui::TextDisabled("menu %s | look %s | reload %s | freecam %s | noclip %s", g_cfg.keyMenu.text, g_cfg.keyToggle.text,
                            g_cfg.keyReload.text, g_cfg.keyFreecam.text, g_cfg.keyNoclip.text);
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("World")) { ToolsPanel(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Look")) { LookPanel(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    if (!open) SetMenuOpen(false);
}

static void RenderFrame(IDXGISwapChain* sc) {
    if (g_notD3D11) return;
    if (!g_ready) {
        InitImGui(sc);
        if (!g_ready) return;
    }
    if (sc != g_swapChain) return;
    g_renderThread = GetCurrentThreadId();
    static bool menuKeyDown = false;
    const bool menuKey = HotkeyDown(g_cfg.keyMenu);
    AcquireSRWLockExclusive(&g_imguiLock);
    if (menuKey && !menuKeyDown) SetMenuOpen(!g_menuOpen);
    menuKeyDown = menuKey;
    ImGui::GetIO().MouseDrawCursor = g_menuOpen;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (g_menuOpen) DrawMenu();
    DrawStatus();
    ImGui::Render();
    ReleaseSRWLockExclusive(&g_imguiLock);

    ImDrawData* dd = ImGui::GetDrawData();
    if (dd && dd->CmdListsCount > 0 && g_rtv) {
        ID3D11RenderTargetView* prevRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* prevDsv = nullptr;
        g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRtv, &prevDsv);
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(dd);
        g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRtv, prevDsv);
        for (auto* r : prevRtv) if (r) r->Release();
        if (prevDsv) prevDsv->Release();
    }
}

static HRESULT STDMETHODCALLTYPE Hooked_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        __try {
            RenderFrame(sc);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static int logged = 0;
            if (logged++ < 5) Log(1, "overlay: exception 0x%08lX in Present", GetExceptionCode());
        }
    }
    return o_Present(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE Hooked_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl) {
    const bool ours = g_ready && sc == g_swapChain;
    if (ours) { ReleaseRtv(); ImGui_ImplDX11_InvalidateDeviceObjects(); }
    const HRESULT hr = o_ResizeBuffers(sc, n, w, h, fmt, fl);
    if (ours) { CreateRtv(); ImGui_ImplDX11_CreateDeviceObjects(); }
    return hr;
}

// ---------------------------------------------------------------- install (worker thread, not DllMain)
bool InstallOverlay() {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"SkyGfxDE_dummy", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sc = nullptr;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr)) {
        void** vt = *(void***)sc;
        void* present = vt[8];
        void* resize = vt[13];
        ok = MH_CreateHook(present, (void*)&Hooked_Present, (void**)&o_Present) == MH_OK &&
             MH_CreateHook(resize, (void*)&Hooked_ResizeBuffers, (void**)&o_ResizeBuffers) == MH_OK &&
             MH_CreateHook((void*)&SetCursorPos, (void*)&Hooked_SetCursorPos, (void**)&o_SetCursorPos) == MH_OK &&
             MH_CreateHook((void*)&GetCursorPos, (void*)&Hooked_GetCursorPos, (void**)&o_GetCursorPos) == MH_OK &&
             MH_CreateHook((void*)&ClipCursor, (void*)&Hooked_ClipCursor, (void**)&o_ClipCursor) == MH_OK &&
             MH_CreateHook((void*)&ReleaseCapture, (void*)&Hooked_ReleaseCapture, (void**)&o_ReleaseCapture) == MH_OK &&
             MH_CreateHook((void*)&GetAsyncKeyState, (void*)&Hooked_GetAsyncKeyState, (void**)&o_GetAsyncKeyState) == MH_OK &&
             MH_EnableHook(present) == MH_OK && MH_EnableHook(resize) == MH_OK &&
             MH_EnableHook((void*)&SetCursorPos) == MH_OK && MH_EnableHook((void*)&GetCursorPos) == MH_OK &&
             MH_EnableHook((void*)&ClipCursor) == MH_OK && MH_EnableHook((void*)&ReleaseCapture) == MH_OK &&
             MH_EnableHook((void*)&GetAsyncKeyState) == MH_OK;
        Log(1, "overlay: Present=%p ResizeBuffers=%p hooks %s", present, resize, ok ? "installed" : "FAILED");
        sc->Release(); ctx->Release(); dev->Release();
    } else {
        Log(1, "overlay: dummy D3D11 device failed (0x%08lX), no overlay", hr);
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}
