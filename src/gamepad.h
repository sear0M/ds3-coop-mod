#pragma once

#include <windows.h>

#include <xinput.h>

struct ImGuiIO;

// Xbox-style controller support (XInput, which is also what Steam Input gives the game for
// PlayStation controllers). DS3 reads the controller with XInputGetState from xinput1_3.dll. We
// hook it so the game sees no buttons while the menu is open, and read the controller ourselves to
// open the menu with L3 and to drive it.
namespace gamepad {

// Hooks XInputGetState. Call once, after MinHook is initialized.
void Init();

// Reads the controller. Call once per frame (from Present), with the menu open or closed.
void Update();

// True once after L3 was tapped with the left stick centered while the game window is in front.
// With the stick pushed L3 is left alone, because that is the game's jump (while running).
bool TakeMenuToggle();

bool Connected();
// The button went down in the last Update(). XINPUT_GAMEPAD_* values.
bool Pressed(WORD button);
// Any button went down in the last Update().
bool AnyPressed();
// Buttons held now (XINPUT_GAMEPAD_* bits), for the log.
WORD Buttons();

// Gives the menu's ImGui frame the D-pad, sticks, A and B. Call between the platform backend's
// NewFrame() and ImGui::NewFrame(). With enabled = false (the menu is driven by mouse and
// keyboard) ImGui is told there is no controller: its controller navigation would otherwise keep
// a click from selecting the password box.
void FeedImGui(ImGuiIO& io, bool enabled);

// The menu closed: buttons and triggers still held stay hidden from the game until they are
// released, so the B that closed the menu does not also make the character roll.
void OnMenuClosed();

}  // namespace gamepad
