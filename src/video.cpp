#include "video.h"

#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "MinHook.h"
#include "log.h"
#include "memory.h"

namespace video {
namespace {

// ---------------------------------------------------------------------------------------------
// Settings (ds3coopmod.ini)
// ---------------------------------------------------------------------------------------------
constexpr const char* kDefaultIni =
    "; DS3 Coop Mod - opcoes de video. Cada jogador tem o seu arquivo, o launcher nao mexe aqui.\r\n"
    "; Depois de mudar, feche e abra o jogo de novo.\r\n"
    "\r\n"
    "[Video]\r\n"
    "; Limite de FPS.\r\n"
    ";   0   = usa a taxa de atualizacao do monitor (ex.: 144 num monitor de 144 Hz)\r\n"
    ";   60  = igual ao jogo original\r\n"
    ";   120, 144, 165, 240... = o valor que voce escolher (maximo 360)\r\n"
    "fps_limit = 0\r\n"
    "\r\n"
    "; Tela cheia sem borda: 1 = ligado, 0 = desligado.\r\n"
    "; Para funcionar, deixe o jogo em modo \"Janela\" nas opcoes de video do jogo,\r\n"
    "; de preferencia com a resolucao do seu monitor.\r\n"
    "borderless = 1\r\n"
    "\r\n"
    "; Corrige a corrida mais lenta acima de ~82 FPS: 1 = ligado, 0 = desligado.\r\n"
    "fix_sprint = 1\r\n";

int g_fpsSetting = 0;
bool g_borderless = true;
bool g_fixSprint = true;
float g_fps = 60.0f;  // resolved limit

// ---------------------------------------------------------------------------------------------
// Game objects
// ---------------------------------------------------------------------------------------------
constexpr uintptr_t kFlipperDebugFps = 0x354;   // float
constexpr uintptr_t kFlipperUseDebug = 0x358;   // byte
constexpr uintptr_t kFlipperFrameTime = 0x26C;  // float, measured seconds per frame

uintptr_t g_moduleBase = 0;
size_t g_moduleSize = 0;
std::atomic<uintptr_t> g_flipper{0};
float* g_speedFactors = nullptr;  // [0] stuck-check scale, [1] slowdown rate, [2] recovery rate

// ---------------------------------------------------------------------------------------------
// Borderless
// ---------------------------------------------------------------------------------------------
constexpr UINT kApplyBorderlessMsg = WM_APP + 0x6D5;
using SetWindowLongWFn = LONG(WINAPI*)(HWND, int, LONG);
using SetWindowLongPtrWFn = LONG_PTR(WINAPI*)(HWND, int, LONG_PTR);
using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
SetWindowLongWFn o_SetWindowLongW = nullptr;
SetWindowLongPtrWFn o_SetWindowLongPtrW = nullptr;
SetWindowPosFn o_SetWindowPos = nullptr;

std::atomic<HWND> g_window{nullptr};
std::atomic<bool> g_windowed{false};  // the swap chain is not exclusive fullscreen
std::atomic<bool> g_applyPending{false};
unsigned g_frame = 0;

void ModuleRange() {
    g_moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_moduleBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_moduleBase + dos->e_lfanew);
    g_moduleSize = nt->OptionalHeader.SizeOfImage;
}

// Pattern scan over committed, readable pages of the game exe. '?' in mask = any byte.
uintptr_t Scan(uintptr_t from, const unsigned char* pattern, const char* mask) {
    const size_t len = std::strlen(mask);
    const uintptr_t end = g_moduleBase + g_moduleSize;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t region = from; region < end && VirtualQuery(reinterpret_cast<void*>(region), &mbi, sizeof(mbi));
         region = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
        const uintptr_t regionEnd = std::min(reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize, end);
        for (uintptr_t p = std::max(region, from); p + len <= regionEnd; ++p) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(p);
            size_t j = 0;
            while (j < len && (mask[j] != 'x' || bytes[j] == pattern[j])) ++j;
            if (j == len) return p;
        }
    }
    return 0;
}

bool InModule(uintptr_t p) { return p >= g_moduleBase && p < g_moduleBase + g_moduleSize; }

// SprjFlipper is created with operator new(0x368); the pointer is then stored in a global:
//   mov ecx, 368h ; call alloc ; ... ; mov [rip+slot], rax
uintptr_t FindFlipper() {
    const unsigned char ctor[] = {0xB9, 0x68, 0x03, 0x00, 0x00, 0xE8};
    for (uintptr_t hit = g_moduleBase; (hit = Scan(hit, ctor, "xxxxxx")) != 0; ++hit) {
        for (int i = 0; i < 64 - 7; ++i) {
            const auto* b = reinterpret_cast<const unsigned char*>(hit + i);
            if (!mem::Readable(b, 7)) break;
            if (b[0] == 0x48 && b[1] == 0x89 && b[2] == 0x05) {
                int32_t rel = 0;
                std::memcpy(&rel, b + 3, 4);
                const uintptr_t slot = hit + i + 7 + rel;
                uintptr_t object = 0, vtable = 0;
                if (mem::Read(slot, object) && object && !(object & 7) && mem::Read(object, vtable) && InModule(vtable) &&
                    mem::Readable(reinterpret_cast<const void*>(object), 0x368))
                    return object;
                break;
            }
        }
    }
    return 0;
}

void WriteFlipperFps(uintptr_t flipper) {
    float current = 0.0f;
    unsigned char useDebug = 0;
    if (!mem::Read(flipper + kFlipperDebugFps, current) || !mem::Read(flipper + kFlipperUseDebug, useDebug)) return;
    if (current != g_fps || useDebug != 1) {
        *reinterpret_cast<float*>(flipper + kFlipperDebugFps) = g_fps;
        *reinterpret_cast<unsigned char*>(flipper + kFlipperUseDebug) = 1;
    }
}

void UpdateSpeedFactors() {
    if (!g_speedFactors) return;
    float frameTime = 1.0f / g_fps;
    float measured = 0.0f;
    const uintptr_t flipper = g_flipper;
    if (flipper && mem::Read(flipper + kFlipperFrameTime, measured) && measured >= 1.0f / 480.0f && measured <= 0.05f)
        frameTime = measured;
    frameTime = std::clamp(frameTime, 1.0f / 480.0f, 0.05f);
    const float slack = g_fps >= 90.0f ? 1.2f : 1.0f;
    const float recovery = g_fps >= 90.0f ? 1.5f : 1.2f;
    g_speedFactors[0] = 0.5f / frameTime * slack;  // 30.0 at 60 FPS
    g_speedFactors[1] = std::exp(std::log(0.8f) * frameTime * 60.0f);
    g_speedFactors[2] = std::exp(std::log(recovery) * frameTime * 60.0f);
}

bool PatchDisp32(uintptr_t disp, const void* target) {
    const int64_t rel = reinterpret_cast<intptr_t>(target) - static_cast<intptr_t>(disp + 4);
    if (rel < INT32_MIN || rel > INT32_MAX) return false;
    const int32_t rel32 = static_cast<int32_t>(rel);
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(disp), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(disp), &rel32, 4);
    VirtualProtect(reinterpret_cast<void*>(disp), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(disp), 4);
    return true;
}

// Movement "stuck" check: (distance * 30.0 < 1) assumes 60 FPS. Redirect its constants.
void FixSprint() {
    const unsigned char pattern[] = {0xF3, 0x0F, 0x58, 0x00, 0x0F, 0xC6, 0x00, 0x00, 0x0F, 0x51, 0x00,
                                     0xF3, 0x0F, 0x59, 0x05, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x2F};
    const uintptr_t hit = Scan(g_moduleBase, pattern, "xxx?xx?xxx?xxxx????xx");
    if (!hit) {
        LOG("Sprint fix: movement check not found, left alone");
        return;
    }
    // The new constants must be within 2 GB of the code (rip-relative operands).
    void* page = nullptr;
    for (uintptr_t hint = (g_moduleBase + g_moduleSize + 0xFFFF) & ~uintptr_t(0xFFFF);
         !page && hint < g_moduleBase + 0x70000000; hint += 0x10000)
        page = VirtualAlloc(reinterpret_cast<void*>(hint), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!page) {
        LOG("Sprint fix: no memory near the game code");
        return;
    }
    g_speedFactors = static_cast<float*>(page);
    UpdateSpeedFactors();
    bool ok = PatchDisp32(hit + 15, &g_speedFactors[0]);
    const auto* b = reinterpret_cast<const unsigned char*>(hit);
    if (std::memcmp(b + 0x29, "\xF3\x0F\x59\x05", 4) == 0 && std::memcmp(b + 0x54, "\xF3\x0F\x59\x05", 4) == 0) {
        ok = PatchDisp32(hit + 0x2D, &g_speedFactors[1]) && ok;
        ok = PatchDisp32(hit + 0x58, &g_speedFactors[2]) && ok;
    }
    LOG("Sprint fix at exe+0x%llX: %s", static_cast<unsigned long long>(hit - g_moduleBase), ok ? "on" : "partial");
}

int MonitorRefresh(HWND window) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return static_cast<int>(dm.dmDisplayFrequency);
    return 60;
}

RECT MonitorRect(HWND window) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor;
}

LONG BorderlessStyle(LONG style) {
    return (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP | WS_VISIBLE;
}

bool ShouldForce(HWND hwnd) {
    return g_borderless && hwnd && hwnd == g_window.load() && g_windowed && !IsIconic(hwnd);
}

// Runs on the window's own thread (posted from the Present hook) to avoid cross-thread deadlocks.
void ApplyBorderless(HWND hwnd) {
    if (!ShouldForce(hwnd) || !o_SetWindowLongW || !o_SetWindowPos) return;
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const RECT target = MonitorRect(hwnd);
    RECT now{};
    GetWindowRect(hwnd, &now);
    if (style == BorderlessStyle(style) && EqualRect(&now, &target)) return;
    o_SetWindowLongW(hwnd, GWL_STYLE, BorderlessStyle(style));
    o_SetWindowPos(hwnd, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top,
                   SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    LOG("Borderless: window set to %ldx%ld at %ld,%ld", target.right - target.left, target.bottom - target.top,
        target.left, target.top);
}

// The game resets its window style/size when the resolution or mode changes: keep it borderless.
LONG WINAPI SetWindowLongWHook(HWND hwnd, int index, LONG value) {
    if (index == GWL_STYLE && ShouldForce(hwnd)) value = BorderlessStyle(value);
    return o_SetWindowLongW(hwnd, index, value);
}

LONG_PTR WINAPI SetWindowLongPtrWHook(HWND hwnd, int index, LONG_PTR value) {
    if (index == GWL_STYLE && ShouldForce(hwnd)) value = BorderlessStyle(static_cast<LONG>(value));
    return o_SetWindowLongPtrW(hwnd, index, value);
}

BOOL WINAPI SetWindowPosHook(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (ShouldForce(hwnd) && !(flags & SWP_NOSIZE)) {
        const RECT r = MonitorRect(hwnd);
        x = r.left;
        y = r.top;
        cx = r.right - r.left;
        cy = r.bottom - r.top;
        flags &= ~static_cast<UINT>(SWP_NOMOVE);
    }
    return o_SetWindowPos(hwnd, after, x, y, cx, cy, flags);
}

template <class Fn>
bool HookUser32(const char* name, void* detour, Fn* original) {
    void* target = nullptr;
    const MH_STATUS s = MH_CreateHookApiEx(L"user32", name, detour, reinterpret_cast<void**>(original), &target);
    if (s == MH_OK) MH_EnableHook(target);
    LOG("Window hook %s: %s", name, MH_StatusToString(s));
    return s == MH_OK;
}

void FlipperLoop() {
    // The flipper exists once the renderer is up; keep re-applying in case the game resets it.
    bool logged = false;
    for (int attempt = 0;; ++attempt) {
        uintptr_t flipper = g_flipper;
        if (!flipper && attempt < 120) {
            flipper = FindFlipper();
            if (flipper) {
                g_flipper = flipper;
                LOG("FPS: SprjFlipper at %p, limit %.0f", reinterpret_cast<void*>(flipper), g_fps);
            }
        }
        if (flipper) {
            WriteFlipperFps(flipper);
            if (!logged) {
                LOG("FPS: limit applied (%.0f)", g_fps);
                logged = true;
            }
        } else if (attempt >= 120) {
            LOG("FPS: SprjFlipper not found, FPS stays at 60");
            return;
        }
        Sleep(flipper ? 2000 : 250);
    }
}

}  // namespace

void LoadConfig(const std::wstring& modDir) {
    const std::wstring ini = modDir + L"\\ds3coopmod.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(ini.c_str(), L"wb")) {
            std::fwrite(kDefaultIni, 1, std::strlen(kDefaultIni), f);
            std::fclose(f);
        }
    }
    g_fpsSetting = static_cast<int>(GetPrivateProfileIntW(L"Video", L"fps_limit", 0, ini.c_str()));
    g_borderless = GetPrivateProfileIntW(L"Video", L"borderless", 1, ini.c_str()) != 0;
    g_fixSprint = GetPrivateProfileIntW(L"Video", L"fix_sprint", 1, ini.c_str()) != 0;
    LOG("Video settings: fps_limit=%d borderless=%d fix_sprint=%d", g_fpsSetting, g_borderless ? 1 : 0,
        g_fixSprint ? 1 : 0);
}

void Start(HWND gameWindow, bool gamePatches) {
    ModuleRange();
    g_window = gameWindow;
    const int fps = g_fpsSetting > 0 ? g_fpsSetting : MonitorRefresh(gameWindow);
    g_fps = static_cast<float>(std::clamp(fps, 30, 360));
    LOG("FPS target: %.0f (%s)", g_fps, g_fpsSetting > 0 ? "ds3coopmod.ini" : "monitor refresh rate");

    if (!gamePatches) {
        LOG("FPS unlock and sprint fix off: game version is not 1.15.2");
        g_fps = 60.0f;
    } else if (g_fps != 60.0f) {
        if (g_fixSprint && g_fps > 61.0f) FixSprint();
        std::thread(FlipperLoop).detach();
    }
    if (g_borderless) {
        HookUser32("SetWindowLongW", reinterpret_cast<void*>(&SetWindowLongWHook), &o_SetWindowLongW);
        HookUser32("SetWindowLongPtrW", reinterpret_cast<void*>(&SetWindowLongPtrWHook), &o_SetWindowLongPtrW);
        HookUser32("SetWindowPos", reinterpret_cast<void*>(&SetWindowPosHook), &o_SetWindowPos);
    }
}

void OnPresent(IDXGISwapChain* swapChain) {
    UpdateSpeedFactors();
    if (!g_borderless || (++g_frame % 30) != 0) return;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc)) || !desc.OutputWindow) return;
    BOOL fullscreen = FALSE;
    swapChain->GetFullscreenState(&fullscreen, nullptr);
    g_windowed = !fullscreen;
    if (!g_window.load()) g_window = desc.OutputWindow;
    if (desc.OutputWindow != g_window.load() || fullscreen) return;
    const LONG style = GetWindowLongW(desc.OutputWindow, GWL_STYLE);
    RECT now{};
    GetWindowRect(desc.OutputWindow, &now);
    const RECT target = MonitorRect(desc.OutputWindow);
    if ((style != BorderlessStyle(style) || !EqualRect(&now, &target)) && !g_applyPending.exchange(true))
        PostMessageW(desc.OutputWindow, kApplyBorderlessMsg, 0, 0);
}

bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM, LPARAM) {
    if (msg != kApplyBorderlessMsg) return false;
    g_applyPending = false;
    ApplyBorderless(hwnd);
    return true;
}

}  // namespace video
