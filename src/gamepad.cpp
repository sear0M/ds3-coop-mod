#include "gamepad.h"

#include <atomic>
#include <cstdlib>
#include <mutex>

#include "MinHook.h"
#include "imgui.h"
#include "log.h"
#include "menu.h"

namespace gamepad {
namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_realGetState = nullptr;  // trampoline, or the plain function if the hook failed

// Test seam for tests/harness.cpp: when the environment variable DS3COOPMOD_TEST_PAD names a
// shared memory block, controller 0 is read from it (an XINPUT_STATE) instead of the real
// XInput. Players never set it.
bool g_testMode = false;
wchar_t g_testName[128] = {};
std::mutex g_testMutex;
const volatile XINPUT_STATE* g_testState = nullptr;

DWORD ReadTestPad(DWORD index, XINPUT_STATE* state) {
    if (index != 0) return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_testState) {
        std::lock_guard<std::mutex> lock(g_testMutex);
        if (!g_testState) {
            HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, g_testName);
            if (!map) return ERROR_DEVICE_NOT_CONNECTED;
            g_testState = static_cast<const volatile XINPUT_STATE*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(XINPUT_STATE)));
            if (!g_testState) return ERROR_DEVICE_NOT_CONNECTED;
        }
    }
    state->dwPacketNumber = g_testState->dwPacketNumber;
    state->Gamepad.wButtons = g_testState->Gamepad.wButtons;
    state->Gamepad.bLeftTrigger = g_testState->Gamepad.bLeftTrigger;
    state->Gamepad.bRightTrigger = g_testState->Gamepad.bRightTrigger;
    state->Gamepad.sThumbLX = g_testState->Gamepad.sThumbLX;
    state->Gamepad.sThumbLY = g_testState->Gamepad.sThumbLY;
    state->Gamepad.sThumbRX = g_testState->Gamepad.sThumbRX;
    state->Gamepad.sThumbRY = g_testState->Gamepad.sThumbRY;
    return ERROR_SUCCESS;
}

DWORD ReadPad(DWORD index, XINPUT_STATE* state) {
    if (g_testMode) return ReadTestPad(index, state);
    return g_realGetState ? g_realGetState(index, state) : ERROR_DEVICE_NOT_CONNECTED;
}

// What the game must not see after the menu closes, until released (see OnMenuClosed).
std::atomic<WORD> g_hiddenButtons{0};
std::atomic<bool> g_hideLeftTrigger{false};
std::atomic<bool> g_hideRightTrigger{false};
// What is held right now, for OnMenuClosed (which can run on the window thread).
std::atomic<WORD> g_heldButtons{0};
std::atomic<bool> g_heldLeftTrigger{false};
std::atomic<bool> g_heldRightTrigger{false};

// The game's XInputGetState.
DWORD WINAPI XInputGetStateHook(DWORD index, XINPUT_STATE* state) {
    const DWORD result = ReadPad(index, state);
    if (result != ERROR_SUCCESS || !state) return result;
    XINPUT_GAMEPAD& pad = state->Gamepad;
    if (menu::IsOpen()) {
        ZeroMemory(&pad, sizeof(pad));  // the menu has the controller
        return result;
    }
    // Keep hiding a button only while it stays held.
    const WORD hidden = g_hiddenButtons.fetch_and(static_cast<WORD>(pad.wButtons)) & pad.wButtons;
    pad.wButtons &= static_cast<WORD>(~hidden);
    if (g_hideLeftTrigger) {
        if (pad.bLeftTrigger < XINPUT_GAMEPAD_TRIGGER_THRESHOLD) g_hideLeftTrigger = false;
        else pad.bLeftTrigger = 0;
    }
    if (g_hideRightTrigger) {
        if (pad.bRightTrigger < XINPUT_GAMEPAD_TRIGGER_THRESHOLD) g_hideRightTrigger = false;
        else pad.bRightTrigger = 0;
    }
    return result;
}

// Our own view of the controller, updated once per frame on the render thread.
XINPUT_GAMEPAD g_now{};
XINPUT_GAMEPAD g_prev{};
bool g_connected = false;
bool g_slotConnected[XUSER_MAX_COUNT] = {};
ULONGLONG g_lastScan = 0;
std::atomic<bool> g_toggleRequested{false};

int Magnitude(SHORT x, SHORT y) { return std::abs(static_cast<int>(x)) + std::abs(static_cast<int>(y)); }

bool GameWindowInFront() {
    if (g_testMode) return true;  // the harness window stays in the background on purpose
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

}  // namespace

void Init() {
    DWORD len = GetEnvironmentVariableW(L"DS3COOPMOD_TEST_PAD", g_testName, 128);
    g_testMode = len > 0 && len < 128;
    if (g_testMode) LOG("Gamepad: TEST controller from shared memory %ls", g_testName);

    // DS3 imports XInputGetState from xinput1_3.dll, so it is already loaded in the game.
    HMODULE xinput = LoadLibraryW(L"xinput1_3.dll");
    auto target = xinput ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
    if (!target) {
        LOG("Gamepad: xinput1_3.dll not found, controller support off");
        return;
    }
    const MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&XInputGetStateHook),
                                      reinterpret_cast<void**>(&g_realGetState));
    if (s == MH_OK && MH_EnableHook(reinterpret_cast<void*>(target)) == MH_OK) {
        LOG("Gamepad: XInputGetState hooked (xinput1_3)");
    } else {
        // The menu can still read the controller, but the game will also react to it.
        g_realGetState = target;
        LOG("Gamepad: XInputGetState hook failed (%s), the game will see menu input", MH_StatusToString(s));
    }
}

void Update() {
    g_prev = g_now;
    XINPUT_GAMEPAD merged{};
    bool any = false;
    // XInputGetState on an empty slot is slow, so empty slots are only checked every 2 seconds.
    const ULONGLONG now = GetTickCount64();
    const bool rescan = now - g_lastScan >= 2000;
    if (rescan) g_lastScan = now;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (!rescan && !g_slotConnected[i]) continue;
        XINPUT_STATE state{};
        g_slotConnected[i] = ReadPad(i, &state) == ERROR_SUCCESS;
        if (!g_slotConnected[i]) continue;
        any = true;
        const XINPUT_GAMEPAD& p = state.Gamepad;
        merged.wButtons |= p.wButtons;
        if (p.bLeftTrigger > merged.bLeftTrigger) merged.bLeftTrigger = p.bLeftTrigger;
        if (p.bRightTrigger > merged.bRightTrigger) merged.bRightTrigger = p.bRightTrigger;
        if (Magnitude(p.sThumbLX, p.sThumbLY) > Magnitude(merged.sThumbLX, merged.sThumbLY)) {
            merged.sThumbLX = p.sThumbLX;
            merged.sThumbLY = p.sThumbLY;
        }
        if (Magnitude(p.sThumbRX, p.sThumbRY) > Magnitude(merged.sThumbRX, merged.sThumbRY)) {
            merged.sThumbRX = p.sThumbRX;
            merged.sThumbRY = p.sThumbRY;
        }
    }
    if (any != g_connected) LOG("Gamepad: controller %s", any ? "connected" : "disconnected");
    g_connected = any;
    g_now = merged;
    g_heldButtons = merged.wButtons;
    g_heldLeftTrigger = merged.bLeftTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    g_heldRightTrigger = merged.bRightTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

    if (Pressed(XINPUT_GAMEPAD_LEFT_THUMB) && GameWindowInFront()) {
        // Running + L3 is the jump: only a tap with the stick centered (standing still) opens the
        // menu. With the menu open the stick moves the cursor, so L3 always closes it.
        const bool centered = std::abs(static_cast<int>(merged.sThumbLX)) < XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE &&
                              std::abs(static_cast<int>(merged.sThumbLY)) < XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
        if (centered || menu::IsOpen()) {
            g_hiddenButtons |= XINPUT_GAMEPAD_LEFT_THUMB;  // the game does not get this L3
            g_toggleRequested = true;
        }
    }
}

bool TakeMenuToggle() { return g_toggleRequested.exchange(false); }

bool Connected() { return g_connected; }

bool Pressed(WORD button) { return (g_now.wButtons & button) && !(g_prev.wButtons & button); }

bool AnyPressed() { return (g_now.wButtons & ~g_prev.wButtons) != 0; }

WORD Buttons() { return g_now.wButtons; }

void FeedImGui(ImGuiIO& io, bool enabled) {
    const bool active = enabled && g_connected;
    if (active) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    static const XINPUT_GAMEPAD kReleased{};
    const XINPUT_GAMEPAD& p = active ? g_now : kReleased;  // all zero releases every key
    auto button = [&](ImGuiKey key, WORD mask) { io.AddKeyEvent(key, (p.wButtons & mask) != 0); };
    auto stick = [&](ImGuiKey key, int value, int deadzone, int full) {
        const float v = static_cast<float>(value - deadzone) / static_cast<float>(full - deadzone);
        const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        io.AddKeyAnalogEvent(key, clamped > 0.10f, clamped);
    };
    button(ImGuiKey_GamepadDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
    button(ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
    button(ImGuiKey_GamepadDpadUp, XINPUT_GAMEPAD_DPAD_UP);
    button(ImGuiKey_GamepadDpadDown, XINPUT_GAMEPAD_DPAD_DOWN);
    // A selects, B closes lists and popups. X, Y and Start are read by the menu itself: ImGui
    // would use X to move windows and Y for its own text input.
    button(ImGuiKey_GamepadFaceDown, XINPUT_GAMEPAD_A);
    button(ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B);
    const int dz = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    stick(ImGuiKey_GamepadLStickLeft, p.sThumbLX, -dz, -32768);
    stick(ImGuiKey_GamepadLStickRight, p.sThumbLX, dz, 32767);
    stick(ImGuiKey_GamepadLStickUp, p.sThumbLY, dz, 32767);
    stick(ImGuiKey_GamepadLStickDown, p.sThumbLY, -dz, -32768);
    stick(ImGuiKey_GamepadRStickLeft, p.sThumbRX, -dz, -32768);
    stick(ImGuiKey_GamepadRStickRight, p.sThumbRX, dz, 32767);
    stick(ImGuiKey_GamepadRStickUp, p.sThumbRY, dz, 32767);
    stick(ImGuiKey_GamepadRStickDown, p.sThumbRY, -dz, -32768);
}

void OnMenuClosed() {
    g_hiddenButtons |= g_heldButtons.load();
    if (g_heldLeftTrigger) g_hideLeftTrigger = true;
    if (g_heldRightTrigger) g_hideRightTrigger = true;
}

}  // namespace gamepad
