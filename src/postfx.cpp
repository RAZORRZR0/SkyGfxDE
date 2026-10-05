// Post effects drawn onto DE's backbuffer before the HUD (overlay.cpp calls PostFxDraw at the HUD's backbuffer bind,
// or at Present). Order as in the original frame: water drops (skygfx draws them in the scene, before CPostEffects),
// then CPostEffects::Render's SpeedFX, radiosity and rain grain. Every effect that reads the frame copies it first.
#include "skygfx.h"
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdlib.h>

static ID3D11Device* D = nullptr;
static ID3D11DeviceContext* C = nullptr;
static ID3D11RenderTargetView* g_back = nullptr; // backbuffer RTV (overlay.cpp's)
static ID3D11Texture2D* g_backTex = nullptr;    // backbuffer (D3D11: the swap chain's; DX12: the D3D11On12-wrapped current one)
static UINT W = 0, H = 0;
static DXGI_FORMAT F = DXGI_FORMAT_UNKNOWN;

static const char kHlsl[] =
    "cbuffer C : register(b0) { float4 uvTop; float4 uvBottom; float4 p; };\n"
    "Texture2D T : register(t0); SamplerState S : register(s0);\n"
    // full-screen strip quad (TL, TR, BL, BR) with per-corner UVs
    "void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {\n"
    "  float2 c = float2(id & 1, id >> 1);\n"
    "  pos = float4(c.x * 2 - 1, 1 - c.y * 2, 0, 1);\n"
    "  float4 row = c.y ? uvBottom : uvTop; uv = c.x ? row.zw : row.xy; }\n"
    "float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return float4(T.Sample(S, uv).rgb, p.x); }\n"
    // radiosity step 2, the fixed-function combiners: (texture - limit) saturated, then current + current
    "float4 thresh(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  return float4(saturate(2 * saturate(T.Sample(S, uv).rgb - p.x)), 1); }\n"
    // PS2 grain: colour ignored (white), alpha MODULATE2X; blended DESTCOLOR/SRCALPHA = dst * (1 + 2a)
    "float4 grain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return float4(1, 1, 1, 2 * T.Sample(S, uv).r * p.x); }\n"
    // neo water drops: pixel positions, uv0 = drop mask, uv1 = frame (a flipped, wider window: the drop acts as a lens)
    "void vsDrop(float2 xy : POSITION, float4 col : COLOR, float4 uv : TEXCOORD0,\n"
    "            out float4 pos : SV_Position, out float4 c : COLOR, out float4 uvo : TEXCOORD0) {\n"
    "  pos = float4(xy.x * p.z * 2 - 1, 1 - xy.y * p.w * 2, 0, 1); c = col; uvo = uv; }\n"
    "float4 psDrop(float4 pos : SV_Position, float4 c : COLOR, float4 uv : TEXCOORD0) : SV_Target {\n"
    "  float2 d = uv.xy * 2 - 1; float r = dot(d, d);\n"
    "  return float4(T.Sample(S, uv.zw).rgb * c.rgb * lerp(1.0, 0.75, r), c.a * saturate((1 - r) * 3)); }\n"
    // D3D12 under-HUD composite: t0 final frame (HUD drawn), t1 scene copied at the HUD bind, t2 the effects on t1.
    // Pixels the HUD left untouched still equal the scene: they take the effects; the others keep the HUD.
    "Texture2D Sc : register(t1); Texture2D Wk : register(t2);\n"
    "float4 composite(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  int3 c = int3(pos.xy, 0); float3 f = T.Load(c).rgb;\n"
    "  return float4(any(abs(f - Sc.Load(c).rgb) > 0.0005) ? f : Wk.Load(c).rgb, 1); }\n";

static ID3D11VertexShader *g_vs = nullptr, *g_vsDrop = nullptr;
static ID3D11PixelShader *g_ps = nullptr, *g_psThresh = nullptr, *g_psGrain = nullptr, *g_psDrop = nullptr, *g_psComposite = nullptr;
static ID3D11InputLayout* g_dropLayout = nullptr;
static ID3D11Buffer *g_cb = nullptr, *g_dropVb = nullptr;
static ID3D11SamplerState *g_point = nullptr, *g_linear = nullptr, *g_wrap = nullptr;
static ID3D11BlendState *g_alpha = nullptr, *g_add = nullptr, *g_grainBlend = nullptr;
static ID3D11RasterizerState* g_raster = nullptr;
static ID3D11DepthStencilState* g_depth = nullptr;
static ID3D11Texture2D* g_grainTex = nullptr;
static ID3D11ShaderResourceView* g_grainSrv = nullptr;
static bool g_initFailed = false;

enum { MAXDROPS = 2000, MAXDROPSMOVING = 700 }; // neo.h
struct DropVertex { float x, y; uint32_t rgba; float u0, v0, u1, v1; };

static ID3DBlob* Compile(const char* entry, const char* target) {
    ID3DBlob *b = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "postfx", nullptr, nullptr, entry, target, 0, 0, &b, &err))) {
        Log(1, "postfx: %s: %s", entry, err ? (const char*)err->GetBufferPointer() : "compile failed");
        b = nullptr;
    }
    if (err) err->Release();
    return b;
}

static bool Init() {
    if (g_vs) return true;
    if (g_initFailed) return false;
    bool ok = true;
    auto makePs = [&](const char* e, ID3D11PixelShader** out) {
        ID3DBlob* b = ok ? Compile(e, "ps_4_0") : nullptr;
        ok = b && SUCCEEDED(D->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, out));
        if (b) b->Release();
    };
    ID3DBlob* vs = Compile("vs", "vs_4_0");
    ID3DBlob* vsDrop = Compile("vsDrop", "vs_4_0");
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ok = vs && vsDrop && SUCCEEDED(D->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
         SUCCEEDED(D->CreateVertexShader(vsDrop->GetBufferPointer(), vsDrop->GetBufferSize(), nullptr, &g_vsDrop)) &&
         SUCCEEDED(D->CreateInputLayout(layout, 3, vsDrop->GetBufferPointer(), vsDrop->GetBufferSize(), &g_dropLayout));
    if (vs) vs->Release();
    if (vsDrop) vsDrop->Release();
    makePs("ps", &g_ps);
    makePs("thresh", &g_psThresh);
    makePs("grain", &g_psGrain);
    makePs("psDrop", &g_psDrop);
    makePs("composite", &g_psComposite);

    D3D11_BUFFER_DESC cb{ 48, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
    D3D11_BUFFER_DESC vb{ MAXDROPS * 6 * sizeof(DropVertex), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE };
    D3D11_SAMPLER_DESC sd{ D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                           D3D11_TEXTURE_ADDRESS_CLAMP };
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(D->CreateBuffer(&cb, nullptr, &g_cb)) && SUCCEEDED(D->CreateBuffer(&vb, nullptr, &g_dropVb)) &&
         SUCCEEDED(D->CreateSamplerState(&sd, &g_point));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ok = ok && SUCCEEDED(D->CreateSamplerState(&sd, &g_linear));
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    ok = ok && SUCCEEDED(D->CreateSamplerState(&sd, &g_wrap));

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0] = { TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ZERO,
                           D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL };
    ok = ok && SUCCEEDED(D->CreateBlendState(&bd, &g_alpha));
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE; // radiosity add: SRCALPHA, ONE
    ok = ok && SUCCEEDED(D->CreateBlendState(&bd, &g_add));
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR; // grain: DESTCOLOR, SRCALPHA
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA;
    ok = ok && SUCCEEDED(D->CreateBlendState(&bd, &g_grainBlend));
    D3D11_RASTERIZER_DESC rd{ D3D11_FILL_SOLID, D3D11_CULL_NONE };
    rd.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC dd{}; // depth and stencil off
    D3D11_TEXTURE2D_DESC gd{ 64, 64, 1, 1, DXGI_FORMAT_R8_UNORM, { 1, 0 }, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
                             D3D11_CPU_ACCESS_WRITE };
    ok = ok && SUCCEEDED(D->CreateRasterizerState(&rd, &g_raster)) && SUCCEEDED(D->CreateDepthStencilState(&dd, &g_depth)) &&
         SUCCEEDED(D->CreateTexture2D(&gd, nullptr, &g_grainTex)) && SUCCEEDED(D->CreateShaderResourceView(g_grainTex, nullptr, &g_grainSrv));
    if (!ok) { g_initFailed = true; g_vs = nullptr; Log(1, "postfx: shader/state creation failed, post effects off"); }
    return ok;
}

// ---------------------------------------------------------------- backbuffer-sized targets
struct Rt { ID3D11Texture2D* t; ID3D11RenderTargetView* rtv; ID3D11ShaderResourceView* srv; UINT w, h; };
static Rt g_copy{};       // CPostEffects::pRasterFrontBuffer
static Rt g_rad[8]{};     // radiosity: halvings [0, g_radN), threshold result [g_radN]
static Rt g_frame{}, g_scene{}, g_work{}; // D3D12 under-HUD: final frame, scene at the HUD bind, effects target
static int g_radN = 0;

static void Free(Rt& r) {
    if (r.srv) r.srv->Release();
    if (r.rtv) r.rtv->Release();
    if (r.t) r.t->Release();
    r = {};
}

void PostFxReleaseSized() {
    Free(g_copy);
    for (Rt& r : g_rad) Free(r);
    Free(g_frame); Free(g_scene); Free(g_work);
    g_radN = 0;
    W = H = 0;
}

static bool Make(Rt& r, UINT w, UINT h, bool target) {
    D3D11_TEXTURE2D_DESC d{ w, h, 1, 1, F, { 1, 0 }, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_SHADER_RESOURCE | (target ? D3D11_BIND_RENDER_TARGET : 0u) };
    r.w = w; r.h = h;
    return SUCCEEDED(D->CreateTexture2D(&d, nullptr, &r.t)) && SUCCEEDED(D->CreateShaderResourceView(r.t, nullptr, &r.srv)) &&
           (!target || SUCCEEDED(D->CreateRenderTargetView(r.t, nullptr, &r.rtv)));
}

// Radiosity works at the PS2's scale: m_RadiosityFilterPasses (2) halvings of 640x448, plus the halvings that bring
// this backbuffer's height down to about 448.
static bool Sized() {
    D3D11_TEXTURE2D_DESC td;
    g_backTex->GetDesc(&td);
    if (td.Width == W && td.Height == H && td.Format == F && g_copy.t) return true;
    PostFxReleaseSized();
    W = td.Width; H = td.Height; F = td.Format;
    bool ok = Make(g_copy, W, H, false);
    g_radN = 2 + (H > 448 ? (int)lroundf(log2f(H / 448.0f)) : 0);
    if (g_radN > 7) g_radN = 7;
    for (int i = 0; i < g_radN && ok; ++i) ok = Make(g_rad[i], W >> (i + 1) ? W >> (i + 1) : 1, H >> (i + 1) ? H >> (i + 1) : 1, true);
    ok = ok && Make(g_rad[g_radN], g_rad[g_radN - 1].w, g_rad[g_radN - 1].h, true);
    if (!ok) { Log(1, "postfx: backbuffer-sized targets failed (%ux%u format %d)", W, H, (int)F); PostFxReleaseSized(); }
    return ok;
}

static void Grab() { C->CopyResource(g_copy.t, g_backTex); }

// One full-screen quad into rt: uv rectangle per corner (TL, TR, BL, BR as 12 floats incl. p), shader, blend, sampler.
static void Quad(ID3D11RenderTargetView* rt, UINT w, UINT h, ID3D11ShaderResourceView* src, ID3D11PixelShader* ps,
                 ID3D11BlendState* bs, ID3D11SamplerState* ss, const float c[12]) {
    C->OMSetRenderTargets(1, &rt, nullptr); // before binding src: a texture still bound as target would unbind the SRV
    const D3D11_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 };
    C->RSSetViewports(1, &vp);
    C->OMSetBlendState(bs, nullptr, 0xFFFFFFFF);
    C->PSSetShader(ps, nullptr, 0);
    C->PSSetShaderResources(0, 1, &src);
    C->PSSetSamplers(0, 1, &ss);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(C->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, c, 48);
    C->Unmap(g_cb, 0);
    C->Draw(4, 0);
}

static void Rect(ID3D11RenderTargetView* rt, UINT w, UINT h, ID3D11ShaderResourceView* src, ID3D11PixelShader* ps,
                 ID3D11BlendState* bs, ID3D11SamplerState* ss, float u0, float v0, float u1, float v1, float p0) {
    const float c[12] = { u0, v0, u1, v0, u0, v1, u1, v1, p0, 0, 0, 0 };
    Quad(rt, w, h, src, ps, bs, ss, c);
}

// ---------------------------------------------------------------- SpeedFX (gta_sa.exe 1.0 US CPostEffects::SpeedFX, 0x7030A0)
// The frame is copied once, then drawn `passes` times as a full-screen strip quad at alpha 36 with point sampling and
// clamp, each pass shrinking the UV rectangle by shift * 0.0025 per side (a zoom) plus a per-frame random wobble of
// wobble * 0.004 * rand()/32767, with the original's corner signs (BL's v uses the u wobble, as in 0x7030A0).
static void SpeedFx() {
    const int packed = g_speedFxRow, row = packed & 0xFF, look = packed >> 8; // look: 1 behind, 2 sideways
    // Looking behind, the original zeroes every UV offset: each pass redraws the frame onto itself, no visible change.
    if (packed < 0 || row > 6 || look == 1) return;
    Grab();
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
        Quad(g_back, W, H, g_copy.srv, g_ps, g_alpha, g_point, c);
    }
}

// ---------------------------------------------------------------- radiosity (CPostEffects::Radiosity, skygfx "PS2" path)
// 1. downsample: m_RadiosityFilterPasses bilinear halvings; each pass starts m_RadiosityFilterUCorrection/VCorrection
//    (2, 2) texels in, so the glow sits (2^passes - 1) * 2 = 6 PS2 pixels up-left of its source;
// 2. D = 2 * (D - limit/2) saturated, limit = CTimeCycle::m_CurrentColours.m_nHighLightMinIntensity (timecyc column
//    50, "IntensityLimit") * 128/255;
// 3. added to the frame with SRCALPHA/ONE at alpha m_RadiosityIntensity, m_RadiosityRenderPasses (1) times.
static void Radiosity() {
    if (!g_cfg.radiosity || !g_curColours) return;
    const int limit = *(const int32_t*)(g_curColours + 0x9C) * 128 / 255; // CColourSet::m_nHighLightMinIntensity
    Grab();
    const float ou = g_cfg.radiosityOffset / 640, ov = g_cfg.radiosityOffset / 448;
    Rect(g_rad[0].rtv, g_rad[0].w, g_rad[0].h, g_copy.srv, g_ps, nullptr, g_linear, ou, ov, 1 + ou, 1 + ov, 1);
    for (int i = 1; i < g_radN; ++i)
        Rect(g_rad[i].rtv, g_rad[i].w, g_rad[i].h, g_rad[i - 1].srv, g_ps, nullptr, g_linear, 0, 0, 1, 1, 1);
    const Rt& th = g_rad[g_radN];
    Rect(th.rtv, th.w, th.h, g_rad[g_radN - 1].srv, g_psThresh, nullptr, g_linear, 0, 0, 1, 1, limit / 255.0f);
    Rect(g_back, W, H, th.srv, g_ps, g_add, g_linear, 0, 0, 1, 1, g_cfg.radiosityIntensity / 255.0f);
}

// ---------------------------------------------------------------- grain (skygfx CPostEffects::Grain_PS2)
// A 64x64 texture of the PS2 VU random generator's low bytes masked by the strength (the mask is a bitmask, as on the
// PS2), regenerated every frame, tiled 5 x 7 times per 640x448 and blended as dst * (1 + 2 * alpha).
static void Grain() {
    const int mask = g_fx.grain;
    if (!g_cfg.grain || mask <= 0 || g_cfg.grainStrength <= 0) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(C->Map(g_grainTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    uint32_t r = 0x3F800000 | (rand() & 0x007FFFFF); // vrinit
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            ((uint8_t*)m.pData)[y * m.RowPitch + x] = (uint8_t)(r & mask);
            r = ((r << 1) ^ ((r >> 4) & 1) ^ ((r >> 22) & 1)) & 0x7FFFFF | 0x3F800000; // vradvance
        }
    C->Unmap(g_grainTex, 0);
    Rect(g_back, W, H, g_grainSrv, g_psGrain, g_grainBlend, g_wrap, 0, 0, 5.0f * W / 640, 7.0f * H / 448, g_cfg.grainStrength);
}

// ---------------------------------------------------------------- neo water drops (skygfx neoWaterdrops.cpp)
// Simulated at a fixed 30 Hz (the original's per-frame steps assume ~30 fps); positions and sizes in backbuffer pixels,
// scaled by height/480 as in skygfx. The drop mask is drawn procedurally (skygfx loads "dropmask" from neo.txd).
// Sources (tools.cpp): rain, DE's water splash FX, boat splash / wake / water splash particles, hydrants and
// fountains. Blood drops (skygfx neoBloodDrops, off by default) are not ported.
struct Drop { float x, y, time, size, uvsize, ttl; uint8_t alpha; bool active, fades; };
struct DropMoving { Drop* drop; float dist; };
static Drop g_drops[MAXDROPS];
static DropMoving g_moving[MAXDROPSMOVING];
static int g_numDrops = 0, g_numMoving = 0, g_splashDuration = -1;
static float g_vec[3], g_vecLen = 0, g_distMoved = 0, g_rainAngle = 0, g_lastPos[3], g_scaling = 1;
static bool g_dropsEnabled = false, g_havePos = false;


static int SC(float x) { return (int)(x * g_scaling); }
static constexpr float MAXSIZE = 15, MINSIZE = 4;

static Drop* PlaceNew(float x, float y, float size, float ttl, bool fades) {
    if (g_fx.noDrops || g_numDrops >= g_cfg.maxDrops) return nullptr; // [WaterDrops] MaxDrops (skygfx: 2000)
    for (Drop& d : g_drops) {
        if (d.active) continue;
        ++g_numDrops;
        d = { x, y, 0.0f, size, (SC(MAXSIZE) - size + 1.0f) / (SC(MAXSIZE) - SC(MINSIZE) + 1.0f), ttl, 0xFF, true, fades };
        return &d;
    }
    return nullptr;
}

static void FillScreenMoving(float amount) {
    int n = (int)((g_vec[2] <= 5.0f ? 1.0f : 1.5f) * amount * 20.0f);
    while (n-- > 0 && g_numDrops < MAXDROPS && g_numMoving < MAXDROPSMOVING) {
        const float x = (float)(rand() % W), y = (float)(rand() % H);
        const float size = (float)(rand() % (SC(MAXSIZE) - SC(MINSIZE)) + SC(MINSIZE));
        if (Drop* d = PlaceNew(x, y, size, 2000.0f, true))
            for (DropMoving& m : g_moving)
                if (!m.drop) { ++g_numMoving; m = { d, 0.0f }; break; }
    }
}

static void NewTrace(DropMoving& m) {
    if (g_numDrops >= MAXDROPS) return;
    m.dist = 0.0f;
    PlaceNew(m.drop->x, m.drop->y, (float)SC(MINSIZE), 500.0f, true);
}

static void MoveDrop(DropMoving& m) {
    Drop* d = m.drop;
    if (!d->active) { m.drop = nullptr; --g_numMoving; return; }
    if (g_vec[2] <= 0.0f || g_distMoved <= 0.3f) return;
    if (g_vecLen <= 0.5f || g_fx.firstPerson) { // movement out of the centre
        const float step = g_vec[2] * 0.2f;
        float dx = d->x - W * 0.5f + g_vec[0];
        float dy = d->y - H * (g_fx.firstPerson ? 1.2f : 0.5f) - g_vec[1];
        const float sum = fabsf(dx) + fabsf(dy);
        if (sum >= 0.001f) { dx /= sum; dy /= sum; }
        m.dist += step;
        if (m.dist > 20.0f) NewTrace(m);
        d->x += dx * step;
        d->y += dy * step;
    } else { // movement when the camera turns
        m.dist += g_vecLen;
        if (m.dist > 20.0f) NewTrace(m);
        d->x -= g_vec[0];
        d->y += g_vec[1];
    }
    if (d->x < 0.0f || d->y < 0.0f || d->x > W || d->y > H) { m.drop = nullptr; --g_numMoving; }
}

static void DropsTick() { // WaterDrops::Process at 30 Hz
    g_scaling = H / 480.0f;
    // CalculateMovement: camera movement in camera space (x right, y up, z forward) x 10. skygfx negates RW's right
    // vector, which points left; DE's matrix right points right.
    const float* c = g_fx.cam;
    float delta[3] = { c[12] - g_lastPos[0], c[13] - g_lastPos[1], c[14] - g_lastPos[2] };
    if (!g_havePos) delta[0] = delta[1] = delta[2] = 0.0f;
    memcpy(g_lastPos, c + 12, sizeof(g_lastPos));
    g_havePos = true;
    g_distMoved = sqrtf(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    for (int i = 0; i < 3; ++i) g_vec[i] = 10.0f * (c[i * 4] * delta[0] + c[i * 4 + 1] * delta[1] + c[i * 4 + 2] * delta[2]);
    const float fwd = g_vec[1], up = g_vec[2]; // rows are right, forward, up: reorder to right, up, forward
    g_vec[1] = up; g_vec[2] = fwd;
    g_vecLen = sqrtf(g_vec[0] * g_vec[0] + g_vec[1] * g_vec[1]);
    g_dropsEnabled = g_fx.dropsEnabled;
    g_rainAngle = acosf(fminf(fmaxf(c[6], -1.0f), 1.0f)) * 57.29578f; // forward.z: 0 looking up, 180 looking down

    // SprayDrops
    if (!g_fx.noRain && g_fx.rain != 0.0f && g_dropsEnabled) {
        const float t = fmaxf(180.0f - g_rainAngle, 40.0f);
        FillScreenMoving((t - 40.0f) / 150.0f * g_fx.rain * 0.5f);
    }
    const int splash = InterlockedExchange((volatile LONG*)&g_splash, -1);
    if (splash >= 0) g_splashDuration = splash;
    if (g_splashDuration >= 0) {
        if (g_numDrops < MAXDROPS) FillScreenMoving(1.0f);
        --g_splashDuration;
    }
    const LONG bits = InterlockedExchange(&g_dropFill, 0); // boat splash / wake / water splash particles
    float fill; memcpy(&fill, &bits, 4);
    if (fill > 0.0f) FillScreenMoving(fill);
    // ProcessMoving
    if (g_dropsEnabled)
        for (DropMoving& m : g_moving)
            if (m.drop) MoveDrop(m);
    // Fade: CTimer::ms_fTimeStep * 1000 / 50 at 30 fps
    for (Drop& d : g_drops) {
        if (!d.active) continue;
        d.time += 33.0f;
        if (d.time >= d.ttl) { --g_numDrops; d.active = false; }
        else if (d.fades) d.alpha = (uint8_t)(255 - d.time / d.ttl * 255);
    }
}

static void ClearDrops() {
    for (Drop& d : g_drops) d.active = false;
    for (DropMoving& m : g_moving) m.drop = nullptr;
    g_numDrops = g_numMoving = 0;
}

static void WaterDrops() {
    static LARGE_INTEGER freq{}, last{};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); last = now; }
    static double acc = 0;
    acc += (double)(now.QuadPart - last.QuadPart) / freq.QuadPart;
    last = now;
    if (acc > 0.25) acc = 1.0 / 30; // after a stall: one step, not a burst
    if (!g_cfg.waterDrops) { if (g_numDrops) ClearDrops(); return; }
    for (; acc >= 1.0 / 30; acc -= 1.0 / 30) DropsTick();

    if (!g_dropsEnabled || g_numDrops <= 0 || g_fx.hideDrops) return;
    if (g_fx.noDrops) { ClearDrops(); return; } // camera underwater: the drops go at once

    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(C->Map(g_dropVb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    DropVertex* v = (DropVertex*)m.pData;
    UINT n = 0;
    static const float xy[8] = { -1, -1, -1, 1, 1, 1, 1, -1 }, uv[8] = { 0, 0, 0, 1, 1, 1, 1, 0 };
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    for (const Drop& d : g_drops) { // AddToRenderList
        if (!d.active) continue;
        const float tmp = (d.uvsize * (300.0f - 40.0f) + 40.0f) * g_scaling; // frame window, scaled like the drop
        const float u1a = fmaxf(d.x - tmp, 0.0f) / W, v1a = fmaxf(d.y - tmp, 0.0f) / H;
        const float u1b = fminf(d.x + tmp, (float)W) / W, v1b = fminf(d.y + tmp, (float)H) / H;
        const float scale = d.size * 0.5f;
        const uint32_t rgba = 0x00FFFFFFu | (uint32_t)d.alpha << 24;
        for (int k : tri)
            v[n++] = { d.x + xy[k * 2] * scale, d.y + xy[k * 2 + 1] * scale, rgba, uv[k * 2], uv[k * 2 + 1],
                       k >= 2 ? u1b : u1a, k % 3 == 0 ? v1b : v1a };
    }
    C->Unmap(g_dropVb, 0);
    if (!n) return;

    Grab();
    const float c[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.0f / W, 1.0f / H };
    if (FAILED(C->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, c, 48);
    C->Unmap(g_cb, 0);
    C->OMSetRenderTargets(1, &g_back, nullptr);
    const D3D11_VIEWPORT vp{ 0, 0, (float)W, (float)H, 0, 1 };
    C->RSSetViewports(1, &vp);
    C->OMSetBlendState(g_alpha, nullptr, 0xFFFFFFFF);
    const UINT stride = sizeof(DropVertex), off = 0;
    C->IASetVertexBuffers(0, 1, &g_dropVb, &stride, &off);
    C->IASetInputLayout(g_dropLayout);
    C->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    C->VSSetShader(g_vsDrop, nullptr, 0);
    C->PSSetShader(g_psDrop, nullptr, 0);
    C->PSSetShaderResources(0, 1, &g_copy.srv);
    C->PSSetSamplers(0, 1, &g_linear);
    C->Draw(n, 0);
    // back to the full-screen quad setup
    C->IASetInputLayout(nullptr);
    C->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    C->VSSetShader(g_vs, nullptr, 0);
}

// ---------------------------------------------------------------- entry
struct SavedState {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]; ID3D11DepthStencilView* dsv;
    ID3D11BlendState* blend; FLOAT bf[4]; UINT mask;
    ID3D11DepthStencilState* depth; UINT stencilRef;
    ID3D11RasterizerState* raster;
    UINT nvp; D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11VertexShader* vs; ID3D11PixelShader* ps; ID3D11GeometryShader* gs;
    ID3D11Buffer *vcb, *pcb, *vb; UINT stride, offset;
    ID3D11ShaderResourceView* srv; ID3D11SamplerState* smp;
    ID3D11InputLayout* il; D3D11_PRIMITIVE_TOPOLOGY topo;

    void Save() {
        C->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        C->OMGetBlendState(&blend, bf, &mask);
        C->OMGetDepthStencilState(&depth, &stencilRef);
        C->RSGetState(&raster);
        nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        C->RSGetViewports(&nvp, vps);
        C->VSGetShader(&vs, nullptr, nullptr); C->PSGetShader(&ps, nullptr, nullptr); C->GSGetShader(&gs, nullptr, nullptr);
        C->VSGetConstantBuffers(0, 1, &vcb); C->PSGetConstantBuffers(0, 1, &pcb);
        C->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
        C->PSGetShaderResources(0, 1, &srv); C->PSGetSamplers(0, 1, &smp);
        C->IAGetInputLayout(&il); C->IAGetPrimitiveTopology(&topo);
    }
    void Restore() {
        C->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        C->OMSetBlendState(blend, bf, mask);
        C->OMSetDepthStencilState(depth, stencilRef);
        C->RSSetState(raster);
        C->RSSetViewports(nvp, vps);
        C->VSSetShader(vs, nullptr, 0); C->PSSetShader(ps, nullptr, 0); C->GSSetShader(gs, nullptr, 0);
        C->VSSetConstantBuffers(0, 1, &vcb); C->PSSetConstantBuffers(0, 1, &pcb);
        C->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        C->PSSetShaderResources(0, 1, &srv); C->PSSetSamplers(0, 1, &smp);
        C->IASetInputLayout(il); C->IASetPrimitiveTopology(topo);
        IUnknown* held[] = { dsv, blend, depth, raster, vs, ps, gs, vcb, pcb, vb, srv, smp, il };
        for (IUnknown* o : held) if (o) o->Release();
        for (auto* r : rtv) if (r) r->Release();
    }
};

static void Run() {
    C->OMSetDepthStencilState(g_depth, 0);
    C->RSSetState(g_raster);
    C->IASetInputLayout(nullptr);
    C->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    C->VSSetShader(g_vs, nullptr, 0);
    C->GSSetShader(nullptr, nullptr, 0);
    C->VSSetConstantBuffers(0, 1, &g_cb);
    C->PSSetConstantBuffers(0, 1, &g_cb);
    WaterDrops();
    SpeedFx();
    Radiosity();
    Grain();
}

static bool Begin(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* back, ID3D11RenderTargetView* rtv) {
    D = dev; C = ctx; g_back = rtv; g_backTex = back;
    return g_active && rtv && back && Init() && Sized();
}

void PostFxDraw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* back, ID3D11RenderTargetView* rtv) {
    if (!Begin(dev, ctx, back, rtv)) return;
    SavedState s{};
    s.Save();
    Run();
    s.Restore();
}

void PostFxDrawUnderHud(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* back, ID3D11RenderTargetView* rtv,
                        ID3D11Texture2D* scene) {
    if (!Begin(dev, ctx, back, rtv)) return;
    if (!g_work.t && !(Make(g_frame, W, H, false) && Make(g_scene, W, H, false) && Make(g_work, W, H, true))) {
        Free(g_frame); Free(g_scene); Free(g_work);
        return;
    }
    SavedState s{};
    s.Save();
    C->CopyResource(g_frame.t, back);
    C->CopyResource(g_scene.t, scene);
    C->CopyResource(g_work.t, scene);
    g_backTex = g_work.t; g_back = g_work.rtv; // the effects run on the scene as if it were the backbuffer
    Run();
    C->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT vp{ 0, 0, (float)W, (float)H, 0, 1 };
    C->RSSetViewports(1, &vp);
    C->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    C->PSSetShader(g_psComposite, nullptr, 0);
    ID3D11ShaderResourceView* srvs[3] = { g_frame.srv, g_scene.srv, g_work.srv };
    C->PSSetShaderResources(0, 3, srvs);
    C->Draw(4, 0);
    ID3D11ShaderResourceView* none[3] = {};
    C->PSSetShaderResources(0, 3, none);
    s.Restore();
}
