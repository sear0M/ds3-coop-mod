#include "menu.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "game.h"
#include "gamepad.h"
#include "imgui.h"
#include "imgui_internal.h"  // SetFocusID, ClearActiveID
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

// Controller mode: the menu was opened with L3, or a controller button was pressed in it. The
// password is typed on the on-screen keyboard and the D-pad moves between buttons.
bool g_padMode = false;
bool g_focusKeyboard = false;  // put the controller cursor on the keyboard's Q this frame
bool g_focusItems = false;     // put it on the first category this frame
bool g_shift = false;          // next letter in upper case
bool g_popupWasOpen = false;   // a popup or list was open at the end of the last frame

// Item window state
int g_category = 0;
char g_search[96] = {};
int g_quantity = 1;
int g_upgrade = 0;
int g_infusion = 0;
std::string g_status = "Clique em um item para colocá-lo no seu inventário.";
bool g_statusError = false;

// Error from "Sair para o menu" / "Voltar para a fogueira" shown on the password screen (the
// item window uses g_status).
std::string g_actionError;
constexpr const char* kQuitPopup = "Sair para o menu?";
constexpr const char* kQuitLabel = "Sair para o menu";
constexpr const char* kBonfirePopup = "Voltar para a fogueira?";
constexpr const char* kBonfireLabel = "Voltar para a fogueira";

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

// Button that asks to go back to the title screen. Seamless Co-op sometimes leaves a boss stuck
// (no health bar, fight never starts) after everyone dies to the same attack; reloading the
// area by quitting to the menu resets it.
void QuitButton(float width) {
    if (ImGui::Button(kQuitLabel, ImVec2(width, 0.0f))) ImGui::OpenPopup(kQuitPopup);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Volta para a tela de título, igual a \"Sair do jogo\" no menu do DS3.\n"
                          "Use quando um boss travar sem barra de vida.");
}

// Centered yes/no popup for the buttons above. Runs action on yes. Returns 1 when the action ran,
// -1 when it failed (message in error), 0 otherwise.
int ConfirmPopup(const char* id, const std::string& text, const char* note, const char* okLabel,
                 bool (*action)(std::string&), std::string& error) {
    int result = 0;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(id, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::Spacing();
        ImGui::TextColored(kDim, "%s", note);
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const float width = ImGui::GetFontSize() * 6.0f;
        if (ImGui::Button(okLabel, ImVec2(width, 0.0f))) {
            result = action(error) ? 1 : -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        // The controller cursor starts on "Cancelar", so pressing A twice does not confirm by
        // accident. B also cancels.
        if (ImGui::Button("Cancelar", ImVec2(width, 0.0f)) || gamepad::Pressed(XINPUT_GAMEPAD_B))
            ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    return result;
}

// Confirmation for QuitButton.
int QuitPopup(std::string& error) {
    return ConfirmPopup(kQuitPopup,
                        "Voltar para a tela de título? É o mesmo que \"Sair do jogo\" no menu do DS3, "
                        "o progresso fica salvo.",
                        "Boss travado sem barra de vida? Quem está hospedando sai e carrega o personagem de novo, "
                        "e a área volta ao normal.",
                        "Sair", game::QuitToMenu, error);
}

// Button that sends you to the last bonfire you rested at, like a Homeward Bone. The game refuses
// the Bone while other players are in your world, which in Seamless Co-op is always. This goes
// through the bonfire "Travel" code instead, the same thing the game runs when you pick a bonfire
// in that menu. Whether Seamless pulls the guests along when the host does it is not documented,
// so the text tells everyone to press their own button if the group does not follow.
void BonfireButton(float width) {
    if (ImGui::Button(kBonfireLabel, ImVec2(width, 0.0f))) ImGui::OpenPopup(kBonfirePopup);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Volta para a última fogueira em que você descansou, igual ao Homeward Bone.\n"
                          "É a mesma viagem do menu \"Viajar\" da fogueira.");
}

// Confirmation for BonfireButton, naming the bonfire.
int BonfirePopup(std::string& error) {
    if (!ImGui::IsPopupOpen(kBonfirePopup)) return 0;
    const int id = game::LastBonfire();
    const char* name = id > 0 ? game::BonfireName(id) : nullptr;
    std::string text = "Voltar para a última fogueira em que você descansou";
    if (name) text += std::string(" (") + name + ")";
    else if (id > 0) text += " (fogueira " + std::to_string(id) + ")";
    text += "? É o mesmo que usar um Homeward Bone.";
    return ConfirmPopup(kBonfirePopup, text,
                        "Em coop: o host aperta primeiro. Se o grupo não for junto, cada um aperta o seu. "
                        "Não use no meio de luta de boss.",
                        "Voltar", game::WarpToLastBonfire, error);
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

// On-screen keyboard for typing the password with a controller (the mouse works too). Every row
// has the same 10 columns, so the D-pad moves straight up and down.
const char* const kKeyRows[] = {"1234567890", "qwertyuiop", "asdfghjkl_", "zxcvbnm,.-", "!@#$%&*?+="};
constexpr int kKeyRowCount = static_cast<int>(sizeof(kKeyRows) / sizeof(kKeyRows[0]));
constexpr int kKeyCols = 10;

float KeySize() { return ImGui::GetFrameHeight() * 1.45f; }

float KeyboardWidth() { return kKeyCols * KeySize() + (kKeyCols - 1) * ImGui::GetStyle().ItemSpacing.x; }

void TypeChar(char c) {
    const size_t n = std::strlen(g_passwordBuf);
    if (n + 1 < sizeof(g_passwordBuf)) {
        g_passwordBuf[n] = c;
        g_passwordBuf[n + 1] = 0;
    }
    g_wrongPassword = false;
}

void Backspace() {
    const size_t n = std::strlen(g_passwordBuf);
    if (n) g_passwordBuf[n - 1] = 0;
}

// Draws the keyboard. Returns true when the player asks to enter the password.
bool VirtualKeyboard() {
    const float key = KeySize();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    for (int r = 0; r < kKeyRowCount; ++r) {
        for (int c = 0; c < kKeyCols; ++c) {
            if (c > 0) ImGui::SameLine(0.0f, gap);
            char ch = kKeyRows[r][c];
            const bool letter = ch >= 'a' && ch <= 'z';
            if (letter && g_shift) ch = static_cast<char>(ch - 'a' + 'A');
            // Letters keep their ID ("###k") when Shift changes the label, or the controller cursor
            // would lose its place. Other keys never change ("#" could not use "###" anyway).
            char label[8] = {ch, 0};
            if (letter) std::snprintf(label, sizeof(label), "%c###k", ch);
            ImGui::PushID(r * kKeyCols + c);
            if (ImGui::Button(label, ImVec2(key, key))) {
                TypeChar(ch);
                if (letter) g_shift = false;  // like a phone: Shift is for one letter
            }
            if (r == 1 && c == 0) {  // the cursor starts on Q
                ImGui::SetItemDefaultFocus();  // when the window appears
                if (g_focusKeyboard) {         // when switching to the controller later
                    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                    ImGui::SetNavCursorVisible(true);
                    g_focusKeyboard = false;
                }
            }
            ImGui::PopID();
        }
    }
    auto wide = [&](int columns) { return ImVec2(columns * key + (columns - 1) * gap, key); };
    bool submit = false;
    if (g_shift) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool shiftClicked = ImGui::Button("Maiúsc.", wide(3));
    if (g_shift) ImGui::PopStyleColor();
    if (shiftClicked) g_shift = !g_shift;
    ImGui::SameLine(0.0f, gap);
    if (ImGui::Button("Espaço", wide(3))) TypeChar(' ');
    ImGui::SameLine(0.0f, gap);
    if (ImGui::Button("Apagar", wide(2))) Backspace();
    ImGui::SameLine(0.0f, gap);
    if (ImGui::Button("Entrar", wide(2))) submit = true;

    // Controller shortcuts, unless a confirmation popup has the controller.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        if (gamepad::Pressed(XINPUT_GAMEPAD_X)) Backspace();
        if (gamepad::Pressed(XINPUT_GAMEPAD_Y)) g_shift = !g_shift;
        if (gamepad::Pressed(XINPUT_GAMEPAD_START)) submit = true;
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kDim, "A digita · X apaga · Y maiúscula · Start entra · B ou L3 fecha");
    ImGui::PopTextWrapPos();
    return submit;
}

void SubmitPassword(ULONGLONG now) {
    if (password::Check(g_passwordBuf)) {
        g_unlocked = true;
        g_wrongPassword = false;
        g_failedAttempts = 0;
        g_screen = Screen::Items;
        g_focusItems = g_padMode;
        LOG("Menu unlocked");
    } else {
        g_wrongPassword = true;
        g_focusPassword = !g_padMode;  // with the controller the cursor stays on the keyboard
        if (++g_failedAttempts >= 5) {
            g_lockedUntil = now + 30000;
            g_failedAttempts = 0;
        }
    }
    g_shift = false;
    SecureZeroMemory(g_passwordBuf, sizeof(g_passwordBuf));
}

void RenderPassword() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    const float width = g_padMode ? std::max(420.0f, KeyboardWidth() + ImGui::GetStyle().WindowPadding.x * 2.0f) : 420.0f;
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    bool open = true;
    ImGui::Begin("Acesso restrito", &open,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    const ULONGLONG now = GetTickCount64();
    if (now < g_lockedUntil) {
        ImGui::TextColored(kRed, "Muitas tentativas erradas.");
        ImGui::Text("Tente de novo em %llu s.", (g_lockedUntil - now) / 1000 + 1);
    } else if (g_padMode) {
        ImGui::TextWrapped("Digite a senha com o controle, no teclado abaixo.");
        ImGui::Spacing();
        // The D-pad skips the text box (it cannot type); a mouse click still selects it.
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        ImGui::SetNextItemWidth(-1.0f);
        const bool enter = ImGui::InputTextWithHint("##senha", "Senha", g_passwordBuf, sizeof(g_passwordBuf),
                                                    ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemFlag();
        if (g_wrongPassword) ImGui::TextColored(kRed, "Senha incorreta.");
        ImGui::Spacing();
        if (VirtualKeyboard() || enter) SubmitPassword(now);
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
        if (ImGui::Button("Entrar", ImVec2(120.0f, 0.0f)) || enter) SubmitPassword(now);
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(120.0f, 0.0f))) open = false;
    }
    // These need no password: they are the same as the game's own "Quit Game" and Homeward Bone.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    BonfireButton(-1.0f);
    QuitButton(-1.0f);
    if (!g_actionError.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kRed, "%s", g_actionError.c_str());
        ImGui::PopTextWrapPos();
    }
    std::string error;
    const int bonfire = BonfirePopup(error);
    const int quit = QuitPopup(error);
    if (bonfire != 0 || quit != 0) g_actionError = error;
    ImGui::End();
    if (!open || bonfire > 0 || quit > 0) Close();
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

    // Options row. With the controller the D-pad skips the search box (it cannot type), and the
    // quantity is a slider: A to grab it, left/right to change, A again to let go.
    ImGui::SetNextItemWidth(280.0f);
    if (g_padMode) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::InputTextWithHint("##busca", "Buscar em todas as categorias...", g_search, sizeof(g_search));
    if (g_padMode) ImGui::PopItemFlag();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (g_padMode) ImGui::SliderInt("Qtd.", &g_quantity, 1, 99);
    else if (ImGui::InputInt("Qtd.", &g_quantity)) g_quantity = std::clamp(g_quantity, 1, 99);
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

    const float footer =
        ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;

    // LB/RB switch category.
    const int categoryCount = static_cast<int>(categories.size());
    if (categoryCount > 0 && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        int step = 0;
        if (gamepad::Pressed(XINPUT_GAMEPAD_LEFT_SHOULDER)) step = -1;
        if (gamepad::Pressed(XINPUT_GAMEPAD_RIGHT_SHOULDER)) step = 1;
        if (step) {
            g_category = (g_category + step + categoryCount) % categoryCount;
            g_search[0] = 0;
        }
    }

    // Category list. NavFlattened: the D-pad goes straight from this list to the items and back.
    ImGui::BeginChild("categorias", ImVec2(210.0f, -footer), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    for (int i = 0; i < categoryCount; ++i) {
        if (ImGui::Selectable(categories[i].name.c_str(), i == g_category && !g_search[0])) {
            g_category = i;
            g_search[0] = 0;
        }
        if (i == 0 && g_padMode) {  // controller cursor starts on the first category
            ImGui::SetItemDefaultFocus();
            if (g_focusItems) {
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                g_focusItems = false;
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Items
    ImGui::BeginChild("itens", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    if (g_search[0]) {
        for (const auto& category : categories) RenderCategory(category, g_search, true);
    } else if (g_category >= 0 && g_category < static_cast<int>(categories.size())) {
        const auto& category = categories[g_category];
        if (category.kind == items::Kind::ArmorSet)
            ImGui::TextColored(kDim, "\"Adicionar set\" dá todas as peças. Abra o set para pegar só uma peça.");
        RenderCategory(category, "", false);
    }
    ImGui::EndChild();

    // Footer: status, then the game state hint with "Voltar para a fogueira" and "Sair para o menu"
    // on the right.
    ImGui::TextColored(g_statusError ? kRed : kGreen, "%s", g_status.c_str());
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float padding = ImGui::GetStyle().FramePadding.x * 2.0f;
    const float quitWidth = ImGui::CalcTextSize(kQuitLabel).x + padding;
    const float bonfireWidth = ImGui::CalcTextSize(kBonfireLabel).x + padding;
    ImGui::AlignTextToFramePadding();
    if (!game::VersionOk())
        ImGui::TextColored(kRed, "Versão do jogo %s não suportada (precisa ser 1.15.2).", game::VersionText().c_str());
    else if (!game::InGame())
        ImGui::TextColored(kGold, "Fora do jogo: carregue um personagem para receber itens.");
    else if (g_padMode)
        ImGui::TextColored(kDim, "A pega o item · LB/RB troca a categoria · B ou L3 fecha.");
    else
        ImGui::TextColored(kDim, "Os itens vão só para o seu inventário.  Insert ou Esc fecha.");
    ImGui::SameLine(right - quitWidth - ImGui::GetStyle().ItemSpacing.x - bonfireWidth);
    BonfireButton(0.0f);
    ImGui::SameLine();
    QuitButton(0.0f);
    std::string error;
    const int bonfire = BonfirePopup(error);
    const int quit = QuitPopup(error);
    if (bonfire > 0) SetStatus("Voltando para a fogueira...", false);
    else if (quit > 0) SetStatus("Saindo para o menu...", false);
    else if (bonfire < 0 || quit < 0) SetStatus(error, true);

    ImGui::End();
    if (!open || bonfire > 0 || quit > 0) Close();
}

}  // namespace

void Toggle(bool fromGamepad) {
    if (g_screen != Screen::Hidden) {
        Close();
        return;
    }
    g_padMode = fromGamepad;
    g_shift = false;
    g_popupWasOpen = false;
    if (g_unlocked) {
        g_focusItems = fromGamepad;
        g_screen = Screen::Items;
    } else {
        g_wrongPassword = false;
        g_focusPassword = !fromGamepad;
        g_focusKeyboard = fromGamepad;
        g_screen = Screen::Password;
    }
    if (fromGamepad) LOG("Menu opened with the controller (L3)");
}

void Close() {
    SecureZeroMemory(g_passwordBuf, sizeof(g_passwordBuf));
    g_screen = Screen::Hidden;
    g_shift = false;
    g_popupWasOpen = false;
    gamepad::OnMenuClosed();  // the button that closed the menu does not reach the game
}

bool IsOpen() { return g_screen != Screen::Hidden; }

bool PadMode() { return g_padMode; }

void Setup(HWND window) {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // do not write imgui.ini into the game folder
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // D-pad/stick + A/B, fed by src/gamepad.cpp

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
    // A controller button switches the menu to controller mode (on-screen keyboard, D-pad cursor).
    if (!g_padMode && gamepad::AnyPressed()) {
        LOG("Menu: controller mode (buttons 0x%04X)", gamepad::Buttons());
        g_padMode = true;
        g_focusKeyboard = g_screen == Screen::Password;
        g_focusItems = g_screen == Screen::Items && ImGui::GetFocusID() == 0;
        ImGui::ClearActiveID();  // stop typing in the password box, the D-pad cannot leave it
    }
    // B closes the menu, unless a list or popup is open (B closes that first).
    if (gamepad::Pressed(XINPUT_GAMEPAD_B) && !g_popupWasOpen) {
        Close();
        return;
    }
    switch (g_screen.load()) {
        case Screen::Password: RenderPassword(); break;
        case Screen::Items: RenderItems(); break;
        default: break;
    }
    g_popupWasOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

}  // namespace menu
