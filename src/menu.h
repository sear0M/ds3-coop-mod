#pragma once

#include <windows.h>

namespace menu {

constexpr UINT kToggleKey = VK_INSERT;

// Opens or closes the menu. Called from the window procedure when Insert is pressed, and from the
// Present hook when L3 is tapped on the controller (fromGamepad: the password screen then shows
// the on-screen keyboard).
void Toggle(bool fromGamepad = false);
void Close();

// True while the password prompt or the item window is on screen. Game input is blocked then.
bool IsOpen();

// True when the menu is driven by the controller (opened with L3, or a controller button was
// pressed in it): the password screen shows the on-screen keyboard.
bool PadMode();

// Called once after ImGui is created (fonts and style).
void Setup(HWND window);

// Draws the menu. Called inside an ImGui frame, only while IsOpen().
void Render();

}  // namespace menu
