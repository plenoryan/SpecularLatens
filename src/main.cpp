//=============================================================================
// ScreenMirror - Duplicação de tela com baixa latência (DXGI + D3D11)
// Captura uma tela via DXGI Desktop Duplication e exibe em outra via D3D11.
// Suporte: 4K, 144 Hz+, cursor de hardware, escala GPU.
// Tecla ESC fecha o espelhamento.
//=============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")

using Microsoft::WRL::ComPtr;

// =============================================================================
// Shaders HLSL embutidos
// =============================================================================

// Quad de tela cheia sem vertex buffer
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

// Quad posicionado para o cursor
static const char* s_vsCursor = R"(
cbuffer CB:register(b0) { float2 ndcPos; float2 ndcSize; };
struct VSOut { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
VSOut main(uint id:SV_VertexID) {
    VSOut o;
    o.uv  = float2((id&2)?1.0:0.0, (id&1)?1.0:0.0);
    float2 p = ndcPos + float2(o.uv.x, -o.uv.y) * ndcSize;
    o.pos = float4(p, 0, 1);
    return o;
})";

static const char* s_psCursor = R"(
Texture2D tex:register(t0); SamplerState s:register(s0);
struct PSIn { float4 p:SV_Position; float2 uv:TEXCOORD0; };
float4 main(PSIn i):SV_Target { return tex.Sample(s, i.uv); })";

// =============================================================================
// Globais D3D
// =============================================================================
static ComPtr<IDXGIFactory1>        g_factory;
static ComPtr<ID3D11Device>         g_device;
static ComPtr<ID3D11DeviceContext>  g_ctx;
static ComPtr<IDXGISwapChain>       g_swapChain;
static ComPtr<ID3D11RenderTargetView> g_rtv;

static ComPtr<ID3D11VertexShader>   g_vsScreen, g_vsCursor;
static ComPtr<ID3D11PixelShader>    g_psScreen, g_psCursor;
static ComPtr<ID3D11SamplerState>   g_sampler;
static ComPtr<ID3D11Buffer>         g_cursorCB;
static ComPtr<ID3D11BlendState>     g_alphaBlend;

static ComPtr<ID3D11Texture2D>      g_cursorTex;
static UINT g_cursorTexW = 0, g_cursorTexH = 0;

static int  g_dstW = 0, g_dstH = 0;
static bool g_running = true;
static UINT g_exitKey = VK_ESCAPE;

// --- Configuração Monitor Virtual (Full Version) ---
static bool g_vEnabled = false;
static int  g_vResIdx = 2; // Default 4K
static int  g_vHzIdx = 0;  // Default 60Hz
static const wchar_t* g_vResList[] = { L"1080p", L"1440p", L"4K", L"8K", L"Custom..." };
static const wchar_t* g_vHzList[]  = { L"60Hz", L"120Hz", L"144Hz", L"240Hz", L"480Hz", L"Custom..." };

static HWND g_hVResCustomW = nullptr, g_hVResCustomH = nullptr, g_hVHzCustom = nullptr;

static bool IsUserAdmin() {
    BOOL b = FALSE;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    PSID AdministratorsGroup;
    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &AdministratorsGroup)) {
        CheckTokenMembership(nullptr, AdministratorsGroup, &b);
        FreeSid(AdministratorsGroup);
    }
    return b == TRUE;
}

static bool DriverFilesExist() {
    return (GetFileAttributesW(L"drivers\\MttVDD.inf") != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(L"drivers\\MttVDD.dll") != INVALID_FILE_ATTRIBUTES);
}

static void RunCmd(const wchar_t* cmd) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, (LPWSTR)cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static void UpdateVirtualRegistry() {
    // Configura a resolu\u00E7\u00E3o no driver MttVDD
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WUDF\\Services\\MttVDD\\Adapter0",
                        0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        
        wchar_t mode[256];
        wchar_t res[64] = L"3840,2160";
        int hz = 60;

        if (g_vResIdx == 4) { // Custom
            wchar_t w[16], h[16];
            GetWindowTextW(g_hVResCustomW, w, 16);
            GetWindowTextW(g_hVResCustomH, h, 16);
            swprintf_s(res, L"%s,%s", w, h);
        } else {
            if (g_vResIdx == 0) wcscpy_s(res, L"1920,1080");
            else if (g_vResIdx == 1) wcscpy_s(res, L"2560,1440");
            else if (g_vResIdx == 2) wcscpy_s(res, L"3840,2160");
            else if (g_vResIdx == 3) wcscpy_s(res, L"7680,4320");
        }

        if (g_vHzIdx == 5) { // Custom Hz
            wchar_t f[16];
            GetWindowTextW(g_hVHzCustom, f, 16);
            hz = _wtoi(f);
        } else {
            if (g_vHzIdx == 1) hz = 120;
            else if (g_vHzIdx == 2) hz = 144;
            else if (g_vHzIdx == 3) hz = 240;
            else if (g_vHzIdx == 4) hz = 480;
        }

        swprintf_s(mode, L"%s,%d", res, hz);
        RegSetValueExW(hKey, L"SupportedModes", 0, REG_SZ, (BYTE*)mode, (DWORD)(wcslen(mode) + 1) * 2);
        RegCloseKey(hKey);
    }
}

static void ToggleVirtualDisplay(bool enable) {
    if (enable) {
        UpdateVirtualRegistry();
        // Tenta adicionar o driver MttVDD (arquivos baixados na pasta drivers/)
        RunCmd(L"pnputil /add-driver drivers\\MttVDD.inf /install");
        g_vEnabled = true;
    } else {
        // Para desativar, poder\u00EDamos usar devcon. Por agora apenas marcamos como desativado no app.
        g_vEnabled = false;
    }
}

// =============================================================================
// Utilitários
// =============================================================================
static void Fatal(const char* msg, HRESULT hr = 0) {
    char buf[512];
    if (hr) sprintf_s(buf, "%s\nHRESULT: 0x%08X", msg, (unsigned)hr);
    else    strcpy_s(buf, msg);
    MessageBoxA(nullptr, buf, "ScreenMirror", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

static ComPtr<ID3DBlob> CompileShader(const char* src, const char* entry, const char* profile) {
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        std::string m = "Shader error:\n";
        if (err) m += (const char*)err->GetBufferPointer();
        Fatal(m.c_str(), hr);
    }
    return code;
}

// =============================================================================
// Enumeração de monitores
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

static BOOL CALLBACK MonitorCb(HMONITOR hm, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hm, &mi);
    DisplayInfo d;
    d.hMon       = hm;
    d.rect       = mi.rcMonitor;
    d.isPrimary  = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    d.name       = mi.szDevice;
    d.adapterIdx = 0; d.outputIdx = 0;
    g_displays.push_back(d);
    return TRUE;
}

static void EnumerateDisplays() {
    g_displays.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorCb, 0);

    UINT ai = 0;
    ComPtr<IDXGIAdapter1> adapter;
    while (g_factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND) {
        UINT oi = 0;
        ComPtr<IDXGIOutput> out;
        while (adapter->EnumOutputs(oi, &out) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC od; out->GetDesc(&od);
            for (auto& d : g_displays)
                if (d.hMon == od.Monitor) { d.adapterIdx = ai; d.outputIdx = oi; }
            ++oi;
        }
        ++ai;
    }
}

// =============================================================================
// Janela de seleção
// =============================================================================
static int  g_srcIdx = 0, g_dstIdx = 1;
static bool g_selOk  = false;
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
    SendMessageW(g_hSrcList, LB_SETCURSEL, 0, 0);
    SendMessageW(g_hDstList, LB_SETCURSEL, g_displays.size() > 1 ? 1 : 0, 0);
}

static LRESULT CALLBACK SelProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 100) {  // Iniciar
            int s = (int)SendMessageW(g_hSrcList, LB_GETCURSEL, 0, 0);
            int d = (int)SendMessageW(g_hDstList, LB_GETCURSEL, 0, 0);
            if (s == LB_ERR || d == LB_ERR) {
                MessageBoxW(hw, L"Selecione a origem e o destino.", L"ScreenMirror", MB_OK | MB_ICONWARNING);
                break;
            }
            if (s == d) {
                MessageBoxW(hw, L"Origem e destino devem ser telas diferentes.", L"ScreenMirror", MB_OK | MB_ICONEXCLAMATION);
                break;
            }

            // Pega o atalho selecionado
            HWND hCombo = GetDlgItem(hw, 103);
            int curKey = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
            if (curKey == 0) g_exitKey = VK_ESCAPE;
            else if (curKey == 1) g_exitKey = VK_F4;
            else if (curKey == 2) g_exitKey = VK_F12;
            else if (curKey == 3) g_exitKey = VK_END;
            else if (curKey == 4) g_exitKey = VK_HOME;

            g_srcIdx = s; g_dstIdx = d; g_selOk = true;
            DestroyWindow(hw);
        }
        if (id == 101) DestroyWindow(hw);  // Cancelar
        if (id == 102) RefreshDisplayList(hw); // Atualizar
        
        if (id == 104) { // Toggle Virtual Monitor
            if (!IsUserAdmin()) {
                MessageBoxW(hw, L"ERRO: Voc\u00EA precisa rodar como ADMINISTRADOR para gerenciar drivers de v\u00EDdeo.", L"ScreenMirror", MB_OK | MB_ICONERROR);
                SendMessageW((HWND)lp, BM_SETCHECK, BST_UNCHECKED, 0);
                break;
            }
            if (!DriverFilesExist()) {
                MessageBoxW(hw, L"ERRO: Arquivos do driver MttVDD n\u00E3o encontrados na pasta 'drivers'.\n\nCertifique-se de que os arquivos extra\u00EDdos est\u00E3o na pasta drivers/ ao lado do execut\u00E1vel.", L"ScreenMirror", MB_OK | MB_ICONERROR);
                SendMessageW((HWND)lp, BM_SETCHECK, BST_UNCHECKED, 0);
                break;
            }

            bool check = (SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            
            // Pega configs atuais
            g_vResIdx = (int)SendMessageW(GetDlgItem(hw, 105), CB_GETCURSEL, 0, 0);
            g_vHzIdx = (int)SendMessageW(GetDlgItem(hw, 106), CB_GETCURSEL, 0, 0);

            if (check) {
                ToggleVirtualDisplay(true);
                Sleep(2000);
                RefreshDisplayList(hw);
            } else {
                ToggleVirtualDisplay(false);
                Sleep(1000);
                RefreshDisplayList(hw);
            }
        }
        if (id == 105 || id == 106) { // ComboBox resolution or Hz changed
            int r = (int)SendMessageW(GetDlgItem(hw, 105), CB_GETCURSEL, 0, 0);
            int h = (int)SendMessageW(GetDlgItem(hw, 106), CB_GETCURSEL, 0, 0);
            BOOL showR = (r == 4);
            BOOL showH = (h == 5);
            ShowWindow(g_hVResCustomW, showR ? SW_SHOW : SW_HIDE);
            ShowWindow(g_hVResCustomH, showR ? SW_SHOW : SW_HIDE);
            ShowWindow(g_hVHzCustom, showH ? SW_SHOW : SW_HIDE);
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

static bool ShowSelectionUI(HINSTANCE hInst) {
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = SelProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SMSel";
    RegisterClassExW(&wc);

    int winW = 740;
    int winH = 680;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND hw = CreateWindowExW(0, L"SMSel",
        L"ScreenMirror v3.2 FULL \u2014 Configura\u00E7\u00E3o de Espelhamento",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        (screenW - winW) / 2, (screenH - winH) / 2, winW, winH,
        nullptr, nullptr, hInst, nullptr);

    g_hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          VARIABLE_PITCH | FF_SWISS, L"Segoe UI");

    // Labels e estilo
    auto CreateLbl = [&](const wchar_t* txt, int x, int y, int w, int h) {
        HWND hL = CreateWindowExW(0, L"STATIC", txt, WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, w, h, hw, nullptr, hInst, nullptr);
        SendMessageW(hL, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        return hL;
    };

    CreateLbl(L"1. SELECIONE A ORIGEM (Capturar desta tela):", 30, 20, 320, 20);
    CreateLbl(L"2. SELECIONE O DESTINO (Exibir nesta tela):", 370, 20, 320, 20);

    // Listas
    g_hSrcList = CreateWindowExW(0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | WS_BORDER,
        30, 45, 330, 300, hw, nullptr, hInst, nullptr);
    g_hDstList = CreateWindowExW(0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | WS_BORDER,
        370, 45, 330, 300, hw, nullptr, hInst, nullptr);

    SendMessageW(g_hSrcList, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(g_hDstList, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // --- MONITOR VIRTUAL (FULL) ---
    HWND hGroup = CreateWindowExW(0, L"BUTTON", L" MONITOR VIRTUAL (NATIVO - MttVDD) ",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 30, 360, 670, 110, hw, nullptr, hInst, nullptr);
    SendMessageW(hGroup, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    HWND hCheck = CreateWindowExW(0, L"BUTTON", L"Habilitar Monitor Virtual (Requer Admin)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 50, 385, 300, 20, hw, (HMENU)104, hInst, nullptr);
    SendMessageW(hCheck, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    CreateLbl(L"Resolu\u00E7\u00E3o:", 50, 420, 80, 20);
    HWND hComboRes = CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 140, 417, 100, 200, hw, (HMENU)105, hInst, nullptr);
    for (auto s : g_vResList) SendMessageW(hComboRes, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessageW(hComboRes, CB_SETCURSEL, g_vResIdx, 0);
    SendMessageW(hComboRes, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    g_hVResCustomW = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1920", WS_CHILD | ES_NUMBER, 250, 417, 55, 24, hw, nullptr, hInst, nullptr);
    g_hVResCustomH = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1080", WS_CHILD | ES_NUMBER, 310, 417, 55, 24, hw, nullptr, hInst, nullptr);
    SendMessageW(g_hVResCustomW, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(g_hVResCustomH, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    CreateLbl(L"Refresh:", 385, 420, 60, 20);
    HWND hComboHz = CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 450, 417, 100, 200, hw, (HMENU)106, hInst, nullptr);
    for (auto s : g_vHzList) SendMessageW(hComboHz, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessageW(hComboHz, CB_SETCURSEL, g_vHzIdx, 0);
    SendMessageW(hComboHz, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    g_hVHzCustom = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"60", WS_CHILD | ES_NUMBER, 560, 417, 50, 24, hw, nullptr, hInst, nullptr);
    SendMessageW(g_hVHzCustom, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // Esconde custom por padr\u00E3o
    ShowWindow(g_hVResCustomW, SW_HIDE); ShowWindow(g_hVResCustomH, SW_HIDE); ShowWindow(g_hVHzCustom, SW_HIDE);

    // --- CONFIGS GERAIS ---
    CreateLbl(L"Atalho de Sa\u00EDda:", 30, 500, 120, 20);
    HWND hComboKey = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        155, 497, 100, 200, hw, (HMENU)103, hInst, nullptr);
    SendMessageW(hComboKey, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"ESC");
    SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"F4");
    SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"F12");
    SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"END");
    SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"HOME");
    SendMessageW(hComboKey, CB_SETCURSEL, 0, 0);

    CreateLbl(L"Dica: Durante o espelhamento, o atalho escolhido fecha o programa.", 30, 535, 500, 20);

    // Popular listas
    RefreshDisplayList(hw);

    // Bot\u00F5es
    auto CreateBtn = [&](const wchar_t* txt, int x, int y, int w, int h, int id, bool def = false) {
        HWND hB = CreateWindowExW(0, L"BUTTON", txt, WS_CHILD | WS_VISIBLE | (def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                                  x, y, w, h, hw, (HMENU)id, hInst, nullptr);
        SendMessageW(hB, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        return hB;
    };

    CreateBtn(L"\u25B6 Iniciar Espelhamento", 30, 575, 220, 50, 100, true);
    CreateBtn(L"Atualizar Lista", 260, 575, 140, 50, 102);
    CreateBtn(L"Sair", 560, 575, 140, 50, 101);

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
    if (msg == WM_KEYDOWN && wp == g_exitKey) { g_running = false; PostQuitMessage(0); }
    if (msg == WM_DESTROY) { g_running = false; PostQuitMessage(0); }
    return DefWindowProcW(hw, msg, wp, lp);
}

static void CreateMirrorWindow(HINSTANCE hInst, const DisplayInfo& dst) {
    g_dstW = dst.rect.right  - dst.rect.left;
    g_dstH = dst.rect.bottom - dst.rect.top;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MirrorProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor       = nullptr;   // cursor renderizado pelo shader
    wc.lpszClassName = L"SMWnd";
    RegisterClassExW(&wc);

    g_mirrorHwnd = CreateWindowExW(
        WS_EX_TOPMOST, L"SMWnd", L"ScreenMirror v2.2",
        WS_POPUP,
        dst.rect.left, dst.rect.top, g_dstW, g_dstH,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_mirrorHwnd, SW_SHOW);
    UpdateWindow(g_mirrorHwnd);
}

// =============================================================================
// SwapChain + recursos de renderização
// =============================================================================
static void CreateSwapChain() {
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount       = 2;
    scd.BufferDesc.Width  = g_dstW;
    scd.BufferDesc.Height = g_dstH;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = g_mirrorHwnd;
    scd.SampleDesc.Count  = 1;
    scd.Windowed          = TRUE;
    scd.SwapEffect        = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags             = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    HRESULT hr = g_factory->CreateSwapChain(g_device.Get(), &scd, &g_swapChain);
    if (FAILED(hr)) {
        // Fallback sem tearing
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        scd.Flags      = 0;
        hr = g_factory->CreateSwapChain(g_device.Get(), &scd, &g_swapChain);
        if (FAILED(hr)) Fatal("CreateSwapChain falhou", hr);
    }
    g_factory->MakeWindowAssociation(g_mirrorHwnd, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3D11Texture2D> bb;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_device->CreateRenderTargetView(bb.Get(), nullptr, &g_rtv);
}

static void CreateRenderResources() {
    auto vsB = CompileShader(s_vsScreen, "main", "vs_5_0");
    auto psB = CompileShader(s_psScreen, "main", "ps_5_0");
    auto vsC = CompileShader(s_vsCursor, "main", "vs_5_0");
    auto psC = CompileShader(s_psCursor, "main", "ps_5_0");

    g_device->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &g_vsScreen);
    g_device->CreatePixelShader( psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &g_psScreen);
    g_device->CreateVertexShader(vsC->GetBufferPointer(), vsC->GetBufferSize(), nullptr, &g_vsCursor);
    g_device->CreatePixelShader( psC->GetBufferPointer(), psC->GetBufferSize(), nullptr, &g_psCursor);

    // Sampler bilinear para escala
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_device->CreateSamplerState(&sd, &g_sampler);

    // Constant buffer do cursor (16 bytes: pos.xy + size.xy)
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth      = 16;
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_device->CreateBuffer(&bd, nullptr, &g_cursorCB);

    // Alpha blend para o cursor
    D3D11_BLEND_DESC bld = {};
    auto& rt = bld.RenderTarget[0];
    rt.BlendEnable           = TRUE;
    rt.SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp               = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha         = D3D11_BLEND_ONE;
    rt.DestBlendAlpha        = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&bld, &g_alphaBlend);
}

// =============================================================================
// Construção da textura do cursor
// =============================================================================
static void BuildCursorTexture(const std::vector<BYTE>& data,
                                const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info) {
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
        UINT mp = (w + 7) / 8;
        const BYTE* andM = data.data();
        const BYTE* xorM = data.data() + mp * h;
        for (UINT y = 0; y < h; y++)
            for (UINT x = 0; x < w; x++) {
                BYTE a  = (andM[y * mp + x / 8] >> (7 - x % 8)) & 1;
                BYTE xr = (xorM[y * mp + x / 8] >> (7 - x % 8)) & 1;
                if      (!a && !xr) px[y * w + x] = 0xFF000000;
                else if (!a &&  xr) px[y * w + x] = 0xFFFFFFFF;
                else if ( a && !xr) px[y * w + x] = 0x00000000;
                else                px[y * w + x] = 0xFF000000;
            }
    }
    else {  // MASKED_COLOR
        for (UINT y = 0; y < h; y++) {
            const UINT32* src = (const UINT32*)(data.data() + y * info.Pitch);
            for (UINT x = 0; x < w; x++)
                px[y * w + x] = (src[x] & 0xFF000000)
                               ? ((src[x] & 0x00FFFFFF) | 0xFF000000)
                               : src[x];
        }
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA srd = {};
    srd.pSysMem = px.data(); srd.SysMemPitch = w * 4;

    g_cursorTex.Reset();
    g_device->CreateTexture2D(&td, &srd, &g_cursorTex);
    g_cursorTexW = w; g_cursorTexH = h;
}

// =============================================================================
// Renderização de um frame
// =============================================================================
static void RenderFrame(ID3D11Texture2D* frame, RECT srcRect,
                        bool cursorVisible, POINT cursorPos) {
    int srcW = srcRect.right  - srcRect.left;
    int srcH = srcRect.bottom - srcRect.top;

    D3D11_VIEWPORT vp = { 0, 0, (float)g_dstW, (float)g_dstH, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->IASetInputLayout(nullptr);

    // --- Frame ---
    D3D11_TEXTURE2D_DESC td; frame->GetDesc(&td);
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format = td.Format;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> frameSrv;
    g_device->CreateShaderResourceView(frame, &srvd, &frameSrv);

    g_ctx->VSSetShader(g_vsScreen.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_psScreen.Get(), nullptr, 0);
    g_ctx->PSSetSamplers(0, 1, g_sampler.GetAddressOf());
    g_ctx->PSSetShaderResources(0, 1, frameSrv.GetAddressOf());
    g_ctx->Draw(4, 0);

    // --- Cursor ---
    if (cursorVisible && g_cursorTex && g_cursorTexW && g_cursorTexH) {
        float sx = (float)g_dstW / srcW;
        float sy = (float)g_dstH / srcH;

        float cx = (cursorPos.x - srcRect.left) * sx;
        float cy = (cursorPos.y - srcRect.top)  * sy;
        float cw = g_cursorTexW * sx;
        float ch = g_cursorTexH * sy;

        float ndcX =  2.f * cx / g_dstW - 1.f;
        float ndcY = -2.f * cy / g_dstH + 1.f;
        float ndcW =  2.f * cw / g_dstW;
        float ndcH =  2.f * ch / g_dstH;

        D3D11_MAPPED_SUBRESOURCE ms;
        g_ctx->Map(g_cursorCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        float* cb = (float*)ms.pData;
        cb[0] = ndcX; cb[1] = ndcY; cb[2] = ndcW; cb[3] = ndcH;
        g_ctx->Unmap(g_cursorCB.Get(), 0);

        D3D11_TEXTURE2D_DESC ctd; g_cursorTex->GetDesc(&ctd);
        D3D11_SHADER_RESOURCE_VIEW_DESC csrvd = {};
        csrvd.Format = ctd.Format;
        csrvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        csrvd.Texture2D.MipLevels = 1;

        ComPtr<ID3D11ShaderResourceView> curSrv;
        g_device->CreateShaderResourceView(g_cursorTex.Get(), &csrvd, &curSrv);

        float bf[4] = {};
        g_ctx->OMSetBlendState(g_alphaBlend.Get(), bf, 0xFFFFFFFF);
        g_ctx->VSSetShader(g_vsCursor.Get(), nullptr, 0);
        g_ctx->VSSetConstantBuffers(0, 1, g_cursorCB.GetAddressOf());
        g_ctx->PSSetShader(g_psCursor.Get(), nullptr, 0);
        g_ctx->PSSetShaderResources(0, 1, curSrv.GetAddressOf());
        g_ctx->Draw(4, 0);
    }

    // Sem vsync: usa ALLOW_TEARING para latência mínima
    g_swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
}

// =============================================================================
// Loop principal de captura e espelhamento
// =============================================================================
static void RunMirrorLoop(UINT srcAdapter, UINT srcOutput, RECT srcRect) {
    ComPtr<IDXGIAdapter1> adapter;
    g_factory->EnumAdapters1(srcAdapter, &adapter);

    ComPtr<IDXGIOutput> out;
    adapter->EnumOutputs(srcOutput, &out);

    ComPtr<IDXGIOutput1> out1;
    out.As(&out1);

    ComPtr<IDXGIOutputDuplication> dup;
    HRESULT hr = out1->DuplicateOutput(g_device.Get(), &dup);
    if (FAILED(hr)) Fatal(
        "DuplicateOutput falhou.\n"
        "Possíveis causas:\n"
        "- Outra instância do ScreenMirror já está rodando\n"
        "- Driver de exibição não suporta Desktop Duplication (Windows 8+)\n"
        "- Aplicativo rodando em sessão remota (RDP)",
        hr);

    bool frameHeld = false;
    bool cursorVisible = false;
    POINT cursorPos = {};
    std::vector<BYTE> cursorData;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO cursorShape = {};

    MSG msg = {};
    while (g_running) {
        // Eventos de janela (não-bloqueante)
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) { g_running = false; break; }
        }
        if (!g_running) break;

        if (frameHeld) { dup->ReleaseFrame(); frameHeld = false; }

        DXGI_OUTDUPL_FRAME_INFO info = {};
        ComPtr<IDXGIResource> res;
        hr = dup->AcquireNextFrame(8 /*ms timeout*/, &info, &res);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            // Resolução/orientação mudou — reinicializa a duplicação
            dup.Reset();
            hr = out1->DuplicateOutput(g_device.Get(), &dup);
            if (FAILED(hr)) Fatal("Reinit DuplicateOutput failed", hr);
            continue;
        }
        if (FAILED(hr)) Fatal("AcquireNextFrame failed", hr);
        frameHeld = true;

        // Atualiza posição do cursor
        if (info.LastMouseUpdateTime.QuadPart != 0) {
            cursorVisible = info.PointerPosition.Visible != FALSE;
            cursorPos.x   = info.PointerPosition.Position.x;
            cursorPos.y   = info.PointerPosition.Position.y;
        }

        // Atualiza forma do cursor
        if (info.PointerShapeBufferSize > 0) {
            cursorData.resize(info.PointerShapeBufferSize);
            UINT required = 0;
            hr = dup->GetFramePointerShape(
                info.PointerShapeBufferSize,
                cursorData.data(), &required, &cursorShape);
            if (SUCCEEDED(hr)) BuildCursorTexture(cursorData, cursorShape);
        }

        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(res.As<ID3D11Texture2D>(&tex))) continue;

        RenderFrame(tex.Get(), srcRect, cursorVisible, cursorPos);
    }

    if (frameHeld) dup->ReleaseFrame();
}

// =============================================================================
// WinMain
// =============================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // A conscientiza\u00E7\u00E3o de DPI agora est\u00E1 no Manifest para ser carregada antes do DXGI.
    
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&g_factory));
    if (FAILED(hr)) Fatal("CreateDXGIFactory1 failed", hr);

    EnumerateDisplays();
    if (g_displays.empty()) Fatal("Nenhum monitor detectado.");

    if (!ShowSelectionUI(hInst)) return 0;

    const DisplayInfo& src = g_displays[g_srcIdx];
    const DisplayInfo& dst = g_displays[g_dstIdx];

    // Cria device D3D11 no adapter da tela de origem
    ComPtr<IDXGIAdapter1> adapter;
    g_factory->EnumAdapters1(src.adapterIdx, &adapter);

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, flOut;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           0, &fl, 1, D3D11_SDK_VERSION,
                           &g_device, &flOut, &g_ctx);
    if (FAILED(hr)) Fatal("D3D11CreateDevice failed", hr);

    CreateMirrorWindow(hInst, dst);
    CreateSwapChain();
    CreateRenderResources();

    RunMirrorLoop(src.adapterIdx, src.outputIdx, src.rect);
    return 0;
}
