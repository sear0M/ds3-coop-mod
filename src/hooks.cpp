#include "hooks.h"

#define DIRECTINPUT_VERSION 0x0800
#include <d3d11.h>
#include <dinput.h>
#include <dxgi.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "log.h"
#include "menu.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace hooks {
namespace {

// ---------------------------------------------------------------------------------------------
// D3D11 / DXGI
// ---------------------------------------------------------------------------------------------
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn o_Present = nullptr;
ResizeBuffersFn o_ResizeBuffers = nullptr;

std::recursive_mutex g_renderMutex;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_window = nullptr;
WNDPROC o_WndProc = nullptr;
bool g_imguiReady = false;
bool g_imguiFailed = false;
bool g_wasOpen = false;

void ReleaseRtv() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool CreateRtv(IDXGISwapChain* swapChain) {
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer) return false;
    HRESULT hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
    if (FAILED(hr)) {
        // Typeless back buffers need an explicit format.
        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM
                         : desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                                        : desc.Format;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = g_device->CreateRenderTargetView(backBuffer, &rtvDesc, &g_rtv);
    }
    backBuffer->Release();
    return SUCCEEDED(hr);
}

bool IsInputMessage(UINT msg) {
    return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
           msg == WM_INPUT || msg == WM_MOUSEHOVER || msg == WM_MOUSELEAVE || msg == WM_NCMOUSEMOVE;
}

LRESULT CALLBACK WndProcHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const bool keyDown = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const bool firstPress = !(lParam & (1 << 30));
    if (keyDown && firstPress && wParam == menu::kToggleKey) {
        menu::Toggle();
        return 0;
    }
    if (menu::IsOpen()) {
        if (keyDown && wParam == VK_ESCAPE) {
            menu::Close();
            return 0;
        }
        if (g_imguiReady) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
        if (msg == WM_INPUT) return DefWindowProcW(hwnd, msg, wParam, lParam);  // required cleanup
        if (IsInputMessage(msg)) return 0;                                      // keep it away from the game
        if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
            SetCursor(nullptr);  // ImGui draws its own cursor
            return TRUE;
        }
    }
    return CallWindowProcW(o_WndProc, hwnd, msg, wParam, lParam);
}

bool InitImGui(IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc)) || !desc.OutputWindow) return false;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&g_device))) || !g_device) {
        LOG("Swap chain is not D3D11, menu disabled");
        return false;
    }
    g_device->GetImmediateContext(&g_context);
    g_window = desc.OutputWindow;
    g_swapChain = swapChain;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    menu::Setup(g_window);
    if (!ImGui_ImplWin32_Init(g_window) || !ImGui_ImplDX11_Init(g_device, g_context)) {
        LOG("ImGui backend init failed");
        return false;
    }
    o_WndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProcHook)));
    LOG("ImGui ready: window=%p %ux%u", reinterpret_cast<void*>(g_window), desc.BufferDesc.Width,
        desc.BufferDesc.Height);
    return o_WndProc != nullptr;
}

// The game recreated its swap chain (e.g. fullscreen/windowed switch).
void AdoptSwapChain(IDXGISwapChain* swapChain) {
    ReleaseRtv();
    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device))) && device) {
        if (device != g_device) {
            ImGui_ImplDX11_Shutdown();
            if (g_context) g_context->Release();
            if (g_device) g_device->Release();
            g_device = device;
            g_device->GetImmediateContext(&g_context);
            ImGui_ImplDX11_Init(g_device, g_context);
            LOG("New D3D11 device adopted");
        } else {
            device->Release();
        }
    }
    g_swapChain = swapChain;
}

void RenderMenu(IDXGISwapChain* swapChain) {
    const bool open = menu::IsOpen();
    ImGuiIO& io = ImGui::GetIO();
    if (open != g_wasOpen) {
        g_wasOpen = open;
        io.MouseDrawCursor = open;
        io.ClearInputKeys();
        if (open) ClipCursor(nullptr);
    }
    if (!open) return;
    ClipCursor(nullptr);  // the game may clip the cursor to the window center

    if (swapChain != g_swapChain) AdoptSwapChain(swapChain);
    if (!g_rtv && !CreateRtv(swapChain)) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    menu::Render();
    ImGui::Render();

    ID3D11RenderTargetView* oldRtv = nullptr;
    ID3D11DepthStencilView* oldDsv = nullptr;
    g_context->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_context->OMSetRenderTargets(1, &oldRtv, oldDsv);
    if (oldRtv) oldRtv->Release();
    if (oldDsv) oldDsv->Release();
}

HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    {
        std::lock_guard<std::recursive_mutex> lock(g_renderMutex);
        if (!g_imguiReady && !g_imguiFailed) {
            g_imguiReady = InitImGui(swapChain);
            g_imguiFailed = !g_imguiReady;
        }
        if (g_imguiReady) {
            DXGI_SWAP_CHAIN_DESC desc{};
            // Only draw on the game's own window (ignore other swap chains, e.g. overlays).
            if (swapChain == g_swapChain || (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow == g_window))
                RenderMenu(swapChain);
        }
    }
    return o_Present(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE ResizeBuffersHook(IDXGISwapChain* swapChain, UINT count, UINT width, UINT height,
                                            DXGI_FORMAT format, UINT flags) {
    {
        std::lock_guard<std::recursive_mutex> lock(g_renderMutex);
        ReleaseRtv();
    }
    return o_ResizeBuffers(swapChain, count, width, height, format, flags);
}

// Reads Present/ResizeBuffers addresses from a throwaway swap chain on a hidden window.
bool GetSwapChainVtable(void*& present, void*& resizeBuffers) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DS3CoopModDummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) return false;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &sd, &swapChain,
                                           &device, nullptr, &context);
        if (SUCCEEDED(hr)) break;
    }
    if (SUCCEEDED(hr)) {
        void** vtable = *reinterpret_cast<void***>(swapChain);
        present = vtable[8];
        resizeBuffers = vtable[13];
        swapChain->Release();
        device->Release();
        context->Release();
    } else {
        LOG("D3D11CreateDeviceAndSwapChain failed: 0x%08lX", static_cast<unsigned long>(hr));
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------------------------
// DirectInput
// ---------------------------------------------------------------------------------------------
using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

// Two copies: one for the Unicode device vtable, one for the ANSI one.
template <int N>
struct InputHook {
    static inline GetDeviceStateFn originalState = nullptr;
    static inline GetDeviceDataFn originalData = nullptr;

    static HRESULT STDMETHODCALLTYPE State(void* self, DWORD size, LPVOID data) {
        HRESULT hr = originalState(self, size, data);
        // Keyboard (256 bytes) and mouse (DIMOUSESTATE/DIMOUSESTATE2) only. Zeroing a joystick
        // state would read as a stick pushed to one side.
        if (SUCCEEDED(hr) && data && menu::IsOpen() &&
            (size == 256 || size == sizeof(DIMOUSESTATE) || size == sizeof(DIMOUSESTATE2)))
            std::memset(data, 0, size);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE Data(void* self, DWORD objectSize, LPDIDEVICEOBJECTDATA data, LPDWORD count,
                                          DWORD flags) {
        HRESULT hr = originalData(self, objectSize, data, count, flags);
        if (SUCCEEDED(hr) && count && menu::IsOpen()) *count = 0;
        return hr;
    }
};

// GUIDs written out here so we do not need dxguid/dinput8 import libraries.
const GUID kIID_IDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const GUID kIID_IDirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const GUID kGUID_SysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

template <int N>
void HookDeviceVtable(HINSTANCE instance, DirectInput8CreateFn realCreate, const GUID& iid) {
    IDirectInput8W* input = nullptr;
    if (FAILED(realCreate(instance, DIRECTINPUT_VERSION, iid, reinterpret_cast<LPVOID*>(&input), nullptr)) || !input) {
        LOG("DirectInput8Create (%d) failed", N);
        return;
    }
    IDirectInputDevice8W* device = nullptr;
    // CreateDevice has the same slot and ABI on the ANSI and Unicode interfaces.
    if (SUCCEEDED(input->CreateDevice(kGUID_SysKeyboard, &device, nullptr)) && device) {
        void** vtable = *reinterpret_cast<void***>(device);
        MH_STATUS s1 = MH_CreateHook(vtable[9], reinterpret_cast<void*>(&InputHook<N>::State),
                                     reinterpret_cast<void**>(&InputHook<N>::originalState));
        MH_STATUS s2 = MH_CreateHook(vtable[10], reinterpret_cast<void*>(&InputHook<N>::Data),
                                     reinterpret_cast<void**>(&InputHook<N>::originalData));
        if (s1 == MH_OK) MH_EnableHook(vtable[9]);
        if (s2 == MH_OK) MH_EnableHook(vtable[10]);
        LOG("DirectInput hooks (%d): state=%s data=%s", N, MH_StatusToString(s1), MH_StatusToString(s2));
        device->Release();
    }
    input->Release();
}

std::once_flag g_inputOnce;
std::atomic<bool> g_minhookReady{false};

bool EnsureMinHook() {
    if (g_minhookReady) return true;
    const MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        LOG("MH_Initialize failed: %s", MH_StatusToString(s));
        return false;
    }
    g_minhookReady = true;
    return true;
}

}  // namespace

bool InstallRenderHooks() {
    if (!EnsureMinHook()) return false;
    void* present = nullptr;
    void* resizeBuffers = nullptr;
    if (!GetSwapChainVtable(present, resizeBuffers)) return false;
    MH_STATUS s1 = MH_CreateHook(present, reinterpret_cast<void*>(&PresentHook), reinterpret_cast<void**>(&o_Present));
    MH_STATUS s2 = MH_CreateHook(resizeBuffers, reinterpret_cast<void*>(&ResizeBuffersHook),
                                 reinterpret_cast<void**>(&o_ResizeBuffers));
    if (s1 != MH_OK || s2 != MH_OK) {
        LOG("Render hooks failed: present=%s resize=%s", MH_StatusToString(s1), MH_StatusToString(s2));
        return false;
    }
    MH_EnableHook(present);
    MH_EnableHook(resizeBuffers);
    LOG("Render hooks installed (Present=%p)", present);
    return true;
}

void InstallInputHooks(HINSTANCE instance, DirectInput8CreateFn realCreate) {
    std::call_once(g_inputOnce, [&] {
        if (!EnsureMinHook()) return;
        HINSTANCE inst = instance ? instance : GetModuleHandleW(nullptr);
        HookDeviceVtable<0>(inst, realCreate, kIID_IDirectInput8W);
        HookDeviceVtable<1>(inst, realCreate, kIID_IDirectInput8A);
    });
}

}  // namespace hooks
