#pragma once

#include <windows.h>

namespace menu {

constexpr UINT kToggleKey = VK_F12;

// Called from the window procedure when F12 is pressed.
void Toggle();
void Close();

// True while the password prompt or the item window is on screen. Game input is blocked then.
bool IsOpen();

// Called once after ImGui is created (fonts and style).
void Setup(HWND window);

// Draws the menu. Called inside an ImGui frame, only while IsOpen().
void Render();

}  // namespace menu
