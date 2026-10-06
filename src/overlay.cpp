// ImGui overlay and post effects on DE's backbuffer. D3D11 (default): on DE's own device. D3D12 (-dx12): on a D3D11On12
// device created on DE's direct queue (captured when DE creates its swap chain), with the swap-chain buffers wrapped as
// D3D11 textures, so the same D3D11 code (ImGui DX11 backend, postfx.cpp) draws on both.
// Present runs on UE's render thread, the window procedure on the game (main) thread: ImGui state is
// shared under g_imguiLock. While the menu or freecam/noclip is active, key-down/char/mouse messages and
// raw mouse input are kept from the game; key-up and button-up always reach it, so no key stays stuck.
#include "skygfx.h"
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <dxgi1_4.h>
#include <math.h>
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
static ID3D11Texture2D* g_backRes = nullptr; // backbuffer texture (D3D11: not referenced, lives with the swap chain)
// D3D12: DE's direct queue, our D3D11On12 device on it, and every swap-chain buffer wrapped (picked per frame)
static ID3D12CommandQueue* g_queue = nullptr;
static ID3D11On12Device* g_on12 = nullptr;
static IDXGISwapChain3* g_sc3 = nullptr;
static ID3D11Texture2D* g_wrapped[8]{};
static ID3D11RenderTargetView* g_wrapRtv[8]{};
static bool g_fxDrawn = false;             // post effects already drawn this frame (render thread)
// D3D12 under the HUD: DE's swap-chain buffers (not referenced, noted at creation/resize so their RTVs can be recognised),
// the RTV handles DE creates for them, and the scene copy recorded into DE's command list at the HUD bind.
static ID3D12Resource* g_bufs[8]{};
struct BbRtv { SIZE_T ptr; int buf; };
static BbRtv g_bbRtv[16]{};                // ponytail: unsynchronised table; DE creates backbuffer RTVs on one thread
static ID3D12Resource* g_scene12 = nullptr;
static ID3D11Texture2D* g_scene11 = nullptr; // g_scene12 wrapped (resting state COPY_DEST)
static volatile LONG g_sceneBuf = -1;      // buffer whose scene was copied this frame, -1 none

static void CreateRtv() {
    if (g_on12) {
        DXGI_SWAP_CHAIN_DESC d{};
        g_swapChain->GetDesc(&d);
        const D3D11_RESOURCE_FLAGS f{ D3D11_BIND_RENDER_TARGET };
        for (UINT i = 0; i < d.BufferCount && i < 8; ++i) {
            ID3D12Resource* r = nullptr;
            if (FAILED(g_swapChain->GetBuffer(i, IID_PPV_ARGS(&r)))) continue;
            if (SUCCEEDED(g_on12->CreateWrappedResource(r, &f, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
                                                        IID_PPV_ARGS(&g_wrapped[i]))))
                g_device->CreateRenderTargetView(g_wrapped[i], nullptr, &g_wrapRtv[i]);
            r->Release();
        }
        // the scene copy: same size and format as the buffers
        ID3D12Resource* b0 = nullptr;
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&b0))) && SUCCEEDED(g_queue->GetDevice(IID_PPV_ARGS(&dev)))) {
            D3D12_RESOURCE_DESC rd = b0->GetDesc();
            rd.Flags = D3D12_RESOURCE_FLAG_NONE;
            const D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
            const D3D11_RESOURCE_FLAGS sf{ D3D11_BIND_SHADER_RESOURCE };
            if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&g_scene12))) &&
                FAILED(g_on12->CreateWrappedResource(g_scene12, &sf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     IID_PPV_ARGS(&g_scene11)))) {
                g_scene12->Release(); g_scene12 = nullptr;
            }
        }
        if (b0) b0->Release();
        if (dev) dev->Release();
        if (!g_scene12) Log(1, "overlay: D3D12 scene copy not created, post effects draw over the HUD");
        return;
    }
    ID3D11Texture2D* back = nullptr;
    if (g_swapChain && SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        if (FAILED(g_device->CreateRenderTargetView(back, nullptr, &g_rtv))) g_rtv = nullptr;
        g_backRes = back;
        back->Release();
    }
}

static void ReleaseRtv() {
    g_backRes = nullptr;
    if (g_on12) { // the wrapped buffers hold the D3D12 buffers: all references must be gone before ResizeBuffers
        g_rtv = nullptr;
        for (auto& v : g_wrapRtv) if (v) { v->Release(); v = nullptr; }
        for (auto& t : g_wrapped) if (t) { t->Release(); t = nullptr; }
        ID3D12Resource* s12 = g_scene12;
        g_scene12 = nullptr;
        if (g_scene11) { g_scene11->Release(); g_scene11 = nullptr; }
        if (s12) s12->Release();
        g_context->ClearState();
        g_context->Flush();
        return;
    }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

// ---------------------------------------------------------------- input
static LRESULT CALLBACK Hooked_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const bool menu = g_menuOpen;
    const bool tool = ToolsWantCapture();
    const bool capture = menu || tool;
    // Freecam/noclip keep the keyboard (WASD flies) and mouse buttons/wheel away from the game, but not mouse
    // movement: the game's camera still follows the mouse and gives the look direction.
    if (msg == WM_INPUT && menu) return DefWindowProcW(hwnd, msg, wp, lp); // keeps raw input from the game, cleans up the buffer
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
        case WM_MOUSEHWHEEL:
            return 0;
        case WM_MOUSEMOVE:
            if (menu) return 0;
            break;
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
        ID3D12Device* d12 = nullptr;
        const bool ok = g_queue && SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d12))) && SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&g_sc3))) &&
                        SUCCEEDED(D3D11On12CreateDevice(d12, 0, nullptr, 0, (IUnknown* const*)&g_queue, 1, 0, &g_device, &g_context, nullptr)) &&
                        SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&g_on12)));
        if (d12) d12->Release();
        if (!ok) {
            g_notD3D11 = true;
            Log(1, "overlay: D3D12 swap chain but no D3D11On12 device (queue %p), overlay disabled", g_queue);
            return;
        }
        Log(1, "overlay: D3D12 swap chain, drawing through D3D11On12 on DE's queue %p", g_queue);
    } else {
        g_device->GetImmediateContext(&g_context);
    }
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

    // D3D12: draw into the current buffer, wrapped; the release + flush submit our D3D11 work to DE's queue before Present
    ID3D11Resource* wrapped[2] = {};
    UINT nWrapped = 0;
    bool underHud = false;
    if (g_on12) {
        const UINT i = g_sc3->GetCurrentBackBufferIndex();
        if (i >= 8 || !g_wrapped[i]) return;
        g_rtv = g_wrapRtv[i];
        g_backRes = g_wrapped[i];
        wrapped[nWrapped++] = g_wrapped[i];
        underHud = g_scene11 && g_sceneBuf == (LONG)i; // DE's HUD bind was reached: its scene copy is this buffer's
        if (underHud) wrapped[nWrapped++] = g_scene11;
        g_on12->AcquireWrappedResources(wrapped, nWrapped);
    }
    if (underHud) {
        static bool logged = false;
        if (!logged) { logged = true; Log(1, "overlay: D3D12 post effects drawn under the HUD (scene copied at bind %d)", g_cfg.speedFxHudBind); }
        PostFxDrawUnderHud(g_device, g_context, g_backRes, g_rtv, g_scene11);
    }
    // HudBind not reached (or 0): at Present, under ImGui. D3D12 menus/loading screens never reach the HUD bind (no 3D
    // scene under them): no effects there, as on D3D11 where the menu is drawn over them. Cutscenes reach it only on
    // frames with subtitles (no HUD otherwise), so they always take this path when it isn't reached.
    else if (!g_fxDrawn && (!g_on12 || g_cfg.speedFxHudBind == 0 || (g_cutsceneRunning && *g_cutsceneRunning)))
        PostFxDraw(g_device, g_context, g_backRes, g_rtv);
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
    if (nWrapped) {
        g_on12->ReleaseWrappedResources(wrapped, nWrapped);
        g_context->Flush();
    }
}

// ---------------------------------------------------------------- post effects before the HUD
// UE binds the backbuffer once for the final scene pass and again for Slate/UMG (the HUD). The post effects run just
// before backbuffer bind number [SpeedFX] HudBind of the frame, so the scene is complete and the HUD not drawn yet.
// The binds per frame are logged once (600 frames) to calibrate HudBind; 0 = draw at Present (HUD blurred).
typedef void(STDMETHODCALLTYPE* OMSetRT_Fn)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* OMSetRTUav_Fn)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
                                                UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
static OMSetRT_Fn o_OMSetRenderTargets = nullptr;
static OMSetRTUav_Fn o_OMSetRenderTargetsUav = nullptr;
static thread_local bool t_ours = false; // our own binds (post effects, ImGui) are not counted
static int g_bbBinds = 0, g_bbBindsLast = 0;
static ID3D11RenderTargetView* g_lastRtv = nullptr;
static bool g_lastIsBack = false;

static void OnBind(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs) {
    if (t_ours || ctx != g_context || !g_ready || !g_backRes || n == 0 || n > 8 || !rtvs || !rtvs[0]) return;
    if (rtvs[0] != g_lastRtv) {
        ID3D11Resource* r = nullptr;
        rtvs[0]->GetResource(&r);
        g_lastRtv = rtvs[0];
        g_lastIsBack = r == g_backRes;
        if (r) r->Release();
    }
    if (!g_lastIsBack || ++g_bbBinds != g_cfg.speedFxHudBind || g_fxDrawn) return;
    t_ours = true;
    __try { PostFxDraw(g_device, g_context, g_backRes, g_rtv); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    t_ours = false;
    g_fxDrawn = true;
}

static void STDMETHODCALLTYPE Hooked_OMSetRenderTargets(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs,
                                                        ID3D11DepthStencilView* dsv) {
    OnBind(ctx, n, rtvs);
    o_OMSetRenderTargets(ctx, n, rtvs, dsv);
}

static void STDMETHODCALLTYPE Hooked_OMSetRenderTargetsUav(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs,
                                                           ID3D11DepthStencilView* dsv, UINT slot, UINT nu,
                                                           ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) OnBind(ctx, n, rtvs);
    o_OMSetRenderTargetsUav(ctx, n, rtvs, dsv, slot, nu, uavs, counts);
}

static HRESULT STDMETHODCALLTYPE Hooked_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        t_ours = true;
        __try {
            RenderFrame(sc);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static int logged = 0;
            if (logged++ < 5) Log(1, "overlay: exception 0x%08lX in Present", GetExceptionCode());
        }
        t_ours = false;
        if (sc == g_swapChain) {
            static int hist[8], frames = 0;
            ++hist[g_bbBinds < 7 ? g_bbBinds : 7];
            if (++frames == 600)
                Log(1, "speedfx: backbuffer binds per frame over 600 frames: 0:%d 1:%d 2:%d 3:%d 4:%d 5:%d 6:%d 7+:%d (HudBind=%d)",
                    hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], g_cfg.speedFxHudBind);
            g_bbBindsLast = g_bbBinds;
            g_bbBinds = 0;
            g_fxDrawn = false;
            g_sceneBuf = -1;
            g_lastRtv = nullptr;
        }
    }
    return o_Present(sc, sync, flags);
}

// D3D12: DXGI takes the direct command queue as the "device" of a D3D12 swap chain; keep DE's for D3D11On12.
// ponytail: only CreateSwapChain / CreateSwapChainForHwnd; add the other Create* if DE ever uses them.
typedef HRESULT(STDMETHODCALLTYPE* CreateSC_Fn)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* CreateSCHwnd_Fn)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef void(STDMETHODCALLTYPE* CreateRtv12_Fn)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE* OMSetRT12_Fn)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE*);
static CreateSC_Fn o_CreateSwapChain = nullptr;
static CreateSCHwnd_Fn o_CreateSwapChainForHwnd = nullptr;
static CreateRtv12_Fn o_CreateRtv12 = nullptr;
static OMSetRT12_Fn o_OMSetRT12 = nullptr;

// ID3D12Device::CreateRenderTargetView: remember the handles of views on the swap-chain buffers (a reused handle is forgotten)
static void STDMETHODCALLTYPE Hooked_CreateRtv12(ID3D12Device* d, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE h) {
    o_CreateRtv12(d, r, desc, h);
    int buf = -1, slot = -1;
    for (int i = 0; i < 8; ++i) if (r && r == g_bufs[i]) buf = i;
    for (int i = 0; i < 16; ++i) if (g_bbRtv[i].ptr == h.ptr) slot = i;
    if (slot < 0 && buf >= 0) for (int i = 0; i < 16 && slot < 0; ++i) if (!g_bbRtv[i].ptr) slot = i;
    if (slot >= 0) g_bbRtv[slot] = { buf >= 0 ? h.ptr : 0, buf };
}

// ID3D12GraphicsCommandList::OMSetRenderTargets: DE's backbuffer binds are counted as on D3D11; at bind HudBind the scene
// (complete, HUD not drawn yet) is copied into g_scene12 in DE's own command list. The bind before it drew the scene, so
// the buffer is a render target here; it is put back to that state, and no pipeline state is touched.
static void STDMETHODCALLTYPE Hooked_OMSetRT12(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts,
                                               BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    if (n && rts) {
        int buf = -1;
        for (const BbRtv& e : g_bbRtv) if (e.ptr && e.ptr == rts[0].ptr) buf = e.buf;
        ID3D12Resource* scene = g_scene12;
        if (buf >= 0 && InterlockedIncrement((volatile LONG*)&g_bbBinds) == g_cfg.speedFxHudBind && scene && g_active) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = { g_bufs[buf], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_COPY_SOURCE };
            cl->ResourceBarrier(1, &b);
            cl->CopyResource(scene, g_bufs[buf]);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            cl->ResourceBarrier(1, &b);
            g_sceneBuf = buf;
        }
    }
    o_OMSetRT12(cl, n, rts, single, ds);
}

static void NoteBuffers(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (!sc || FAILED(sc->GetDesc(&d))) return;
    for (auto& b : g_bufs) b = nullptr;
    for (auto& e : g_bbRtv) e = {};
    for (UINT i = 0; i < d.BufferCount && i < 8; ++i) {
        ID3D12Resource* r = nullptr;
        if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&r)))) { g_bufs[i] = r; r->Release(); }
    }
}

static void NoteQueue(IUnknown* dev) {
    ID3D12CommandQueue* q = nullptr;
    if (!dev || FAILED(dev->QueryInterface(IID_PPV_ARGS(&q)))) return;
    if (g_queue) g_queue->Release();
    g_queue = q;
    static bool hooked = false;
    if (hooked) return;
    hooked = true;
    // vtables: ID3D12Device::CreateRenderTargetView (20), ID3D12GraphicsCommandList::OMSetRenderTargets (46)
    ID3D12Device* d = nullptr;
    ID3D12CommandAllocator* a = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    bool ok = SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&d))) &&
              SUCCEEDED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) &&
              SUCCEEDED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&cl)));
    if (ok) {
        void* rtv = (*(void***)d)[20];
        void* om = (*(void***)cl)[46];
        ok = MH_CreateHook(rtv, (void*)&Hooked_CreateRtv12, (void**)&o_CreateRtv12) == MH_OK &&
             MH_CreateHook(om, (void*)&Hooked_OMSetRT12, (void**)&o_OMSetRT12) == MH_OK &&
             MH_EnableHook(rtv) == MH_OK && MH_EnableHook(om) == MH_OK;
    }
    if (cl) cl->Release();
    if (a) a->Release();
    if (d) d->Release();
    Log(1, ok ? "overlay: D3D12 backbuffer bind hooks ready (post effects under the HUD)"
              : "overlay: D3D12 bind hooks failed, post effects draw over the HUD");
}
static HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** sc) {
    NoteQueue(dev);
    const HRESULT hr = o_CreateSwapChain(f, dev, d, sc);
    if (SUCCEEDED(hr) && g_queue && sc) NoteBuffers(*sc);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** sc) {
    NoteQueue(dev);
    const HRESULT hr = o_CreateSwapChainForHwnd(f, dev, w, d, fs, o, sc);
    if (SUCCEEDED(hr) && g_queue && sc) NoteBuffers(*sc);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl) {
    const bool ours = g_ready && sc == g_swapChain;
    if (ours) {
        ReleaseRtv(); PostFxReleaseSized(); ImGui_ImplDX11_InvalidateDeviceObjects();
    }
    const HRESULT hr = o_ResizeBuffers(sc, n, w, h, fmt, fl);
    if (g_queue && (sc == g_swapChain || !g_swapChain)) NoteBuffers(sc); // DE creates the new buffers' RTVs next
    if (ours) { CreateRtv(); ImGui_ImplDX11_CreateDeviceObjects(); Log(2, "overlay: ResizeBuffers %ux%u -> 0x%08lX", w, h, hr); }
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
        // ID3D11DeviceContext::OMSetRenderTargets (33) / OMSetRenderTargetsAndUnorderedAccessViews (34): shared vtable.
        void** cvt = *(void***)ctx;
        const bool ctxOk = MH_CreateHook(cvt[33], (void*)&Hooked_OMSetRenderTargets, (void**)&o_OMSetRenderTargets) == MH_OK &&
                           MH_CreateHook(cvt[34], (void*)&Hooked_OMSetRenderTargetsUav, (void**)&o_OMSetRenderTargetsUav) == MH_OK &&
                           MH_EnableHook(cvt[33]) == MH_OK && MH_EnableHook(cvt[34]) == MH_OK;
        if (!ctxOk) Log(1, "overlay: OMSetRenderTargets hooks failed, post effects draw at Present (HUD included)");
        // IDXGIFactory::CreateSwapChain (10) / IDXGIFactory2::CreateSwapChainForHwnd (15): DE's D3D12 queue
        IDXGIFactory2* fac = nullptr;
        bool facOk = false;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) {
            void** fvt = *(void***)fac;
            facOk = MH_CreateHook(fvt[10], (void*)&Hooked_CreateSwapChain, (void**)&o_CreateSwapChain) == MH_OK &&
                    MH_CreateHook(fvt[15], (void*)&Hooked_CreateSwapChainForHwnd, (void**)&o_CreateSwapChainForHwnd) == MH_OK &&
                    MH_EnableHook(fvt[10]) == MH_OK && MH_EnableHook(fvt[15]) == MH_OK;
            fac->Release();
        }
        if (!facOk) Log(1, "overlay: DXGI factory hooks failed, no overlay with -dx12");
        Log(1, "overlay: Present=%p ResizeBuffers=%p hooks %s", present, resize, ok ? "installed" : "FAILED");
        sc->Release(); ctx->Release(); dev->Release();
    } else {
        Log(1, "overlay: dummy D3D11 device failed (0x%08lX), no overlay", hr);
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}
