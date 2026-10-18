#pragma once
// A aplicação: janela, áudio, interface (Dear ImGui), ritmo da emulação e as ações do usuário.
// O desenho dos menus e telas fica em ui.cpp.

#include <SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/gba.hpp"
#include "frontend/config.hpp"

struct ImFont;
struct ImDrawList;

struct Options {
    std::string romPath;  // opcional: abre direto este jogo
    std::string biosPath;
    bool testPattern = false;
    bool probing = false;
    bool mute = false;
    int scale = 0;        // 0 = usa a da configuração
    std::vector<std::pair<uint32_t, uint32_t>> dumps;
};

class App {
public:
    explicit App(Options options);
    ~App();
    int run();

private:
    // ---- ciclo de vida
    bool init();
    void shutdown();
    void handleEvent(const SDL_Event& ev);
    void runEmulation();
    bool runOneFrame(bool keepAudio);
    void render();

    // ---- ações
    void openRom(const std::string& path);
    void closeRom();
    void resetConsole();
    void togglePause() { if (console_) paused_ = !paused_; }
    void saveStateSlot(int slot);
    void loadStateSlot(int slot);
    void setScale(int scale);
    void toggleFullscreen();
    void saveBackupNow();
    void toast(std::string message);
    std::string statePath(int slot) const;
    bool stateExists(int slot) const;
    uint16_t readKeys() const;
    bool padFastForward() const;
    void openPad(int deviceIndex);
    void closePad(int32_t instanceId);
    std::string padName() const;

    // ---- interface (ui.cpp)
    void applyTheme();
    void loadFonts();
    void drawUi();
    void drawMenuBar();
    void drawWelcome();
    void drawOverlays();
    void drawStoppedPopup();
    void drawControlsWindow();
    void drawAboutWindow();
    void drawBindingRow(int index, bool pad);
    void drawHeart(ImDrawList* dl, float cx, float cy, float size, uint32_t color) const;

    Options options_;
    Config config_;
    std::string configPath_;

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    SDL_AudioDeviceID audio_ = 0;
    ImFont* titleFont_ = nullptr;
    float menuBarHeight_ = 0;

    std::unique_ptr<gba::Gba> console_;
    std::string romPath_, savePath_, gameTitle_;

    bool running_ = true;
    bool paused_ = false;
    bool stopped_ = false;     // a emulação parou por um erro (por exemplo, SWI não implementada)
    std::string stopReason_;
    bool fastForward_ = false;
    bool fullscreen_ = false;
    bool showControls_ = false;
    bool showAbout_ = false;

    uint64_t frame_ = 0;
    size_t probesPrinted_ = 0;
    uint64_t lastTick_ = 0;
    double timeDebt_ = 0;

    // Controles conectados e o remapeamento em andamento (clique num botão e aperte a tecla).
    std::vector<SDL_GameController*> pads_;
    enum class Capture { None, Key, Pad } capture_ = Capture::None;
    int captureIndex_ = -1;

    std::string toast_;
    double toastUntil_ = 0;
};
