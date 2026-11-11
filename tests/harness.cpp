// Test harness: a tiny D3D11 app that pretends to be DarkSoulsIII.exe so the mod (dinput8.dll)
// loads into it. It opens the menu with F12, types the password, clicks around and saves
// screenshots of what the mod drew. No game is needed; game-specific features (giving items)
// correctly refuse to run because the harness is not version 1.15.2.
//
// Build + run: powershell -ExecutionPolicy Bypass -File tests\run_harness.ps1

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>

#include <d3d11.h>
#include <dinput.h>

#include <cstdio>
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
    void (*run)(HWND);
};

}  // namespace

int main(int argc, char** argv) {
    const char* password = argc > 1 ? argv[1] : "";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
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
    const Step steps[] = {
        {4.0, [](HWND h) { Key(h, VK_F12); }},
        {4.6, [](HWND) { SaveBackBuffer("1_senha.bmp"); }},
        {4.8, [](HWND h) { Type(h, "senha-errada"); Key(h, VK_RETURN); }},
        {5.3, [](HWND) { SaveBackBuffer("2_senha_errada.bmp"); }},
        {5.5, [](HWND h) { Type(h, s_password.c_str()); Key(h, VK_RETURN); }},
        {6.2, [](HWND) { SaveBackBuffer("3_itens.bmp"); }},
        // Coordinates match the 1280x720 layout seen in 3_itens.png.
        {6.4, [](HWND h) { Click(h, 210, 254); }},  // "Sets de armadura"
        {7.0, [](HWND) { SaveBackBuffer("4_sets.bmp"); }},
        {7.2, [](HWND h) { Click(h, 1010, 155); }},  // "Adicionar set" on the first set
        {7.8, [](HWND) { SaveBackBuffer("5_dar_item.bmp"); }},
        {8.0, [](HWND h) { Click(h, 290, 97); }},  // search box
        {8.3, [](HWND h) { Type(h, "moon"); }},
        {8.9, [](HWND) { SaveBackBuffer("6_busca.bmp"); }},
        {9.1, [](HWND h) { Key(h, VK_F12); }},
        {9.7, [](HWND) { SaveBackBuffer("7_fechado.bmp"); }},
        {9.9, [](HWND h) { PostMessageW(h, WM_CLOSE, 0, 0); }},
    };
    size_t next = 0;

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
        while (next < sizeof(steps) / sizeof(steps[0]) && t >= steps[next].at) steps[next++].run(hwnd);
        if (t > 12.0) break;
    }
    std::printf("done\n");
    return 0;
}
