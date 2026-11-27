#include "dx11_blur.h"
#include "config.h"
#include "telemetry.h"
#include "MinHook.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

typedef HRESULT(WINAPI* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* ResizeBuffersFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef void   (WINAPI* DrawFn)(ID3D11DeviceContext*, UINT, UINT);
typedef void   (WINAPI* DrawIndexedFn)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void   (WINAPI* OMSetRTFn)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef HRESULT(WINAPI* D3DCompileFn)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

PresentFn       oPresent           = nullptr;
ResizeBuffersFn oResizeBuffers     = nullptr;
DrawFn          oDraw              = nullptr;
DrawIndexedFn   oDrawIndexed       = nullptr;
OMSetRTFn       oOMSetRenderTargets = nullptr;

void*           g_presentAddr      = nullptr;
void*           g_resizeAddr       = nullptr;
void*           g_drawAddr         = nullptr;
void*           g_drawIdxAddr      = nullptr;
void*           g_omSetRtAddr      = nullptr;

CRITICAL_SECTION g_cs;
bool             g_csInit        = false;
std::atomic<bool> g_stopFlag{false};
std::atomic<bool> g_hooked{false};
bool             g_ctxHooked     = false;
HANDLE           g_initThread    = nullptr;

ID3D11Device*             g_dev      = nullptr;
ID3D11DeviceContext*      g_ctx      = nullptr;
ID3D11VertexShader*       g_vs       = nullptr;
ID3D11PixelShader*        g_ps       = nullptr;
ID3D11SamplerState*       g_sampler  = nullptr;
ID3D11Buffer*             g_cb       = nullptr;
ID3D11RasterizerState*    g_rs       = nullptr;
ID3D11BlendState*         g_blend    = nullptr;
ID3D11DepthStencilState*  g_dss      = nullptr;

ID3D11Texture2D*          g_copyTex  = nullptr;
ID3D11ShaderResourceView* g_copySRV  = nullptr;
UINT                      g_texW     = 0;
UINT                      g_texH     = 0;
DXGI_FORMAT               g_texFmt   = DXGI_FORMAT_UNKNOWN;

// Pre-HUD blur tracking
ID3D11Texture2D* g_nextBackbuf      = nullptr; // raw pointer identity only (released immediately)
bool             g_onBackbuf        = false;
bool             g_blurDoneThisFrame = false;
bool             g_wantBlurThisFrame = false;
float            g_frameKmh         = 0.0f;
bool             g_inOurDraw        = false;

// ImGui / Win32 state
HWND    g_hwnd           = nullptr;
WNDPROC oWndProc         = nullptr;
typedef BOOL (WINAPI* SetCursorPosFn)(int X, int Y);
typedef BOOL (WINAPI* ClipCursorFn)(const RECT* lpRect);
SetCursorPosFn oSetCursorPos = nullptr;
ClipCursorFn   oClipCursor   = nullptr;
void*   g_setCursorAddr  = nullptr;
void*   g_clipCursorAddr = nullptr;
bool    g_imguiInit      = false;
bool    g_menuOpen       = false;

BOOL WINAPI hkSetCursorPos(int X, int Y) {
    if (g_menuOpen) return TRUE;
    return oSetCursorPos ? oSetCursorPos(X, Y) : FALSE;
}

BOOL WINAPI hkClipCursor(const RECT* lpRect) {
    if (g_menuOpen) return oClipCursor ? oClipCursor(nullptr) : TRUE;
    return oClipCursor ? oClipCursor(lpRect) : FALSE;
}
bool    g_bindingJoyBtn  = false;
DWORD   g_prevJoyBtns[4] = {0, 0, 0, 0};
bool    g_joyPrimed[4]   = {false, false, false, false};

bool g_prevF6  = false;
bool g_prevIns = false;
bool g_prevF9  = false;
bool g_prevF10 = false;

struct alignas(16) BlurCB {
    float speedFactor;   // 0..1
    float strength;      // radial streak scale
    float clearRadius;   // inner sharp zone
    float aspect;        // width / height
    float vignette;      // edge darkening
    int   samples;       // 4..16
    float pad[2];
};

static const char* kVSCode = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

static const char* kPSCode = R"(
Texture2D    sceneTex : register(t0);
SamplerState samp     : register(s0);

cbuffer BlurParams : register(b0) {
    float speedFactor;
    float strength;
    float clearRadius;
    float aspect;
    float vignette;
    int   samples;
    float2 pad;
};

struct PSIn {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSIn i) : SV_TARGET {
    float2 center = float2(0.5, 0.52);
    float2 dir = i.uv - center;
    float2 adir = float2(dir.x * aspect, dir.y);
    float dist = length(adir);

    float edgeMask = smoothstep(clearRadius, clearRadius + 0.45, dist);
    float blurAmt = strength * speedFactor * edgeMask;

    if (blurAmt < 0.0002) {
        return sceneTex.Sample(samp, i.uv);
    }

    float4 accum = float4(0.0, 0.0, 0.0, 0.0);
    float totalW = 0.0;
    int n = clamp(samples, 4, 16);
    float invN = 1.0 / (float)(n - 1);

    [loop]
    for (int s = 0; s < n; ++s) {
        float t = (float)s * invN;
        float w = 1.0 - 0.55 * t;
        float2 sampleUV = saturate(i.uv - dir * (blurAmt * t));
        accum += sceneTex.Sample(samp, sampleUV) * w;
        totalW += w;
    }

    float4 col = accum / totalW;

    if (vignette > 0.0001) {
        float vig = 1.0 - vignette * speedFactor * edgeMask * edgeMask;
        col.rgb *= vig;
    }
    return col;
}
)";

template <typename T>
void safe_release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

void release_copy_resources() {
    safe_release(g_copySRV);
    safe_release(g_copyTex);
    g_texW = 0;
    g_texH = 0;
    g_texFmt = DXGI_FORMAT_UNKNOWN;
    g_nextBackbuf = nullptr;
    g_onBackbuf = false;
}

void shutdown_imgui() {
    if (g_hwnd && oWndProc) {
        SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oWndProc));
        oWndProc = nullptr;
    }
    if (g_imguiInit) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiInit = false;
    }
    g_hwnd = nullptr;
}

void release_all_d3d() {
    shutdown_imgui();
    release_copy_resources();
    safe_release(g_dss);
    safe_release(g_blend);
    safe_release(g_rs);
    safe_release(g_cb);
    safe_release(g_sampler);
    safe_release(g_ps);
    safe_release(g_vs);
    safe_release(g_ctx);
    safe_release(g_dev);
}

D3DCompileFn load_d3dcompile() {
    HMODULE h = LoadLibraryA("d3dcompiler_47.dll");
    if (!h) h = LoadLibraryA("d3dcompiler_43.dll");
    if (!h) return nullptr;
    return reinterpret_cast<D3DCompileFn>(GetProcAddress(h, "D3DCompile"));
}

LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_imguiInit && g_menuOpen) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
            return TRUE;
        }
        switch (msg) {
            case WM_SETCURSOR:
                SetCursor(nullptr);
                return TRUE;
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
            case WM_INPUT:
            case WM_CHAR:
                return TRUE;
        }
    }
    return CallWindowProcA(oWndProc, hWnd, msg, wParam, lParam);
}

void init_imgui(IDXGISwapChain* sc) {
    if (g_imguiInit || !g_dev || !g_ctx) return;

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) return;

    g_hwnd = desc.OutputWindow;
    oWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc)));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 8.0f;
    st.FrameRounding  = 5.0f;
    st.GrabRounding   = 5.0f;
    st.Alpha          = 0.94f;
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.34f, 0.58f, 1.0f);
    st.Colors[ImGuiCol_CheckMark]     = ImVec4(0.35f, 0.78f, 1.00f, 1.0f);
    st.Colors[ImGuiCol_SliderGrab]    = ImVec4(0.28f, 0.65f, 0.92f, 1.0f);

    if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES) {
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    }

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    g_imguiInit = true;
}

bool ensure_copy_tex(const D3D11_TEXTURE2D_DESC& bbDesc) {
    if (g_copyTex && g_copySRV && g_texW == bbDesc.Width && g_texH == bbDesc.Height && g_texFmt == bbDesc.Format) {
        return true;
    }
    safe_release(g_copySRV);
    safe_release(g_copyTex);

    D3D11_TEXTURE2D_DESC td = bbDesc;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;

    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_copyTex)) || !g_copyTex) {
        return false;
    }
    if (FAILED(g_dev->CreateShaderResourceView(g_copyTex, nullptr, &g_copySRV)) || !g_copySRV) {
        safe_release(g_copySRV);
        safe_release(g_copyTex);
        return false;
    }
    g_texW = bbDesc.Width;
    g_texH = bbDesc.Height;
    g_texFmt = bbDesc.Format;
    return true;
}

void apply_blur_to_target(ID3D11Texture2D* backbuf, ID3D11RenderTargetView* rtv, float kmh) {
    if (!g_dev || !g_ctx || !g_vs || !g_ps || !backbuf || !rtv) return;

    D3D11_TEXTURE2D_DESC bbDesc = {};
    backbuf->GetDesc(&bbDesc);
    if (!ensure_copy_tex(bbDesc)) return;

    if (bbDesc.SampleDesc.Count > 1) {
        g_ctx->ResolveSubresource(g_copyTex, 0, backbuf, 0, bbDesc.Format);
    } else {
        g_ctx->CopyResource(g_copyTex, backbuf);
    }

    float denom = (g_cfg.max_kmh > g_cfg.start_kmh + 1.0f) ? (g_cfg.max_kmh - g_cfg.start_kmh) : 100.0f;
    float t = (kmh - g_cfg.start_kmh) / denom;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float speedFactor = t * t * (3.0f - 2.0f * t);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto* cb = static_cast<BlurCB*>(mapped.pData);
        cb->speedFactor = speedFactor;
        cb->strength    = g_cfg.strength;
        cb->clearRadius = g_cfg.clear_radius;
        cb->aspect      = (bbDesc.Height > 0) ? (static_cast<float>(bbDesc.Width) / static_cast<float>(bbDesc.Height)) : 1.777f;
        cb->vignette    = g_cfg.vignette;
        cb->samples     = g_cfg.samples;
        cb->pad[0] = cb->pad[1] = 0.0f;
        g_ctx->Unmap(g_cb, 0);
    }

    // Backup DX11 state
    ID3D11RenderTargetView* oldRTVs[8] = {};
    ID3D11DepthStencilView* oldDSV = nullptr;
    g_ctx->OMGetRenderTargets(8, oldRTVs, &oldDSV);

    UINT numVP = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT oldVPs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    g_ctx->RSGetViewports(&numVP, oldVPs);

    ID3D11RasterizerState*   oldRS = nullptr;
    g_ctx->RSGetState(&oldRS);

    ID3D11BlendState*        oldBlend = nullptr;
    FLOAT                    oldBlendFactor[4] = {};
    UINT                     oldSampleMask = 0;
    g_ctx->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);

    ID3D11DepthStencilState* oldDSS = nullptr;
    UINT                     oldStencilRef = 0;
    g_ctx->OMGetDepthStencilState(&oldDSS, &oldStencilRef);

    D3D11_PRIMITIVE_TOPOLOGY oldTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    g_ctx->IAGetPrimitiveTopology(&oldTopo);

    ID3D11InputLayout*       oldIL = nullptr;
    g_ctx->IAGetInputLayout(&oldIL);

    ID3D11VertexShader*      oldVS = nullptr;
    ID3D11PixelShader*       oldPS = nullptr;
    ID3D11GeometryShader*    oldGS = nullptr;
    g_ctx->VSGetShader(&oldVS, nullptr, nullptr);
    g_ctx->PSGetShader(&oldPS, nullptr, nullptr);
    g_ctx->GSGetShader(&oldGS, nullptr, nullptr);

    ID3D11ShaderResourceView* oldSRV = nullptr;
    ID3D11SamplerState*       oldSamp = nullptr;
    ID3D11Buffer*             oldCB = nullptr;
    g_ctx->PSGetShaderResources(0, 1, &oldSRV);
    g_ctx->PSGetSamplers(0, 1, &oldSamp);
    g_ctx->PSGetConstantBuffers(0, 1, &oldCB);

    D3D11_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(bbDesc.Width), static_cast<float>(bbDesc.Height), 0.0f, 1.0f };
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_rs);
    FLOAT zeroFactor[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(g_blend, zeroFactor, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(g_dss, 0);

    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_copySRV);
    g_ctx->PSSetSamplers(0, 1, &g_sampler);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);

    g_inOurDraw = true;
    g_ctx->Draw(3, 0);
    g_inOurDraw = false;

    ID3D11ShaderResourceView* nullSRV = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nullSRV);

    // Restore DX11 state
    g_ctx->OMSetRenderTargets(8, oldRTVs, oldDSV);
    g_ctx->RSSetViewports(numVP, oldVPs);
    g_ctx->RSSetState(oldRS);
    g_ctx->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
    g_ctx->OMSetDepthStencilState(oldDSS, oldStencilRef);
    g_ctx->IASetPrimitiveTopology(oldTopo);
    g_ctx->IASetInputLayout(oldIL);
    g_ctx->VSSetShader(oldVS, nullptr, 0);
    g_ctx->PSSetShader(oldPS, nullptr, 0);
    g_ctx->GSSetShader(oldGS, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &oldSRV);
    g_ctx->PSSetSamplers(0, 1, &oldSamp);
    g_ctx->PSSetConstantBuffers(0, 1, &oldCB);

    safe_release(oldCB);
    safe_release(oldSamp);
    safe_release(oldSRV);
    safe_release(oldGS);
    safe_release(oldPS);
    safe_release(oldVS);
    safe_release(oldIL);
    safe_release(oldDSS);
    safe_release(oldBlend);
    safe_release(oldRS);
    safe_release(oldDSV);
    for (int i = 0; i < 8; ++i) safe_release(oldRTVs[i]);
}

void maybe_prehud_blur(ID3D11DeviceContext* ctx) {
    if (ctx != g_ctx || g_inOurDraw || !g_onBackbuf || g_blurDoneThisFrame || !g_wantBlurThisFrame || !g_nextBackbuf) {
        return;
    }

    ID3D11BlendState* bs = nullptr;
    FLOAT bf[4] = {};
    UINT sm = 0;
    ctx->OMGetBlendState(&bs, bf, &sm);
    bool isAlphaBlended = false;
    if (bs) {
        D3D11_BLEND_DESC bd = {};
        bs->GetDesc(&bd);
        isAlphaBlended = (bd.RenderTarget[0].BlendEnable != FALSE);
        bs->Release();
    }
    if (!isAlphaBlended) return;

    ID3D11RenderTargetView* curRTV = nullptr;
    ID3D11DepthStencilView* curDSV = nullptr;
    ctx->OMGetRenderTargets(1, &curRTV, &curDSV);
    if (curRTV && !curDSV) {
        ID3D11Resource* res = nullptr;
        curRTV->GetResource(&res);
        if (res == static_cast<ID3D11Resource*>(g_nextBackbuf)) {
            apply_blur_to_target(g_nextBackbuf, curRTV, g_frameKmh);
            g_blurDoneThisFrame = true;
        }
        safe_release(res);
    }
    safe_release(curDSV);
    safe_release(curRTV);
}

void WINAPI hkOMSetRenderTargets(
    ID3D11DeviceContext* ctx, UINT numViews, ID3D11RenderTargetView* const* ppRTVs, ID3D11DepthStencilView* pDSV)
{
    oOMSetRenderTargets(ctx, numViews, ppRTVs, pDSV);
    if (ctx != g_ctx || g_inOurDraw) return;

    bool onBB = false;
    if (numViews == 1 && ppRTVs && ppRTVs[0] != nullptr && pDSV == nullptr && g_nextBackbuf != nullptr) {
        ID3D11Resource* res = nullptr;
        ppRTVs[0]->GetResource(&res);
        onBB = (res == static_cast<ID3D11Resource*>(g_nextBackbuf));
        safe_release(res);
    }
    g_onBackbuf = onBB;
}

void WINAPI hkDraw(ID3D11DeviceContext* ctx, UINT vertexCount, UINT startVertexLocation) {
    maybe_prehud_blur(ctx);
    oDraw(ctx, vertexCount, startVertexLocation);
}

void WINAPI hkDrawIndexed(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndexLocation, INT baseVertexLocation) {
    maybe_prehud_blur(ctx);
    oDrawIndexed(ctx, indexCount, startIndexLocation, baseVertexLocation);
}

void hook_device_context() {
    if (g_ctxHooked || !g_ctx) return;
    void** vt = *reinterpret_cast<void***>(g_ctx);
    g_drawIdxAddr = vt[12];
    g_drawAddr    = vt[13];
    g_omSetRtAddr = vt[33];

    if (MH_CreateHook(g_omSetRtAddr, reinterpret_cast<void*>(&hkOMSetRenderTargets), reinterpret_cast<void**>(&oOMSetRenderTargets)) == MH_OK &&
        MH_CreateHook(g_drawAddr,    reinterpret_cast<void*>(&hkDraw),              reinterpret_cast<void**>(&oDraw)) == MH_OK &&
        MH_CreateHook(g_drawIdxAddr, reinterpret_cast<void*>(&hkDrawIndexed),       reinterpret_cast<void**>(&oDrawIndexed)) == MH_OK) {
        if (MH_EnableHook(g_omSetRtAddr) == MH_OK &&
            MH_EnableHook(g_drawAddr)    == MH_OK &&
            MH_EnableHook(g_drawIdxAddr) == MH_OK) {
            g_ctxHooked = true;
            plugin_log(SCS_LOG_TYPE_message, "[ets2_car_enhance] Pre-HUD DX11 blur hook active (HUD stays sharp)");
        }
    }
}

bool ensure_pipeline(IDXGISwapChain* sc) {
    if (g_dev && g_vs && g_ps) {
        ID3D11Device* scDev = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&scDev))) && scDev) {
            bool same = (scDev == g_dev);
            scDev->Release();
            if (same) return true;
        }
        release_all_d3d();
    }

    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_dev))) || !g_dev) {
        return false;
    }
    g_dev->GetImmediateContext(&g_ctx);
    if (!g_ctx) {
        release_all_d3d();
        return false;
    }

    D3DCompileFn compileFn = load_d3dcompile();
    if (!compileFn) {
        plugin_log(SCS_LOG_TYPE_error, "[ets2_car_enhance] d3dcompiler_47.dll not found");
        release_all_d3d();
        return false;
    }

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errBlob = nullptr;

    HRESULT hr = compileFn(kVSCode, strlen(kVSCode), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr) || !vsBlob) {
        if (errBlob) {
            plugin_log(SCS_LOG_TYPE_error, static_cast<const char*>(errBlob->GetBufferPointer()));
            errBlob->Release();
        }
        release_all_d3d();
        return false;
    }
    safe_release(errBlob);

    hr = compileFn(kPSCode, strlen(kPSCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr) || !psBlob) {
        if (errBlob) {
            plugin_log(SCS_LOG_TYPE_error, static_cast<const char*>(errBlob->GetBufferPointer()));
            errBlob->Release();
        }
        safe_release(vsBlob);
        release_all_d3d();
        return false;
    }
    safe_release(errBlob);

    hr = g_dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs);
    safe_release(vsBlob);
    if (FAILED(hr)) { safe_release(psBlob); release_all_d3d(); return false; }

    hr = g_dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps);
    safe_release(psBlob);
    if (FAILED(hr)) { release_all_d3d(); return false; }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_dev->CreateSamplerState(&sd, &g_sampler))) { release_all_d3d(); return false; }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(BlurCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&cbd, nullptr, &g_cb))) { release_all_d3d(); return false; }

    D3D11_RASTERIZER_DESC rsd = {};
    rsd.FillMode = D3D11_FILL_SOLID;
    rsd.CullMode = D3D11_CULL_NONE;
    rsd.DepthClipEnable = FALSE;
    if (FAILED(g_dev->CreateRasterizerState(&rsd, &g_rs))) { release_all_d3d(); return false; }

    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = FALSE;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_dev->CreateBlendState(&bld, &g_blend))) { release_all_d3d(); return false; }

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.StencilEnable = FALSE;
    if (FAILED(g_dev->CreateDepthStencilState(&dsd, &g_dss))) { release_all_d3d(); return false; }

    init_imgui(sc);
    plugin_log(SCS_LOG_TYPE_message, "[ets2_car_enhance] DX11 + ImGui overlay ready (Insert/F6 or Wheel Btn)");
    return true;
}

void poll_joy_and_hotkeys() {
    if (g_hwnd && GetForegroundWindow() != g_hwnd) return;

    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool f6   = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    bool ins  = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (((f6 && !ctrl) && !g_prevF6) || (ins && !g_prevIns)) {
        g_menuOpen = !g_menuOpen;
    }
    g_prevF6  = f6;
    g_prevIns = ins;

    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !ctrl && !g_prevF9) {
        g_cfg.blur_enabled = !g_cfg.blur_enabled;
        plugin_log(SCS_LOG_TYPE_message, g_cfg.blur_enabled
            ? "[ets2_car_enhance] Speed blur ON (F9)"
            : "[ets2_car_enhance] Speed blur OFF (F9)");
    }
    g_prevF9 = f9;

    bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10 && !g_prevF10) {
        load_config();
        plugin_log(SCS_LOG_TYPE_message, "[ets2_car_enhance] Reloaded ets2_car_enhance.ini (F10)");
    }
    g_prevF10 = f10;

    for (UINT id = 0; id < 4; ++id) {
        JOYINFOEX j = {};
        j.dwSize  = sizeof(j);
        j.dwFlags = JOY_RETURNBUTTONS;
        if (joyGetPosEx(id, &j) == JOYERR_NOERROR) {
            if (!g_joyPrimed[id]) {
                g_prevJoyBtns[id] = j.dwButtons;
                g_joyPrimed[id] = true;
                continue;
            }
            DWORD newlyPressed = j.dwButtons & ~g_prevJoyBtns[id];
            g_prevJoyBtns[id] = j.dwButtons;

            if (newlyPressed != 0) {
                if (g_bindingJoyBtn) {
                    for (int b = 0; b < 32; ++b) {
                        if (newlyPressed & (1u << b)) {
                            g_cfg.menu_joy_btn = b + 1;
                            g_bindingJoyBtn = false;
                            save_config();
                            break;
                        }
                    }
                } else if (g_cfg.menu_joy_btn >= 1 && g_cfg.menu_joy_btn <= 32) {
                    DWORD mask = 1u << (g_cfg.menu_joy_btn - 1);
                    if (newlyPressed & mask) {
                        g_menuOpen = !g_menuOpen;
                    }
                }
            }
        } else {
            g_prevJoyBtns[id] = 0;
            g_joyPrimed[id] = false;
        }
    }
}

static ImU32 tire_temp_color(float t) {
    float coldT = 40.0f, optLow = 50.0f, optHigh = 95.0f, hotT = 110.0f;
    if (g_cfg.tire_type == 0) { // Slick
        coldT = 60.0f; optLow = 78.0f; optHigh = 105.0f; hotT = 118.0f;
    } else if (g_cfg.tire_type == 1) { // Semi-slick
        coldT = 52.0f; optLow = 70.0f; optHigh = 100.0f; hotT = 114.0f;
    }
    if (t < coldT)   return IM_COL32(75, 165, 255, 255);  // Cold: blue
    if (t < optLow)  return IM_COL32(80, 225, 200, 255);  // Warming: cyan-teal
    if (t <= optHigh) return IM_COL32(70, 235, 95, 255);  // Optimal: bright green
    if (t <= hotT)   return IM_COL32(255, 175, 45, 255);  // Hot: orange
    return IM_COL32(255, 65, 65, 255);                    // Overheated: red
}

static ImU32 brake_temp_color(float t) {
    float fadeT = (g_cfg.brake_type == 0) ? 430.0f : (g_cfg.brake_type == 2 ? 780.0f : 560.0f);
    float hotT  = fadeT * 0.72f;
    if (t < 140.0f) return IM_COL32(130, 145, 165, 255); // Cool: slate
    if (t < hotT)   return IM_COL32(110, 220, 120, 255); // Working: green
    if (t < fadeT)  return IM_COL32(255, 165, 40, 255);  // Hot: orange
    return IM_COL32(255, 45, 45, 255);                   // Brake fade: glowing red
}

static void draw_body_silhouette(ImDrawList* dl, ImVec2 p0, ImVec2 size, CarClass cls) {
    ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, IM_COL32(20, 24, 34, 255), 6.0f);
    dl->AddRect(p0, p1, IM_COL32(55, 85, 150, 180), 6.0f, 0, 1.2f);

    float w = size.x;
    float h = size.y;
    float gy = p0.y + h * 0.78f; // ground line

    ImU32 accent = IM_COL32(80, 175, 255, 255);
    if (cls == CarClass::Sport)    accent = IM_COL32(255, 175, 55, 255);
    if (cls == CarClass::Supercar) accent = IM_COL32(255, 75, 95, 255);
    if (cls == CarClass::Truck)    accent = IM_COL32(140, 150, 165, 255);

    // Subtle ground glow
    dl->AddLine(ImVec2(p0.x + w * 0.08f, gy), ImVec2(p0.x + w * 0.92f, gy), IM_COL32(70, 90, 120, 140), 1.5f);

    auto pt = [&](float nx, float ny) {
        return ImVec2(p0.x + w * nx, p0.y + h * ny);
    };

    if (cls == CarClass::Civilian) {
        // Sedan / Muscle silhouette
        ImVec2 pts[] = {
            pt(0.12f, 0.68f), pt(0.13f, 0.50f), pt(0.32f, 0.46f),
            pt(0.45f, 0.28f), pt(0.70f, 0.28f), pt(0.84f, 0.46f),
            pt(0.90f, 0.52f), pt(0.89f, 0.68f)
        };
        dl->AddPolyline(pts, 8, accent, ImDrawFlags_Closed, 2.2f);
        ImVec2 win[] = { pt(0.36f, 0.45f), pt(0.47f, 0.31f), pt(0.68f, 0.31f), pt(0.79f, 0.45f) };
        dl->AddPolyline(win, 4, IM_COL32(150, 210, 255, 160), ImDrawFlags_Closed, 1.3f);
    } else if (cls == CarClass::Sport) {
        // JDM Sport Coupe / Hot Hatch with rear lip wing
        ImVec2 pts[] = {
            pt(0.11f, 0.68f), pt(0.12f, 0.52f), pt(0.35f, 0.46f),
            pt(0.49f, 0.30f), pt(0.73f, 0.31f), pt(0.86f, 0.46f),
            pt(0.91f, 0.50f), pt(0.90f, 0.68f)
        };
        dl->AddPolyline(pts, 8, accent, ImDrawFlags_Closed, 2.2f);
        // Rear sport spoiler
        dl->AddLine(pt(0.84f, 0.45f), pt(0.91f, 0.38f), accent, 2.0f);
        dl->AddLine(pt(0.87f, 0.38f), pt(0.94f, 0.38f), accent, 2.2f);
        ImVec2 win[] = { pt(0.39f, 0.45f), pt(0.51f, 0.33f), pt(0.71f, 0.34f), pt(0.81f, 0.45f) };
        dl->AddPolyline(win, 4, IM_COL32(255, 215, 140, 160), ImDrawFlags_Closed, 1.3f);
    } else {
        // Low wedge Supercar with big rear GT wing
        ImVec2 pts[] = {
            pt(0.09f, 0.68f), pt(0.12f, 0.56f), pt(0.37f, 0.47f),
            pt(0.51f, 0.33f), pt(0.72f, 0.34f), pt(0.88f, 0.49f),
            pt(0.92f, 0.68f)
        };
        dl->AddPolyline(pts, 7, accent, ImDrawFlags_Closed, 2.3f);
        // GT Wing
        dl->AddLine(pt(0.85f, 0.47f), pt(0.89f, 0.34f), accent, 1.8f);
        dl->AddLine(pt(0.82f, 0.34f), pt(0.95f, 0.33f), accent, 2.5f);
        ImVec2 win[] = { pt(0.41f, 0.46f), pt(0.52f, 0.36f), pt(0.69f, 0.37f), pt(0.78f, 0.47f) };
        dl->AddPolyline(win, 4, IM_COL32(255, 160, 175, 160), ImDrawFlags_Closed, 1.3f);
    }

    // Wheels
    float wr = h * 0.125f;
    ImVec2 wf = pt(0.26f, 0.69f);
    ImVec2 wrPos = pt(0.76f, 0.69f);
    dl->AddCircleFilled(wf, wr, IM_COL32(18, 20, 26, 255), 20);
    dl->AddCircle(wf, wr, accent, 20, 2.0f);
    dl->AddCircle(wf, wr * 0.52f, IM_COL32(180, 195, 215, 200), 14, 1.4f);

    dl->AddCircleFilled(wrPos, wr, IM_COL32(18, 20, 26, 255), 20);
    dl->AddCircle(wrPos, wr, accent, 20, 2.0f);
    dl->AddCircle(wrPos, wr * 0.52f, IM_COL32(180, 195, 215, 200), 14, 1.4f);
}

static bool level_button(const char* label, bool active, const ImVec2& sz) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.88f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.56f, 0.96f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.18f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.27f, 0.35f, 1.0f));
    }
    bool clicked = ImGui::Button(label, sz);
    ImGui::PopStyleColor(2);
    return clicked;
}

static bool g_resetMiniHudPos = false;

static void draw_mini_hud() {
    if (!g_cfg.mini_hud_enabled) return;

    float s = g_cfg.mini_hud_scale;
    if (s < 0.70f) s = 0.70f;
    if (s > 1.80f) s = 1.80f;

    float hudW = 336.0f * s;
    float hudH = 132.0f * s;

    ImGuiIO& io = ImGui::GetIO();
    ImGuiCond posCond = g_resetMiniHudPos ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    g_resetMiniHudPos = false;
    if (g_cfg.mini_hud_x < 0.0f || g_cfg.mini_hud_y < 0.0f) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - hudW * 0.5f, io.DisplaySize.y - hudH - 14.0f), posCond);
    } else {
        ImGui::SetNextWindowPos(ImVec2(g_cfg.mini_hud_x, g_cfg.mini_hud_y), posCond);
    }
    ImGui::SetNextWindowSize(ImVec2(hudW, hudH), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;
    if (!g_menuOpen) {
        flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    }

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.08f, 0.12f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.42f, 0.88f, 0.95f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * s, 4.0f * s));

    if (ImGui::Begin("##MiniTelemetryHUD", nullptr, flags)) {
        ImGui::SetWindowFontScale(s);
        if (g_menuOpen) {
            ImVec2 wp = ImGui::GetWindowPos();
            g_cfg.mini_hud_x = wp.x;
            g_cfg.mini_hud_y = wp.y;
        }

        float tt[4], bt[4];
        float avgTire = 0.0f, maxBrake = 0.0f;
        for (int i = 0; i < 4; ++i) {
            tt[i] = g_tel.dbg_tire_temp[i].load(std::memory_order_relaxed);
            bt[i] = g_tel.dbg_brake_temp[i].load(std::memory_order_relaxed);
            avgTire += tt[i];
            if (bt[i] > maxBrake) maxBrake = bt[i];
        }
        avgTire *= 0.25f;

        bool absAct = g_tel.dbg_abs_active.load(std::memory_order_relaxed);
        bool espAct = g_tel.dbg_esp_active.load(std::memory_order_relaxed);
        float steerRaw = g_tel.steer.load(std::memory_order_relaxed); // + = left, - = right
        float steerClamped = steerRaw;
        if (steerClamped >  1.0f) steerClamped =  1.0f;
        if (steerClamped < -1.0f) steerClamped = -1.0f;

        ImVec2 wp = ImGui::GetWindowPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Top Steering Strip across the full width of the HUD (with glowing dot)
        float topH = 18.0f * s;
        float barX0 = wp.x + 12.0f * s;
        float barX1 = wp.x + hudW - 12.0f * s;
        float barMidX = 0.5f * (barX0 + barX1);
        float barY = wp.y + 9.0f * s;
        float barH = 4.0f * s;
        dl->AddRectFilled(ImVec2(barX0, barY - barH * 0.5f), ImVec2(barX1, barY + barH * 0.5f), IM_COL32(18, 24, 38, 240), 2.0f * s);
        dl->AddLine(ImVec2(barMidX, barY - 4.0f * s), ImVec2(barMidX, barY + 4.0f * s), IM_COL32(140, 165, 210, 170), 1.2f * s);
        float dotX = barMidX - steerClamped * ((barX1 - barX0) * 0.5f - 4.0f * s);
        if (std::fabs(dotX - barMidX) > 1.0f) {
            float xA = (dotX < barMidX) ? dotX : barMidX;
            float xB = (dotX < barMidX) ? barMidX : dotX;
            dl->AddRectFilled(ImVec2(xA, barY - barH * 0.5f), ImVec2(xB, barY + barH * 0.5f), IM_COL32(70, 180, 255, 190), 2.0f * s);
        }
        dl->AddCircleFilled(ImVec2(dotX, barY), 5.2f * s, IM_COL32(70, 185, 255, 85), 14);
        dl->AddCircleFilled(ImVec2(dotX, barY), 3.4f * s, IM_COL32(230, 245, 255, 255), 14);

        // Horizontal divider under steering strip + 2 vertical column dividers
        dl->AddLine(ImVec2(wp.x + 3.0f * s, wp.y + topH), ImVec2(wp.x + hudW - 3.0f * s, wp.y + topH), IM_COL32(55, 105, 225, 210), 1.6f * s);
        float col1X = wp.x + 88.0f * s;
        float col2X = wp.x + 206.0f * s;
        dl->AddLine(ImVec2(col1X, wp.y + topH), ImVec2(col1X, wp.y + hudH - 3.0f * s), IM_COL32(55, 105, 225, 210), 1.6f * s);
        dl->AddLine(ImVec2(col2X, wp.y + topH), ImVec2(col2X, wp.y + hudH - 3.0f * s), IM_COL32(55, 105, 225, 210), 1.6f * s);

        // Column 1: ABS & TC status + Clutch / Brake / Throttle vertical bars
        ImGui::SetCursorPos(ImVec2(9.0f * s, 32.0f * s));
        ImVec4 absCol = (g_cfg.abs_level == 0) ? ImVec4(0.45f, 0.48f, 0.55f, 1.0f)
                      : (absAct ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) : ImVec4(0.45f, 0.90f, 0.35f, 1.0f));
        ImGui::TextColored(absCol, "ABS");
        ImGui::SetCursorPos(ImVec2(9.0f * s, 48.0f * s));
        ImGui::TextColored(absCol, g_cfg.abs_level == 0 ? "OFF" : "L%d", g_cfg.abs_level);

        ImGui::SetCursorPos(ImVec2(9.0f * s, 76.0f * s));
        ImVec4 tcCol = (g_cfg.esp_level == 0) ? ImVec4(0.45f, 0.48f, 0.55f, 1.0f)
                     : (espAct ? ImVec4(1.0f, 0.65f, 0.2f, 1.0f) : ImVec4(0.45f, 0.90f, 0.35f, 1.0f));
        ImGui::TextColored(tcCol, "TC");
        ImGui::SetCursorPos(ImVec2(9.0f * s, 92.0f * s));
        ImGui::TextColored(tcCol, g_cfg.esp_level == 0 ? "OFF" : "L%d", g_cfg.esp_level);

        // Pedal bars: Clutch (blue), Brake (red), Throttle (green)
        float pedalVals[3] = {
            g_tel.clutch.load(std::memory_order_relaxed),
            g_tel.brake.load(std::memory_order_relaxed),
            g_tel.throttle.load(std::memory_order_relaxed)
        };
        ImU32 pedalCols[3] = {
            IM_COL32(75, 175, 255, 255),
            IM_COL32(255, 65, 65, 255),
            IM_COL32(70, 235, 95, 255)
        };
        const char* pedalLbls[3] = { "C", "B", "T" };
        float pBarW = 7.0f * s;
        float pBarY0 = wp.y + 26.0f * s;
        float pBarY1 = wp.y + 104.0f * s;
        for (int p = 0; p < 3; ++p) {
            float px = wp.x + (46.0f + p * 12.0f) * s;
            float v = pedalVals[p];
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            dl->AddRectFilled(ImVec2(px, pBarY0), ImVec2(px + pBarW, pBarY1), IM_COL32(20, 24, 36, 235), 2.0f * s);
            if (v > 0.01f) {
                float fillY = pBarY1 - (pBarY1 - pBarY0) * v;
                dl->AddRectFilled(ImVec2(px, fillY), ImVec2(px + pBarW, pBarY1), pedalCols[p], 2.0f * s);
            }
            dl->AddRect(ImVec2(px, pBarY0), ImVec2(px + pBarW, pBarY1), IM_COL32(70, 95, 145, 160), 2.0f * s, 0, 1.0f * s);
            ImGui::SetCursorPos(ImVec2((45.5f + p * 12.0f) * s, 107.0f * s));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pedalCols[p]), "%s", pedalLbls[p]);
        }

        // Column 2: Tire & Brake temperatures in C
        ImGui::SetCursorPos(ImVec2(96.0f * s, 29.0f * s));
        ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.95f, 1.0f), "TIRES");
        ImGui::SetCursorPos(ImVec2(96.0f * s, 46.0f * s));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(tire_temp_color(avgTire)), "%.0f C", avgTire);

        ImGui::SetCursorPos(ImVec2(96.0f * s, 74.0f * s));
        ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.95f, 1.0f), "BRAKES");
        ImGui::SetCursorPos(ImVec2(96.0f * s, 91.0f * s));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(brake_temp_color(maxBrake)), "%.0f C", maxBrake);

        // Column 3: Top-down chassis with filled treaded tires, turning front wheels & center G-meter
        float cx  = wp.x + 271.0f * s;
        float fy  = wp.y + 40.0f * s;
        float ry  = wp.y + 92.0f * s;
        float cy  = 0.5f * (fy + ry);
        float axW = 26.0f * s;

        // Spine & axles
        dl->AddLine(ImVec2(cx, fy), ImVec2(cx, ry), IM_COL32(150, 165, 190, 150), 2.0f * s);
        dl->AddLine(ImVec2(cx - axW, fy), ImVec2(cx + axW, fy), IM_COL32(140, 150, 170, 220), 2.2f * s);
        dl->AddLine(ImVec2(cx - axW, ry), ImVec2(cx + axW, ry), IM_COL32(140, 150, 170, 220), 2.2f * s);

        struct WheelPos { float wx; float wy; float dx; };
        WheelPos wpos[4] = {
            { cx - axW - 10.5f * s, fy, cx - axW - 1.5f * s }, // FL
            { cx + axW + 10.5f * s, fy, cx + axW + 1.5f * s }, // FR
            { cx - axW - 10.5f * s, ry, cx - axW - 1.5f * s }, // RL
            { cx + axW + 10.5f * s, ry, cx + axW + 1.5f * s }  // RR
        };

        auto draw_rot_tire = [&](float bx, float by, float hw, float hh, float ang, ImU32 fill, int tireType) {
            float c = std::cos(ang), sn = std::sin(ang);
            auto rot = [&](float lx, float ly) {
                return ImVec2(bx + lx * c - ly * sn, by + lx * sn + ly * c);
            };
            ImVec2 p[4] = { rot(-hw, -hh), rot(hw, -hh), rot(hw, hh), rot(-hw, hh) };
            dl->AddQuadFilled(p[0], p[1], p[2], p[3], fill);
            dl->AddQuad(p[0], p[1], p[2], p[3], IM_COL32(12, 16, 22, 235), 1.4f * s);

            // Tread pattern lines inside tire
            ImU32 groove = IM_COL32(14, 18, 24, 180);
            if (tireType == 0) {
                // Slick: single subtle center seam
                dl->AddLine(rot(0.0f, -hh * 0.72f), rot(0.0f, hh * 0.72f), IM_COL32(14, 18, 24, 75), 1.0f * s);
            } else if (tireType == 1) {
                // Semi-slick: 2 longitudinal + angled sport cuts
                dl->AddLine(rot(-hw * 0.35f, -hh * 0.75f), rot(-hw * 0.35f, hh * 0.75f), groove, 1.1f * s);
                dl->AddLine(rot( hw * 0.35f, -hh * 0.75f), rot( hw * 0.35f, hh * 0.75f), groove, 1.1f * s);
                dl->AddLine(rot(-hw * 0.7f, -hh * 0.25f), rot(0.0f, 0.0f), groove, 1.0f * s);
                dl->AddLine(rot( hw * 0.7f,  hh * 0.25f), rot(0.0f, 0.0f), groove, 1.0f * s);
            } else {
                // Street / Narrow street: full chevron + center groove
                dl->AddLine(rot(0.0f, -hh * 0.8f), rot(0.0f, hh * 0.8f), groove, 1.1f * s);
                for (int g = -1; g <= 1; ++g) {
                    float gy = g * hh * 0.42f;
                    dl->AddLine(rot(-hw * 0.75f, gy - hh * 0.18f), rot(0.0f, gy + hh * 0.08f), groove, 1.1f * s);
                    dl->AddLine(rot( hw * 0.75f, gy - hh * 0.18f), rot(0.0f, gy + hh * 0.08f), groove, 1.1f * s);
                }
            }
        };

        auto draw_rot_disc = [&](float bx, float by, float hw, float hh, float ang, ImU32 fill) {
            float c = std::cos(ang), sn = std::sin(ang);
            auto rot = [&](float lx, float ly) {
                return ImVec2(bx + lx * c - ly * sn, by + lx * sn + ly * c);
            };
            ImVec2 p[4] = { rot(-hw, -hh), rot(hw, -hh), rot(hw, hh), rot(-hw, hh) };
            dl->AddQuadFilled(p[0], p[1], p[2], p[3], fill);
        };

        for (int i = 0; i < 4; ++i) {
            ImU32 tCol = tire_temp_color(tt[i]);
            ImU32 bCol = brake_temp_color(bt[i]);
            float wAng = (i < 2) ? (-steerClamped * 0.45f) : 0.0f;
            draw_rot_tire(wpos[i].wx, wpos[i].wy, 7.2f * s, 11.2f * s, wAng, tCol, g_cfg.tire_type);
            draw_rot_disc(wpos[i].dx, wpos[i].wy, 2.3f * s, 6.4f * s, wAng, bCol);
        }

        // Center G-meter rings & dynamic weight-transfer ball
        float gLat  = g_tel.dbg_g_lat.load(std::memory_order_relaxed);
        float gLong = g_tel.dbg_g_long.load(std::memory_order_relaxed);
        float gMag  = std::sqrt(gLat * gLat + gLong * gLong);
        float maxG  = 1.4f;
        float ringR = 16.5f * s;

        dl->AddCircleFilled(ImVec2(cx, cy), ringR, IM_COL32(14, 18, 28, 220), 24);
        dl->AddCircle(ImVec2(cx, cy), ringR, IM_COL32(80, 130, 230, 155), 24, 1.2f * s);
        dl->AddCircle(ImVec2(cx, cy), ringR * 0.55f, IM_COL32(80, 130, 230, 85), 20, 1.0f * s);
        dl->AddLine(ImVec2(cx - ringR, cy), ImVec2(cx + ringR, cy), IM_COL32(120, 150, 210, 80), 1.0f * s);
        dl->AddLine(ImVec2(cx, cy - ringR), ImVec2(cx, cy + ringR), IM_COL32(120, 150, 210, 80), 1.0f * s);

        float nx = gLat / maxG;
        float ny = -gLong / maxG;
        float nLen = std::sqrt(nx * nx + ny * ny);
        if (nLen > 1.0f) { nx /= nLen; ny /= nLen; }
        ImVec2 ballPos(cx + nx * (ringR - 3.5f * s), cy + ny * (ringR - 3.5f * s));

        ImU32 ballCol = IM_COL32(80, 200, 255, 255);
        if (gMag >= 1.15f)      ballCol = IM_COL32(255, 65, 65, 255);
        else if (gMag >= 0.80f) ballCol = IM_COL32(255, 175, 45, 255);
        else if (gMag >= 0.45f) ballCol = IM_COL32(80, 235, 130, 255);

        dl->AddCircleFilled(ballPos, 6.2f * s, (ballCol & 0x00FFFFFF) | 0x44000000, 16);
        dl->AddCircleFilled(ballPos, 3.8f * s, ballCol, 16);

        // Clean G-value placed below the rear axle (no overlap with axle line!)
        ImGui::SetCursorPos(ImVec2(256.0f * s, 108.0f * s));
        ImGui::TextColored(ImVec4(0.82f, 0.90f, 1.0f, 1.0f), "%.1f G", gMag);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

static inline const char* L(const char* en, const char* ru) {
    return (g_cfg.ui_lang == 1) ? ru : en;
}

void draw_imgui_menu(ID3D11RenderTargetView* rtv, float kmh) {
    if (!g_imguiInit || (!g_menuOpen && !g_cfg.mini_hud_enabled)) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_menuOpen) {
        if (oClipCursor) oClipCursor(nullptr);
        else ClipCursor(nullptr);
        ImGui::GetIO().MouseDrawCursor = true;
    } else {
        ImGui::GetIO().MouseDrawCursor = false;
    }

    draw_mini_hud();

    if (g_menuOpen) {
        ImGui::SetNextWindowSize(ImVec2(550, 735), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("ETS2 Car Enhance v2.1.4 by Fel1neee", &g_menuOpen, ImGuiWindowFlags_NoCollapse)) {
            float thr     = g_tel.throttle.load(std::memory_order_relaxed) * 100.0f;
            float brk     = g_tel.brake.load(std::memory_order_relaxed) * 100.0f;
            float str     = g_tel.steer.load(std::memory_order_relaxed);
            float dfG     = g_tel.dbg_downforce_g.load(std::memory_order_relaxed);
            float slip    = g_tel.dbg_slip_deg.load(std::memory_order_relaxed);
            bool  esp     = g_tel.dbg_esp_active.load(std::memory_order_relaxed);
            bool  absAct  = g_tel.dbg_abs_active.load(std::memory_order_relaxed);
            float gripPct = g_tel.dbg_tire_grip.load(std::memory_order_relaxed) * 100.0f;
            int   kState  = g_tel.dbg_kamm_state.load(std::memory_order_relaxed);

            char carName[128] = {};
            get_car_name(carName, sizeof(carName));
            CarClass detCls = static_cast<CarClass>(g_tel.detected_class.load(std::memory_order_relaxed));
            const char* clsName = (g_cfg.ui_lang == 1) ? car_class_name_ru(detCls) : car_class_name_en(detCls);

            // Top toolbar: [Load] [Save] [HUD] [Blur]   ...   [EN/RU] [Basic/Advanced]
            if (ImGui::Button(L("Load", "Загрузить"))) {
                load_config();
                if (!g_cfg.ui_advanced) {
                    apply_abs_level(g_cfg.abs_level);
                    apply_esp_level(g_cfg.esp_level);
                    apply_tire_type(g_cfg.tire_type);
                    apply_brake_type(g_cfg.brake_type);
                }
                g_resetMiniHudPos = true;
                plugin_log(SCS_LOG_TYPE_message, "[ets2_car_enhance] Loaded config from .ini");
            }
            ImGui::SameLine();
            if (ImGui::Button(L("Save", "Сохранить"))) {
                save_config();
                plugin_log(SCS_LOG_TYPE_message, "[ets2_car_enhance] Saved config to .ini");
            }
            ImGui::SameLine();
            if (level_button(g_cfg.mini_hud_enabled ? L("HUD: ON", "HUD: ВКЛ") : L("HUD: OFF", "HUD: ВЫКЛ"), g_cfg.mini_hud_enabled, ImVec2(76, 0))) {
                g_cfg.mini_hud_enabled = !g_cfg.mini_hud_enabled;
            }
            ImGui::SameLine();
            if (level_button(g_cfg.blur_enabled ? L("Blur: ON", "Блюр: ВКЛ") : L("Blur: OFF", "Блюр: ВЫКЛ"), g_cfg.blur_enabled, ImVec2(82, 0))) {
                g_cfg.blur_enabled = !g_cfg.blur_enabled;
            }
            ImGui::SameLine(ImGui::GetWindowWidth() - 190.0f);
            if (level_button(g_cfg.ui_lang == 1 ? "RU" : "EN", true, ImVec2(36, 0))) {
                g_cfg.ui_lang = (g_cfg.ui_lang == 1) ? 0 : 1;
            }
            ImGui::SameLine();
            if (level_button(g_cfg.ui_advanced ? L("Mode: Advanced", "Режим: Продвинутый") : L("Mode: Basic", "Режим: Базовый"), g_cfg.ui_advanced, ImVec2(140, 0))) {
                g_cfg.ui_advanced = !g_cfg.ui_advanced;
            }

            ImGui::Separator();
            ImGui::Text(L("Car: %s [%s] | %.0f km/h | Grip: %.0f%% | Slip: %.1f deg",
                          "Авто: %s [%s] | %.0f км/ч | Держак: %.0f%% | Снос: %.1f°"),
                        carName, clsName, kmh, gripPct, slip);
            if (absAct) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "[ABS]"); }
            if (esp)    { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.2f, 0.95f, 0.4f, 1.0f), "[ESP]"); }
            if (kState == 1)      { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s", L("[Edge]", "[Грань]")); }
            else if (kState == 2) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", L("[Slide]", "[Срыв]")); }
            else if (kState == 3) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), "[Trail]"); }
            else if (kState == 4) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "%s", L("[Under]", "[Андер]")); }
            else if (kState == 5) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.9f, 1.0f), "%s", L("[Drift]", "[Занос]")); }

            if (!g_cfg.ui_advanced) {
                // =============================================================
                // BASIC MODE
                // =============================================================
                ImGui::Spacing();
                ImGui::BeginChild("##BasicABS", ImVec2(0, 108), true);
                ImGui::Text("%s", L("ABS & Brakes:", "АБС и Тормоза:"));
                if (level_button("OFF##abs", g_cfg.abs_level == 0, ImVec2(56, 26))) apply_abs_level(0);
                for (int i = 1; i <= 5; ++i) {
                    ImGui::SameLine();
                    char lbl[16]; std::snprintf(lbl, sizeof(lbl), "%d##abs", i);
                    if (level_button(lbl, g_cfg.abs_level == i, ImVec2(44, 26))) apply_abs_level(i);
                }
                ImGui::Spacing();
                ImGui::Text("%s", L("Discs:", "Диски:"));
                ImGui::SameLine();
                if (level_button(L("Standard", "Обычные"), g_cfg.brake_type == 0, ImVec2(95, 24))) apply_brake_type(0);
                ImGui::SameLine();
                if (level_button(L("Sport", "Спорт"), g_cfg.brake_type == 1, ImVec2(85, 24))) apply_brake_type(1);
                ImGui::SameLine();
                if (level_button(L("Ceramic", "Керамика"), g_cfg.brake_type == 2, ImVec2(100, 24))) apply_brake_type(2);
                const char* brkHint = (g_cfg.brake_type == 0) ? L("Standard: instant cold bite, fades above 430 C", "Обычные: сразу цепляют, но плывут после 430 C")
                                    : (g_cfg.brake_type == 1) ? L("Sport: better airflow cooling, fades above 560 C", "Спорт: лучше продуваются, перегрев с 560 C")
                                                              : L("Ceramic: 86% cold bite (<150 C), holds up to 780 C", "Керамика: 86% на холодную (<150 C), держат до 780 C");
                ImGui::TextDisabled("%s", brkHint);
                ImGui::EndChild();

                ImGui::BeginChild("##BasicESP", ImVec2(0, 74), true);
                ImGui::Text("%s", L("TC / ESP - Stability & Traction:", "ТС / ЕСП (TC / ESP) — стабилизация и тяга:"));
                if (level_button("OFF##esp", g_cfg.esp_level == 0, ImVec2(56, 26))) apply_esp_level(0);
                for (int i = 1; i <= 5; ++i) {
                    ImGui::SameLine();
                    char lbl[16]; std::snprintf(lbl, sizeof(lbl), "%d##esp", i);
                    if (level_button(lbl, g_cfg.esp_level == i, ImVec2(44, 26))) apply_esp_level(i);
                }
                ImGui::TextDisabled("%s", L("1 - allows drifting (minimal speed loss) | 5 - strict safety net",
                                            "1 — даёт валить боком (скорость не падает) | 5 — жёсткий ошейник"));
                ImGui::EndChild();

                ImGui::BeginChild("##BasicTire", ImVec2(0, 96), true);
                ImGui::Text("%s", L("TIRES - Compound:", "ШИНЫ — тип резины:"));
                const char* tireLabels[4] = {
                    L("Slick", "Слик"),
                    L("Semi-Slick", "Полуслик"),
                    L("Street", "Дорожная"),
                    L("Narrow Street", "Узкая дорожная")
                };
                for (int i = 0; i < 4; ++i) {
                    if (i > 0) ImGui::SameLine();
                    float bw = (i == 3) ? 132.0f : 98.0f;
                    if (level_button(tireLabels[i], g_cfg.tire_type == i, ImVec2(bw, 26))) {
                        apply_tire_type(i);
                    }
                }
                const char* tireDesc[4] = {
                    L("Max dry grip, but heats up fastest (optimal 78-105 C).",
                      "Макс. зацеп на сухой трассе, но быстрее всех греется (оптимум 78-105 C)."),
                    L("Sport balance: high grip and predictable slip edge (70-100 C).",
                      "Спортивный баланс: высокий держак и предсказуемое окно срыва (70-100 C)."),
                    L("Longest life and stable temps (50-95 C), lower peak grip.",
                      "Живёт дольше всех и стабильна по температуре (50-95 C), но держит слабее."),
                    L("Lowest grip: breaks traction easily, built for classics and drift.",
                      "Минимальный держак: легко срывается в скольжение, для классики и дрифта.")
                };
                ImGui::TextWrapped("%s", tireDesc[g_cfg.tire_type]);
                ImGui::EndChild();

                ImGui::BeginChild("##BasicAero", ImVec2(0, 225), true);
                ImGui::Text("%s", L("AERODYNAMICS & BODY CLASS:", "АЭРОДИНАМИКА И ТИП КУЗОВА:"));
                CarClass curCls = static_cast<CarClass>(g_cfg.active_preset);
                if (curCls == CarClass::Unknown) curCls = detCls;

                if (level_button(L("Civilian", "Гражданский"), curCls == CarClass::Civilian, ImVec2(115, 28))) apply_car_preset(CarClass::Civilian);
                ImGui::SameLine();
                if (level_button(L("Sport", "Спорт"), curCls == CarClass::Sport, ImVec2(95, 28))) apply_car_preset(CarClass::Sport);
                ImGui::SameLine();
                if (level_button(L("Supercar", "Спорткар"), curCls == CarClass::Supercar, ImVec2(110, 28))) apply_car_preset(CarClass::Supercar);
                ImGui::SameLine();
                if (level_button(L("Truck (Off)", "Тягач (Выкл)"), curCls == CarClass::Truck, ImVec2(120, 28))) apply_car_preset(CarClass::Truck);

                ImGui::Spacing();
                ImVec2 imgPos = ImGui::GetCursorScreenPos();
                ImVec2 imgSz(185.0f, 104.0f);
                draw_body_silhouette(ImGui::GetWindowDrawList(), imgPos, imgSz, curCls);
                ImGui::Dummy(imgSz);

                ImGui::SameLine();
                ImGui::BeginGroup();
                if (curCls == CarClass::Civilian) {
                    ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "%s", L("Civilian Body", "Гражданский кузов"));
                    ImGui::TextWrapped("%s", L("Sedans & muscle cars: S-Class, Lexus LS500, Hellcat.\nDownforce: 0.08 G (at 300 km/h)\nBalance: 50%% front / 50%% rear",
                                               "Седаны и маслкары: S-Class, Lexus LS500, Hellcat.\nПрижим: 0.08 G (300 км/ч)\nБаланс: 50%% перед / 50%% зад"));
                } else if (curCls == CarClass::Sport) {
                    ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "%s", L("Sport / Coupe / Hot-Hatch", "Спорт / Купе / Хот-хэтч"));
                    ImGui::TextWrapped("%s", L("Golf, Silvia, Supra, M3, RS6.\nDownforce: 0.15 G (at 300 km/h)\nBalance: 44%% front / 56%% rear",
                                               "Golf, Silvia, Supra, M3, RS6.\nПрижим: 0.15 G (300 км/ч)\nБаланс: 44%% перед / 56%% зад"));
                } else if (curCls == CarClass::Supercar) {
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.45f, 1.0f), "%s", L("Supercar / Hypercar", "Спорткар / Суперкар"));
                    ImGui::TextWrapped("%s", L("Lamborghini, Ferrari, McLaren, GT3.\nDownforce: 0.22 G (at 300 km/h)\nBalance: 38%% front / 62%% rear",
                                               "Lamborghini, Ferrari, McLaren, GT3.\nПрижим: 0.22 G (300 км/ч)\nБаланс: 38%% перед / 62%% зад"));
                } else {
                    ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.75f, 1.0f), "%s", L("Truck (plugin disabled)", "Тягач (плагин отключён)"));
                    ImGui::TextWrapped("%s", L("Vanilla truck and trailer physics without interference.",
                                               "Родная физика грузовиков и прицепов без вмешательства."));
                }
                ImGui::EndGroup();

                ImGui::Spacing();
                if (ImGui::Checkbox(L("Auto-preset on car change", "Авто-пресет при смене машины"), &g_cfg.auto_preset)) {
                    if (g_cfg.auto_preset && detCls != CarClass::Unknown) apply_car_preset(detCls);
                }
                ImGui::SameLine();
                ImGui::TextDisabled(L("(Live downforce: %.2f G)", "(Прижим сейчас: %.2f G)"), dfG);
                ImGui::EndChild();
            } else {
                // =============================================================
                // ADVANCED MODE (all sliders & fine tuning)
                // =============================================================
                ImGui::Text(L("Throttle: %.0f%% | Brake: %.0f%% | Steer: %.2f | Downforce: %.2f G",
                              "Газ: %.0f%% | Тормоз: %.0f%% | Руль: %.2f | Прижим: %.2f G"),
                            thr, brk, str, dfG);
                ImGui::Separator();

                if (ImGui::CollapsingHeader(L("1. Speed Motion Blur (DX11)", "1. Размытие краёв на скорости (DX11)"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Checkbox(L("Enable Blur##blur", "Включить размытие##blur"), &g_cfg.blur_enabled);
                    ImGui::SliderFloat(L("Start (km/h)##bstart", "Старт (км/ч)##bstart"), &g_cfg.start_kmh, 50.0f, 200.0f, "%.0f");
                    ImGui::SliderFloat(L("Max (km/h)##bmax", "Максимум (км/ч)##bmax"), &g_cfg.max_kmh, 180.0f, 400.0f, "%.0f");
                    ImGui::SliderFloat(L("Strength", "Сила размытия"), &g_cfg.strength, 0.005f, 0.060f, "%.3f");
                    ImGui::SliderFloat(L("Clear Center", "Чёткий центр"), &g_cfg.clear_radius, 0.20f, 0.55f, "%.2f");
                    ImGui::SliderFloat(L("Vignette", "Виньетка"), &g_cfg.vignette, 0.0f, 0.40f, "%.2f");
                    ImGui::SliderInt(L("Quality (samples)", "Качество (сэмплы)"), &g_cfg.samples, 4, 16);
                }

                if (ImGui::CollapsingHeader(L("2. Aero Downforce", "2. Аэродинамический прижим (Downforce)"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Checkbox(L("Enable Downforce##df", "Включить прижим##df"), &g_cfg.downforce_enabled);
                    ImGui::SliderFloat(L("Start (km/h)##dstart", "Старт (км/ч)##dstart"), &g_cfg.downforce_start_kmh, 50.0f, 160.0f, "%.0f");
                    ImGui::SliderFloat(L("Downforce at 300 km/h (G)", "Прижим на 300 км/ч (G)"), &g_cfg.downforce_g_at_300, 0.05f, 1.20f, "%.2f G");
                    ImGui::SliderFloat(L("Max Downforce (G)", "Лимит прижима (макс. G)"), &g_cfg.downforce_max_g, 0.15f, 1.50f, "%.2f G");
                    ImGui::SliderFloat(L("Aero Balance (% front)", "Баланс прижима (% перед)"), &g_cfg.aero_front_pct, 25.0f, 70.0f, "%.0f%%");
                    ImGui::SliderFloat(L("Anti-Bounce", "Анти-подскок на стыках"), &g_cfg.anti_bounce, 0.0f, 0.80f, "%.2f");
                }

                if (ImGui::CollapsingHeader(L("3. ESP / ABS / TC Stabilizer", "3. Стабилизатор ESP / ABS / TC"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Checkbox(L("Enable ESP / ABS / TC##esp", "Включить ESP / ABS / TC##esp"), &g_cfg.assist_enabled);
                    ImGui::SliderFloat(L("Start (km/h)##astart", "Старт (км/ч)##astart"), &g_cfg.assist_start_kmh, 20.0f, 120.0f, "%.0f");
                    ImGui::SliderFloat(L("ESP Yaw Damping", "Сила ESP (вращение)"), &g_cfg.esp_strength, 0.0f, 2.5f, "%.2f");
                    ImGui::SliderFloat(L("ESP Slip Align", "Сила ESP (боковой снос)"), &g_cfg.slip_esp_strength, 0.0f, 2.5f, "%.2f");
                    ImGui::SliderFloat(L("ABS Brake Boost", "Усиление на тормозе (ABS)"), &g_cfg.abs_brake_boost, 0.0f, 3.0f, "%.2f");
                    ImGui::SliderFloat(L("TC Throttle Boost", "Усиление на газу (TC)"), &g_cfg.tc_throttle_boost, 1.0f, 2.5f, "%.2f");
                    ImGui::SliderFloat(L("Max Yaw Decel (rad/s^2)", "Макс. гашение (рад/с^2)"), &g_cfg.max_yaw_decel, 0.5f, 5.0f, "%.1f");
                    ImGui::SliderFloat(L("ESP Speed Scrub (G)", "Потеря тяги ESP (G)"), &g_cfg.esp_brake_drag, 0.0f, 0.50f, "%.2f");
                    ImGui::SliderFloat(L("Brake Dive", "Клевок при торможении"), &g_cfg.brake_dive, 0.0f, 0.60f, "%.2f");
                    ImGui::SliderFloat(L("Speed Steer Damp", "Стабилизация руля (скорость)"), &g_cfg.speed_steer_damp, 0.0f, 2.0f, "%.2f");
                    ImGui::SliderFloat(L("Chassis Rumble (160+)", "Вибрация кузова (160+)"), &g_cfg.speed_rumble, 0.0f, 1.0f, "%.2f");
                }

                if (ImGui::CollapsingHeader(L("4. Tire Model (Pacejka + Kamm Circle)", "4. Шинная модель (Pacejka + Круг Камма)"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Checkbox(L("Enable Tire Model##tire", "Включить шинную модель##tire"), &g_cfg.tire_enabled);
                    ImGui::SliderFloat(L("Lateral Tire Grip", "Зацеп шин (боковой держак)"), &g_cfg.tire_grip, 0.0f, 1.50f, "%.2f");
                    ImGui::SliderFloat(L("Kamm Circle (Trail / Drift)", "Круг Камма (Trail / Занос)"), &g_cfg.kamm_strength, 0.0f, 1.50f, "%.2f");
                }
            }

            // Shared footer (both Basic and Advanced): mini-HUD scale, wheel button bind & reset
            ImGui::Separator();
            ImGui::SetNextItemWidth(160.0f);
            ImGui::SliderFloat(L("Mini-HUD Scale", "Размер мини-HUD"), &g_cfg.mini_hud_scale, 0.70f, 1.80f, "%.2fx");
            if (g_bindingJoyBtn) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", L("Press any wheel button...", "Нажми любую кнопку на руле..."));
                ImGui::SameLine();
                if (ImGui::Button(L("Cancel", "Отмена"))) g_bindingJoyBtn = false;
            } else {
                ImGui::Text(L("Menu key: b%d (Insert / F6)", "Кнопка меню: b%d (Insert / F6)"), g_cfg.menu_joy_btn);
                ImGui::SameLine();
                if (ImGui::Button(L("Bind Wheel Button", "Назначить на руле"))) g_bindingJoyBtn = true;
                ImGui::SameLine();
                if (ImGui::Button(L("Reset", "Сброс"))) {
                    reset_defaults();
                    g_resetMiniHudPos = true;
                }
            }
        }
        ImGui::End();
    }

    ImGui::Render();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

HRESULT WINAPI hkPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
    if (!g_stopFlag.load(std::memory_order_relaxed) && g_csInit) {
        EnterCriticalSection(&g_cs);

        poll_joy_and_hotkeys();

        bool paused = g_tel.paused.load(std::memory_order_relaxed);
        float kmh = std::fabs(g_tel.speed_mps.load(std::memory_order_relaxed)) * 3.6f;
        bool doBlur = g_cfg.blur_enabled && !paused && kmh > g_cfg.start_kmh && g_cfg.strength > 0.0001f;

        if (ensure_pipeline(sc)) {
            bool showMiniHud = g_cfg.mini_hud_enabled && !paused;
            if (doBlur || g_menuOpen || showMiniHud) {
                ID3D11Texture2D* backbuf = nullptr;
                if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuf))) && backbuf) {
                    ID3D11RenderTargetView* rtv = nullptr;
                    if (SUCCEEDED(g_dev->CreateRenderTargetView(backbuf, nullptr, &rtv)) && rtv) {
                        ID3D11RenderTargetView* oldRTV = nullptr;
                        ID3D11DepthStencilView* oldDSV = nullptr;
                        g_ctx->OMGetRenderTargets(1, &oldRTV, &oldDSV);

                        if (doBlur) {
                            apply_blur_to_target(backbuf, rtv, kmh);
                        }

                        if (g_menuOpen || showMiniHud) {
                            draw_imgui_menu(rtv, kmh);
                        } else if (g_imguiInit) {
                            ImGui::GetIO().MouseDrawCursor = false;
                        }

                        g_ctx->OMSetRenderTargets(1, &oldRTV, oldDSV);
                        safe_release(oldDSV);
                        safe_release(oldRTV);
                        safe_release(rtv);
                    }
                    safe_release(backbuf);
                }
            } else if (g_imguiInit) {
                ImGui::GetIO().MouseDrawCursor = false;
            }
        }

        LeaveCriticalSection(&g_cs);
    }

    return oPresent(sc, syncInterval, flags);
}

HRESULT WINAPI hkResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    if (g_csInit) {
        EnterCriticalSection(&g_cs);
        release_copy_resources();
        if (g_imguiInit) {
            ImGui_ImplDX11_InvalidateDeviceObjects();
        }
        LeaveCriticalSection(&g_cs);
    }
    HRESULT hr = oResizeBuffers(sc, count, w, h, fmt, flags);
    if (SUCCEEDED(hr) && sc) {
        ID3D11Texture2D* nextBB = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&nextBB))) && nextBB) {
            g_nextBackbuf = nextBB;
            nextBB->Release();
        }
    }
    return hr;
}

DWORD WINAPI init_thread_proc(LPVOID) {
    for (int i = 0; i < 100 && !g_stopFlag.load(); ++i) {
        if (GetModuleHandleA("d3d11.dll") && GetModuleHandleA("dxgi.dll")) break;
        Sleep(100);
    }
    if (g_stopFlag.load()) return 0;

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ETS2CarEnhanceDummyDX11";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
                                0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return 0;
    }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* dummySC = nullptr;
    ID3D11Device* dummyDev = nullptr;
    ID3D11DeviceContext* dummyCtx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &fl, 1, D3D11_SDK_VERSION, &sd, &dummySC, &dummyDev, nullptr, &dummyCtx);

    if (SUCCEEDED(hr) && dummySC) {
        void** vtable = *reinterpret_cast<void***>(dummySC);
        g_presentAddr = vtable[8];
        g_resizeAddr  = vtable[13];

        dummySC->Release();
        dummyDev->Release();
        dummyCtx->Release();

        if (!g_stopFlag.load()) {
            if (MH_CreateHook(g_presentAddr, reinterpret_cast<void*>(&hkPresent), reinterpret_cast<void**>(&oPresent)) == MH_OK &&
                MH_CreateHook(g_resizeAddr, reinterpret_cast<void*>(&hkResizeBuffers), reinterpret_cast<void**>(&oResizeBuffers)) == MH_OK) {
                if (MH_EnableHook(g_presentAddr) == MH_OK && MH_EnableHook(g_resizeAddr) == MH_OK) {
                    g_hooked.store(true);
                    plugin_log(SCS_LOG_TYPE_message, "[ets2_car_enhance] DX11 Present hook installed");
                }
            }
            if (MH_CreateHookApiEx(L"user32", "SetCursorPos", reinterpret_cast<void*>(&hkSetCursorPos), reinterpret_cast<void**>(&oSetCursorPos), &g_setCursorAddr) == MH_OK && g_setCursorAddr) {
                MH_EnableHook(g_setCursorAddr);
            }
            if (MH_CreateHookApiEx(L"user32", "ClipCursor", reinterpret_cast<void*>(&hkClipCursor), reinterpret_cast<void**>(&oClipCursor), &g_clipCursorAddr) == MH_OK && g_clipCursorAddr) {
                MH_EnableHook(g_clipCursorAddr);
            }
        }
    } else {
        plugin_log(SCS_LOG_TYPE_warning, "[ets2_car_enhance] Dummy DX11 device creation failed");
    }

    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return 0;
}

} // namespace

void dx11_blur_start() {
    g_stopFlag.store(false);
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }
    g_initThread = CreateThread(nullptr, 0, init_thread_proc, nullptr, 0, nullptr);
}

void dx11_blur_stop() {
    g_stopFlag.store(true);
    if (g_initThread) {
        WaitForSingleObject(g_initThread, 3000);
        CloseHandle(g_initThread);
        g_initThread = nullptr;
    }
    if (g_ctxHooked) {
        if (g_omSetRtAddr) { MH_DisableHook(g_omSetRtAddr); MH_RemoveHook(g_omSetRtAddr); }
        if (g_drawAddr)    { MH_DisableHook(g_drawAddr);    MH_RemoveHook(g_drawAddr);    }
        if (g_drawIdxAddr) { MH_DisableHook(g_drawIdxAddr); MH_RemoveHook(g_drawIdxAddr); }
        g_ctxHooked = false;
    }
    if (g_hooked.load()) {
        if (g_presentAddr)    { MH_DisableHook(g_presentAddr);    MH_RemoveHook(g_presentAddr);    }
        if (g_resizeAddr)     { MH_DisableHook(g_resizeAddr);     MH_RemoveHook(g_resizeAddr);     }
        if (g_setCursorAddr)  { MH_DisableHook(g_setCursorAddr);  MH_RemoveHook(g_setCursorAddr);  }
        if (g_clipCursorAddr) { MH_DisableHook(g_clipCursorAddr); MH_RemoveHook(g_clipCursorAddr); }
        g_hooked.store(false);
    }
    if (g_csInit) {
        EnterCriticalSection(&g_cs);
        release_all_d3d();
        LeaveCriticalSection(&g_cs);
        DeleteCriticalSection(&g_cs);
        g_csInit = false;
    }
}
