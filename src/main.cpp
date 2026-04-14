//=============================================================================
// SpecularLatens v1.00 — Defitive Lite Edition
// Duplicação de tela com baixa latência (DXGI Desktop Duplication + D3D11)
//
// Otimizações v1.00:
//  - GPU CURSOR OVERLAY: Cursor renderizado via D3D11 como segundo quad
//    com blend premultiplied alpha. Elimina Map/Unmap GPU sync (v3.3-v3.9
//    usava CPU blitting que causava pipeline stall a cada frame).
//    Elimina renderTex — frame renderizado direto de copyTex.
//  - CURSOR PRE-ALOCADO: Textura GPU grow-only (nunca destrói/recria).
//    UV scaling no shader compensa textura > cursor. Elimina ping-pong
//    DXGI/Win32 que recriava textura 32↔64 a cada frame (~14ms stall).
//  - VSYNC VBLANK-ALIGNED: VSync ON usa WaitForVBlank() no output destino
//    para frame delivery perfeitamente alinhado com o refresh do display.
//    Elimina jitter de timer que causava frame skipping percebido.
//  - HYBRID SPIN PACING: Low latency e VSync OFF usam sleep + SwitchToThread()
//    spin (500µs). SwitchToThread cede CPU para DWM e outros threads.
//  - LOW LATENCY PIPELINE: Pacing ANTES da captura (sleep-then-capture).
//    BufferCount=2, MaxFrameLatency=1 sempre.
//  - OPÇÃO "BAIXA LATÊNCIA": ALLOW_TEARING + timer ao Hz do monitor.
//    Bypass do DWM compositor no destino (~7ms a menos de lag).
//  - GPU SELECTION: Filtra adaptadores virtuais/software.
//  - PRIORIDADE via MMCSS "Pro Audio" CRITICAL + TIME_CRITICAL thread
//    + HIGH_PRIORITY_CLASS processo.
//  - FRAME TIMING LOG: min/max/avg frame time a cada 5s no log.
//  - SetThreadExecutionState para impedir throttling de display/sistema.
//  - FPS: timeBeginPeriod(1) para resolução de 1ms no sleep.
//  - FPS customizável: campo EDIT separado para digitar qualquer valor.
//  - Configurações persistentes via SpecularLatens.ini.
//  - Retorno ao menu ao fechar (sem PostQuitMessage no espelho).
//  - Cache de SRV. Sempre DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING.
//  - DIRTY RECTS: CopySubresourceRegion para regiões alteradas (GetFrameDirtyRects
//    + GetFrameMoveRects) em vez de CopyResource do frame inteiro. Reduz
//    bandwidth GPU significativamente quando só parte da tela muda.
//  - MONITOR PERSISTENCE: Seleção de monitor origem/destino salva no INI.
//  - ACCESS_LOST STARTUP GRACE: Retry rápido (1ms) nos primeiros ACCESS_LOST
//    (esperados na inicialização) para reduzir spike inicial.
//  - Ordem: CopyResource → ReleaseFrame → Render.
//  - Double buffer (BufferCount=2) na swap chain.
//=============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <timeapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <avrt.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <wrl/client.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <chrono>
#include <mutex>
#include <algorithm>
#include "resource.h"

using Microsoft::WRL::ComPtr;

// --- Debug Logging ---
static std::mutex g_logMtx;
static void Logn(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMtx);
    FILE* f = nullptr;
    if (fopen_s(&f, "SpecularLatens_Debug.log", "a") == 0) {
        auto now = std::chrono::system_clock::now();
        time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_info;
        localtime_s(&tm_info, &t);
        fprintf(f, "[%02d:%02d:%02d] ", tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\n");
        fclose(f);
    }
}

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "avrt.lib")

// =============================================================================
// Shaders HLSL
// =============================================================================
// Quad fullscreen do frame
static const char* s_vsScreen = R"(
struct VSOut { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
VSOut main(uint id:SV_VertexID) {
    VSOut o;
    o.uv  = float2((id&2)?1.0:0.0, (id&1)?0.0:1.0);
    o.pos = float4(o.uv * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
})";

static const char* s_psScreen = R"(
Texture2D tex:register(t0); SamplerState s:register(s0);
struct PSIn { float4 p:SV_Position; float2 uv:TEXCOORD0; };
float4 main(PSIn i):SV_Target { return tex.Sample(s, i.uv); })";

// Quad posicionado para cursor overlay (posição via constant buffer)
// UV scaling permite textura pre-alocada maior que o cursor real.
static const char* s_vsCursor = R"(
cbuffer CB : register(b0) { float4 rect; float4 uvInfo; };
struct VSOut { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
VSOut main(uint id:SV_VertexID) {
    VSOut o;
    float2 t = float2((id&2)?1.0:0.0, (id&1)?0.0:1.0);
    o.uv = t * uvInfo.xy;
    o.pos = float4(
        rect.x + t.x * (rect.z - rect.x),
        rect.y + t.y * (rect.w - rect.y),
        0, 1);
    return o;
})";

// =============================================================================
// Globais
// =============================================================================
// Render device (na GPU selecionada pelo usuário)
static ComPtr<IDXGIFactory1>          g_factory;
static ComPtr<ID3D11Device>           g_device;
static ComPtr<ID3D11DeviceContext>    g_ctx;
static ComPtr<IDXGISwapChain>         g_swapChain;
static ComPtr<ID3D11RenderTargetView> g_rtv;

// Capture device (na GPU da origem — pode ser == g_device se mesma GPU)
static ComPtr<ID3D11Device>        g_captureDevice;
static ComPtr<ID3D11DeviceContext> g_captureCtx;
static bool g_crossAdapter = false;

// Bridge staging (cross-adapter): na capture device, CPU-readable
static ComPtr<ID3D11Texture2D> g_bridgeStaging;
static UINT g_bridgeStagingW = 0, g_bridgeStagingH = 0;

static ComPtr<ID3D11VertexShader>   g_vsScreen;
static ComPtr<ID3D11PixelShader>    g_psScreen;
static ComPtr<ID3D11SamplerState>   g_sampler;       // POINT — usado pelo cursor
static ComPtr<ID3D11SamplerState>   g_samplerLinear; // LINEAR — usado pelo frame

static ComPtr<ID3D11ShaderResourceView> g_frameSrv;

// Frame texture (no render device):
// g_copyTex = frame do DXGI (cursor é renderizado como overlay, não composto)
static ComPtr<ID3D11Texture2D> g_copyTex;
static UINT        g_copyTexW   = 0;
static UINT        g_copyTexH   = 0;
static DXGI_FORMAT g_copyTexFmt = DXGI_FORMAT_UNKNOWN;

// Cursor: pixels em CPU (premultiplied alpha BGRA)
static std::vector<UINT32> g_cursorPixelsCPU;
static UINT  g_cursorPixelW = 0, g_cursorPixelH = 0;
static POINT g_cursorHotspot = {};

// Cursor GPU overlay resources (no render device)
static ComPtr<ID3D11VertexShader>       g_vsCursorShader;
static ComPtr<ID3D11BlendState>         g_cursorBlend;
static ComPtr<ID3D11Buffer>             g_cursorCB;
static ComPtr<ID3D11Texture2D>          g_cursorTex;
static ComPtr<ID3D11ShaderResourceView> g_cursorSrv;
static UINT g_cursorTexW = 0, g_cursorTexH = 0;
static bool g_cursorTexDirty = true; // flag: precisa atualizar textura GPU

// Output destino
static ComPtr<IDXGIOutput> g_dstOutput;
static bool g_tearingSupported = false;
static int  g_dstHz = 60; // refresh rate do monitor destino

static int  g_dstW = 0, g_dstH = 0;
static bool g_running = true;
static UINT g_exitKey = VK_ESCAPE;

static bool g_vsync = true;
static bool g_showCursor = true;
static int  g_targetFps = 240;
static bool g_lowLatency = false; // ALLOW_TEARING + timer (bypass DWM)

// GPU selection & priority
static int  g_renderGpuIdx = -1; // -1 = auto (mesma GPU da origem)
static bool g_highPriority = false;

// INI path
static wchar_t g_iniPath[MAX_PATH] = {};

// --- Adapter Info ---
struct AdapterInfo {
    std::wstring name;
    UINT idx;
    SIZE_T dedicatedVRAM; // bytes
};
static std::vector<AdapterInfo> g_adapters;

// --- Monitor Virtual ---
static bool g_vEnabled = false;
static std::atomic<bool> g_vBusy{ false };
static int  g_vResIdx = 2;
static int  g_vHzIdx = 0;
static const wchar_t* g_vResList[] = { L"1080p", L"1440p", L"4K", L"8K", L"Custom..." };
static const wchar_t* g_vHzList[]  = { L"60Hz", L"120Hz", L"144Hz", L"240Hz", L"480Hz", L"Custom..." };
static HWND g_hVResCustomW = nullptr, g_hVResCustomH = nullptr, g_hVHzCustom = nullptr;

// =============================================================================
// Monitor virtual (funções auxiliares — inalteradas)
// =============================================================================
static bool IsUserAdmin() {
    BOOL b = FALSE;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    PSID AdministratorsGroup;
    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                 &AdministratorsGroup)) {
        CheckTokenMembership(nullptr, AdministratorsGroup, &b);
        FreeSid(AdministratorsGroup);
    }
    return b == TRUE;
}

static bool DriverFilesExist() {
    return (GetFileAttributesW(L"drivers\\MttVDD.inf") != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(L"drivers\\MttVDD.dll") != INVALID_FILE_ATTRIBUTES);
}

static void RunCmdInternal(const wchar_t* cmd) {
    wchar_t tempFile[MAX_PATH];
    GetTempPathW(MAX_PATH, tempFile);
    wcscat_s(tempFile, L"sm_trace.txt");
    wchar_t fullCmd[1024];
    swprintf_s(fullCmd, L"cmd.exe /c \"%ls > \"%ls\" 2>&1\"", cmd, tempFile);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, fullCmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        Logn("RunCmd: ExitCode: %d", exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        std::ifstream f(tempFile);
        if (f.is_open()) {
            std::string line;
            Logn("--- Saida do Comando ---");
            while (std::getline(f, line)) {
                if (!line.empty()) Logn("  > %s", line.c_str());
            }
            Logn("--- Fim da Saida ---");
            f.close();
        }
        DeleteFileW(tempFile);
    } else {
        Logn("RunCmd: FALHA. Error: %d", GetLastError());
    }
}

static void RunCmd(const wchar_t* cmd) {
    __try { RunCmdInternal(cmd); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Logn("RunCmd: EXCECAO SEH!"); }
}

static std::wstring GetPnpPath() {
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring pnp = sysDir;
    if (pnp.find(L"SysWOW64") != std::wstring::npos)
        return L"C:\\Windows\\Sysnative\\pnputil.exe";
    return pnp + L"\\pnputil.exe";
}

static bool IsVirtualDisplayStarted() {
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    for (int i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++) {
        if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
            if (wcsstr(dd.DeviceString, L"MttVDD")) return true;
        }
    }
    return false;
}

static void UpdateVirtualRegistry() {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WUDF\\Services\\MttVDD\\Adapter0",
                        0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        wchar_t mode[256];
        wchar_t res[64] = L"3840,2160";
        int hz = 60;
        if (g_vResIdx == 4) {
            wchar_t w[16] = {}, h[16] = {};
            if (g_hVResCustomW) GetWindowTextW(g_hVResCustomW, w, 16);
            if (g_hVResCustomH) GetWindowTextW(g_hVResCustomH, h, 16);
            swprintf_s(res, L"%s,%s", w, h);
        } else {
            if (g_vResIdx == 0) wcscpy_s(res, L"1920,1080");
            else if (g_vResIdx == 1) wcscpy_s(res, L"2560,1440");
            else if (g_vResIdx == 2) wcscpy_s(res, L"3840,2160");
            else if (g_vResIdx == 3) wcscpy_s(res, L"7680,4320");
        }
        if (g_vHzIdx == 5) {
            wchar_t f[16] = {};
            if (g_hVHzCustom) GetWindowTextW(g_hVHzCustom, f, 16);
            hz = _wtoi(f);
        } else {
            if (g_vHzIdx == 1) hz = 120;
            else if (g_vHzIdx == 2) hz = 144;
            else if (g_vHzIdx == 3) hz = 240;
            else if (g_vHzIdx == 4) hz = 480;
        }
        swprintf_s(mode, L"%s,%d", res, hz);
        RegSetValueExW(hKey, L"SupportedModes", 0, REG_SZ,
                       (BYTE*)mode, (DWORD)(wcslen(mode) + 1) * 2);
        RegCloseKey(hKey);
    }
}

static void ToggleVirtualDisplayInternal(bool enable, HWND hNotify) {
    std::wstring pnp = GetPnpPath();
    if (enable) {
        UpdateVirtualRegistry();
        wchar_t cmdEnable[512], cmdScan[512];
        swprintf_s(cmdEnable, L"\"%ls\" /enable-device \"Root\\MttVDD\"", pnp.c_str());
        swprintf_s(cmdScan, L"\"%ls\" /scan-devices", pnp.c_str());
        RunCmd(cmdEnable);
        RunCmd(cmdScan);
        if (g_vEnabled) {
            wchar_t cmdDisable[512];
            swprintf_s(cmdDisable, L"\"%ls\" /disable-device \"Root\\MttVDD\"", pnp.c_str());
            RunCmd(cmdDisable);
            Sleep(1000);
            RunCmd(cmdEnable);
        }
        g_vEnabled = true;
        Sleep(12000);
    } else {
        wchar_t cmdDisable[512];
        swprintf_s(cmdDisable, L"\"%ls\" /disable-device \"Root\\MttVDD\"", pnp.c_str());
        RunCmd(cmdDisable);
        g_vEnabled = false;
        Sleep(1000);
    }
    g_vBusy = false;
    PostMessageW(hNotify, WM_COMMAND, 102, 0);
}

static void ToggleVirtualDisplay(bool enable, HWND hNotify) {
    if (g_vBusy) return;
    g_vBusy = true;
    std::thread([enable, hNotify]() {
        __try { ToggleVirtualDisplayInternal(enable, hNotify); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Logn("Thread VirtualMonitor: EXCECAO SEH!");
            g_vBusy = false;
        }
    }).detach();
}

// =============================================================================
// Utilitários
// =============================================================================
static void Fatal(const char* msg, HRESULT hr = 0) {
    char buf[512];
    if (hr) sprintf_s(buf, "%s\nHRESULT: 0x%08X", msg, (unsigned)hr);
    else    strcpy_s(buf, msg);
    MessageBoxA(nullptr, buf, "SpecularLatens", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

static ComPtr<ID3DBlob> CompileShader(const char* src, const char* entry,
                                       const char* profile) {
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                            0, &code, &err);
    if (FAILED(hr)) {
        std::string m = "Shader error:\n";
        if (err) m += (const char*)err->GetBufferPointer();
        Fatal(m.c_str(), hr);
    }
    return code;
}

// =============================================================================
// Persistência de configurações (INI)
// =============================================================================
// Monitor indices (declarados antes de SaveSettings/LoadSettings para referência)
static int  g_srcIdx = 0, g_dstIdx = 1;

static void InitIniPath() {
    GetModuleFileNameW(nullptr, g_iniPath, MAX_PATH);
    wchar_t* slash = wcsrchr(g_iniPath, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat_s(g_iniPath, L"SpecularLatens.ini");
}

static void SaveSettings() {
    auto writeInt = [](const wchar_t* key, int val) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d", val);
        WritePrivateProfileStringW(L"Settings", key, buf, g_iniPath);
    };
    writeInt(L"VSync", g_vsync ? 1 : 0);
    writeInt(L"ShowCursor", g_showCursor ? 1 : 0);
    writeInt(L"TargetFPS", g_targetFps);
    writeInt(L"ExitKey", (int)g_exitKey);
    writeInt(L"RenderGPU", g_renderGpuIdx);
    writeInt(L"HighPriority", g_highPriority ? 1 : 0);
    writeInt(L"LowLatency", g_lowLatency ? 1 : 0);
    writeInt(L"SrcMonitor", g_srcIdx);
    writeInt(L"DstMonitor", g_dstIdx);
    Logn("SaveSettings: vsync=%d cursor=%d fps=%d key=%u gpu=%d priority=%d lowlat=%d src=%d dst=%d",
         g_vsync, g_showCursor, g_targetFps, g_exitKey, g_renderGpuIdx, g_highPriority, g_lowLatency,
         g_srcIdx, g_dstIdx);
}

static void LoadSettings() {
    if (GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES) return;
    g_vsync        = GetPrivateProfileIntW(L"Settings", L"VSync", 1, g_iniPath) != 0;
    g_showCursor   = GetPrivateProfileIntW(L"Settings", L"ShowCursor", 1, g_iniPath) != 0;
    g_targetFps    = GetPrivateProfileIntW(L"Settings", L"TargetFPS", 240, g_iniPath);
    g_exitKey      = (UINT)GetPrivateProfileIntW(L"Settings", L"ExitKey", VK_ESCAPE, g_iniPath);
    g_renderGpuIdx = GetPrivateProfileIntW(L"Settings", L"RenderGPU", -1, g_iniPath);
    g_highPriority = GetPrivateProfileIntW(L"Settings", L"HighPriority", 0, g_iniPath) != 0;
    g_lowLatency   = GetPrivateProfileIntW(L"Settings", L"LowLatency", 0, g_iniPath) != 0;
    g_srcIdx       = GetPrivateProfileIntW(L"Settings", L"SrcMonitor", 0, g_iniPath);
    g_dstIdx       = GetPrivateProfileIntW(L"Settings", L"DstMonitor", 1, g_iniPath);
    Logn("LoadSettings: vsync=%d cursor=%d fps=%d key=%u gpu=%d priority=%d lowlat=%d src=%d dst=%d",
         g_vsync, g_showCursor, g_targetFps, g_exitKey, g_renderGpuIdx, g_highPriority, g_lowLatency,
         g_srcIdx, g_dstIdx);
}

// =============================================================================
// Enumeração de monitores e adaptadores
// =============================================================================
struct DisplayInfo {
    HMONITOR     hMon;
    RECT         rect;
    bool         isPrimary;
    std::wstring name;
    UINT         adapterIdx;
    UINT         outputIdx;
};
static std::vector<DisplayInfo> g_displays;
static bool g_selOk  = false;

static void EnsureFrameCopyTexture(UINT w, UINT h, DXGI_FORMAT fmt) {
    if (g_copyTex && g_copyTexW == w && g_copyTexH == h && g_copyTexFmt == fmt)
        return;
    g_copyTex.Reset();
    g_frameSrv.Reset();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = fmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = g_device->CreateTexture2D(&td, nullptr, &g_copyTex);
    if (FAILED(hr)) {
        Logn("EnsureFrameCopyTexture: FALHA 0x%08X", hr);
        return;
    }
    g_copyTexW = w; g_copyTexH = h; g_copyTexFmt = fmt;
    Logn("EnsureFrameCopyTexture: %ux%u fmt=%u", w, h, (UINT)fmt);
}

// Bridge staging: no capture device, para cross-adapter
static void EnsureBridgeStaging(UINT w, UINT h, DXGI_FORMAT fmt) {
    if (g_bridgeStaging && g_bridgeStagingW == w && g_bridgeStagingH == h)
        return;
    g_bridgeStaging.Reset();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = fmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_STAGING;
    td.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
    HRESULT hr = g_captureDevice->CreateTexture2D(&td, nullptr, &g_bridgeStaging);
    if (SUCCEEDED(hr)) {
        g_bridgeStagingW = w; g_bridgeStagingH = h;
        Logn("EnsureBridgeStaging: %ux%u fmt=%u", w, h, (UINT)fmt);
    } else {
        Logn("EnsureBridgeStaging: FALHA 0x%08X", hr);
    }
}

static void ResetGraphicsStack() {
    Logn("ResetGraphicsStack: limpando recursos...");
    g_dstOutput.Reset();
    g_dstHz = 60;
    g_frameSrv.Reset();
    g_copyTex.Reset();
    g_copyTexW = g_copyTexH = 0;
    g_copyTexFmt = DXGI_FORMAT_UNKNOWN;
    g_cursorTex.Reset();
    g_cursorSrv.Reset();
    g_cursorCB.Reset();
    g_cursorBlend.Reset();
    g_vsCursorShader.Reset();
    g_cursorTexW = g_cursorTexH = 0;
    g_cursorTexDirty = true;
    g_cursorPixelsCPU.clear();
    g_cursorPixelW = g_cursorPixelH = 0;
    g_cursorHotspot = {};
    g_bridgeStaging.Reset();
    g_bridgeStagingW = g_bridgeStagingH = 0;
    g_captureDevice.Reset();
    g_captureCtx.Reset();
    g_crossAdapter = false;
    g_factory.Reset();
    g_device.Reset();
    g_ctx.Reset();
    g_swapChain.Reset();
    g_rtv.Reset();
    g_vsScreen.Reset();
    g_psScreen.Reset();
    g_sampler.Reset();
    g_samplerLinear.Reset();
    g_displays.clear();
    g_adapters.clear();
}

static void EnumerateDisplays() {
    Logn("EnumerateDisplays...");
    ResetGraphicsStack();

    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&g_factory));
    if (FAILED(hr)) { Logn("FALHA DXGI Factory: 0x%08X", hr); return; }

    UINT aIdx = 0;
    ComPtr<IDXGIAdapter1> adapter;
    while (g_factory->EnumAdapters1(aIdx, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 ad;
        adapter->GetDesc1(&ad);
        Logn("Adaptador [%d]: %ls (VRAM: %zu MB, Flags: 0x%X)", aIdx, ad.Description,
             ad.DedicatedVideoMemory / (1024 * 1024), ad.Flags);

        // Filtrar adaptadores de software/virtuais
        bool isSoftware = (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        bool isBasic = (wcsstr(ad.Description, L"Microsoft Basic") != nullptr);
        bool isRemote = (wcsstr(ad.Description, L"Microsoft Remote") != nullptr);
        bool isVirtual = (wcsstr(ad.Description, L"Virtual") != nullptr &&
                          ad.DedicatedVideoMemory == 0);

        // Verificar duplicata (mesmo nome)
        bool isDuplicate = false;
        for (const auto& existing : g_adapters) {
            if (existing.name == ad.Description) {
                isDuplicate = true; break;
            }
        }

        bool skip = isSoftware || isBasic || isRemote || isVirtual || isDuplicate;
        if (skip) {
            Logn("  -> FILTRADO (sw=%d basic=%d remote=%d virtual=%d dup=%d)",
                 isSoftware, isBasic, isRemote, isVirtual, isDuplicate);
        } else {
            AdapterInfo ai;
            ai.name = ad.Description;
            ai.idx = aIdx;
            ai.dedicatedVRAM = ad.DedicatedVideoMemory;
            g_adapters.push_back(ai);
        }

        // Enumerar outputs mesmo de adaptadores filtrados (para lista de monitores)
        UINT oIdx = 0;
        ComPtr<IDXGIOutput> out;
        while (adapter->EnumOutputs(oIdx, &out) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC od;
            out->GetDesc(&od);
            DisplayInfo info;
            info.hMon       = od.Monitor;
            info.name       = od.DeviceName;
            info.rect       = od.DesktopCoordinates;
            info.adapterIdx = aIdx;
            info.outputIdx  = oIdx;
            info.isPrimary  = (od.DesktopCoordinates.left == 0 &&
                               od.DesktopCoordinates.top == 0);
            g_displays.push_back(info);
            Logn("  Saida [%d]: %ls (%dx%d)", oIdx, od.DeviceName,
                 od.DesktopCoordinates.right - od.DesktopCoordinates.left,
                 od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
            oIdx++;
            out.Reset();
        }
        aIdx++;
        adapter.Reset();
    }
    Logn("EnumerateDisplays: %zu monitores, %zu GPUs.",
         g_displays.size(), g_adapters.size());
}

// =============================================================================
// Cursor: captura de pixels via "draw on black + draw on white"
// =============================================================================
static void UpdateCursorShapeFromSystem() {
    CURSORINFO ci = { sizeof(ci) };
    if (!GetCursorInfo(&ci) || !ci.hCursor) return;

    ICONINFO ii;
    if (!GetIconInfo(ci.hCursor, &ii)) return;

    POINT hotspot = { (LONG)ii.xHotspot, (LONG)ii.yHotspot };

    BITMAP bm;
    GetObject(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
    int w = bm.bmWidth;
    int h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;

    if (w <= 0 || h <= 0) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
        return;
    }

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bitsBlack = nullptr;
    HBITMAP hbmpBlack = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS,
                                         &bitsBlack, nullptr, 0);
    if (!hbmpBlack) {
        DeleteDC(hdcMem); ReleaseDC(nullptr, hdcScreen);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
        return;
    }
    HGDIOBJ old = SelectObject(hdcMem, hbmpBlack);
    memset(bitsBlack, 0x00, w * h * 4);
    DrawIconEx(hdcMem, 0, 0, ci.hCursor, w, h, 0, nullptr, DI_NORMAL);
    GdiFlush();
    SelectObject(hdcMem, old);

    void* bitsWhite = nullptr;
    HBITMAP hbmpWhite = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS,
                                         &bitsWhite, nullptr, 0);
    if (!hbmpWhite) {
        DeleteObject(hbmpBlack);
        DeleteDC(hdcMem); ReleaseDC(nullptr, hdcScreen);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
        return;
    }
    old = SelectObject(hdcMem, hbmpWhite);
    memset(bitsWhite, 0xFF, w * h * 4);
    DrawIconEx(hdcMem, 0, 0, ci.hCursor, w, h, 0, nullptr, DI_NORMAL);
    GdiFlush();
    SelectObject(hdcMem, old);

    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    UINT32* pxB = (UINT32*)bitsBlack;
    UINT32* pxW = (UINT32*)bitsWhite;
    std::vector<UINT32> pixels(w * h);
    bool anyVisible = false;

    for (int i = 0; i < w * h; i++) {
        UINT32 cb = pxB[i];
        UINT32 cw = pxW[i];

        int bB = cb & 0xFF, gB = (cb >> 8) & 0xFF, rB = (cb >> 16) & 0xFF;
        int bW = cw & 0xFF, gW = (cw >> 8) & 0xFF, rW = (cw >> 16) & 0xFF;

        int aR = 255 - (rW - rB);
        int aG = 255 - (gW - gB);
        int aB = 255 - (bW - bB);
        int alpha = (std::min)({aR, aG, aB});
        if (alpha < 0)   alpha = 0;
        if (alpha > 255) alpha = 255;

        if (alpha > 0) anyVisible = true;
        pixels[i] = ((UINT32)alpha << 24) | (cb & 0x00FFFFFF);
    }

    DeleteObject(hbmpBlack);
    DeleteObject(hbmpWhite);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);

    if (!anyVisible) {
        Logn("UpdateCursorShapeFromSystem: FALLBACK cursor solido");
        w = 24; h = 24;
        pixels.resize(w * h, 0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (x <= y && x < w / 2)
                    pixels[y * w + x] = 0xFF00FF00;
        hotspot = { 0, 0 };
    }

    g_cursorPixelsCPU = std::move(pixels);
    g_cursorPixelW = (UINT)w;
    g_cursorPixelH = (UINT)h;
    g_cursorHotspot = hotspot;

    Logn("UpdateCursorShapeFromSystem: %dx%d hotspot=(%d,%d) visible=%d",
         w, h, hotspot.x, hotspot.y, anyVisible);
}

static void BuildCursorPixelsFromDXGI(const std::vector<BYTE>& data,
                                       const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info) {
    g_cursorHotspot = { (LONG)info.HotSpot.x, (LONG)info.HotSpot.y };
    UINT w = info.Width;
    UINT h = info.Height;
    bool mono = (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME);
    if (mono) h /= 2;
    if (!w || !h) return;

    std::vector<UINT32> px(w * h, 0);

    if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        for (UINT y = 0; y < h; y++)
            memcpy(px.data() + y * w, data.data() + y * info.Pitch, w * 4);
    }
    else if (mono) {
        // Monochrome cursor: AND mask + XOR mask
        // AND=0,XOR=0 → preto opaco; AND=0,XOR=1 → branco opaco;
        // AND=1,XOR=0 → transparente; AND=1,XOR=1 → inversão (XOR com tela).
        // Não é possível fazer XOR real com overlay blend. Aproximação:
        // inversão → branco semi-transparente (visível em fundos claros e escuros).
        UINT mp = (w + 7) / 8;
        const BYTE* andM = data.data();
        const BYTE* xorM = data.data() + mp * h;
        for (UINT y = 0; y < h; y++)
            for (UINT x = 0; x < w; x++) {
                BYTE a  = (andM[y * mp + x / 8] >> (7 - x % 8)) & 1;
                BYTE xr = (xorM[y * mp + x / 8] >> (7 - x % 8)) & 1;
                if      (!a && !xr) px[y * w + x] = 0xFF000000; // preto opaco
                else if (!a &&  xr) px[y * w + x] = 0xFFFFFFFF; // branco opaco
                else if ( a && !xr) px[y * w + x] = 0x00000000; // transparente
                else                px[y * w + x] = 0x80FFFFFF; // inversão → branco semi-transparente (premultiplied)
            }
    }
    else {
        // MASKED_COLOR: pixels com alpha=0xFF são cor direta;
        // pixels com alpha=0x00 são XOR (inversão com cor).
        // Aproximação: XOR → semi-transparente com a cor invertida.
        for (UINT y = 0; y < h; y++) {
            const UINT32* src = (const UINT32*)(data.data() + y * info.Pitch);
            for (UINT x = 0; x < w; x++) {
                if (src[x] & 0xFF000000)
                    px[y * w + x] = (src[x] & 0x00FFFFFF) | 0xFF000000;
                else {
                    // XOR pixel: inverter cor e renderizar semi-transparente
                    UINT32 rgb = src[x] & 0x00FFFFFF;
                    if (rgb == 0) {
                        px[y * w + x] = 0x00000000; // XOR preto = sem mudança = transparente
                    } else {
                        // Premultiplied alpha ~50%: escalar RGB por 0.5
                        UINT32 r = ((rgb >> 16) & 0xFF) / 2;
                        UINT32 g = ((rgb >>  8) & 0xFF) / 2;
                        UINT32 b = ((rgb      ) & 0xFF) / 2;
                        px[y * w + x] = 0x80000000 | (r << 16) | (g << 8) | b;
                    }
                }
            }
        }
    }

    g_cursorPixelsCPU = std::move(px);
    g_cursorPixelW = w;
    g_cursorPixelH = h;
}

// =============================================================================
// GPU cursor overlay: atualiza textura do cursor na GPU quando shape muda.
// Textura é pre-alocada (grow-only, nunca destrói) para evitar stalls de
// criação/destruição. UV scaling no shader compensa textura > cursor.
// =============================================================================
static void UpdateCursorGPUTexture() {
    if (g_cursorPixelsCPU.empty() || !g_cursorPixelW || !g_cursorPixelH || !g_device) return;

    // Grow-only: só recriar se cursor é MAIOR que textura atual
    bool needCreate = !g_cursorTex ||
                      g_cursorPixelW > g_cursorTexW ||
                      g_cursorPixelH > g_cursorTexH;

    if (needCreate) {
        g_cursorTex.Reset();
        g_cursorSrv.Reset();

        // Alocar com folga: arredondar para múltiplo de 64, mínimo 64
        UINT allocW = ((std::max(g_cursorPixelW, g_cursorTexW) + 63) / 64) * 64;
        UINT allocH = ((std::max(g_cursorPixelH, g_cursorTexH) + 63) / 64) * 64;
        if (allocW < 64) allocW = 64;
        if (allocH < 64) allocH = 64;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width            = allocW;
        td.Height           = allocH;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = g_device->CreateTexture2D(&td, nullptr, &g_cursorTex);
        if (FAILED(hr)) {
            Logn("UpdateCursorGPUTexture: CreateTexture2D FALHA 0x%08X", hr);
            return;
        }
        hr = g_device->CreateShaderResourceView(g_cursorTex.Get(), nullptr, &g_cursorSrv);
        if (FAILED(hr)) {
            Logn("UpdateCursorGPUTexture: CreateSRV FALHA 0x%08X", hr);
            g_cursorTex.Reset();
            return;
        }
        g_cursorTexW = allocW;
        g_cursorTexH = allocH;
        Logn("UpdateCursorGPUTexture: Alocado %ux%u (cursor %ux%u)",
             allocW, allocH, g_cursorPixelW, g_cursorPixelH);
    }

    // Upload pixels com padding transparente (preenche textura inteira)
    std::vector<UINT32> padded(g_cursorTexW * g_cursorTexH, 0);
    for (UINT y = 0; y < g_cursorPixelH && y < g_cursorTexH; y++)
        memcpy(padded.data() + y * g_cursorTexW,
               g_cursorPixelsCPU.data() + y * g_cursorPixelW,
               g_cursorPixelW * 4);

    g_ctx->UpdateSubresource(g_cursorTex.Get(), 0, nullptr,
                              padded.data(), g_cursorTexW * 4, 0);
    g_cursorTexDirty = false;
}

// =============================================================================
// Janela de seleção
// =============================================================================
static HWND g_hSrcList = nullptr, g_hDstList = nullptr;
static HFONT g_hFont = nullptr;

static void RefreshDisplayList(HWND hw) {
    EnumerateDisplays();
    SendMessageW(g_hSrcList, LB_RESETCONTENT, 0, 0);
    SendMessageW(g_hDstList, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_displays.size(); i++) {
        const auto& d = g_displays[i];
        int w = d.rect.right - d.rect.left;
        int h = d.rect.bottom - d.rect.top;
        wchar_t buf[256];
        swprintf_s(buf, L"Tela %zu: %dx%d%s", i + 1, w, h,
                   d.isPrimary ? L" [Principal]" : L"");
        SendMessageW(g_hSrcList, LB_ADDSTRING, 0, (LPARAM)buf);
        SendMessageW(g_hDstList, LB_ADDSTRING, 0, (LPARAM)buf);
    }
    int src = (g_srcIdx < (int)g_displays.size()) ? g_srcIdx : 0;
    int dst = (g_dstIdx < (int)g_displays.size()) ? g_dstIdx
              : ((int)g_displays.size() > 1 ? 1 : 0);
    SendMessageW(g_hSrcList, LB_SETCURSEL, src, 0);
    SendMessageW(g_hDstList, LB_SETCURSEL, dst, 0);

    // Atualizar combo de GPU
    HWND hGpuCombo = GetDlgItem(hw, 108);
    if (hGpuCombo) {
        SendMessageW(hGpuCombo, CB_RESETCONTENT, 0, 0);
        SendMessageW(hGpuCombo, CB_ADDSTRING, 0, (LPARAM)L"Auto (GPU da origem)");
        for (size_t i = 0; i < g_adapters.size(); i++) {
            wchar_t buf[256];
            SIZE_T mb = g_adapters[i].dedicatedVRAM / (1024 * 1024);
            swprintf_s(buf, L"%ls (%zu MB)", g_adapters[i].name.c_str(), mb);
            SendMessageW(hGpuCombo, CB_ADDSTRING, 0, (LPARAM)buf);
        }
        // Selecionar GPU salva
        int gpuSel = 0; // Auto
        if (g_renderGpuIdx >= 0) {
            for (size_t i = 0; i < g_adapters.size(); i++) {
                if ((int)g_adapters[i].idx == g_renderGpuIdx) {
                    gpuSel = (int)i + 1;
                    break;
                }
            }
        }
        SendMessageW(hGpuCombo, CB_SETCURSEL, gpuSel, 0);
    }
}

enum {
    ID_BTN_START    = 100,
    ID_BTN_EXIT     = 101,
    ID_BTN_REFRESH  = 102,
    ID_COMBO_KEY    = 103,
    ID_CHK_VSYNC    = 104,
    ID_CHK_CURSOR   = 105,
    ID_COMBO_FPS    = 106,
    ID_EDIT_FPS     = 107,
    ID_COMBO_GPU    = 108,
    ID_CHK_PRIORITY = 109,
    ID_CHK_LOWLAT   = 110,
};

static LRESULT CALLBACK SelProcInternal(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND) {
        int id = LOWORD(wp);
        int notif = HIWORD(wp);

        // Quando o usuário seleciona um preset no combo de FPS, limpar o campo EDIT
        // para que o preset tenha efeito (senão o EDIT antigo sobrescreve).
        if (id == ID_COMBO_FPS && notif == CBN_SELCHANGE) {
            HWND hFpsEdit = GetDlgItem(hw, ID_EDIT_FPS);
            if (hFpsEdit) SetWindowTextW(hFpsEdit, L"");
            return 0;
        }

        if (id == ID_BTN_START) {
            if (g_vBusy) {
                MessageBoxW(hw, L"Aguarde o monitor virtual.",
                            L"SpecularLatens", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            int s = (int)SendMessageW(g_hSrcList, LB_GETCURSEL, 0, 0);
            int d = (int)SendMessageW(g_hDstList, LB_GETCURSEL, 0, 0);
            if (s == LB_ERR || d == LB_ERR) {
                MessageBoxW(hw, L"Selecione ORIGEM e DESTINO.",
                            L"SpecularLatens", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (s == d) {
                MessageBoxW(hw, L"Origem e destino devem ser diferentes.",
                            L"SpecularLatens", MB_OK | MB_ICONEXCLAMATION);
                return 0;
            }

            // Atalho de saída
            HWND hCombo = GetDlgItem(hw, ID_COMBO_KEY);
            int curKey = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
            if (curKey == 0) g_exitKey = VK_ESCAPE;
            else if (curKey == 1) g_exitKey = VK_F4;
            else if (curKey == 2) g_exitKey = VK_F12;
            else if (curKey == 3) g_exitKey = VK_END;
            else if (curKey == 4) g_exitKey = VK_HOME;

            // Checkboxes
            HWND hChkV = GetDlgItem(hw, ID_CHK_VSYNC);
            HWND hChkC = GetDlgItem(hw, ID_CHK_CURSOR);
            HWND hChkP = GetDlgItem(hw, ID_CHK_PRIORITY);
            HWND hChkLL = GetDlgItem(hw, ID_CHK_LOWLAT);
            g_vsync        = (hChkV && SendMessageW(hChkV, BM_GETCHECK, 0, 0) == BST_CHECKED);
            g_showCursor   = (hChkC && SendMessageW(hChkC, BM_GETCHECK, 0, 0) == BST_CHECKED);
            g_highPriority = (hChkP && SendMessageW(hChkP, BM_GETCHECK, 0, 0) == BST_CHECKED);
            g_lowLatency   = (hChkLL && SendMessageW(hChkLL, BM_GETCHECK, 0, 0) == BST_CHECKED);

            // GPU
            HWND hGpuCombo = GetDlgItem(hw, ID_COMBO_GPU);
            if (hGpuCombo) {
                int gpuSel = (int)SendMessageW(hGpuCombo, CB_GETCURSEL, 0, 0);
                if (gpuSel <= 0) {
                    g_renderGpuIdx = -1; // Auto
                } else {
                    int adapterListIdx = gpuSel - 1;
                    if (adapterListIdx < (int)g_adapters.size())
                        g_renderGpuIdx = (int)g_adapters[adapterListIdx].idx;
                    else
                        g_renderGpuIdx = -1;
                }
            }

            // FPS
            HWND hFpsEdit = GetDlgItem(hw, ID_EDIT_FPS);
            HWND hFpsCombo = GetDlgItem(hw, ID_COMBO_FPS);
            bool usedCustom = false;
            if (hFpsEdit) {
                wchar_t buf[32] = {};
                GetWindowTextW(hFpsEdit, buf, 32);
                int v = _wtoi(buf);
                if (v > 0) { g_targetFps = v; usedCustom = true; }
            }
            if (!usedCustom && hFpsCombo) {
                int sel = (int)SendMessageW(hFpsCombo, CB_GETCURSEL, 0, 0);
                switch (sel) {
                    case 0: g_targetFps = 60;  break;
                    case 1: g_targetFps = 120; break;
                    case 2: g_targetFps = 144; break;
                    case 3: g_targetFps = 240; break;
                    case 4: g_targetFps = 360; break;
                    case 5: g_targetFps = 0;   break;
                    default: g_targetFps = 240;
                }
            }

            Logn("Iniciando: vsync=%d showCursor=%d targetFps=%d exitKey=%u gpu=%d priority=%d lowlat=%d",
                 g_vsync, g_showCursor, g_targetFps, g_exitKey, g_renderGpuIdx, g_highPriority, g_lowLatency);

            g_srcIdx = s; g_dstIdx = d; g_selOk = true;
            SaveSettings();
            DestroyWindow(hw);
        }
        if (id == ID_BTN_EXIT) { g_selOk = false; DestroyWindow(hw); }
        if (id == ID_BTN_REFRESH) RefreshDisplayList(hw);
    }

    if (msg == WM_CTLCOLORSTATIC) {
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

static LRESULT CALLBACK SelProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    __try { return SelProcInternal(hw, msg, wp, lp); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Logn("CRITICAL: SEH in SelProc! 0x%08X", GetExceptionCode());
        return 0;
    }
}

static bool ShowSelectionUI(HINSTANCE hInst) {
    LoadSettings();

    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SelProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SMSel";

    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_APP_ICON));
    wc.hIconSm = LoadIcon(hInst, MAKEINTRESOURCE(IDI_APP_ICON));

    RegisterClassExW(&wc);

    int winW = 740, winH = 810;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND hw = CreateWindowExW(0, L"SMSel",
        L"SpecularLatens v1.00 \u2014 Definitive Lite Edition",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        (screenW - winW) / 2, (screenH - winH) / 2, winW, winH,
        nullptr, nullptr, hInst, nullptr);

    // Ícone da janela
    SendMessage(hw, WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(hInst, MAKEINTRESOURCE(IDI_APP_ICON)));
    SendMessage(hw, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon(hInst, MAKEINTRESOURCE(IDI_APP_ICON)));

    g_hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");

    auto CreateLbl = [&](const wchar_t* txt, int x, int y, int w, int h) {
        HWND hL = CreateWindowExW(0, L"STATIC", txt,
            WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, w, h,
            hw, nullptr, hInst, nullptr);
        SendMessageW(hL, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        return hL;
    };

    CreateLbl(L"1. SELECIONE A ORIGEM (Capturar desta tela):", 30, 20, 320, 20);
    CreateLbl(L"2. SELECIONE O DESTINO (Exibir nesta tela):", 370, 20, 320, 20);

    g_hSrcList = CreateWindowExW(0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | WS_BORDER,
        30, 45, 330, 280, hw, nullptr, hInst, nullptr);
    g_hDstList = CreateWindowExW(0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | WS_BORDER,
        370, 45, 330, 280, hw, nullptr, hInst, nullptr);
    SendMessageW(g_hSrcList, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(g_hDstList, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // Atalho de saída
    CreateLbl(L"Atalho de Sa\u00EDda:", 30, 340, 120, 20);
    HWND hComboKey = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        155, 337, 100, 200, hw, (HMENU)ID_COMBO_KEY, hInst, nullptr);
    SendMessageW(hComboKey, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"ESC");
    SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"F4");
    SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"F12");
    SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"END");
    SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"HOME");
    int keySel = 0;
    if (g_exitKey == VK_F4) keySel = 1;
    else if (g_exitKey == VK_F12) keySel = 2;
    else if (g_exitKey == VK_END) keySel = 3;
    else if (g_exitKey == VK_HOME) keySel = 4;
    SendMessageW(hComboKey, CB_SETCURSEL, keySel, 0);

    // Renderização
    CreateLbl(L"Renderiza\u00E7\u00E3o:", 30, 375, 120, 20);

    HWND hChkVsync = CreateWindowExW(0, L"BUTTON",
        L"VSync (FPS est\u00E1vel, lat\u00EAncia +1 frame)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        30, 398, 320, 22, hw, (HMENU)ID_CHK_VSYNC, hInst, nullptr);
    SendMessageW(hChkVsync, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(hChkVsync, BM_SETCHECK, g_vsync ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND hChkCursor = CreateWindowExW(0, L"BUTTON",
        L"Exibir cursor no espelhamento",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        30, 424, 280, 22, hw, (HMENU)ID_CHK_CURSOR, hInst, nullptr);
    SendMessageW(hChkCursor, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(hChkCursor, BM_SETCHECK, g_showCursor ? BST_CHECKED : BST_UNCHECKED, 0);

    // Prioridade alta
    HWND hChkPriority = CreateWindowExW(0, L"BUTTON",
        L"Prioridade alta (CPU + GPU \u2014 garante recursos)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        30, 450, 350, 22, hw, (HMENU)ID_CHK_PRIORITY, hInst, nullptr);
    SendMessageW(hChkPriority, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(hChkPriority, BM_SETCHECK, g_highPriority ? BST_CHECKED : BST_UNCHECKED, 0);

    // Baixa latência
    HWND hChkLowLat = CreateWindowExW(0, L"BUTTON",
        L"Baixa lat\u00EAncia (bypass DWM, pode ter leve tearing)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        30, 476, 380, 22, hw, (HMENU)ID_CHK_LOWLAT, hInst, nullptr);
    SendMessageW(hChkLowLat, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(hChkLowLat, BM_SETCHECK, g_lowLatency ? BST_CHECKED : BST_UNCHECKED, 0);

    // FPS
    CreateLbl(L"Limite FPS (VSync OFF):", 30, 508, 160, 20);
    HWND hFpsCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        195, 505, 100, 200, hw, (HMENU)ID_COMBO_FPS, hInst, nullptr);
    SendMessageW(hFpsCombo, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(hFpsCombo, CB_ADDSTRING, 0, (LPARAM)L"60");
    SendMessageW(hFpsCombo, CB_ADDSTRING, 0, (LPARAM)L"120");
    SendMessageW(hFpsCombo, CB_ADDSTRING, 0, (LPARAM)L"144");
    SendMessageW(hFpsCombo, CB_ADDSTRING, 0, (LPARAM)L"240");
    SendMessageW(hFpsCombo, CB_ADDSTRING, 0, (LPARAM)L"360");
    SendMessageW(hFpsCombo, CB_ADDSTRING, 0, (LPARAM)L"Sem Limite");
    int fpsSel = 3;
    if (g_targetFps == 60) fpsSel = 0;
    else if (g_targetFps == 120) fpsSel = 1;
    else if (g_targetFps == 144) fpsSel = 2;
    else if (g_targetFps == 240) fpsSel = 3;
    else if (g_targetFps == 360) fpsSel = 4;
    else if (g_targetFps == 0) fpsSel = 5;
    SendMessageW(hFpsCombo, CB_SETCURSEL, fpsSel, 0);

    CreateLbl(L"ou digite:", 305, 508, 70, 20);
    HWND hFpsEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | ES_NUMBER,
        380, 505, 60, 24, hw, (HMENU)ID_EDIT_FPS, hInst, nullptr);
    SendMessageW(hFpsEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    if (g_targetFps != 60 && g_targetFps != 120 && g_targetFps != 144 &&
        g_targetFps != 240 && g_targetFps != 360 && g_targetFps != 0) {
        wchar_t buf[16];
        swprintf_s(buf, L"%d", g_targetFps);
        SetWindowTextW(hFpsEdit, buf);
    }

    // GPU para renderização
    CreateLbl(L"GPU para renderiza\u00E7\u00E3o:", 30, 540, 160, 20);
    HWND hGpuCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        195, 537, 500, 200, hw, (HMENU)ID_COMBO_GPU, hInst, nullptr);
    SendMessageW(hGpuCombo, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    CreateLbl(
        L"Dica: Use GPU integrada para espelhar e dedicada para o jogo. "
        L"Prioridade alta garante recursos.",
        30, 570, 680, 20);

    RefreshDisplayList(hw);

    auto CreateBtn = [&](const wchar_t* txt, int x, int y, int w, int h,
                         int id, bool def = false) {
        HWND hB = CreateWindowExW(0, L"BUTTON", txt,
            WS_CHILD | WS_VISIBLE | (def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
            x, y, w, h, hw, (HMENU)(INT_PTR)id, hInst, nullptr);
        SendMessageW(hB, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        return hB;
    };

    CreateBtn(L"\u25B6 Iniciar Espelhamento", 30, 680, 220, 50, ID_BTN_START, true);
    CreateBtn(L"Atualizar Lista", 260, 680, 140, 50, ID_BTN_REFRESH);
    CreateBtn(L"Sair", 560, 680, 140, 50, ID_BTN_EXIT);

    ShowWindow(hw, SW_SHOW);
    UpdateWindow(hw);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return g_selOk;
}

// =============================================================================
// Janela espelho (fullscreen borderless)
// =============================================================================
static HWND g_mirrorHwnd = nullptr;

static LRESULT CALLBACK MirrorProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == g_exitKey) {
        g_running = false;
        DestroyWindow(hw);
        return 0;
    }
    if (msg == WM_DESTROY) {
        g_running = false;
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

static void CreateMirrorWindow(HINSTANCE hInst, const DisplayInfo& dst) {
    g_dstW = dst.rect.right  - dst.rect.left;
    g_dstH = dst.rect.bottom - dst.rect.top;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MirrorProc;
    wc.hInstance      = hInst;
    wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor        = nullptr;
    wc.lpszClassName  = L"SMWnd";
    RegisterClassExW(&wc);

    g_mirrorHwnd = CreateWindowExW(
        WS_EX_TOPMOST, L"SMWnd", L"SpecularLatens v1.00",
        WS_POPUP,
        dst.rect.left, dst.rect.top, g_dstW, g_dstH,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_mirrorHwnd, SW_SHOW);
    UpdateWindow(g_mirrorHwnd);
}

// =============================================================================
// Obter refresh rate do monitor destino
// =============================================================================
static int GetMonitorRefreshRate(HMONITOR hMon) {
    if (!hMon) return 60;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            if (dm.dmDisplayFrequency > 0) {
                Logn("GetMonitorRefreshRate: %ls = %dHz",
                     mi.szDevice, dm.dmDisplayFrequency);
                return (int)dm.dmDisplayFrequency;
            }
        }
    }
    Logn("GetMonitorRefreshRate: fallback 60Hz");
    return 60;
}

// =============================================================================
// Verificar suporte a tearing (DXGI 1.5)
// =============================================================================
static void CheckTearingSupport() {
    g_tearingSupported = false;
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(g_factory.As(&factory5))) {
        BOOL supported = FALSE;
        HRESULT hr = factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &supported, sizeof(supported));
        if (SUCCEEDED(hr) && supported) {
            g_tearingSupported = true;
        }
    }
    Logn("CheckTearingSupport: %s", g_tearingSupported ? "SIM" : "NAO");
}

// =============================================================================
// SwapChain + recursos de renderização
// =============================================================================
static void CreateSwapChain() {
    CheckTearingSupport();

    UINT scFlags = g_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    bool created = false;

    ComPtr<IDXGIFactory2> factory2;
    if (SUCCEEDED(g_factory.As(&factory2))) {
        DXGI_SWAP_CHAIN_DESC1 scd1 = {};
        scd1.Width       = (UINT)g_dstW;
        scd1.Height      = (UINT)g_dstH;
        scd1.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd1.BufferCount = 2;
        scd1.SampleDesc.Count = 1;
        scd1.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd1.Flags       = scFlags;

        ComPtr<IDXGISwapChain1> sc1;
        HRESULT hr = factory2->CreateSwapChainForHwnd(
            g_device.Get(), g_mirrorHwnd, &scd1, nullptr, nullptr, &sc1);

        if (SUCCEEDED(hr)) {
            sc1.As(&g_swapChain);
            factory2->MakeWindowAssociation(g_mirrorHwnd, DXGI_MWA_NO_ALT_ENTER);
            created = true;
            Logn("CreateSwapChain: FLIP_DISCARD OK (flags=0x%X buffers=2)",
                 scFlags);
        } else {
            Logn("CreateSwapChain: Factory2 falhou (0x%08X), fallback.", hr);
        }
    }

    if (!created) {
        DXGI_SWAP_CHAIN_DESC scd = {};
        scd.BufferCount       = 2;
        scd.BufferDesc.Width  = (UINT)g_dstW;
        scd.BufferDesc.Height = (UINT)g_dstH;
        scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow      = g_mirrorHwnd;
        scd.SampleDesc.Count  = 1;
        scd.Windowed          = TRUE;
        scd.SwapEffect        = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.Flags             = scFlags;


        HRESULT hr = g_factory->CreateSwapChain(g_device.Get(), &scd, &g_swapChain);
        if (FAILED(hr)) {
            scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            scd.Flags      = 0;
            hr = g_factory->CreateSwapChain(g_device.Get(), &scd, &g_swapChain);
            if (FAILED(hr)) Fatal("CreateSwapChain falhou", hr);
        }
        g_factory->MakeWindowAssociation(g_mirrorHwnd, DXGI_MWA_NO_ALT_ENTER);
        Logn("CreateSwapChain: fallback legado OK.");
    }

    g_swapChain->GetContainingOutput(&g_dstOutput);
    if (g_dstOutput) Logn("CreateSwapChain: dstOutput obtido para WaitForVBlank.");
    else             Logn("CreateSwapChain: AVISO - dstOutput nao obtido.");

    ComPtr<ID3D11Texture2D> bb;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_device->CreateRenderTargetView(bb.Get(), nullptr, &g_rtv);
}

static void CreateRenderResources() {
    auto vsB = CompileShader(s_vsScreen, "main", "vs_5_0");
    auto psB = CompileShader(s_psScreen, "main", "ps_5_0");

    g_device->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(),
                                 nullptr, &g_vsScreen);
    g_device->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(),
                                nullptr, &g_psScreen);

    // Cursor vertex shader (posição via constant buffer)
    auto vsCurB = CompileShader(s_vsCursor, "main", "vs_5_0");
    g_device->CreateVertexShader(vsCurB->GetBufferPointer(), vsCurB->GetBufferSize(),
                                 nullptr, &g_vsCursorShader);

    // Cursor blend state: premultiplied alpha (ONE / INV_SRC_ALPHA)
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&bd, &g_cursorBlend);

    // Cursor constant buffer (8 floats: rect NDC + uvScale + padding)
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = 32; // float4 rect + float4 uvInfo = 32 bytes
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_device->CreateBuffer(&cbd, nullptr, &g_cursorCB);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT; // Point filter para cursor nítido
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_device->CreateSamplerState(&sd, &g_sampler);

    // Sampler linear para o frame — evita pixelização quando src != dst
    D3D11_SAMPLER_DESC sdLin = {};
    sdLin.Filter         = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sdLin.AddressU       = sdLin.AddressV = sdLin.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sdLin.MaxAnisotropy  = 1;
    sdLin.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sdLin.MinLOD         = 0;
    sdLin.MaxLOD         = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&sdLin, &g_samplerLinear);

    Logn("CreateRenderResources: cursor overlay pipeline criado.");
}

// =============================================================================
// Renderização — frame quad + cursor overlay (sem CPU blitting)
// =============================================================================
static void RenderFrame(bool drawCursor, POINT cursorPos, int srcW, int srcH) {
    if (!g_copyTex) return;

    D3D11_VIEWPORT vp = { 0, 0, (float)g_dstW, (float)g_dstH, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);

    static const float kBlack[4] = { 0.f, 0.f, 0.f, 1.f };
    g_ctx->ClearRenderTargetView(g_rtv.Get(), kBlack);

    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->IASetInputLayout(nullptr);

    // SRV do frame (cria sob demanda, aponta para copyTex)
    if (!g_frameSrv) {
        D3D11_TEXTURE2D_DESC td;
        g_copyTex->GetDesc(&td);
        D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
        srvd.Format                    = td.Format;
        srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels       = 1;
        g_device->CreateShaderResourceView(g_copyTex.Get(), &srvd, &g_frameSrv);
    }

    // 1. Desenhar frame fullscreen
    if (g_frameSrv) {
        g_ctx->VSSetShader(g_vsScreen.Get(), nullptr, 0);
        g_ctx->PSSetShader(g_psScreen.Get(), nullptr, 0);
        g_ctx->PSSetSamplers(0, 1, g_samplerLinear.GetAddressOf()); // linear para frame
        g_ctx->PSSetShaderResources(0, 1, g_frameSrv.GetAddressOf());
        g_ctx->Draw(4, 0);
    }

    // 2. Cursor overlay via GPU (sem Map/Unmap, sem pipeline stall)
    if (drawCursor && g_cursorSrv && g_vsCursorShader && g_cursorBlend && g_cursorCB &&
        g_cursorPixelW > 0 && g_cursorPixelH > 0 && srcW > 0 && srcH > 0 &&
        g_cursorTexW > 0 && g_cursorTexH > 0) {

        // Calcular posição NDC do cursor
        float cx = (float)(cursorPos.x - g_cursorHotspot.x);
        float cy = (float)(cursorPos.y - g_cursorHotspot.y);
        float cw = (float)g_cursorPixelW;
        float ch = (float)g_cursorPixelH;
        float fw = (float)srcW;
        float fh = (float)srcH;

        // NDC: x = pixel/width * 2 - 1, y = 1 - pixel/height * 2
        float ndcLeft   = cx / fw * 2.0f - 1.0f;
        float ndcTop    = 1.0f - cy / fh * 2.0f;
        float ndcRight  = (cx + cw) / fw * 2.0f - 1.0f;
        float ndcBottom = 1.0f - (cy + ch) / fh * 2.0f;

        // UV scale: cursor pode ser menor que textura (pre-alocada grow-only)
        float uvScaleX = (float)g_cursorPixelW / (float)g_cursorTexW;
        float uvScaleY = (float)g_cursorPixelH / (float)g_cursorTexH;

        // Atualizar constant buffer: rect NDC + UV scale
        D3D11_MAPPED_SUBRESOURCE ms;
        HRESULT hr = g_ctx->Map(g_cursorCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        if (SUCCEEDED(hr)) {
            float* data = (float*)ms.pData;
            data[0] = ndcLeft;
            data[1] = ndcTop;
            data[2] = ndcRight;
            data[3] = ndcBottom;
            data[4] = uvScaleX;
            data[5] = uvScaleY;
            data[6] = 0.0f;
            data[7] = 0.0f;
            g_ctx->Unmap(g_cursorCB.Get(), 0);

            // Ativar blend premultiplied alpha
            g_ctx->OMSetBlendState(g_cursorBlend.Get(), nullptr, 0xFFFFFFFF);

            // Shader do cursor + SRV
            g_ctx->VSSetShader(g_vsCursorShader.Get(), nullptr, 0);
            g_ctx->VSSetConstantBuffers(0, 1, g_cursorCB.GetAddressOf());
            g_ctx->PSSetShaderResources(0, 1, g_cursorSrv.GetAddressOf());

            g_ctx->Draw(4, 0);

            // Restaurar blend state (sem blend para próximo frame)
            g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        }
    }

    // Apresentação:
    // - VSync ON (sem low latency): Present(0, 0) — DWM compõe sem tearing.
    // - VSync ON + low latency: Present(0, ALLOW_TEARING) — bypass DWM, timer faz pacing.
    // - VSync OFF: Present(0, ALLOW_TEARING) — bypass DWM, mínima latência.
    if (g_vsync && !g_lowLatency) {
        g_swapChain->Present(0, 0);
    } else {
        UINT presentFlags = g_tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0;
        g_swapChain->Present(0, presentFlags);
    }
}

// =============================================================================
// Loop principal
// =============================================================================
static void RunMirrorLoop(UINT srcAdapter, UINT srcOutput, RECT srcRect) {
    // Captura sempre no capture device (pode ser == g_device se mesma GPU)
    ComPtr<IDXGIAdapter1> adapter;
    g_factory->EnumAdapters1(srcAdapter, &adapter);

    ComPtr<IDXGIOutput> out;
    adapter->EnumOutputs(srcOutput, &out);

    ComPtr<IDXGIOutput1> out1;
    out.As(&out1);

    ComPtr<IDXGIOutputDuplication> dup;
    HRESULT hr = out1->DuplicateOutput(g_captureDevice.Get(), &dup);
    if (FAILED(hr)) Fatal(
        "DuplicateOutput falhou.\n"
        "Poss\u00EDveis causas:\n"
        "- Outra inst\u00E2ncia do SpecularLatens j\u00E1 rodando\n"
        "- Driver n\u00E3o suporta Desktop Duplication (Windows 8+)\n"
        "- Sess\u00E3o remota (RDP)", hr);

    int srcW = srcRect.right  - srcRect.left;
    int srcH = srcRect.bottom - srcRect.top;

    // Bootstrap cursor
    UpdateCursorShapeFromSystem();
    g_cursorTexDirty = true;

    bool    cursorVisible = false;
    POINT   cursorPos     = {};
    HCURSOR lastCursorHandle = nullptr;
    {
        CURSORINFO ci = { sizeof(ci) };
        if (GetCursorInfo(&ci)) {
            cursorVisible = (ci.flags & CURSOR_SHOWING) != 0;
            cursorPos = { ci.ptScreenPos.x - srcRect.left,
                          ci.ptScreenPos.y - srcRect.top };
            lastCursorHandle = ci.hCursor;
        }
    }
    Logn("RunMirrorLoop: bootstrap cursor vis=%d pos=(%d,%d) "
         "hotspot=(%d,%d) pixW=%u pixH=%u crossAdapter=%d",
         cursorVisible, cursorPos.x, cursorPos.y,
         g_cursorHotspot.x, g_cursorHotspot.y,
         g_cursorPixelW, g_cursorPixelH, g_crossAdapter);

    std::vector<BYTE>               cursorData;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO cursorShape = {};

    // Frame pacing
    using Clock    = std::chrono::steady_clock;
    using Duration = std::chrono::microseconds;

    // VSync ON (sem low latency): WaitForVBlank no output destino — alinhado ao VBlank.
    // Low latency ON: timer ao Hz do monitor + ALLOW_TEARING (bypass DWM).
    // VSync OFF: timer ao target FPS configurado.
    bool useVBlankSync = g_vsync && !g_lowLatency && g_dstOutput;
    int effectiveFps;
    if (useVBlankSync)
        effectiveFps = 0; // pacing via WaitForVBlank, não timer
    else if (g_vsync || g_lowLatency)
        effectiveFps = g_dstHz;
    else
        effectiveFps = g_targetFps;
    Duration frameDuration = (effectiveFps > 0)
        ? Duration(1000000 / effectiveFps)
        : Duration(0);
    // Spin-wait threshold: dormir até 500µs antes, spin com SwitchToThread()
    // SwitchToThread() cede a CPU para outros threads (incluindo DWM),
    // diferente de YieldProcessor() que é apenas PAUSE (não cede).
    Duration spinThreshold = Duration(500);
    auto nextFrameTime = Clock::now();

    Logn("RunMirrorLoop: effectiveFps=%d vblankSync=%d (vsync=%d lowlat=%d dstHz=%d targetFps=%d)",
         effectiveFps, useVBlankSync, g_vsync, g_lowLatency, g_dstHz, g_targetFps);

    // Frame timing statistics (log a cada 5s)
    auto statsStart  = Clock::now();
    long long statsFrames = 0;
    long long statsMinUs  = 999999;
    long long statsMaxUs  = 0;
    long long statsSumUs  = 0;
    auto lastFrameTime   = Clock::now();

    int accessLostRetries = 0;
    const int kMaxAccessLostRetries = 10;
    bool startupGrace = true; // Primeiros ACCESS_LOST são esperados (DWM rearranja)
    bool needFullCopy = true; // Primeiro frame precisa cópia completa (dirty rects parciais)

    timeBeginPeriod(1);
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

    // Prioridade via MMCSS (Multimedia Class Scheduler Service)
    // Mesma API que Discord, OBS e Chrome usam para prioridade de tempo real.
    // TIME_CRITICAL é o nível máximo de prioridade de thread sem admin.
    HANDLE mmcssHandle = nullptr;
    DWORD  mmcssTaskIndex = 0;
    if (g_highPriority) {
        mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
        if (mmcssHandle) {
            AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_CRITICAL);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            Logn("MMCSS: Pro Audio (CRITICAL) + TIME_CRITICAL ativo. TaskIndex=%u", mmcssTaskIndex);
        } else {
            Logn("MMCSS: FALHA (erro %d). Fallback: TIME_CRITICAL.", GetLastError());
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        }
    }

    auto lastCursorLog = Clock::now();

    MSG msg = {};
    while (g_running) {
        // ============================================================
        // 1. PACING: sincronizar com o display ou timer.
        // ============================================================
        if (useVBlankSync) {
            // VSync ON: WaitForVBlank no output destino.
            // Frames perfeitamente alinhados com o refresh do display.
            // Funciona em background (não depende de foco da janela).
            g_dstOutput->WaitForVBlank();
        } else if (effectiveFps > 0) {
            // Timer-based: sleep + SwitchToThread spin para precisão.
            auto spinTarget = nextFrameTime - spinThreshold;
            if (Clock::now() < spinTarget)
                std::this_thread::sleep_until(spinTarget);
            while (Clock::now() < nextFrameTime)
                SwitchToThread(); // cede CPU para DWM e outros
            nextFrameTime += frameDuration;
            auto now = Clock::now();
            if (now - nextFrameTime > frameDuration * 2)
                nextFrameTime = now + frameDuration;
        }

        // Frame timing statistics
        {
            auto now = Clock::now();
            long long dtUs = std::chrono::duration_cast<Duration>(now - lastFrameTime).count();
            lastFrameTime = now;
            if (statsFrames > 0) { // pular o primeiro frame (startup)
                if (dtUs < statsMinUs) statsMinUs = dtUs;
                if (dtUs > statsMaxUs) statsMaxUs = dtUs;
                statsSumUs += dtUs;
            }
            statsFrames++;
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - statsStart).count();
            if (elapsed >= 5 && statsFrames > 1) {
                long long avgUs = statsSumUs / (statsFrames - 1);
                Logn("FRAME TIMING: frames=%lld avg=%.2fms min=%.2fms max=%.2fms (target=%.2fms vblank=%d)",
                     statsFrames, avgUs / 1000.0, statsMinUs / 1000.0, statsMaxUs / 1000.0,
                     useVBlankSync ? (1000000.0 / g_dstHz / 1000.0) : (effectiveFps > 0 ? 1000000.0 / effectiveFps / 1000.0 : 0.0),
                     useVBlankSync);
                statsStart  = now;
                statsFrames = 0;
                statsMinUs  = 999999;
                statsMaxUs  = 0;
                statsSumUs  = 0;
            }
        }

        // 2. Processar mensagens Windows
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        // 3. Cursor: posição/visibilidade via Win32
        if (g_showCursor) {
            CURSORINFO ci = { sizeof(ci) };
            if (GetCursorInfo(&ci)) {
                cursorVisible = (ci.flags & CURSOR_SHOWING) != 0;
                cursorPos.x = ci.ptScreenPos.x - srcRect.left;
                cursorPos.y = ci.ptScreenPos.y - srcRect.top;
                if (ci.hCursor && ci.hCursor != lastCursorHandle) {
                    lastCursorHandle = ci.hCursor;
                    UpdateCursorShapeFromSystem();
                    g_cursorTexDirty = true;
                }
            }
        }

        // Atualizar textura GPU do cursor se shape mudou
        if (g_cursorTexDirty && !g_cursorPixelsCPU.empty()) {
            UpdateCursorGPUTexture();
        }

        // Log cursor periódico
        {
            auto now = Clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCursorLog).count() >= 5) {
                lastCursorLog = now;
                Logn("CURSOR: show=%d vis=%d pixW=%u pixH=%u pos=(%d,%d) hot=(%d,%d) pixels=%zu gpuTex=%d",
                     g_showCursor, cursorVisible, g_cursorPixelW, g_cursorPixelH,
                     cursorPos.x, cursorPos.y,
                     g_cursorHotspot.x, g_cursorHotspot.y,
                     g_cursorPixelsCPU.size(), g_cursorTex ? 1 : 0);
            }
        }

        // 4. Captura (no capture device)
        DXGI_OUTDUPL_FRAME_INFO info = {};
        ComPtr<IDXGIResource>   res;
        hr = dup->AcquireNextFrame(0, &info, &res);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // Nenhum frame novo — re-renderizar com cursor atualizado (posição).
            // SEM CopyResource — cursor é overlay GPU, copyTex já tem o frame.
            if (g_copyTex) {
                bool dc = g_showCursor && cursorVisible;
                RenderFrame(dc, cursorPos, srcW, srcH);
            }
            continue;
        }

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            Logn("RunMirrorLoop: ACCESS_LOST - retry %d/%d%s",
                 accessLostRetries + 1, kMaxAccessLostRetries,
                 startupGrace ? " (startup)" : "");
            dup.Reset();
            if (++accessLostRetries > kMaxAccessLostRetries) {
                Fatal("DXGI_ERROR_ACCESS_LOST persistente.");
                break;
            }
            // Startup: retry rápido (1ms). Normal: 16ms (1 frame@60Hz).
            Sleep(startupGrace ? 1 : 16);
            ComPtr<IDXGIOutput> outRetry;
            ComPtr<IDXGIOutput1> out1Retry;
            hr = adapter->EnumOutputs(srcOutput, &outRetry);
            if (FAILED(hr)) continue;
            hr = outRetry.As(&out1Retry);
            if (FAILED(hr)) continue;
            hr = out1Retry->DuplicateOutput(g_captureDevice.Get(), &dup);
            if (FAILED(hr)) continue;
            DXGI_OUTPUT_DESC odRetry;
            outRetry->GetDesc(&odRetry);
            srcRect = odRetry.DesktopCoordinates;
            srcW = srcRect.right  - srcRect.left;
            srcH = srcRect.bottom - srcRect.top;
            UpdateCursorShapeFromSystem();
            g_cursorTexDirty = true;
            // Capturar handle atual — NÃO usar nullptr (evita ping-pong)
            {
                CURSORINFO ci2 = { sizeof(ci2) };
                if (GetCursorInfo(&ci2)) lastCursorHandle = ci2.hCursor;
            }
            Logn("RunMirrorLoop: Duplication recriada. Src: %dx%d", srcW, srcH);
            needFullCopy = true; // Nova duplication — dirty rects inválidos
            continue;
        }

        if (FAILED(hr)) {
            Logn("RunMirrorLoop: AcquireNextFrame ERRO 0x%08X", hr);
            continue;
        }

        accessLostRetries = 0;
        startupGrace = false; // Primeiro frame OK — não é mais startup

        // DXGI cursor shape (quando disponível — prioridade sobre Win32)
        if (info.PointerShapeBufferSize > 0) {
            cursorData.resize(info.PointerShapeBufferSize);
            UINT required = 0;
            hr = dup->GetFramePointerShape(
                (UINT)cursorData.size(), cursorData.data(),
                &required, &cursorShape);
            if (SUCCEEDED(hr)) {
                BuildCursorPixelsFromDXGI(cursorData, cursorShape);
                // NÃO resetar lastCursorHandle — causa ping-pong DXGI/Win32
                // que recria textura a cada frame (32→64→32→64...).
                // DXGI é autoritativo; Win32 só atualiza quando handle muda.
                g_cursorTexDirty = true;
                UpdateCursorGPUTexture();
            }
        }

        // 5. Pipeline: Captura → Bridge (se cross-adapter) → Render
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(res.As<ID3D11Texture2D>(&tex))) {
            D3D11_TEXTURE2D_DESC td;
            tex->GetDesc(&td);
            EnsureFrameCopyTexture(td.Width, td.Height, td.Format);

            if (g_crossAdapter) {
                EnsureBridgeStaging(td.Width, td.Height, td.Format);
                if (g_bridgeStaging && g_copyTex) {
                    g_captureCtx->CopyResource(g_bridgeStaging.Get(), tex.Get());
                    dup->ReleaseFrame();

                    D3D11_MAPPED_SUBRESOURCE ms;
                    hr = g_captureCtx->Map(g_bridgeStaging.Get(), 0,
                                            D3D11_MAP_READ, 0, &ms);
                    if (SUCCEEDED(hr)) {
                        g_ctx->UpdateSubresource(g_copyTex.Get(), 0, nullptr,
                                                  ms.pData, ms.RowPitch, 0);
                        g_captureCtx->Unmap(g_bridgeStaging.Get(), 0);
                    }
                } else {
                    dup->ReleaseFrame();
                }
            } else {
                if (g_copyTex) {
                    bool usedDirtyRects = false;
                    if (!needFullCopy) {
                        // Dirty rects: copiar apenas regiões alteradas
                        UINT dirtySize = 0;
                        HRESULT drHr = dup->GetFrameDirtyRects(0, nullptr, &dirtySize);
                        if (drHr == DXGI_ERROR_MORE_DATA && dirtySize > 0) {
                            UINT nDirty = dirtySize / sizeof(RECT);
                            std::vector<RECT> dirtyRects(nDirty);
                            drHr = dup->GetFrameDirtyRects(dirtySize, dirtyRects.data(), &dirtySize);
                            if (SUCCEEDED(drHr)) {
                                // Move rects
                                UINT moveSize = 0;
                                HRESULT mrHr = dup->GetFrameMoveRects(0, nullptr, &moveSize);
                                if (mrHr == DXGI_ERROR_MORE_DATA && moveSize > 0) {
                                    UINT nMove = moveSize / sizeof(DXGI_OUTDUPL_MOVE_RECT);
                                    std::vector<DXGI_OUTDUPL_MOVE_RECT> moveRects(nMove);
                                    mrHr = dup->GetFrameMoveRects(moveSize, moveRects.data(), &moveSize);
                                    if (SUCCEEDED(mrHr)) {
                                        for (const auto& m : moveRects) {
                                            D3D11_BOX box = {};
                                            box.left   = (UINT)m.DestinationRect.left;
                                            box.top    = (UINT)m.DestinationRect.top;
                                            box.right  = (UINT)m.DestinationRect.right;
                                            box.bottom = (UINT)m.DestinationRect.bottom;
                                            box.back   = 1;
                                            g_ctx->CopySubresourceRegion(g_copyTex.Get(), 0,
                                                box.left, box.top, 0, tex.Get(), 0, &box);
                                        }
                                    }
                                }
                                // Dirty rects
                                for (const auto& r : dirtyRects) {
                                    D3D11_BOX box = {};
                                    box.left   = (UINT)r.left;
                                    box.top    = (UINT)r.top;
                                    box.right  = (UINT)r.right;
                                    box.bottom = (UINT)r.bottom;
                                    box.back   = 1;
                                    g_ctx->CopySubresourceRegion(g_copyTex.Get(), 0,
                                        r.left, r.top, 0, tex.Get(), 0, &box);
                                }
                                usedDirtyRects = true;
                            }
                        }
                    }
                    if (!usedDirtyRects) {
                        g_ctx->CopyResource(g_copyTex.Get(), tex.Get());
                        needFullCopy = false;
                    }
                }
                dup->ReleaseFrame();
            }
        } else {
            dup->ReleaseFrame();
        }

        // 6. Render frame + cursor overlay (apresentação imediata)
        if (g_copyTex) {
            bool dc = g_showCursor && cursorVisible;
            RenderFrame(dc, cursorPos, srcW, srcH);
        }
    }

    timeEndPeriod(1);
    SetThreadExecutionState(ES_CONTINUOUS);
    if (mmcssHandle) {
        AvRevertMmThreadCharacteristics(mmcssHandle);
        Logn("MMCSS: Revertido.");
    }
    if (g_highPriority) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    }
}

// =============================================================================
// WinMain
// =============================================================================
static int WinMainInternal(HINSTANCE hInst) {
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&g_factory));
    if (FAILED(hr)) {
        Logn("FALHA DXGI inicial: 0x%08X", hr);
        Fatal("Erro ao inicializar DXGI.");
        return 0;
    }

    EnumerateDisplays();
    if (g_displays.empty()) Logn("ERRO: Nenhum monitor detectado.");

    if (!ShowSelectionUI(hInst)) {
        Logn("Saindo: Selecao cancelada.");
        return 0;
    }

    const DisplayInfo& src = g_displays[g_srcIdx];
    const DisplayInfo& dst = g_displays[g_dstIdx];
    Logn("Capturador: Origem=%ls, Destino=%ls", src.name.c_str(), dst.name.c_str());

    // Obter refresh rate do monitor destino (para VSync timer-based)
    g_dstHz = GetMonitorRefreshRate(dst.hMon);
    Logn("Monitor destino: %dHz", g_dstHz);

    UINT srcAdapterIdx = src.adapterIdx;
    UINT renderAdapterIdx = (g_renderGpuIdx >= 0) ? (UINT)g_renderGpuIdx : srcAdapterIdx;
    g_crossAdapter = (renderAdapterIdx != srcAdapterIdx);

    Logn("GPU: srcAdapter=%u renderAdapter=%u crossAdapter=%d",
         srcAdapterIdx, renderAdapterIdx, g_crossAdapter);

    // Criar render device na GPU selecionada
    {
        ComPtr<IDXGIAdapter1> renderAdapter;
        g_factory->EnumAdapters1(renderAdapterIdx, &renderAdapter);
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, flOut;
        hr = D3D11CreateDevice(renderAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               0, &fl, 1, D3D11_SDK_VERSION,
                               &g_device, &flOut, &g_ctx);
        if (FAILED(hr)) {
            Logn("FALHA D3D11CreateDevice (render GPU %u): 0x%08X", renderAdapterIdx, hr);
            Fatal("Erro ao criar dispositivo D3D11 na GPU selecionada.", hr);
        }
        DXGI_ADAPTER_DESC1 ad;
        renderAdapter->GetDesc1(&ad);
        Logn("Render device criado: %ls", ad.Description);
    }

    if (g_crossAdapter) {
        // Criar capture device na GPU da origem (para DuplicateOutput)
        ComPtr<IDXGIAdapter1> srcAdapter;
        g_factory->EnumAdapters1(srcAdapterIdx, &srcAdapter);
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, flOut;
        hr = D3D11CreateDevice(srcAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               0, &fl, 1, D3D11_SDK_VERSION,
                               &g_captureDevice, &flOut, &g_captureCtx);
        if (FAILED(hr)) {
            Logn("FALHA D3D11CreateDevice (capture GPU %u): 0x%08X", srcAdapterIdx, hr);
            Fatal("Erro ao criar dispositivo D3D11 na GPU da origem.", hr);
        }
        DXGI_ADAPTER_DESC1 ad;
        srcAdapter->GetDesc1(&ad);
        Logn("Capture device criado (cross-adapter): %ls", ad.Description);
    } else {
        // Mesma GPU: capture == render
        g_captureDevice = g_device;
        g_captureCtx = g_ctx;
        Logn("Same adapter: capture == render device.");
    }

    // Prioridade
    if (g_highPriority) {
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        Logn("SetPriorityClass: HIGH_PRIORITY_CLASS");

        ComPtr<IDXGIDevice> dxgiDev;
        if (SUCCEEDED(g_device.As(&dxgiDev))) {
            // SetGPUThreadPriority > 0 requer privilégio de admin.
            // Tentar valor máximo (7), se falhar tentar valores menores.
            bool gpuPrioritySet = false;
            for (int p = 7; p >= 1; p -= 2) {
                hr = dxgiDev->SetGPUThreadPriority(p);
                if (SUCCEEDED(hr)) {
                    Logn("SetGPUThreadPriority(%d): OK", p);
                    gpuPrioritySet = true;
                    break;
                }
            }
            if (!gpuPrioritySet) {
                Logn("SetGPUThreadPriority: FALHA (requer admin). "
                     "CPU priority HIGH_PRIORITY_CLASS ativo.");
            }
        }
    }

    // MaxFrameLatency — sempre 1 para mínima latência
    {
        ComPtr<IDXGIDevice1> dxgiDev1;
        if (SUCCEEDED(g_device.As(&dxgiDev1))) {
            dxgiDev1->SetMaximumFrameLatency(1);
            Logn("SetMaximumFrameLatency(1).");
        }
    }

    CreateMirrorWindow(hInst, dst);
    CreateSwapChain();
    CreateRenderResources();

    RunMirrorLoop(srcAdapterIdx, src.outputIdx, src.rect);

    // Restaurar prioridade
    if (g_highPriority) {
        SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    }

    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    InitIniPath();
    Logn("--- SpecularLatens v1.00 Iniciado ---");

    while (true) {
        g_running = true;
        g_selOk   = false;

        MSG drain;
        while (PeekMessageW(&drain, nullptr, 0, 0, PM_REMOVE)) {}

        int ret = 0;
        __try {
            ret = WinMainInternal(hInst);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Logn("FATAL: SEH in WinMain! 0x%08X", GetExceptionCode());
            MessageBoxW(nullptr,
                L"Erro fatal capturado. Veja o log para detalhes.",
                L"SpecularLatens CRASH", MB_OK | MB_ICONERROR);
            break;
        }

        if (!g_selOk) break;

        Logn("Retornando ao menu principal...");
        ResetGraphicsStack();
    }

    Logn("--- SpecularLatens v1.00 Encerrado ---");
    return 0;
}
