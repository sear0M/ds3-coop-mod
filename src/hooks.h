#pragma once

#include <windows.h>

#include <unknwn.h>

namespace hooks {

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);

// Hooks IDXGISwapChain::Present/ResizeBuffers to draw the menu. Called from the init thread.
bool InstallRenderHooks();

// Hooks DirectInput keyboard/mouse reads so the game ignores input while the menu is open.
// Called from our DirectInput8Create proxy, the first time the game creates DirectInput.
void InstallInputHooks(HINSTANCE instance, DirectInput8CreateFn realCreate);

}  // namespace hooks
