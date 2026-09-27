// ImGui overlay for DE's D3D11 renderer (the game only loads d3d11/dxgi).
// Present runs on UE's render thread, the window procedure on the game (main) thread: ImGui state is
// shared under g_imguiLock. While the menu or freecam/noclip is active, key-down/char/mouse messages and
// raw mouse input are kept from the game; key-up and button-up always reach it, so no key stays stuck.
#include "skygfx.h"
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
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
static ID3D11Resource* g_backRes = nullptr; // backbuffer texture (not referenced; lives with the swap chain)
static bool g_fxDrawn = false;             // SpeedFX already drawn this frame (render thread)

static void CreateRtv() {
    ID3D11Texture2D* back = nullptr;
    if (g_swapChain && SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        if (FAILED(g_device->CreateRenderTargetView(back, nullptr, &g_rtv))) g_rtv = nullptr;
        g_backRes = back;
        back->Release();
    }
}

static void ReleaseRtv() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    g_backRes = nullptr;
}

// ---------------------------------------------------------------- SpeedFX (gta_sa.exe 1.0 US CPostEffects::SpeedFX, 0x7030A0)
// The frame is copied once, then drawn `passes` times as a full-screen strip quad (TL, TR, BL, BR) at alpha 36 with point
// sampling and clamp, each pass shrinking the UV rectangle by shift * 0.0025 per side (a zoom) plus a per-frame random
// wobble of wobble * 0.004 * rand()/32767, with the original's corner signs (BL's v uses the u wobble, as in 0x7030A0).
static ID3D11Texture2D* g_fxCopy = nullptr;
static ID3D11ShaderResourceView* g_fxSrv = nullptr;
static ID3D11VertexShader* g_fxVs = nullptr;
static ID3D11PixelShader* g_fxPs = nullptr;
static ID3D11Buffer* g_fxCb = nullptr;
static ID3D11SamplerState* g_fxSampler = nullptr;
static ID3D11BlendState* g_fxBlend = nullptr;
static ID3D11RasterizerState* g_fxRaster = nullptr;
static ID3D11DepthStencilState* g_fxDepth = nullptr;
static bool g_fxFailed = false;

static const char kFxHlsl[] =
    "cbuffer C : register(b0) { float4 uvTop; float4 uvBottom; float alpha; };\n"
    "Texture2D T : register(t0); SamplerState S : register(s0);\n"
    "void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {\n"
    "  float2 c = float2(id & 1, id >> 1);\n"                   // 0 TL, 1 TR, 2 BL, 3 BR (triangle strip)
    "  pos = float4(c.x * 2 - 1, 1 - c.y * 2, 0, 1);\n"
    "  float4 row = c.y ? uvBottom : uvTop; uv = c.x ? row.zw : row.xy; }\n"
    "float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return float4(T.Sample(S, uv).rgb, alpha); }\n";

static void ReleaseFxCopy() {
    if (g_fxSrv) { g_fxSrv->Release(); g_fxSrv = nullptr; }
    if (g_fxCopy) { g_fxCopy->Release(); g_fxCopy = nullptr; }
}

static bool FxInit() {
    if (g_fxVs) return true;
    if (g_fxFailed) return false;
    ID3DBlob *vs = nullptr, *ps = nullptr;
    bool ok = SUCCEEDED(D3DCompile(kFxHlsl, sizeof(kFxHlsl) - 1, "speedfx", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vs, nullptr)) &&
              SUCCEEDED(D3DCompile(kFxHlsl, sizeof(kFxHlsl) - 1, "speedfx", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &ps, nullptr)) &&
              SUCCEEDED(g_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_fxVs)) &&
              SUCCEEDED(g_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_fxPs));
    if (vs) vs->Release();
    if (ps) ps->Release();
    D3D11_BUFFER_DESC cb{ 48, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
    D3D11_SAMPLER_DESC sd{ D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                           D3D11_TEXTURE_ADDRESS_CLAMP };
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0] = { TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                           D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL };
    D3D11_RASTERIZER_DESC rd{ D3D11_FILL_SOLID, D3D11_CULL_NONE };
    rd.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC dd{}; // depth and stencil off
    ok = ok && SUCCEEDED(g_device->CreateBuffer(&cb, nullptr, &g_fxCb)) && SUCCEEDED(g_device->CreateSamplerState(&sd, &g_fxSampler)) &&
         SUCCEEDED(g_device->CreateBlendState(&bd, &g_fxBlend)) && SUCCEEDED(g_device->CreateRasterizerState(&rd, &g_fxRaster)) &&
         SUCCEEDED(g_device->CreateDepthStencilState(&dd, &g_fxDepth));
    if (!ok) { g_fxFailed = true; Log(1, "speedfx: shader/state creation failed, SpeedFX off"); }
    return ok;
}

static void SpeedFx() {
    const int packed = g_speedFxRow, row = packed & 0xFF, look = packed >> 8; // look: 1 behind, 2 sideways
    // Looking behind, the original zeroes every UV offset: each pass redraws the frame onto itself, no visible change.
    if (packed < 0 || row > 6 || look == 1 || !g_rtv || !FxInit()) return;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC td;
    back->GetDesc(&td);
    D3D11_TEXTURE2D_DESC have{};
    if (g_fxCopy) g_fxCopy->GetDesc(&have);
    if (!g_fxCopy || have.Width != td.Width || have.Height != td.Height || have.Format != td.Format) {
        ReleaseFxCopy();
        D3D11_TEXTURE2D_DESC cd = td;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0; cd.MiscFlags = 0;
        cd.SampleDesc = { 1, 0 }; cd.MipLevels = 1; cd.ArraySize = 1;
        if (FAILED(g_device->CreateTexture2D(&cd, nullptr, &g_fxCopy)) || FAILED(g_device->CreateShaderResourceView(g_fxCopy, nullptr, &g_fxSrv))) {
            ReleaseFxCopy(); back->Release(); return;
        }
    }
    g_context->CopyResource(g_fxCopy, back); // pRasterFrontBuffer
    back->Release();

    // Save the state we touch (the game's, restored after).
    ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    g_context->OMGetRenderTargets(1, &rtv, &dsv);
    ID3D11BlendState* blend = nullptr; FLOAT bf[4]; UINT mask; g_context->OMGetBlendState(&blend, bf, &mask);
    ID3D11DepthStencilState* depth = nullptr; UINT stencilRef; g_context->OMGetDepthStencilState(&depth, &stencilRef);
    ID3D11RasterizerState* raster = nullptr; g_context->RSGetState(&raster);
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    g_context->RSGetViewports(&nvp, vps);
    ID3D11VertexShader* vs = nullptr; g_context->VSGetShader(&vs, nullptr, nullptr);
    ID3D11PixelShader* ps = nullptr; g_context->PSGetShader(&ps, nullptr, nullptr);
    ID3D11Buffer* vcb = nullptr; g_context->VSGetConstantBuffers(0, 1, &vcb);
    ID3D11Buffer* pcb = nullptr; g_context->PSGetConstantBuffers(0, 1, &pcb);
    ID3D11ShaderResourceView* srv = nullptr; g_context->PSGetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = nullptr; g_context->PSGetSamplers(0, 1, &smp);
    ID3D11InputLayout* il = nullptr; g_context->IAGetInputLayout(&il);
    D3D11_PRIMITIVE_TOPOLOGY topo; g_context->IAGetPrimitiveTopology(&topo);
    ID3D11GeometryShader* gs = nullptr; g_context->GSGetShader(&gs, nullptr, nullptr);

    const D3D11_VIEWPORT vp{ 0, 0, (float)td.Width, (float)td.Height, 0, 1 };
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->OMSetBlendState(g_fxBlend, nullptr, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(g_fxDepth, 0);
    g_context->RSSetState(g_fxRaster);
    g_context->RSSetViewports(1, &vp);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_context->VSSetShader(g_fxVs, nullptr, 0);
    g_context->GSSetShader(nullptr, nullptr, 0);
    g_context->PSSetShader(g_fxPs, nullptr, 0);
    g_context->VSSetConstantBuffers(0, 1, &g_fxCb);
    g_context->PSSetConstantBuffers(0, 1, &g_fxCb);
    g_context->PSSetShaderResources(0, 1, &g_fxSrv);
    g_context->PSSetSamplers(0, 1, &g_fxSampler);

    const SpeedFxRow& r = kSpeedFx[row];
    const int shift = look ? r.shift / 2 : r.shift, wobble = look ? 0 : r.wobble; // integer halving, as in 0x7030A0
    const float u0 = 0, v0 = 0, u1 = 1, v1 = 1; // whole copy (the original's raster could be larger than the screen)
    float rx = 0, ry = 0;
    if (wobble > 0) {
        rx = (float)rand() * 0.000030518509f * (u1 * wobble * 0.004f);
        ry = (float)rand() * 0.000030518509f * (v1 * wobble * 0.004f);
    }
    const float stepU = u1 * shift * 0.0025f, stepV = v1 * shift * 0.0025f;
    for (int k = 1; k <= r.passes; ++k) {
        // Sideways, the original keeps only the right-edge u offsets (TR, BR): a horizontal stretch from the left.
        const float s = look ? 0 : stepU * k, t = look ? 0 : stepV * k, sr = stepU * k;
        const float c[12] = { u0 + s + rx, v0 + t + ry, u1 - sr - rx, v0 + t + ry,  // TL, TR
                              u0 + s + rx, v1 - t - rx, u1 - sr - rx, v1 - t - ry,  // BL, BR
                              kSpeedFxAlpha / 255.0f, 0, 0, 0 };
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g_context->Map(g_fxCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) break;
        memcpy(m.pData, c, sizeof(c));
        g_context->Unmap(g_fxCb, 0);
        g_context->Draw(4, 0);
    }

    g_context->OMSetRenderTargets(1, &rtv, dsv);
    g_context->OMSetBlendState(blend, bf, mask);
    g_context->OMSetDepthStencilState(depth, stencilRef);
    g_context->RSSetState(raster);
    g_context->RSSetViewports(nvp, vps);
    g_context->VSSetShader(vs, nullptr, 0); g_context->PSSetShader(ps, nullptr, 0); g_context->GSSetShader(gs, nullptr, 0);
    g_context->VSSetConstantBuffers(0, 1, &vcb); g_context->PSSetConstantBuffers(0, 1, &pcb);
    g_context->PSSetShaderResources(0, 1, &srv); g_context->PSSetSamplers(0, 1, &smp);
    g_context->IASetInputLayout(il); g_context->IASetPrimitiveTopology(topo);
    IUnknown* held[] = { rtv, dsv, blend, depth, raster, vs, ps, gs, vcb, pcb, srv, smp, il };
    for (IUnknown* o : held)
        if (o) o->Release();
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

    if (!g_fxDrawn) SpeedFx(); // HudBind not reached this frame (or 0): draw at Present, under the ImGui menu
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

// ---------------------------------------------------------------- SpeedFX before the HUD
// UE binds the backbuffer once for the final scene pass and again for Slate/UMG (the HUD). SpeedFX runs just before
// backbuffer bind number [SpeedFX] HudBind of the frame, so the scene is complete and the HUD not drawn yet.
// The binds per frame are logged once (600 frames) to calibrate HudBind; 0 = draw at Present (HUD blurred).
typedef void(STDMETHODCALLTYPE* OMSetRT_Fn)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* OMSetRTUav_Fn)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
                                                UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
static OMSetRT_Fn o_OMSetRenderTargets = nullptr;
static OMSetRTUav_Fn o_OMSetRenderTargetsUav = nullptr;
static thread_local bool t_ours = false; // our own binds (SpeedFX, ImGui) are not counted
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
    __try { SpeedFx(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
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
            g_lastRtv = nullptr;
        }
    }
    return o_Present(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE Hooked_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl) {
    const bool ours = g_ready && sc == g_swapChain;
    if (ours) { ReleaseRtv(); ReleaseFxCopy(); ImGui_ImplDX11_InvalidateDeviceObjects(); }
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
        // ID3D11DeviceContext::OMSetRenderTargets (33) / OMSetRenderTargetsAndUnorderedAccessViews (34): shared vtable.
        void** cvt = *(void***)ctx;
        const bool ctxOk = MH_CreateHook(cvt[33], (void*)&Hooked_OMSetRenderTargets, (void**)&o_OMSetRenderTargets) == MH_OK &&
                           MH_CreateHook(cvt[34], (void*)&Hooked_OMSetRenderTargetsUav, (void**)&o_OMSetRenderTargetsUav) == MH_OK &&
                           MH_EnableHook(cvt[33]) == MH_OK && MH_EnableHook(cvt[34]) == MH_OK;
        if (!ctxOk) Log(1, "overlay: OMSetRenderTargets hooks failed, SpeedFX draws at Present (HUD blurred)");
        Log(1, "overlay: Present=%p ResizeBuffers=%p hooks %s", present, resize, ok ? "installed" : "FAILED");
        sc->Release(); ctx->Release(); dev->Release();
    } else {
        Log(1, "overlay: dummy D3D11 device failed (0x%08lX), no overlay", hr);
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}
