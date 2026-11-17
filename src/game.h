#pragma once

#include <cstdint>
#include <string>

// Access to Dark Souls III 1.15.2 game state.
namespace game {

// Resolves the exe base address and checks the game version. Call once at startup.
void Init();

// True when the running exe is version 1.15.2 (the only version the addresses below are for).
bool VersionOk();
std::string VersionText();

// True when a character is loaded (not on the title screen or a loading screen).
bool InGame();

// True when the game's item tables (params) are loaded and readable.
bool ParamsReady();

// True when the item id exists in the game's tables (EquipParamWeapon/Protector/Accessory/Goods).
bool ItemExists(uint32_t itemId);

// Highest valid upgrade level (0-10) for base weapon + infusion, or -1 if that combination
// does not exist.
int MaxUpgrade(uint32_t baseWeaponId, uint32_t infusionOffset);

// Adds the item to the local player's inventory. Only the local player gets it: the game
// function used is the same one that hands you items picked up from the ground.
bool GiveItem(uint32_t itemId, uint32_t quantity, std::string& error);

// Goes back to the title screen, the same as "Quit Game" in the game's own menu. Used to reset
// an area when Seamless Co-op leaves a boss stuck.
bool QuitToMenu(std::string& error);

// Id of the last bonfire the local player rested at (e.g. 4002950 = Firelink Shrine), or -1 when
// it cannot be read (wrong version, title screen, never rested).
int LastBonfire();

// English name of a bonfire id ("Firelink Shrine"), or nullptr when the id is not in the list.
const char* BonfireName(int bonfireId);

// Warps the local player to LastBonfire(), like a Homeward Bone. It calls the same game function
// the bonfire "Travel" menu calls, so Seamless Co-op sees it as a normal bonfire travel (it hooks
// that path). Only the local player moves, unless Seamless itself pulls the guests along.
bool WarpToLastBonfire(std::string& error);

}  // namespace game
