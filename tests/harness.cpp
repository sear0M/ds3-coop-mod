// Test harness: a tiny D3D11 app that pretends to be DarkSoulsIII.exe so the mod (dinput8.dll)
// loads into it. It opens the menu with Insert, types the password, clicks around and saves
// screenshots of what the mod drew. With "pad" it does the same with a simulated controller
// (L3, on-screen keyboard) and checks what the game gets from the controller. No game is needed;
// game-specific features (giving items, "Voltar para a fogueira", "Sair para o menu") correctly
// refuse to run because the harness is not version 1.15.2.
//
// Build + run: powershell -ExecutionPolicy Bypass -File tests\run_harness.ps1

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>

#include <d3d11.h>
#include <dinput.h>
#include <xinput.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;

const GUID kIID_IDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const GUID kGUID_SysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

void SaveBackBuffer(const char* path) {
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    g_device->CreateTexture2D(&desc, nullptr, &staging);
    g_context->CopyResource(staging, back);
    D3D11_MAPPED_SUBRESOURCE map{};
    g_context->Map(staging, 0, D3D11_MAP_READ, 0, &map);

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(desc.Width);
    ih.biHeight = -static_cast<LONG>(desc.Height);  // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    const DWORD imageSize = desc.Width * desc.Height * 4;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + imageSize;
    FILE* f = std::fopen(path, "wb");
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    std::vector<unsigned char> row(desc.Width * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* src = static_cast<const unsigned char*>(map.pData) + y * map.RowPitch;
        for (UINT x = 0; x < desc.Width; ++x) {  // RGBA -> BGRA
            row[x * 4 + 0] = src[x * 4 + 2];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + 0];
            row[x * 4 + 3] = 255;
        }
        std::fwrite(row.data(), row.size(), 1, f);
    }
    std::fclose(f);
    g_context->Unmap(staging, 0);
    staging->Release();
    back->Release();
    std::printf("saved %s\n", path);
}

void Key(HWND hwnd, WPARAM vk) {
    PostMessageW(hwnd, WM_KEYDOWN, vk, 1);
    PostMessageW(hwnd, WM_KEYUP, vk, (1u << 31) | (1u << 30) | 1);
}

void Type(HWND hwnd, const char* text) {
    for (const char* p = text; *p; ++p) PostMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(*p), 1);
}

void Click(HWND hwnd, int x, int y) {
    const LPARAM pos = MAKELPARAM(x, y);
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, pos);
    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
    PostMessageW(hwnd, WM_LBUTTONUP, 0, pos);
}

struct Step {
    double at;  // seconds after start
    std::function<void(HWND)> run;
};

// --- Controller ("pad" mode) ---------------------------------------------------------------
// When DS3COOPMOD_TEST_PAD names a shared memory block (run_harness.ps1 sets it), the mod reads
// controller 0 from it instead of XInput. The harness writes the buttons there, and checks what
// the "game" gets by calling xinput1_3!XInputGetState, the function DS3 uses and the mod hooks.
volatile XINPUT_STATE* g_pad = nullptr;
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_gameGetState = nullptr;

bool OpenTestPad() {
    wchar_t name[128] = {};
    if (!GetEnvironmentVariableW(L"DS3COOPMOD_TEST_PAD", name, 128)) return false;
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(XINPUT_STATE), name);
    if (!map) return false;
    g_pad = static_cast<volatile XINPUT_STATE*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(XINPUT_STATE)));
    HMODULE xinput = LoadLibraryW(L"xinput1_3.dll");
    g_gameGetState = xinput ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
    return g_pad && g_gameGetState;
}

void Pad(WORD buttons, SHORT leftStickX = 0) {
    g_pad->Gamepad.wButtons = buttons;
    g_pad->Gamepad.sThumbLX = leftStickX;
    g_pad->dwPacketNumber = g_pad->dwPacketNumber + 1;
}

// Prints "PAD OK"/"PAD FALHA" for run_harness.ps1.
void GameSees(const char* what, WORD mask, bool expectDown) {
    XINPUT_STATE s{};
    const DWORD r = g_gameGetState(0, &s);
    const bool down = r == ERROR_SUCCESS && (s.Gamepad.wButtons & mask) != 0;
    std::printf("PAD %s %s (game buttons=0x%04X)\n", down == expectDown ? "OK   " : "FALHA", what, s.Gamepad.wButtons);
}

// Same layout as the mod's on-screen keyboard (src/menu.cpp kKeyRows); the cursor starts on Q.
const char* const kKeyRows[] = {"1234567890", "qwertyuiop", "asdfghjkl_", "zxcvbnm,.-", "!@#$%&*?+="};

// Like the game's window procedure: counts the messages that tell it the window changed size,
// position or style once the window is set up. DS3 crashed when it handled the ones caused by the
// mod's borderless resize, so the mod must change the window without the "game" seeing any of
// them. A WM_WINDOWPOSCHANGED with SWP_NOSIZE | SWP_NOMOVE (Windows refreshing the frame after a
// style change, no WM_SIZE follows) changes nothing and is only printed.
bool g_countResize = false;
int g_resizeSeen = 0;

LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_countResize && (msg == WM_SIZE || msg == WM_MOVE || msg == WM_WINDOWPOSCHANGED || msg == WM_STYLECHANGED)) {
        const UINT flags = msg == WM_WINDOWPOSCHANGED ? reinterpret_cast<const WINDOWPOS*>(lParam)->flags : 0;
        const bool noChange = msg == WM_WINDOWPOSCHANGED && (flags & (SWP_NOSIZE | SWP_NOMOVE)) == (SWP_NOSIZE | SWP_NOMOVE);
        if (!noChange) ++g_resizeSeen;
        std::printf("game got message 0x%04X flags=0x%X%s\n", msg, flags, noChange ? " (no size/position change)" : "");
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int main(int argc, char** argv) {
    // Arguments: [password] [borderless|pad], in any order (PowerShell 5.1 drops an empty password).
    const char* password = "";
    bool borderlessTest = false;
    bool padTest = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "borderless") borderlessTest = true;
        else if (std::string(argv[i]) == "pad") padTest = true;
        else password = argv[i];
    }
    if (padTest && !OpenTestPad()) {
        std::printf("pad test: set DS3COOPMOD_TEST_PAD and make sure xinput1_3.dll exists\n");
        return 1;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = GameWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HarnessWindow";
    RegisterClassExW(&wc);
    RECT rc{0, 0, kWidth, kHeight};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    // Off-screen and not activated, so the test does not get in the way of the desktop.
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"DARK SOULS III", WS_OVERLAPPEDWINDOW, -4000, -4000,
                                rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    // Like the game: create DirectInput (this goes through the mod's proxy).
    IDirectInput8W* input = nullptr;
    HRESULT hr = DirectInput8Create(wc.hInstance, DIRECTINPUT_VERSION, kIID_IDirectInput8W,
                                    reinterpret_cast<void**>(&input), nullptr);
    std::printf("DirectInput8Create: 0x%08lX\n", static_cast<unsigned long>(hr));
    IDirectInputDevice8W* keyboard = nullptr;
    if (input) input->CreateDevice(kGUID_SysKeyboard, &keyboard, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = kWidth;
    sd.BufferDesc.Height = kHeight;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;  // keeps the back buffer after Present, for screenshots
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                       &sd, &g_swapChain, &g_device, nullptr, &g_context);
    if (FAILED(hr)) {
        std::printf("D3D11 init failed 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();

    static std::string s_password = password;
    // "borderless" mode: the mod should turn this window into a borderless window covering the
    // monitor while the back buffer stays 1280x720; the menu must still be centered and sized right.
    const std::vector<Step> borderlessSteps = {
        {4.2, [](HWND h) {
             RECT r{};
             GetWindowRect(h, &r);
             const LONG style = GetWindowLongW(h, GWL_STYLE);
             std::printf("window style=0x%08lX caption=%d rect=%ld,%ld %ldx%ld screen=%dx%d\n", style,
                         (style & WS_CAPTION) == WS_CAPTION ? 1 : 0, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
             std::printf("game saw %d resize messages\n", g_resizeSeen);
         }},
        {4.3, [](HWND h) { Key(h, VK_INSERT); }},
        {4.9, [](HWND) { SaveBackBuffer("b_senha.bmp"); }},
        // Late enough for the mod's crash guard (DS3COOPMOD_GUARD_MS from run_harness.ps1) to clear.
        {6.0, [](HWND h) { PostMessageW(h, WM_CLOSE, 0, 0); }},
    };
    // Coordinates match the 1280x720 layout seen in the screenshots (01_senha.png, 05_itens.png).
    const std::vector<Step> menuSteps = {
        {4.0, [](HWND h) { Key(h, VK_INSERT); }},
        {4.6, [](HWND) { SaveBackBuffer("01_senha.bmp"); }},
        // "Voltar para a fogueira" needs no password, so friends use it from this screen.
        {4.8, [](HWND h) { Click(h, 640, 404); }},
        {5.4, [](HWND) { SaveBackBuffer("02_fogueira_confirmar.bmp"); }},
        {5.6, [](HWND h) { Click(h, 508, 411); }},  // "Voltar" (refused: not the real game)
        {6.2, [](HWND) { SaveBackBuffer("03_fogueira_recusado.bmp"); }},
        {6.4, [](HWND h) { Click(h, 640, 336); }},  // password box
        {6.6, [](HWND h) { Type(h, "senha-errada"); Key(h, VK_RETURN); }},
        {7.1, [](HWND) { SaveBackBuffer("04_senha_errada.bmp"); }},
        {7.3, [](HWND h) { Type(h, s_password.c_str()); Key(h, VK_RETURN); }},
        {8.0, [](HWND) { SaveBackBuffer("05_itens.bmp"); }},
        {8.2, [](HWND h) { Click(h, 210, 254); }},  // "Sets de armadura"
        {8.8, [](HWND) { SaveBackBuffer("06_sets.bmp"); }},
        {9.0, [](HWND h) { Click(h, 1010, 155); }},  // "Adicionar set" on the first set
        {9.6, [](HWND) { SaveBackBuffer("07_dar_item.bmp"); }},
        {9.8, [](HWND h) { Click(h, 290, 97); }},  // search box
        {10.1, [](HWND h) { Type(h, "moon"); }},
        {10.7, [](HWND) { SaveBackBuffer("08_busca.bmp"); }},
        {10.9, [](HWND h) { Click(h, 958, 641); }},  // "Voltar para a fogueira" in the footer
        {11.5, [](HWND) { SaveBackBuffer("09_fogueira_rodape.bmp"); }},
        {11.7, [](HWND h) { Click(h, 611, 411); }},  // "Cancelar"
        {11.9, [](HWND h) { Click(h, 1078, 641); }},  // "Sair para o menu"
        {12.5, [](HWND) { SaveBackBuffer("10_sair_confirmar.bmp"); }},
        {12.7, [](HWND h) { Click(h, 508, 411); }},  // "Sair" (refused: not the real game)
        {13.3, [](HWND) { SaveBackBuffer("11_sair_recusado.bmp"); }},
        {13.5, [](HWND h) { Key(h, VK_INSERT); }},
        {14.1, [](HWND) { SaveBackBuffer("12_fechado.bmp"); }},
        {14.3, [](HWND h) { PostMessageW(h, WM_CLOSE, 0, 0); }},
    };
    // "pad" mode: the same menu driven only by a controller, opened with L3, the password typed on
    // the on-screen keyboard. It also checks what the game gets from the controller.
    std::vector<Step> padSteps;
    double t = 3.8;
    auto add = [&](double delay, std::function<void(HWND)> run) {
        t += delay;
        padSteps.push_back({t, std::move(run)});
    };
    auto tap = [&](WORD button) {
        add(0.0, [button](HWND) { Pad(button); });
        add(0.08, [](HWND) { Pad(0); });
        t += 0.08;
    };
    if (padTest) {
        constexpr WORD kRB = XINPUT_GAMEPAD_RIGHT_SHOULDER, kL3 = XINPUT_GAMEPAD_LEFT_THUMB, kB = XINPUT_GAMEPAD_B;
        add(0.0, [](HWND) { GameSees("no buttons held", 0xFFFF, false); });
        // Menu closed: the game gets the controller.
        add(0.2, [](HWND) { Pad(kRB); });
        add(0.15, [](HWND) { GameSees("menu closed: the game gets RB", kRB, true); });
        add(0.05, [](HWND) { Pad(0); });
        // Running (stick pushed) + L3 is the jump: no menu, the game gets L3.
        add(0.2, [](HWND) { Pad(kL3, 30000); });
        add(0.15, [](HWND) { GameSees("running + L3: the game gets the jump", kL3, true); });
        add(0.05, [](HWND) { Pad(0); });
        add(0.4, [](HWND) { SaveBackBuffer("p01_l3_correndo.bmp"); });  // no menu
        // Standing still + L3 opens the menu, and the game does not get that L3.
        add(0.2, [](HWND) { Pad(kL3); });
        add(0.15, [](HWND) { GameSees("standing + L3: the game does not get it", kL3, false); });
        add(0.05, [](HWND) { Pad(0); });
        add(0.5, [](HWND) { SaveBackBuffer("p02_teclado.bmp"); });
        // Menu open: the game gets nothing.
        add(0.2, [](HWND) { Pad(kRB); });
        add(0.15, [](HWND) { GameSees("menu open: the game gets nothing", 0xFFFF, false); });
        add(0.05, [](HWND) { Pad(0); });
        // Type the password: D-pad to each key, A to press it, Y for an upper case letter.
        t += 0.2;
        int row = 1, col = 0;
        for (const char* p = s_password.c_str(); *p; ++p) {
            char ch = *p;
            if (ch >= 'A' && ch <= 'Z') {
                tap(XINPUT_GAMEPAD_Y);
                ch = static_cast<char>(ch - 'A' + 'a');
            }
            int r = -1, c = -1;
            for (int i = 0; i < 5 && r < 0; ++i)
                if (const char* f = std::strchr(kKeyRows[i], ch)) r = i, c = static_cast<int>(f - kKeyRows[i]);
            if (r < 0) {
                std::printf("pad test: '%c' is not on the on-screen keyboard\n", ch);
                continue;
            }
            for (; row < r; ++row) tap(XINPUT_GAMEPAD_DPAD_DOWN);
            for (; row > r; --row) tap(XINPUT_GAMEPAD_DPAD_UP);
            for (; col < c; ++col) tap(XINPUT_GAMEPAD_DPAD_RIGHT);
            for (; col > c; --col) tap(XINPUT_GAMEPAD_DPAD_LEFT);
            tap(XINPUT_GAMEPAD_A);
        }
        add(0.3, [](HWND) { SaveBackBuffer("p03_senha_digitada.bmp"); });
        tap(XINPUT_GAMEPAD_START);
        add(0.5, [](HWND) { SaveBackBuffer("p04_itens.bmp"); });
        tap(kRB);  // next category
        add(0.4, [](HWND) { SaveBackBuffer("p05_categoria.bmp"); });
        tap(XINPUT_GAMEPAD_DPAD_RIGHT);  // into the item list
        tap(XINPUT_GAMEPAD_DPAD_DOWN);
        tap(XINPUT_GAMEPAD_A);  // give it (refused: not the real game)
        add(0.4, [](HWND) { SaveBackBuffer("p06_dar_item.bmp"); });
        // B closes the menu. While it is still held the game must not get it (B is roll).
        add(0.2, [](HWND) { Pad(kB); });
        add(0.2, [](HWND) { GameSees("B that closed the menu: the game does not get it", kB, false); });
        add(0.05, [](HWND) { SaveBackBuffer("p07_fechado.bmp"); });
        add(0.1, [](HWND) { Pad(0); });
        add(0.2, [](HWND) { Pad(kB); });
        add(0.15, [](HWND) { GameSees("next B: the game gets it again", kB, true); });
        add(0.05, [](HWND) { Pad(0); });
        add(0.3, [](HWND h) { PostMessageW(h, WM_CLOSE, 0, 0); });
    }
    const std::vector<Step>& steps = padTest ? padSteps : (borderlessTest ? borderlessSteps : menuSteps);
    const size_t stepCount = steps.size();
    const double endAt = steps.empty() ? 17.0 : steps.back().at + 3.0;
    size_t next = 0;

    g_countResize = true;
    const ULONGLONG start = GetTickCount64();
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(hwnd)) break;
        const double t = (GetTickCount64() - start) / 1000.0;
        // Draw a "game frame", then present (the mod draws inside Present).
        const float color[4] = {0.10f, 0.12f, 0.16f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, color);
        g_swapChain->Present(1, 0);
        if (padTest) {  // like DS3, read the controller every frame
            XINPUT_STATE frameState{};
            g_gameGetState(0, &frameState);
        }
        while (next < stepCount && t >= steps[next].at) steps[next++].run(hwnd);
        if (t > endAt) break;
    }
    std::printf("done\n");
    return 0;
}
