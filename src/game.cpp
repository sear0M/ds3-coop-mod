#include "game.h"

#include <windows.h>

#include <cstring>
#include <cwchar>
#include <mutex>
#include <vector>

#include "log.h"
#include "memory.h"

namespace game {
namespace {

// Offsets from the exe base for DARK SOULS III 1.15.2 (exe timestamp 0x639C4DDD).
// Source: veeenu/darksoulsiii-practice-tool, lib/libds3/src/codegen/base_addresses.rs.
// kGiveItemFn was checked against the exe: it is a function start listed in .pdata.
constexpr uintptr_t kWorldChrMan = 0x477FDB8;  // WorldChrMan*
constexpr uintptr_t kMapItemMan = 0x4769240;   // MapItemMan*
constexpr uintptr_t kGiveItemFn = 0x7C4080;    // void ItemGib(MapItemMan*, ItemRequest*, uint32_t*)
constexpr uintptr_t kParamRepo = 0x479B8B0;    // SoloParamRepository*
constexpr uintptr_t kMenuMan = 0x4763258;      // MenuMan*

constexpr uintptr_t kWorldChrManPlayer = 0x80;  // WorldChrMan -> PlayerIns*
// MenuMan + 0x250: writing 1 makes the game quit to the title screen (practice tool "Quitout",
// lib/libds3/src/pointers.rs: pointer_chain!(menu_man, 0x250)).
constexpr uintptr_t kMenuManQuitout = 0x250;

// Bonfire warp. Source: The Grand Archives' DS3 cheat table (app ver. 1.15.2), script
// "BonfireWarp" by inuNorii. Checked against the 1.15.2 exe:
// - kLuaWarpFn runs the event script "Lua_Warp" (string at exe+0x276E968). The game calls it
//   from exactly two places, both bonfire travel menus (exe+0x770984 and exe+0xAF231C). Seamless
//   Co-op hooks the handler of that script (ds3sc.dll names it Lua_Warp_1 / LuaWarp1Info).
// - Its arguments: the bonfire you travel from, the bonfire you travel to, both as the bonfire's
//   entity id (= warp id - 1000, so Firelink Shrine 4002950 -> 4001950), and a flag.
// - kSprjLuaEventMan is the singleton kLuaWarpFn null-checks ("SprjLuaEventMan" error string).
// - GameMan + 0xACC is the table's "Last Bonfire" and is written by SetLastBonfire
//   (exe+0x628960). The GameMan address comes from the table's pattern 48 8B ?? ?? ?? ?? 04 89 48 28 C3.
constexpr uintptr_t kGameMan = 0x475AC00;          // GameMan*
constexpr uintptr_t kSprjLuaEventMan = 0x4751B08;  // SprjLuaEventMan*
constexpr uintptr_t kLuaWarpFn = 0x475F00;         // void LuaWarp(SprjLuaEventMan*, int from, int to, int flag)
constexpr uintptr_t kGameManLastBonfire = 0xACC;   // int, e.g. 4002950
constexpr uintptr_t kLuaEventManState = 0x08;      // kLuaWarpFn does nothing while this is null
constexpr int kBonfireEntityOffset = 1000;
// First bytes of kLuaWarpFn in the 1.15.2 exe (mov rax,rsp; push rbp; push r12..r15).
constexpr unsigned char kLuaWarpPrologue[] = {0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
using LuaWarpFn = void (*)(void* luaEventMan, int fromEntity, int toEntity, int flag);

// Bonfire warp ids and names, from the same cheat table ("TargetBonfireId" list).
struct Bonfire {
    int id;
    const char* name;
};
const Bonfire kBonfires[] = {
    {4002959, "Ashen Grave"}, {4002950, "Firelink Shrine"}, {4002951, "Cemetery of Ash"},
    {4002952, "Iudex Gundyr"}, {4002953, "Untended Graves"}, {4002954, "Champion Gundyr"},
    {3002950, "High Wall of Lothric"}, {3002955, "Tower on the Wall"}, {3002952, "Vordt of the Boreal Valley"},
    {3002954, "Dancer of the Boreal Valley"}, {3002951, "Oceiros, the Consumed King"},
    {3002960, "High Wall of Lothric, Teleport"}, {3102954, "Foot of the High Wall"}, {3102950, "Undead Settlement"},
    {3102952, "Cliff Underside"}, {3102953, "Dilapidated Bridge"}, {3102951, "Pit of Hollows"},
    {3302956, "Road of Sacrifices"}, {3302950, "Halfway Fortress"}, {3302957, "Crucifixion Woods"},
    {3302952, "Crystal Sage"}, {3302953, "Farron Keep"}, {3302954, "Keep Ruins"},
    {3302958, "Farron Keep Perimeter"}, {3302955, "Old Wolf of Farron"}, {3302951, "Abyss Watchers"},
    {3502953, "Cathedral of the Deep"}, {3502950, "Cleansing Chapel"}, {3502951, "Deacons of the Deep"},
    {3502952, "Rosaria's Bed Chamber"}, {3802956, "Catacombs of Carthus"}, {3802950, "High Lord Wolnir"},
    {3802951, "Abandoned Tomb"}, {3802952, "Old King's Antechamber"}, {3802953, "Demon Ruins"},
    {3802954, "Old Demon King"}, {3702957, "Irithyll of the Boreal Valley"}, {3702954, "Central Irithyll"},
    {3702950, "Church of Yorshka"}, {3702955, "Distant Manor"}, {3702951, "Pontiff Sulyvahn"},
    {3702956, "Water Reserve"}, {3702953, "Anor Londo"}, {3702958, "Prison Tower"},
    {3702952, "Aldrich, Devourer of Gods"}, {3902950, "Irithyll Dungeon"}, {3902952, "Profaned Capital"},
    {3902951, "Yhorm the Giant"}, {3012950, "Lothric Castle"}, {3012952, "Dragon Barracks"},
    {3012951, "Dragonslayer Armour"}, {3412951, "Grand Archives"}, {3412950, "Twin Princes"},
    {3202950, "Archdragon Peak"}, {3202953, "Dragon-Kin Mausoleum"}, {3202952, "Great Belfry"},
    {3202951, "Nameless King"}, {4102950, "Flameless Shrine"}, {4102951, "Kiln of the First Flame"},
    {4102952, "The First Flame"}, {4502951, "Snowfield"}, {4502952, "Rope Bridge Cave"},
    {4502953, "Corvian Settlement"}, {4502954, "Snowy Mountain Pass"}, {4502955, "Ariandel Chapel"},
    {4502950, "Sister Friede"}, {4502957, "Depths of the Painting"}, {4502956, "Champion's Gravetender"},
    {5002951, "The Dreg Heap"}, {5002952, "Earthen Peak Ruins"}, {5002953, "Within the Earthen Peak Ruins"},
    {5002950, "The Demon Prince"}, {5102110, "The Ringed City"}, {5102952, "Mausoleum Lookout"},
    {5102953, "Ringed Inner Wall"}, {5102954, "Ringed City Streets"}, {5102955, "Shared Grave"},
    {5102950, "Church of Filianore"}, {5112951, "Filianore's Rest"}, {5112950, "Slave Knight Gael"},
    {5102951, "Darkeater Midir"},
};

// A Homeward Bone takes a few seconds (animation + loading screen). Pressing the button again in
// that time would queue a second warp.
constexpr ULONGLONG kWarpCooldownMs = 15000;
ULONGLONG g_lastWarpAt = 0;

uintptr_t g_base = 0;
bool g_versionOk = false;
std::string g_versionText = "?";

struct ParamTable {
    uintptr_t data = 0;
    uint16_t count = 0;
};
std::mutex g_paramMutex;
ParamTable g_tables[4];  // weapon, protector, accessory, goods
const wchar_t* const kTableNames[4] = {L"EquipParamWeapon", L"EquipParamProtector", L"EquipParamAccessory",
                                       L"EquipParamGoods"};

struct ParamRow {
    uint64_t id;
    int64_t offset;
    uint64_t unk;
};
static_assert(sizeof(ParamRow) == 24, "ParamRow layout");

#pragma pack(push, 4)
struct ItemRequest {
    uint32_t count;  // number of item entries in this request (always 1)
    uint32_t itemId;
    uint32_t quantity;
    uint32_t durability;
};
#pragma pack(pop)
using GiveItemFn = void (*)(void* mapItemMan, ItemRequest* request, uint32_t* result);

bool ReadParamName(uintptr_t entry, wchar_t* out, size_t outLen) {
    uint64_t len = 0;
    if (!mem::Read(entry + 0x20, len) || len == 0 || len >= outLen) return false;
    const void* src = nullptr;
    if (len <= 7) {
        src = reinterpret_cast<const void*>(entry + 0x10);  // short names are stored inline
    } else {
        uintptr_t p = 0;
        if (!mem::Read(entry + 0x10, p)) return false;
        src = reinterpret_cast<const void*>(p);
    }
    if (!mem::Readable(src, len * sizeof(wchar_t))) return false;
    std::memcpy(out, src, len * sizeof(wchar_t));
    out[len] = 0;
    return true;
}

// Walks SoloParamRepository looking for a param table by name.
bool FindParamTable(const wchar_t* name, ParamTable& out) {
    uintptr_t repo = 0, start = 0, end = 0;
    if (!mem::Read(g_base + kParamRepo, repo) || !repo) return false;
    if (!mem::Read(repo + 0x10, start) || !mem::Read(repo + 0x18, end)) return false;
    if (end <= start || (end - start) / sizeof(uintptr_t) > 4096) return false;
    for (uintptr_t it = start; it < end; it += sizeof(uintptr_t)) {
        uintptr_t entry = 0;
        if (!mem::Read(it, entry) || !entry) continue;
        wchar_t buf[96];
        if (!ReadParamName(entry, buf, 96) || std::wcscmp(buf, name) != 0) continue;
        uintptr_t holder = 0, data = 0;
        uint16_t count = 0;
        if (!mem::Read(entry + 0x68, holder) || !mem::Read(holder + 0x68, data) || !data) return false;
        if (!mem::Read(data + 0x0A, count) || count == 0) return false;
        if (!mem::Readable(reinterpret_cast<const void*>(data + 0x40), count * sizeof(ParamRow))) return false;
        out.data = data;
        out.count = count;
        return true;
    }
    return false;
}

int TableIndexForItem(uint32_t itemId) {
    switch (itemId >> 28) {
        case 0x0: return 0;  // weapons (also arrows/bolts)
        case 0x1: return 1;  // armor
        case 0x2: return 2;  // rings
        case 0x4: return 3;  // goods (consumables, spells, keys...)
        default: return -1;
    }
}

bool GetTable(int index, ParamTable& out) {
    std::lock_guard<std::mutex> lock(g_paramMutex);
    if (!g_tables[index].data) {
        ParamTable t;
        if (!FindParamTable(kTableNames[index], t)) return false;
        g_tables[index] = t;
        LOG("Param %ls: %u rows", kTableNames[index], t.count);
    }
    out = g_tables[index];
    return true;
}

bool RowExists(const ParamTable& t, uint64_t rowId) {
    // Param rows are sorted by id, the game itself binary-searches them.
    const auto* rows = reinterpret_cast<const ParamRow*>(t.data + 0x40);
    int lo = 0, hi = static_cast<int>(t.count) - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint64_t id = rows[mid].id;
        if (id == rowId) return true;
        if (id < rowId) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

bool ReadFileVersion(const wchar_t* path, uint16_t v[4]) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (!size) return false;
    std::vector<unsigned char> buf(size);
    if (!GetFileVersionInfoW(path, 0, size, buf.data())) return false;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return false;
    v[0] = HIWORD(info->dwFileVersionMS);
    v[1] = LOWORD(info->dwFileVersionMS);
    v[2] = HIWORD(info->dwFileVersionLS);
    v[3] = LOWORD(info->dwFileVersionLS);
    return true;
}

}  // namespace

void Init() {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    uint16_t v[4] = {};
    if (ReadFileVersion(exe, v)) {
        char text[32];
        std::snprintf(text, sizeof(text), "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
        g_versionText = text;
        g_versionOk = v[0] == 1 && v[1] == 15 && v[2] == 2;
    }
    LOG("Game exe base=%p version=%s supported=%d", reinterpret_cast<void*>(g_base), g_versionText.c_str(),
        g_versionOk ? 1 : 0);
}

bool VersionOk() { return g_versionOk; }

std::string VersionText() { return g_versionText; }

bool InGame() {
    if (!g_versionOk) return false;
    uintptr_t worldChrMan = 0, player = 0;
    if (!mem::Read(g_base + kWorldChrMan, worldChrMan) || !worldChrMan) return false;
    if (!mem::Read(worldChrMan + kWorldChrManPlayer, player) || !player) return false;
    return mem::Readable(reinterpret_cast<const void*>(player), 0x100);
}

bool ParamsReady() {
    if (!g_versionOk) return false;
    ParamTable t;
    for (int i = 0; i < 4; ++i)
        if (!GetTable(i, t)) return false;
    return true;
}

bool ItemExists(uint32_t itemId) {
    if (!g_versionOk) return false;
    const int index = TableIndexForItem(itemId);
    if (index < 0) return false;
    ParamTable t;
    if (!GetTable(index, t)) return false;
    return RowExists(t, itemId & 0x0FFFFFFF);
}

int MaxUpgrade(uint32_t baseWeaponId, uint32_t infusionOffset) {
    const uint32_t id = baseWeaponId + infusionOffset;
    if (!ItemExists(id)) return -1;
    for (int level = 10; level > 0; --level)
        if (ItemExists(id + level)) return level;
    return 0;
}

bool GiveItem(uint32_t itemId, uint32_t quantity, std::string& error) {
    if (!g_versionOk) {
        error = "Versão do jogo não suportada (" + g_versionText + "). O mod é para a 1.15.2.";
        return false;
    }
    if (!InGame()) {
        error = "Entre com um personagem antes de pegar itens.";
        return false;
    }
    if (!ParamsReady()) {
        error = "As tabelas de itens do jogo ainda não carregaram.";
        return false;
    }
    if (!ItemExists(itemId)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Item 0x%08X não existe no jogo.", itemId);
        error = buf;
        return false;
    }
    uintptr_t mapItemMan = 0;
    if (!mem::Read(g_base + kMapItemMan, mapItemMan) || !mapItemMan) {
        error = "MapItemMan não encontrado.";
        return false;
    }
    ItemRequest request{1, itemId, quantity ? quantity : 1, 100};
    uint32_t result[16] = {};
    auto fn = reinterpret_cast<GiveItemFn>(g_base + kGiveItemFn);
    LOG("GiveItem id=0x%08X qty=%u", itemId, request.quantity);
    fn(reinterpret_cast<void*>(mapItemMan), &request, result);
    return true;
}

bool QuitToMenu(std::string& error) {
    if (!g_versionOk) {
        error = "Sair para o menu só funciona no jogo 1.15.2 (este é " + g_versionText + ").";
        return false;
    }
    if (!InGame()) {
        error = "Você já está fora do jogo (tela de título ou carregando).";
        return false;
    }
    uintptr_t menuMan = 0;
    if (!mem::Read(g_base + kMenuMan, menuMan) || !menuMan || !mem::Write<uint8_t>(menuMan + kMenuManQuitout, 1)) {
        error = "MenuMan não encontrado.";
        return false;
    }
    LOG("Quit to menu");
    return true;
}

int LastBonfire() {
    if (!g_versionOk || !InGame()) return -1;
    uintptr_t gameMan = 0;
    int32_t id = -1;
    if (!mem::Read(g_base + kGameMan, gameMan) || !gameMan || !mem::Read(gameMan + kGameManLastBonfire, id)) return -1;
    // Real ids have 7 digits (3002950...5402950). 999 is the "Shadow Realm" placeholder, -1/0 unset.
    if (id < 1000000 || id > 9999999) return -1;
    return id;
}

const char* BonfireName(int bonfireId) {
    for (const auto& b : kBonfires)
        if (b.id == bonfireId) return b.name;
    return nullptr;
}

bool WarpToLastBonfire(std::string& error) {
    if (!g_versionOk) {
        error = "Voltar para a fogueira só funciona no jogo 1.15.2 (este é " + g_versionText + ").";
        LOG("Bonfire warp refused: version %s", g_versionText.c_str());
        return false;
    }
    if (!InGame()) {
        error = "Carregue um personagem antes de voltar para a fogueira.";
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_lastWarpAt && now - g_lastWarpAt < kWarpCooldownMs) {
        error = "Já está voltando para a fogueira, espere a tela de carregamento.";
        return false;
    }
    const int bonfire = LastBonfire();
    if (bonfire < 0) {
        error = "Não achei a última fogueira. Descanse numa fogueira primeiro.";
        LOG("Bonfire warp refused: no last bonfire");
        return false;
    }
    uintptr_t luaEventMan = 0, luaState = 0;
    if (!mem::Read(g_base + kSprjLuaEventMan, luaEventMan) || !luaEventMan ||
        !mem::Read(luaEventMan + kLuaEventManState, luaState) || !luaState) {
        error = "O jogo ainda não está pronto para viajar (SprjLuaEventMan). Tente de novo em alguns segundos.";
        LOG("Bonfire warp refused: SprjLuaEventMan=%p state=%p", reinterpret_cast<void*>(luaEventMan),
            reinterpret_cast<void*>(luaState));
        return false;
    }
    // The bytes must be the 1.15.2 function, or a jump if another mod (Seamless) hooked it.
    const auto* code = reinterpret_cast<const unsigned char*>(g_base + kLuaWarpFn);
    if (!mem::Readable(code, sizeof(kLuaWarpPrologue)) ||
        (std::memcmp(code, kLuaWarpPrologue, sizeof(kLuaWarpPrologue)) != 0 && code[0] != 0xE9 &&
         !(code[0] == 0xFF && code[1] == 0x25))) {
        error = "A função de viagem do jogo não foi encontrada (exe diferente do 1.15.2 esperado).";
        LOG("Bonfire warp refused: unexpected bytes at exe+0x%llX", static_cast<unsigned long long>(kLuaWarpFn));
        return false;
    }
    // Same arguments as the cheat table: travel from and to the last bonfire. Flag 0xA is what it
    // passes for the two Untended Graves bonfires (that area is a separate "ceremony" of Firelink).
    const int entity = bonfire - kBonfireEntityOffset;
    const int flag = (bonfire == 4002953 || bonfire == 4002954) ? 0xA : 0;
    const char* name = BonfireName(bonfire);
    LOG("Bonfire warp to %d (%s) flag=%d", bonfire, name ? name : "?", flag);
    g_lastWarpAt = now;
    reinterpret_cast<LuaWarpFn>(g_base + kLuaWarpFn)(reinterpret_cast<void*>(luaEventMan), entity, entity, flag);
    return true;
}

}  // namespace game
