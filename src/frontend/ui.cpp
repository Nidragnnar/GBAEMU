// Interface do NiGBA com o Dear ImGui: tema pastel, barra de menus, tela de boas-vindas e avisos.

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "core/cartridge.hpp"
#include "frontend/app.hpp"
#include "frontend/diagnostics.hpp"
#include "frontend/file_dialog.hpp"

namespace {
// Paleta "fofa": rosa e lavanda em tons pastel, texto ameixa.
const ImVec4 kPink{0.97f, 0.80f, 0.88f, 1.0f};
const ImVec4 kPinkStrong{0.93f, 0.58f, 0.76f, 1.0f};
const ImVec4 kPinkDeep{0.86f, 0.42f, 0.64f, 1.0f};
const ImVec4 kLavender{0.85f, 0.79f, 0.96f, 1.0f};
const ImVec4 kLavenderStrong{0.74f, 0.64f, 0.93f, 1.0f};
const ImVec4 kCream{1.00f, 0.96f, 0.98f, 1.0f};
const ImVec4 kPlum{0.38f, 0.24f, 0.40f, 1.0f};
const ImVec4 kPlumSoft{0.62f, 0.50f, 0.64f, 1.0f};

const char* const kKeyNames[10] = {"A", "B", "SELECT", "START", "Direita", "Esquerda", "Cima", "Baixo", "R", "L"};

// Nomes dos botões do controle (ordem do SDL_GameControllerButton), com os rótulos do Xbox.
const char* padButtonName(int button) {
    static const char* const kNames[] = {
        "A (baixo)",          "B (direita)",        "X (esquerda)",       "Y (cima)",
        "Back / Select",      "Guia",               "Start",              "Clique analógico esq.",
        "Clique analógico dir.", "LB / L1",         "RB / R1",            "Direcional cima",
        "Direcional baixo",   "Direcional esquerda", "Direcional direita", "Botão extra",
        "Pá 1",               "Pá 2",               "Pá 3",               "Pá 4",
        "Touchpad",
    };
    if (button < 0) return "(nenhum)";
    if (button < static_cast<int>(sizeof kNames / sizeof kNames[0])) return kNames[button];
    return "?";
}
} // namespace

void App::applyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsLight(&style);

    style.WindowRounding = 14.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 10.0f;
    style.PopupRounding = 12.0f;
    style.GrabRounding = 10.0f;
    style.ScrollbarRounding = 12.0f;
    style.TabRounding = 10.0f;
    style.WindowPadding = ImVec2(16, 14);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.WindowBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = kPlum;
    c[ImGuiCol_TextDisabled] = kPlumSoft;
    c[ImGuiCol_WindowBg] = kCream;
    c[ImGuiCol_ChildBg] = ImVec4(1, 1, 1, 0.5f);
    c[ImGuiCol_PopupBg] = kCream;
    c[ImGuiCol_Border] = ImVec4(0.90f, 0.72f, 0.84f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(1.0f, 0.90f, 0.95f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = kPink;
    c[ImGuiCol_FrameBgActive] = kPinkStrong;
    c[ImGuiCol_TitleBg] = kLavender;
    c[ImGuiCol_TitleBgActive] = kLavenderStrong;
    c[ImGuiCol_TitleBgCollapsed] = kLavender;
    c[ImGuiCol_MenuBarBg] = kPink;
    c[ImGuiCol_Button] = kPink;
    c[ImGuiCol_ButtonHovered] = kPinkStrong;
    c[ImGuiCol_ButtonActive] = kPinkDeep;
    c[ImGuiCol_Header] = kPink;
    c[ImGuiCol_HeaderHovered] = kPinkStrong;
    c[ImGuiCol_HeaderActive] = kPinkDeep;
    c[ImGuiCol_CheckMark] = kPinkDeep;
    c[ImGuiCol_SliderGrab] = kPinkStrong;
    c[ImGuiCol_SliderGrabActive] = kPinkDeep;
    c[ImGuiCol_Separator] = ImVec4(0.92f, 0.78f, 0.88f, 1.0f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(1, 1, 1, 0);
    c[ImGuiCol_ScrollbarGrab] = kPink;
    c[ImGuiCol_ScrollbarGrabHovered] = kPinkStrong;
    c[ImGuiCol_ScrollbarGrabActive] = kPinkDeep;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.55f, 0.35f, 0.55f, 0.35f);
}

void App::loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    // Fonte arredondada do Windows, se existir; senão, a padrão do ImGui. Os acentos do português
    // estão na faixa padrão (Latin-1).
    const char* candidates[] = {"C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeui.ttf",
                                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    for (const char* path : candidates) {
        if (!std::filesystem::exists(path)) continue;
        if (io.Fonts->AddFontFromFileTTF(path, 18.0f)) {
            titleFont_ = io.Fonts->AddFontFromFileTTF(path, 40.0f);
            return;
        }
    }
    io.Fonts->AddFontDefault();
}

// Coração desenhado com dois círculos e um triângulo (não depende de a fonte ter emoji).
void App::drawHeart(ImDrawList* dl, float cx, float cy, float size, uint32_t color) const {
    const float r = size * 0.5f;
    dl->AddCircleFilled(ImVec2(cx - r * 0.5f, cy), r * 0.55f, color, 20);
    dl->AddCircleFilled(ImVec2(cx + r * 0.5f, cy), r * 0.55f, color, 20);
    dl->AddTriangleFilled(ImVec2(cx - r * 1.02f, cy + r * 0.12f), ImVec2(cx + r * 1.02f, cy + r * 0.12f),
                          ImVec2(cx, cy + r * 1.15f), color);
}

void App::drawUi() {
    // Em tela cheia, a barra de menus só aparece com o mouse no topo.
    const bool showMenu = !fullscreen_ || ImGui::GetIO().MousePos.y < 40.0f;
    if (showMenu) drawMenuBar();
    if (!console_) drawWelcome();
    drawOverlays();
    drawStoppedPopup();
    if (showControls_) drawControlsWindow();
    if (showAbout_) drawAboutWindow();
    drawDebugWindows();
}

void App::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    menuBarHeight_ = ImGui::GetWindowSize().y;

    if (ImGui::BeginMenu("Arquivo")) {
        if (ImGui::MenuItem("Abrir ROM...", "Ctrl+O")) {
            if (auto path = openRomDialog(window_)) openRom(*path);
        }
        if (ImGui::BeginMenu("Recentes", !config_.recentRoms.empty())) {
            std::string chosen;
            for (const auto& path : config_.recentRoms) {
                const std::string label = gba::pathToUtf8(gba::utf8Path(path).filename());
                if (ImGui::MenuItem(label.c_str())) chosen = path;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Limpar lista")) { config_.recentRoms.clear(); config_.save(configPath_); }
            ImGui::EndMenu();
            if (!chosen.empty()) openRom(chosen); // fora do laço: openRom altera a lista
        }
        if (ImGui::MenuItem("Capturar tela", "F12", false, console_ != nullptr)) takeScreenshot();
        if (ImGui::MenuItem("Fechar ROM", nullptr, false, console_ != nullptr)) closeRom();
        ImGui::Separator();
        if (ImGui::MenuItem("Sair", "Alt+F4")) running_ = false;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Emulação", console_ != nullptr)) {
        if (!patchName_.empty()) {
            ImGui::TextColored(kPinkDeep, "Patch: %s", patchName_.c_str());
            if (ImGui::MenuItem("Reabrir sem o patch")) openRom(romPath_, "", false);
            ImGui::Separator();
        }
        if (ImGui::MenuItem(paused_ ? "Continuar" : "Pausar", "Esc")) togglePause();
        ImGui::MenuItem("Voltar no tempo", "segure R", false, false); // só informa o atalho
        ImGui::TextDisabled("  Histórico: %.0f s, %.1f MB", static_cast<double>(rewind_.size() * kRewindInterval) / 60.0,
                            static_cast<double>(rewind_.memoryUsed()) / (1024.0 * 1024.0));
        if (ImGui::MenuItem("Reiniciar", "Ctrl+R")) resetConsole();
        ImGui::Separator();
        const std::string slotLabel = " (slot " + std::to_string(config_.stateSlot) + ")";
        if (ImGui::MenuItem(("Salvar estado" + slotLabel).c_str(), "F5")) saveStateSlot(config_.stateSlot);
        if (ImGui::MenuItem(("Carregar estado" + slotLabel).c_str(), "F8", false, stateExists(config_.stateSlot))) {
            loadStateSlot(config_.stateSlot);
        }
        if (ImGui::BeginMenu("Escolher slot")) {
            for (int slot = 1; slot <= 4; ++slot) {
                const std::string label = "Slot " + std::to_string(slot) + (stateExists(slot) ? "" : "  (vazio)");
                const std::string shortcut = "Ctrl+" + std::to_string(slot);
                if (ImGui::MenuItem(label.c_str(), shortcut.c_str(), config_.stateSlot == slot)) config_.stateSlot = slot;
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::TextDisabled("Segure Tab para acelerar");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Vídeo")) {
        if (ImGui::BeginMenu("Tamanho da janela")) {
            for (int s = 1; s <= 6; ++s) {
                const std::string label = std::to_string(s) + "x  (" + std::to_string(240 * s) + " x " + std::to_string(160 * s) + ")";
                if (ImGui::MenuItem(label.c_str(), nullptr, config_.scale == s && !fullscreen_)) setScale(s);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Tela cheia", "F11", fullscreen_)) toggleFullscreen();
        ImGui::Separator();
        bool changed = false;
        changed |= ImGui::MenuItem("Cores do GBA original", nullptr, &config_.gbaColors);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A tela do GBA era mais escura e menos saturada que um monitor;\nos jogos foram pintados para ela.");
        changed |= ImGui::MenuItem("Suavizar imagem", nullptr, &config_.smoothing);
        changed |= ImGui::MenuItem("Efeito de tela LCD", nullptr, &config_.lcdGrid);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Grade fina entre os pixels, como na tela real (a partir de 3x).");
        if (changed) config_.save(configPath_);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Áudio")) {
        ImGui::MenuItem("Mudo", nullptr, &config_.mute);
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt("Volume", &config_.volume, 0, 100, "%d%%");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Depuração", console_ != nullptr)) {
        ImGui::MenuItem("CPU", nullptr, &showCpu_);
        ImGui::MenuItem("Memória", nullptr, &showMemory_);
        ImGui::MenuItem("VRAM (tiles)", nullptr, &showVram_);
        ImGui::MenuItem("Paletas", nullptr, &showPalette_);
        ImGui::MenuItem("Sprites", nullptr, &showSprites_);
        ImGui::MenuItem("Registradores de I/O", nullptr, &showIo_);
        ImGui::Separator();
        if (ImGui::MenuItem("Avançar 1 quadro", "F10")) stepFrame();
        ImGui::Separator();
        if (ImGui::MenuItem("Diagnóstico no terminal", "F1")) diag::dumpState(*console_, frame_);
        if (ImGui::MenuItem("Rastro da CPU no terminal", "F2")) diag::dumpTrace(*console_);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Ajuda")) {
        if (ImGui::MenuItem("Controles")) showControls_ = true;
        if (ImGui::MenuItem("Sobre o NiGBA")) showAbout_ = true;
        ImGui::EndMenu();
    }

    // Lado direito: estado atual, discreto.
    const char* status = !console_ ? "" : stopped_ ? "parado" : paused_ ? "pausado" : fastForward_ ? "acelerado >>" : "";
    if (*status) {
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(status).x - 20);
        ImGui::TextDisabled("%s", status);
    }
    ImGui::EndMainMenuBar();
}

void App::drawWelcome() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 pos(vp->WorkPos.x, vp->WorkPos.y), size(vp->WorkSize.x, vp->WorkSize.y);

    // Fundo em degradê pastel com corações flutuando.
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilledMultiColor(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(253, 228, 240, 255),
                                IM_COL32(236, 226, 252, 255), IM_COL32(226, 236, 252, 255), IM_COL32(252, 236, 244, 255));
    const float t = static_cast<float>(ImGui::GetTime());
    const ImU32 heartColors[3] = {IM_COL32(247, 168, 205, 150), IM_COL32(200, 176, 245, 140), IM_COL32(250, 196, 214, 160)};
    for (int i = 0; i < 9; ++i) {
        const float fx = std::fmod(0.11f + i * 0.137f, 1.0f);
        const float rise = std::fmod(t * (0.03f + 0.01f * (i % 3)) + i * 0.21f, 1.0f);
        const float x = pos.x + size.x * fx + std::sin(t * 1.3f + static_cast<float>(i)) * 12.0f;
        const float y = pos.y + size.y * (1.05f - rise * 1.2f);
        drawHeart(bg, x, y, 16.0f + 6.0f * static_cast<float>(i % 3), heartColors[i % 3]);
    }

    // Cartão central.
    const float cardW = std::min(440.0f, size.x - 32.0f);
    ImGui::SetNextWindowPos(ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(cardW, 0));
    ImGui::Begin("##boas-vindas", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBringToFrontOnFocus);

    const char* title = "NiGBA";
    if (titleFont_) ImGui::PushFont(titleFont_);
    ImGui::SetCursorPosX((cardW - ImGui::CalcTextSize(title).x) * 0.5f);
    ImGui::TextColored(kPinkDeep, "%s", title);
    if (titleFont_) ImGui::PopFont();

    const char* subtitle = "Seu Emulador de Game Boy Advance";
    ImGui::SetCursorPosX((cardW - ImGui::CalcTextSize(subtitle).x) * 0.5f);
    ImGui::TextDisabled("%s", subtitle);
    ImGui::Spacing();

    const float buttonW = cardW - 2 * ImGui::GetStyle().WindowPadding.x;
    ImGui::PushStyleColor(ImGuiCol_Button, kPinkStrong);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button("Abrir uma ROM...", ImVec2(buttonW, 40))) {
        if (auto path = openRomDialog(window_)) openRom(*path);
    }
    ImGui::PopStyleColor(2);
    const char* hint = "ou arraste o arquivo .gba para esta janela";
    ImGui::SetCursorPosX((cardW - ImGui::CalcTextSize(hint).x) * 0.5f);
    ImGui::TextDisabled("%s", hint);

    if (!config_.recentRoms.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Jogados recentemente");
        std::string chosen;
        for (const auto& path : config_.recentRoms) {
            const std::string name = gba::pathToUtf8(gba::utf8Path(path).stem());
            if (ImGui::Button(name.c_str(), ImVec2(buttonW, 0))) chosen = path;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
        }
        if (!chosen.empty()) openRom(chosen);
    }
    ImGui::End();
}

void App::drawOverlays() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* fg = ImGui::GetForegroundDrawList();

    // "Pausado": uma pílula no centro da tela.
    if (console_ && paused_ && !stopped_) {
        const char* text = "Pausado  -  Esc para continuar";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 c(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f);
        fg->AddRectFilled(ImVec2(c.x - ts.x / 2 - 22, c.y - ts.y / 2 - 12), ImVec2(c.x + ts.x / 2 + 22, c.y + ts.y / 2 + 12),
                          IM_COL32(255, 244, 250, 235), 999.0f);
        fg->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), ImGui::GetColorU32(kPlum), text);
        drawHeart(fg, c.x - ts.x / 2 - 8, c.y - 3, 12, IM_COL32(240, 140, 190, 255));
    }

    // Voltando no tempo: uma pílula no topo com os segundos de histórico que ainda restam.
    if (rewinding_) {
        char text[64];
        std::snprintf(text, sizeof text, "<<  Voltando no tempo  (%.0f s)", static_cast<double>(rewind_.size() * kRewindInterval) / 60.0);
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 c(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 30);
        fg->AddRectFilled(ImVec2(c.x - ts.x / 2 - 18, c.y - ts.y / 2 - 9), ImVec2(c.x + ts.x / 2 + 18, c.y + ts.y / 2 + 9),
                          IM_COL32(236, 228, 252, 235), 999.0f);
        fg->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), ImGui::GetColorU32(kPlum), text);
    }

    // Aviso temporário (save state, erros): canto inferior esquerdo, sumindo devagar.
    const double now = ImGui::GetTime();
    if (!toast_.empty() && now < toastUntil_) {
        const float alpha = static_cast<float>(std::min(1.0, (toastUntil_ - now) / 0.5));
        const ImVec2 ts = ImGui::CalcTextSize(toast_.c_str());
        const ImVec2 p(vp->WorkPos.x + 14, vp->WorkPos.y + vp->WorkSize.y - ts.y - 28);
        fg->AddRectFilled(p, ImVec2(p.x + ts.x + 44, p.y + ts.y + 16), IM_COL32(255, 238, 247, static_cast<int>(235 * alpha)), 999.0f);
        drawHeart(fg, p.x + 16, p.y + ts.y / 2 + 5, 11, IM_COL32(240, 140, 190, static_cast<int>(255 * alpha)));
        fg->AddText(ImVec2(p.x + 30, p.y + 8), IM_COL32(97, 61, 102, static_cast<int>(255 * alpha)), toast_.c_str());
    }
}

void App::drawStoppedPopup() {
    if (stopped_ && !ImGui::IsPopupOpen("O jogo parou")) ImGui::OpenPopup("O jogo parou");
    ImGui::SetNextWindowSize(ImVec2(420, 0));
    if (!ImGui::BeginPopupModal("O jogo parou", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped("A emulação encontrou algo que ainda não sabe fazer:");
    ImGui::Spacing();
    ImGui::TextColored(kPinkDeep, "%s", stopReason_.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("Os detalhes estão no terminal (F1 e F2 mostram mais).");
    ImGui::Spacing();
    if (ImGui::Button("Reiniciar o jogo", ImVec2(180, 0))) { resetConsole(); ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("Fechar a ROM", ImVec2(180, 0))) { closeRom(); ImGui::CloseCurrentPopup(); }
    if (!stopped_) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Uma linha da tabela de controles: nome do botão do GBA e a tecla/botão atual (clique para trocar).
void App::drawBindingRow(int index, bool pad) {
    const size_t i = static_cast<size_t>(index);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", kKeyNames[i]);
    ImGui::SameLine(110);
    const bool waiting = capture_ == (pad ? Capture::Pad : Capture::Key) && captureIndex_ == index;
    std::string label = waiting ? "aperte..." :
                        pad     ? padButtonName(config_.padButtons[i]) :
                                  SDL_GetScancodeName(static_cast<SDL_Scancode>(config_.keys[i]));
    if (label.empty()) label = "(nenhuma)";
    label += std::string(pad ? "##pad" : "##tecla") + std::to_string(index);
    if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, kLavenderStrong);
    if (ImGui::Button(label.c_str(), ImVec2(210, 0))) {
        capture_ = pad ? Capture::Pad : Capture::Key;
        captureIndex_ = index;
    }
    if (waiting) ImGui::PopStyleColor();
}

void App::drawControlsWindow() {
    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Appearing);
    if (ImGui::Begin("Controles", &showControls_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::BeginTabBar("##abas")) {
            if (ImGui::BeginTabItem("Teclado")) {
                for (int i = 0; i < 10; ++i) drawBindingRow(i, false);
                ImGui::Spacing();
                if (ImGui::Button("Restaurar o padrão##teclado")) { config_.keys = Config::defaultKeys(); config_.save(configPath_); }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controle")) {
                if (pads_.empty()) ImGui::TextDisabled("Nenhum controle conectado (é só conectar).");
                else ImGui::TextColored(kPinkDeep, "%s", padName().c_str());
                ImGui::Spacing();
                for (int i = 0; i < 10; ++i) drawBindingRow(i, true);
                ImGui::Spacing();
                ImGui::TextDisabled("Analógico esquerdo: direcional  |  Gatilho direito: acelerar");
                ImGui::TextDisabled("Gatilho esquerdo: voltar no tempo");
                if (ImGui::Button("Restaurar o padrão##controle")) { config_.padButtons = Config::defaultPadButtons(); config_.save(configPath_); }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Atalhos")) {
                ImGui::BulletText("Tab (segurar)   acelerar");
                ImGui::BulletText("Esc             pausar / continuar");
                ImGui::BulletText("F5 / F8         salvar / carregar estado");
                ImGui::BulletText("Ctrl+1..4       escolher o slot");
                ImGui::BulletText("Ctrl+O / Ctrl+R abrir ROM / reiniciar");
                ImGui::BulletText("F11             tela cheia");
                ImGui::BulletText("F10             avançar 1 quadro (pausa)");
                ImGui::BulletText("F12             capturar a tela (PNG)");
                ImGui::BulletText("R (segurar)     voltar no tempo (até ~20 s)");
                ImGui::BulletText("F1 / F2         diagnóstico no terminal");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (capture_ != Capture::None) {
            ImGui::Spacing();
            ImGui::TextColored(kPinkDeep, capture_ == Capture::Key ? "Aperte a tecla desejada (Esc cancela)"
                                                                   : "Aperte o botão no controle (Esc cancela)");
        }
    }
    ImGui::End();
    if (!showControls_) capture_ = Capture::None; // fechou a janela no meio de um remapeamento
}

void App::drawAboutWindow() {
    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Appearing);
    if (ImGui::Begin("Sobre o NiGBA", &showAbout_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(kPinkDeep, "NiGBA");
        ImGui::TextWrapped("Um emulador de Game Boy Advance escrito do zero em C++20, a partir da documentação "
                           "do hardware (GBATEK).");
        ImGui::Spacing();
        ImGui::BulletText("CPU ARM7TDMI (ARM e Thumb)");
        ImGui::BulletText("Vídeo: todos os modos, sprites, janelas e efeitos");
        ImGui::BulletText("Áudio: 4 canais do Game Boy + 2 de amostras");
        ImGui::BulletText("Saves: SRAM, Flash e EEPROM; save states");
        ImGui::Spacing();
        ImGui::TextDisabled("Feito com SDL2 e Dear ImGui.");
    }
    ImGui::End();
}
