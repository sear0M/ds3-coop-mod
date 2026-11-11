#include "hooks.h"

#define DIRECTINPUT_VERSION 0x0800
#include <d3d11.h>
#include <dinput.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstring>
#include <mutex>
#include <unordered_map>

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

// ---------------------------------------------------------------------------------------------
// Mouse while the menu is open
//
// DS3 keeps the Windows cursor locked in the middle of the window (SetCursorPos/ClipCursor) and
// may read the mouse straight from DirectInput or Raw Input. While the menu is open we:
//   1. ignore the game's SetCursorPos/ClipCursor calls, so the real cursor can move;
//   2. if the real cursor still does not move (no WM_MOUSE* messages reach the window), drive a
//      virtual cursor from the DirectInput / Raw Input mouse deltas the game receives.
// ---------------------------------------------------------------------------------------------
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
SetCursorPosFn o_SetCursorPos = nullptr;
ClipCursorFn o_ClipCursor = nullptr;

std::atomic<int> g_mouseDx{0}, g_mouseDy{0}, g_mouseWheel{0};
std::atomic<unsigned> g_mouseButtons{0};    // bit 0 left, 1 right, 2 middle
std::atomic<bool> g_windowMouseSeen{false};  // window got real mouse messages since the menu opened
std::atomic<int> g_centerX{-1}, g_centerY{-1};  // where we put the cursor when the menu opened (client)
ImVec2 g_virtualPos{0.0f, 0.0f};
unsigned g_buttonsFed = 0;
bool g_loggedVirtual = false;

BOOL WINAPI SetCursorPosHook(int x, int y) {
    if (menu::IsOpen()) return TRUE;  // the game recenters the cursor every frame
    return o_SetCursorPos(x, y);
}

BOOL WINAPI ClipCursorHook(const RECT* rect) {
    if (menu::IsOpen() && rect) return TRUE;  // let the cursor leave the 1-pixel cage
    return o_ClipCursor(rect);
}

void SetButton(int index, bool down) {
    const unsigned bit = 1u << index;
    if (down) g_mouseButtons.fetch_or(bit);
    else g_mouseButtons.fetch_and(~bit);
}

void ResetMouseForMenu() {
    g_mouseDx = 0;
    g_mouseDy = 0;
    g_mouseWheel = 0;
    g_mouseButtons = 0;
    g_buttonsFed = 0;
    g_windowMouseSeen = false;
    RECT rc{};
    GetClientRect(g_window, &rc);
    g_virtualPos = ImVec2((rc.right - rc.left) * 0.5f, (rc.bottom - rc.top) * 0.5f);
    POINT center{(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
    g_centerX = center.x;
    g_centerY = center.y;
    ClientToScreen(g_window, &center);
    if (o_ClipCursor) o_ClipCursor(nullptr);
    else ClipCursor(nullptr);
    if (GetForegroundWindow() == g_window) {  // never grab the cursor when the game is in the background
        if (o_SetCursorPos) o_SetCursorPos(center.x, center.y);
        else SetCursorPos(center.x, center.y);
    }
}

// Called between ImGui_ImplWin32_NewFrame() and ImGui::NewFrame(), so it overrides the backend's
// GetCursorPos() fallback when the virtual cursor is in use.
void ApplyVirtualMouse(ImGuiIO& io) {
    const int dx = g_mouseDx.exchange(0), dy = g_mouseDy.exchange(0), wheel = g_mouseWheel.exchange(0);
    if (g_windowMouseSeen) {  // the real cursor works: the Win32 backend handles everything
        if (io.MousePos.x > -FLT_MAX) g_virtualPos = io.MousePos;
        return;
    }
    RECT rc{};
    GetClientRect(g_window, &rc);
    const float w = static_cast<float>(rc.right - rc.left), h = static_cast<float>(rc.bottom - rc.top);
    const float speed = std::max(1.0f, w / 1920.0f) * 1.25f;
    if (dx || dy) {
        if (!g_loggedVirtual) {
            LOG("Menu mouse: using DirectInput/Raw Input deltas (window gets no mouse messages)");
            g_loggedVirtual = true;
        }
        g_virtualPos.x = std::clamp(g_virtualPos.x + dx * speed, 0.0f, std::max(0.0f, w - 1.0f));
        g_virtualPos.y = std::clamp(g_virtualPos.y + dy * speed, 0.0f, std::max(0.0f, h - 1.0f));
    }
    io.AddMousePosEvent(g_virtualPos.x, g_virtualPos.y);
    const unsigned buttons = g_mouseButtons.load();
    for (int i = 0; i < 3; ++i)
        if ((buttons ^ g_buttonsFed) & (1u << i)) io.AddMouseButtonEvent(i, (buttons >> i) & 1u);
    g_buttonsFed = buttons;
    if (wheel) io.AddMouseWheelEvent(0.0f, static_cast<float>(wheel) / WHEEL_DELTA);
}

void ReadRawMouse(LPARAM lParam) {
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) ==
            static_cast<UINT>(-1) ||
        raw.header.dwType != RIM_TYPEMOUSE)
        return;
    const RAWMOUSE& m = raw.data.mouse;
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        g_mouseDx += m.lLastX;
        g_mouseDy += m.lLastY;
    }
    const USHORT f = m.usButtonFlags;
    if (f & RI_MOUSE_LEFT_BUTTON_DOWN) SetButton(0, true);
    if (f & RI_MOUSE_LEFT_BUTTON_UP) SetButton(0, false);
    if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) SetButton(1, true);
    if (f & RI_MOUSE_RIGHT_BUTTON_UP) SetButton(1, false);
    if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) SetButton(2, true);
    if (f & RI_MOUSE_MIDDLE_BUTTON_UP) SetButton(2, false);
    if (f & RI_MOUSE_WHEEL) g_mouseWheel += static_cast<SHORT>(m.usButtonData);
}

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
        // Real mouse messages mean the Windows cursor works. The WM_MOUSEMOVE caused by our own
        // recentering when the menu opened does not count.
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
            const bool ownRecenter = msg == WM_MOUSEMOVE && static_cast<short>(LOWORD(lParam)) == g_centerX &&
                                     static_cast<short>(HIWORD(lParam)) == g_centerY;
            if (!ownRecenter) g_windowMouseSeen = true;
        }
        if (g_imguiReady) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
        if (msg == WM_INPUT) {
            ReadRawMouse(lParam);
            return DefWindowProcW(hwnd, msg, wParam, lParam);  // required cleanup
        }
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
        if (open) ResetMouseForMenu();
    }
    if (!open) return;

    if (swapChain != g_swapChain) AdoptSwapChain(swapChain);
    if (!g_rtv && !CreateRtv(swapChain)) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ApplyVirtualMouse(io);
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

// Buffered data has no device type in it: keyboard key codes and mouse axis offsets overlap.
bool IsMouseDevice(void* self) {
    static std::mutex mutex;
    static std::unordered_map<void*, bool> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(self);
    if (it != cache.end()) return it->second;
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    const bool mouse = SUCCEEDED(static_cast<IDirectInputDevice8W*>(self)->GetCapabilities(&caps)) &&
                       GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_MOUSE;
    cache[self] = mouse;
    return mouse;
}

// Copy the game's mouse movement into the menu's virtual cursor before we hide it from the game.
void CaptureMouseState(const void* data, DWORD size) {
    const auto* s = static_cast<const DIMOUSESTATE*>(data);  // DIMOUSESTATE2 starts the same way
    g_mouseDx += s->lX;
    g_mouseDy += s->lY;
    g_mouseWheel += s->lZ;
    for (int i = 0; i < 3; ++i) SetButton(i, (s->rgbButtons[i] & 0x80) != 0);
}

void CaptureMouseEvents(const DIDEVICEOBJECTDATA* data, DWORD objectSize, DWORD count) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    for (DWORD i = 0; i < count; ++i) {
        const auto* e = reinterpret_cast<const DIDEVICEOBJECTDATA*>(bytes + i * objectSize);
        const LONG value = static_cast<LONG>(e->dwData);
        switch (e->dwOfs) {
            case DIMOFS_X: g_mouseDx += value; break;
            case DIMOFS_Y: g_mouseDy += value; break;
            case DIMOFS_Z: g_mouseWheel += value; break;
            case DIMOFS_BUTTON0: SetButton(0, (e->dwData & 0x80) != 0); break;
            case DIMOFS_BUTTON1: SetButton(1, (e->dwData & 0x80) != 0); break;
            case DIMOFS_BUTTON2: SetButton(2, (e->dwData & 0x80) != 0); break;
            default: break;
        }
    }
}

// Two copies: one for the Unicode device vtable, one for the ANSI one.
template <int N>
struct InputHook {
    static inline GetDeviceStateFn originalState = nullptr;
    static inline GetDeviceDataFn originalData = nullptr;

    static HRESULT STDMETHODCALLTYPE State(void* self, DWORD size, LPVOID data) {
        HRESULT hr = originalState(self, size, data);
        if (SUCCEEDED(hr) && data && menu::IsOpen()) {
            const bool mouse = size == sizeof(DIMOUSESTATE) || size == sizeof(DIMOUSESTATE2);
            if (mouse) CaptureMouseState(data, size);
            // Keyboard (256 bytes) and mouse only. Zeroing a joystick state would read as a stick
            // pushed to one side.
            if (mouse || size == 256) std::memset(data, 0, size);
        }
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE Data(void* self, DWORD objectSize, LPDIDEVICEOBJECTDATA data, LPDWORD count,
                                          DWORD flags) {
        HRESULT hr = originalData(self, objectSize, data, count, flags);
        if (SUCCEEDED(hr) && count && menu::IsOpen()) {
            if (data && *count && objectSize >= sizeof(DWORD) * 2 && !(flags & DIGDD_PEEK) && IsMouseDevice(self))
                CaptureMouseEvents(data, objectSize, *count);
            *count = 0;
        }
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

    // Stop the game from pinning the cursor while the menu is open.
    void* target = nullptr;
    MH_STATUS s3 = MH_CreateHookApiEx(L"user32", "SetCursorPos", reinterpret_cast<void*>(&SetCursorPosHook),
                                      reinterpret_cast<void**>(&o_SetCursorPos), &target);
    if (s3 == MH_OK) MH_EnableHook(target);
    MH_STATUS s4 = MH_CreateHookApiEx(L"user32", "ClipCursor", reinterpret_cast<void*>(&ClipCursorHook),
                                      reinterpret_cast<void**>(&o_ClipCursor), &target);
    if (s4 == MH_OK) MH_EnableHook(target);
    LOG("Cursor hooks: SetCursorPos=%s ClipCursor=%s", MH_StatusToString(s3), MH_StatusToString(s4));
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
