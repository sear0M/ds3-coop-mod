#pragma once

#include <windows.h>

#include <string>

struct IDXGISwapChain;

// FPS unlock, sprint fix and borderless fullscreen. Settings live in ds3coopmod.ini next to the
// mod (one per player, the launcher never touches it).
//
// Technique credit: DS3DebugFPS by 0dm (Apache-2.0), https://github.com/0dm/ds3debugfps
//   - FPS: the game has a debug frame limiter in its SprjFlipper object (float at +0x354, enabled
//     by the byte at +0x358). We only write those two values, no game code is changed.
//   - Sprint: above ~82 FPS a movement check tuned for 60 FPS slows sprinting; three float
//     constants it reads are redirected to values computed from the real frame time.
namespace video {

// Reads ds3coopmod.ini (creates it with defaults if missing). Call once, early.
void LoadConfig(const std::wstring& modDir);

// Called from the init thread once the game window exists: installs the window hooks for
// borderless mode and, when gamePatches is true (exe is 1.15.2), applies the FPS limit and the
// sprint fix.
void Start(HWND gameWindow, bool gamePatches);

// Called every frame from the Present hook.
void OnPresent(IDXGISwapChain* swapChain);

// Called from the game window procedure hook. Returns true when the message was ours.
bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

}  // namespace video
