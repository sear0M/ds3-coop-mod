#include "game.h"

#include <windows.h>

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

}  // namespace game
