// dinput8.dll proxy: the game loads this DLL from its own folder, we forward DirectInput8Create to
// the real System32 dinput8.dll and start the mod from a background thread.

#include <windows.h>

#include <string>

#include "game.h"
#include "hooks.h"
#include "log.h"
#include "video.h"

namespace {

HMODULE g_self = nullptr;
HMODULE g_realDinput = nullptr;
hooks::DirectInput8CreateFn g_realCreate = nullptr;
bool g_isGame = false;

std::wstring ModuleDir(HMODULE module) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring dir = path;
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
}

bool LoadRealDinput() {
    if (g_realCreate) return true;
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring path = std::wstring(system) + L"\\dinput8.dll";
    g_realDinput = LoadLibraryW(path.c_str());
    if (!g_realDinput) return false;
    g_realCreate = reinterpret_cast<hooks::DirectInput8CreateFn>(GetProcAddress(g_realDinput, "DirectInput8Create"));
    return g_realCreate != nullptr;
}

// Waits for the game's main window so D3D is up before we create our throwaway swap chain.
HWND WaitForGameWindow(DWORD timeoutMs) {
    struct Search {
        DWORD pid;
        HWND found;
    } search{GetCurrentProcessId(), nullptr};
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        EnumWindows(
            [](HWND hwnd, LPARAM param) -> BOOL {
                auto* s = reinterpret_cast<Search*>(param);
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid == s->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
                    s->found = hwnd;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&search));
        if (search.found) return search.found;
        Sleep(250);
    }
    return nullptr;
}

DWORD WINAPI InitThread(LPVOID) {
    logx::Init(ModuleDir(g_self) + L"\\ds3coopmod.log");
    LOG("DS3 Coop Mod starting");
    game::Init();
    video::LoadConfig(ModuleDir(g_self));
    HWND window = WaitForGameWindow(120000);
    LOG("Game window: %p", reinterpret_cast<void*>(window));
    Sleep(1000);
    if (!hooks::InstallRenderHooks()) LOG("Menu disabled: render hooks failed");
    // Borderless works on any window; the FPS/sprint patches are only verified on 1.15.2.
    video::Start(window, game::VersionOk());
    return 0;
}

bool HostIsGame() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"DarkSoulsIII.exe") == 0;
}

}  // namespace

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* out,
                                                   IUnknown* outer) {
    if (!LoadRealDinput()) return E_FAIL;
    const HRESULT hr = g_realCreate(instance, version, iid, out, outer);
    if (SUCCEEDED(hr) && g_isGame) hooks::InstallInputHooks(instance, g_realCreate);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        g_isGame = HostIsGame();
        if (g_isGame) {
            HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (thread) CloseHandle(thread);
        }
    }
    return TRUE;
}
