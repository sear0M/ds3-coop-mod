#include "menu.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "game.h"
#include "imgui.h"
#include "items.h"
#include "log.h"
#include "password.h"

namespace menu {
namespace {

enum class Screen { Hidden, Password, Items };

std::atomic<Screen> g_screen{Screen::Hidden};
bool g_unlocked = false;  // stays unlocked until the game is closed

// Password prompt state
char g_passwordBuf[128] = {};
bool g_focusPassword = false;
bool g_wrongPassword = false;
int g_failedAttempts = 0;
ULONGLONG g_lockedUntil = 0;

// Item window state
int g_category = 0;
char g_search[96] = {};
int g_quantity = 1;
int g_upgrade = 0;
int g_infusion = 0;
std::string g_status = "Clique em um item para colocá-lo no seu inventário.";
bool g_statusError = false;

struct Infusion {
    const char* name;
    uint32_t offset;
};
const Infusion kInfusions[] = {
    {"Normal", 0},        {"Pesada (Heavy)", 100},  {"Afiada (Sharp)", 200},    {"Refinada (Refined)", 300},
    {"Simples (Simple)", 400}, {"Cristal (Crystal)", 500}, {"Fogo (Fire)", 600},     {"Caos (Chaos)", 700},
    {"Raio (Lightning)", 800}, {"Profunda (Deep)", 900},   {"Sombria (Dark)", 1000}, {"Veneno (Poison)", 1100},
    {"Sangue (Blood)", 1200},  {"Bruta (Raw)", 1300},      {"Abençoada (Blessed)", 1400}, {"Vazia (Hollow)", 1500},
};

constexpr ImVec4 kGreen{0.45f, 0.85f, 0.45f, 1.0f};
constexpr ImVec4 kRed{0.95f, 0.40f, 0.40f, 1.0f};
constexpr ImVec4 kGold{0.90f, 0.75f, 0.40f, 1.0f};
constexpr ImVec4 kDim{0.65f, 0.65f, 0.65f, 1.0f};

void SetStatus(const std::string& text, bool error) {
    g_status = text;
    g_statusError = error;
}

bool Matches(const std::string& haystack, const char* needle) {
    if (!needle[0]) return true;
    std::string h = haystack, n = needle;
    auto lower = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    };
    lower(h);
    lower(n);
    return h.find(n) != std::string::npos;
}

// Returns the final item id for a weapon after applying infusion and upgrade, or 0 if invalid.
uint32_t ResolveWeapon(const items::Item& item, std::string& label, std::string& note) {
    uint32_t infusion = kInfusions[g_infusion].offset;
    int maxLevel = game::MaxUpgrade(item.id, infusion);
    if (maxLevel < 0 && infusion != 0) {
        note = " (essa arma não aceita infusão)";
        infusion = 0;
        maxLevel = game::MaxUpgrade(item.id, 0);
    }
    if (maxLevel < 0) return 0;
    const int level = std::min(g_upgrade, maxLevel);
    if (level < g_upgrade) note += " (nível máximo dessa arma: +" + std::to_string(maxLevel) + ")";
    label = item.name;
    if (infusion != 0) {
        for (const auto& inf : kInfusions)
            if (inf.offset == infusion) label = std::string(inf.name) + " " + label;
    }
    if (level > 0) label += " +" + std::to_string(level);
    return item.id + infusion + static_cast<uint32_t>(level);
}

bool GiveOne(const items::Item& item, items::Kind kind, std::string& label, std::string& note, std::string& error) {
    uint32_t id = item.id;
    uint32_t quantity = 1;
    label = item.name;
    if (kind == items::Kind::Weapon) {
        id = ResolveWeapon(item, label, note);
        if (!id) {
            error = item.name + ": não existe nessa versão do jogo.";
            return false;
        }
    } else if (kind == items::Kind::Stackable) {
        quantity = static_cast<uint32_t>(g_quantity);
        label = std::to_string(quantity) + "x " + item.name;
    }
    return game::GiveItem(id, quantity, error);
}

void Give(const items::Item& item, items::Kind kind) {
    std::string label, note, error;
    if (GiveOne(item, kind, label, note, error)) SetStatus("Adicionado: " + label + note, false);
    else SetStatus(error, true);
}

void GiveSet(const items::Group& set) {
    int given = 0;
    std::string error;
    for (const auto& piece : set.items) {
        std::string label, note;
        if (GiveOne(piece, items::Kind::ArmorSet, label, note, error)) ++given;
        else break;
    }
    if (given == static_cast<int>(set.items.size()))
        SetStatus("Adicionado: set " + set.name + " (" + std::to_string(given) + " peças)", false);
    else
        SetStatus(error, true);
}

void ItemRow(const items::Item& item, items::Kind kind) {
    ImGui::PushID(static_cast<int>(item.id));
    if (ImGui::Selectable(item.name.c_str())) Give(item, kind);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clique para adicionar  (ID 0x%08X)", item.id);
    ImGui::PopID();
}

void SetRow(const items::Group& set, bool forceOpen) {
    ImGui::PushID(set.name.c_str());
    if (forceOpen) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    const int pieces = static_cast<int>(set.items.size());
    const bool open = ImGui::TreeNodeEx("##set", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap,
                                        "%s  (%d %s)", set.name.c_str(), pieces, pieces == 1 ? "peça" : "peças");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 130.0f);
    if (ImGui::SmallButton("Adicionar set")) GiveSet(set);
    if (open) {
        for (const auto& piece : set.items) ItemRow(piece, items::Kind::ArmorSet);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void RenderCategory(const items::Category& category, const char* filter, bool showCategoryName) {
    for (size_t g = 0; g < category.groups.size(); ++g) {
        const auto& group = category.groups[g];
        if (category.kind == items::Kind::ArmorSet) {
            bool any = Matches(group.name, filter);
            for (const auto& piece : group.items) any = any || Matches(piece.name, filter);
            if (any) SetRow(group, filter[0] != 0);
            continue;
        }
        bool any = false;
        for (const auto& item : group.items) any = any || Matches(item.name, filter);
        if (!any) continue;
        std::string header = showCategoryName ? category.name + "  /  " + group.name : group.name;
        header += "##" + category.name + std::to_string(g);
        ImGui::SetNextItemOpen(true, filter[0] ? ImGuiCond_Always : ImGuiCond_Once);
        if (ImGui::CollapsingHeader(header.c_str())) {
            ImGui::Indent(8.0f);
            for (const auto& item : group.items)
                if (Matches(item.name, filter)) ItemRow(item, category.kind);
            ImGui::Unindent(8.0f);
        }
    }
}

void RenderPassword() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Always);
    bool open = true;
    ImGui::Begin("Acesso restrito", &open,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    const ULONGLONG now = GetTickCount64();
    if (now < g_lockedUntil) {
        ImGui::TextColored(kRed, "Muitas tentativas erradas.");
        ImGui::Text("Tente de novo em %llu s.", (g_lockedUntil - now) / 1000 + 1);
    } else {
        ImGui::TextWrapped("Digite a senha para abrir o menu de itens.");
        ImGui::Spacing();
        if (g_focusPassword) {
            ImGui::SetKeyboardFocusHere();
            g_focusPassword = false;
        }
        ImGui::SetNextItemWidth(-1.0f);
        const bool enter = ImGui::InputTextWithHint("##senha", "Senha", g_passwordBuf, sizeof(g_passwordBuf),
                                                    ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        if (g_wrongPassword) ImGui::TextColored(kRed, "Senha incorreta.");
        ImGui::Spacing();
        if (ImGui::Button("Entrar", ImVec2(120.0f, 0.0f)) || enter) {
            if (password::Check(g_passwordBuf)) {
                g_unlocked = true;
                g_wrongPassword = false;
                g_failedAttempts = 0;
                g_screen = Screen::Items;
                LOG("Menu unlocked");
            } else {
                g_wrongPassword = true;
                g_focusPassword = true;
                if (++g_failedAttempts >= 5) {
                    g_lockedUntil = now + 30000;
                    g_failedAttempts = 0;
                }
            }
            SecureZeroMemory(g_passwordBuf, sizeof(g_passwordBuf));
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(120.0f, 0.0f))) open = false;
    }
    ImGui::End();
    if (!open) Close();
}

void RenderItems() {
    const auto& categories = items::All();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(1000.0f, io.DisplaySize.x * 0.9f), std::min(680.0f, io.DisplaySize.y * 0.85f)),
                             ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("Itens  -  DS3 Coop Mod", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!open) Close();
        return;
    }

    // Options row
    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputTextWithHint("##busca", "Buscar em todas as categorias...", g_search, sizeof(g_search));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::InputInt("Qtd.", &g_quantity)) g_quantity = std::clamp(g_quantity, 1, 99);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Quantidade para consumíveis, flechas, almas e melhorias.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderInt("Nível", &g_upgrade, 0, 10, "+%d");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nível de melhoria das armas. Armas únicas vão até +5.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::BeginCombo("Infusão", kInfusions[g_infusion].name)) {
        for (int i = 0; i < static_cast<int>(sizeof(kInfusions) / sizeof(kInfusions[0])); ++i)
            if (ImGui::Selectable(kInfusions[i].name, i == g_infusion)) g_infusion = i;
        ImGui::EndCombo();
    }
    ImGui::Separator();

    const float footer = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;

    // Category list
    ImGui::BeginChild("categorias", ImVec2(210.0f, -footer), ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(categories.size()); ++i) {
        if (ImGui::Selectable(categories[i].name.c_str(), i == g_category && !g_search[0])) {
            g_category = i;
            g_search[0] = 0;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Items
    ImGui::BeginChild("itens", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders);
    if (g_search[0]) {
        for (const auto& category : categories) RenderCategory(category, g_search, true);
    } else if (g_category >= 0 && g_category < static_cast<int>(categories.size())) {
        const auto& category = categories[g_category];
        if (category.kind == items::Kind::ArmorSet)
            ImGui::TextColored(kDim, "\"Adicionar set\" dá todas as peças. Abra o set para pegar só uma peça.");
        RenderCategory(category, "", false);
    }
    ImGui::EndChild();

    // Footer
    ImGui::TextColored(g_statusError ? kRed : kGreen, "%s", g_status.c_str());
    if (!game::VersionOk())
        ImGui::TextColored(kRed, "Versão do jogo %s não suportada (precisa ser 1.15.2).", game::VersionText().c_str());
    else if (!game::InGame())
        ImGui::TextColored(kGold, "Fora do jogo: carregue um personagem para receber itens.");
    else
        ImGui::TextColored(kDim, "Os itens vão só para o seu inventário.  F12 ou Esc fecha.");

    ImGui::End();
    if (!open) Close();
}

}  // namespace

void Toggle() {
    if (g_screen != Screen::Hidden) {
        Close();
        return;
    }
    if (g_unlocked) {
        g_screen = Screen::Items;
    } else {
        g_wrongPassword = false;
        g_focusPassword = true;
        g_screen = Screen::Password;
    }
}

void Close() {
    SecureZeroMemory(g_passwordBuf, sizeof(g_passwordBuf));
    g_screen = Screen::Hidden;
}

bool IsOpen() { return g_screen != Screen::Hidden; }

void Setup(HWND window) {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // do not write imgui.ini into the game folder
    io.LogFilename = nullptr;

    RECT rc{};
    GetClientRect(window, &rc);
    const float height = static_cast<float>(rc.bottom - rc.top);
    const float fontSize = std::clamp(height / 55.0f, 16.0f, 30.0f);

    // Segoe UI covers accented Portuguese letters (the built-in ImGui font does not).
    wchar_t fontsDir[MAX_PATH] = {};
    GetWindowsDirectoryW(fontsDir, MAX_PATH);
    char fontPath[MAX_PATH * 2] = {};
    std::snprintf(fontPath, sizeof(fontPath), "%ls\\Fonts\\segoeui.ttf", fontsDir);
    if (GetFileAttributesA(fontPath) != INVALID_FILE_ATTRIBUTES &&
        io.Fonts->AddFontFromFileTTF(fontPath, fontSize, nullptr, io.Fonts->GetGlyphRangesDefault())) {
        LOG("Font: %s %.0fpx", fontPath, fontSize);
    } else {
        io.Fonts->AddFontDefault();
        LOG("Font: ImGui default");
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.ScaleAllSizes(fontSize / 16.0f);
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.06f, 0.05f, 0.96f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.30f, 0.20f, 0.10f, 1.0f);
    c[ImGuiCol_TitleBg] = ImVec4(0.18f, 0.12f, 0.07f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.35f, 0.24f, 0.12f, 0.55f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.55f, 0.38f, 0.18f, 0.80f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.65f, 0.45f, 0.20f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.35f, 0.24f, 0.12f, 0.70f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.55f, 0.38f, 0.18f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.65f, 0.45f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.14f, 0.10f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.75f, 0.55f, 0.25f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.90f, 0.70f, 0.30f, 1.0f);
}

void Render() {
    switch (g_screen.load()) {
        case Screen::Password: RenderPassword(); break;
        case Screen::Items: RenderItems(); break;
        default: break;
    }
}

}  // namespace menu
