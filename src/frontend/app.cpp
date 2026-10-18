#include "frontend/app.hpp"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "core/cartridge.hpp"
#include "frontend/diagnostics.hpp"
#include "frontend/file_dialog.hpp"

namespace {
constexpr uint32_t kAudioTargetBytes = gba::Apu::kSampleRate * 2 * 2 / 20; // 50 ms de som na fila
constexpr double kFrameSeconds = static_cast<double>(gba::kCyclesPerFrame) / 16777216.0;

std::string prefFile(const char* app, const char* file) {
    if (char* dir = SDL_GetPrefPath(app, app)) {
        std::string path = std::string(dir) + file;
        SDL_free(dir);
        return path;
    }
    return file;
}
} // namespace

App::App(Options options) : options_(std::move(options)) {}

App::~App() { shutdown(); }

// ---------------------------------------------------------------- ciclo de vida

bool App::init() {
    configPath_ = prefFile("NiGBA", "NiGBA.ini");
    // Primeira execução com o nome novo: importa as preferências da época em que se chamava gbaemu.
    const std::string legacyPath = prefFile("gbaemu", "gbaemu.ini");
    const bool useLegacy = !std::filesystem::exists(gba::utf8Path(configPath_)) &&
                           std::filesystem::exists(gba::utf8Path(legacyPath));
    config_ = Config::load(useLegacy ? legacyPath : configPath_);
    if (options_.scale > 0) config_.scale = options_.scale;
    if (options_.mute) config_.mute = true;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init falhou: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); // pixels nítidos ao ampliar

    // A altura da barra de menus depende da fonte; a janela é criada já com espaço para ela.
    menuBarHeight_ = 18.0f + 2 * 6.0f;
    window_ = SDL_CreateWindow("NiGBA", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               gba::kScreenWidth * config_.scale,
                               gba::kScreenHeight * config_.scale + static_cast<int>(menuBarHeight_),
                               SDL_WINDOW_RESIZABLE);
    renderer_ = window_ ? SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    texture_ = renderer_ ? SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                             gba::kScreenWidth, gba::kScreenHeight)
                         : nullptr;
    if (!window_ || !renderer_ || !texture_) {
        std::fprintf(stderr, "Falha ao criar a janela: %s\n", SDL_GetError());
        return false;
    }

    // Áudio a 32.768 Hz, estéreo, 16 bits (o SDL converte para a taxa da placa de som).
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want{};
        want.freq = gba::Apu::kSampleRate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        audio_ = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        if (audio_) SDL_PauseAudioDevice(audio_, 0);
    }
    if (!audio_) std::fprintf(stderr, "Aviso: sem áudio (%s); usando o relógio do sistema.\n", SDL_GetError());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // a posição das janelas não precisa ser lembrada
    applyTheme();
    loadFonts();
    ImGui_ImplSDL2_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer2_Init(renderer_);

    lastTick_ = SDL_GetPerformanceCounter();
    if (!options_.romPath.empty()) openRom(options_.romPath);
    return true;
}

void App::shutdown() {
    if (!window_) return;
    saveBackupNow();
    for (SDL_GameController* pad : pads_) SDL_GameControllerClose(pad);
    pads_.clear();
    config_.save(configPath_);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (audio_) SDL_CloseAudioDevice(audio_);
    if (vramTexture_) SDL_DestroyTexture(vramTexture_);
    if (spriteTexture_) SDL_DestroyTexture(spriteTexture_);
    if (texture_) SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
    window_ = nullptr;
}

int App::run() {
    if (!init()) return 1;
    while (running_) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            handleEvent(ev);
        }
        runEmulation();
        render();
    }
    shutdown();
    return 0;
}

void App::openPad(int deviceIndex) {
    if (!SDL_IsGameController(deviceIndex)) return;
    if (SDL_GameController* pad = SDL_GameControllerOpen(deviceIndex)) {
        pads_.push_back(pad);
        toast(std::string("Controle conectado: ") + (SDL_GameControllerName(pad) ? SDL_GameControllerName(pad) : "?"));
    }
}

void App::closePad(int32_t instanceId) {
    for (auto it = pads_.begin(); it != pads_.end(); ++it) {
        if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(*it)) == instanceId) {
            SDL_GameControllerClose(*it);
            pads_.erase(it);
            toast("Controle desconectado");
            return;
        }
    }
}

std::string App::padName() const {
    if (pads_.empty()) return "";
    const char* name = SDL_GameControllerName(pads_.front());
    return name ? name : "Controle";
}

void App::handleEvent(const SDL_Event& ev) {
    if (ev.type == SDL_CONTROLLERDEVICEADDED) { openPad(ev.cdevice.which); return; }
    if (ev.type == SDL_CONTROLLERDEVICEREMOVED) { closePad(ev.cdevice.which); return; }

    // Remapeando: a próxima tecla (ou botão do controle) vira a nova escolha. Esc cancela.
    if (capture_ != Capture::None) {
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
            if (ev.key.keysym.sym != SDLK_ESCAPE && capture_ == Capture::Key) {
                config_.keys[static_cast<size_t>(captureIndex_)] = ev.key.keysym.scancode;
                config_.save(configPath_);
            }
            capture_ = Capture::None;
        } else if (ev.type == SDL_CONTROLLERBUTTONDOWN && capture_ == Capture::Pad) {
            config_.padButtons[static_cast<size_t>(captureIndex_)] = ev.cbutton.button;
            config_.save(configPath_);
            capture_ = Capture::None;
        }
        if (ev.type == SDL_QUIT) running_ = false;
        return;
    }

    if (ev.type == SDL_QUIT) running_ = false;
    if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE) running_ = false;
    if (ev.type == SDL_DROPFILE) { // ROM arrastada para a janela
        const std::string path = ev.drop.file;
        SDL_free(ev.drop.file);
        openRom(path);
        return;
    }
    if (ev.type != SDL_KEYDOWN || ev.key.repeat) return;

    const SDL_Keycode key = ev.key.keysym.sym;
    const bool ctrl = (ev.key.keysym.mod & KMOD_CTRL) != 0;
    if (ctrl && key == SDLK_o) { if (auto p = openRomDialog(window_)) openRom(*p); }
    else if (ctrl && key == SDLK_r) resetConsole();
    else if (ctrl && key >= SDLK_1 && key <= SDLK_4) { config_.stateSlot = static_cast<int>(key - SDLK_1) + 1; toast("Slot " + std::to_string(config_.stateSlot)); }
    else if (key == SDLK_ESCAPE) togglePause();
    else if (key == SDLK_F5) saveStateSlot(config_.stateSlot);
    else if (key == SDLK_F8) loadStateSlot(config_.stateSlot);
    else if (key == SDLK_F11) toggleFullscreen();
    else if (key == SDLK_F10) stepFrame();
    else if (key == SDLK_F1 && console_) diag::dumpState(*console_, frame_);
    else if (key == SDLK_F2 && console_) diag::dumpTrace(*console_);
}

// ---------------------------------------------------------------- emulação

bool App::runOneFrame(bool keepAudio) {
    try {
        console_->runFrame();
    } catch (const std::exception& e) {
        stopped_ = true;
        stopReason_ = e.what();
        std::printf("CPU parou em %08X após %llu instruções: %s\n", console_->cpu().pc(),
                    static_cast<unsigned long long>(console_->instructions()), e.what());
        std::fflush(stdout);
        return false;
    }
    ++frame_;
    if (frame_ % 300 == 0) saveBackupNow(); // ~5 s: protege contra fechamento inesperado
    auto samples = console_->apu().takeSamples();
    if (audio_ && keepAudio && !config_.mute && !samples.empty()) {
        if (config_.volume < 100) {
            for (auto& s : samples) s = static_cast<int16_t>(s * config_.volume / 100);
        }
        SDL_QueueAudio(audio_, samples.data(), static_cast<Uint32>(samples.size() * sizeof(int16_t)));
    }
    if (options_.probing) diag::printNewProbes(*console_, probesPrinted_);
    return true;
}

// Com áudio, o relógio é a placa de som (mantém ~50 ms na fila); sem áudio, o relógio do sistema.
// Assim a velocidade é a do GBA (59,73 quadros/s) em qualquer monitor.
void App::runEmulation() {
    const uint64_t now = SDL_GetPerformanceCounter();
    timeDebt_ = std::min(timeDebt_ + static_cast<double>(now - lastTick_) / static_cast<double>(SDL_GetPerformanceFrequency()),
                         4 * kFrameSeconds);
    lastTick_ = now;

    if (!console_ || paused_ || stopped_) { timeDebt_ = 0; SDL_Delay(1); return; }
    console_->bus().setKeyInput(readKeys());

    const bool wantAudio = audio_ && !config_.mute;
    int ran = 0;
    if (fastForward_) {
        for (; ran < 6 && !stopped_; ++ran) runOneFrame(false); // acelerado: sem som
        timeDebt_ = 0;
    } else if (wantAudio) {
        while (ran < 4 && !stopped_ && SDL_GetQueuedAudioSize(audio_) < kAudioTargetBytes) { runOneFrame(true); ++ran; }
    } else {
        while (ran < 4 && !stopped_ && timeDebt_ >= kFrameSeconds) { runOneFrame(false); timeDebt_ -= kFrameSeconds; ++ran; }
    }
    if (ran == 0) SDL_Delay(1);
}

uint16_t App::readKeys() const {
    if (capture_ != Capture::None) return 0x03FF; // remapeando: nada chega ao jogo
    uint16_t keys = 0x03FF;
    const auto press = [&keys](size_t bit) { keys = static_cast<uint16_t>(keys & ~(1u << bit)); };

    // Teclado (com um menu aberto ou uma janela em foco, o teclado é da interface, não do jogo).
    if (!ImGui::GetIO().WantCaptureKeyboard) {
        const Uint8* state = SDL_GetKeyboardState(nullptr);
        for (size_t i = 0; i < config_.keys.size(); ++i) {
            if (config_.keys[i] > 0 && config_.keys[i] < SDL_NUM_SCANCODES && state[config_.keys[i]]) press(i);
        }
    }

    // Controles: botões mapeados e o analógico esquerdo como direcional.
    constexpr Sint16 kDeadZone = 12000;
    for (SDL_GameController* pad : pads_) {
        for (size_t i = 0; i < config_.padButtons.size(); ++i) {
            const int b = config_.padButtons[i];
            if (b >= 0 && SDL_GameControllerGetButton(pad, static_cast<SDL_GameControllerButton>(b))) press(i);
        }
        const Sint16 x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        const Sint16 y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (x > kDeadZone) press(4);
        if (x < -kDeadZone) press(5);
        if (y < -kDeadZone) press(6);
        if (y > kDeadZone) press(7);
    }
    return keys;
}

// Gatilho direito (RT/R2) acelera, como o Tab.
bool App::padFastForward() const {
    for (SDL_GameController* pad : pads_) {
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) return true;
    }
    return false;
}

void App::render() {
    fastForward_ = capture_ == Capture::None &&
                   ((!ImGui::GetIO().WantCaptureKeyboard && SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_TAB] != 0) ||
                    padFastForward());

    SDL_SetRenderDrawColor(renderer_, 0xF3, 0xE6, 0xF7, 0xFF); // lavanda clarinho nas bordas
    SDL_RenderClear(renderer_);

    if (console_) {
        if (stopped_ || paused_) console_->ppu().renderFrame(); // mantém a imagem na tela
        SDL_UpdateTexture(texture_, nullptr, console_->ppu().framebuffer().data(),
                          gba::kScreenWidth * static_cast<int>(sizeof(uint32_t)));
        // Maior tamanho que cabe na área abaixo dos menus, mantendo a proporção 3:2.
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer_, &w, &h);
        const float top = fullscreen_ ? 0.0f : menuBarHeight_;
        const float areaH = static_cast<float>(h) - top;
        const float s = std::min(static_cast<float>(w) / gba::kScreenWidth, areaH / gba::kScreenHeight);
        SDL_FRect dst{(static_cast<float>(w) - gba::kScreenWidth * s) / 2, top + (areaH - gba::kScreenHeight * s) / 2,
                      gba::kScreenWidth * s, gba::kScreenHeight * s};
        SDL_RenderCopyF(renderer_, texture_, nullptr, &dst);
    }

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    drawUi();
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer_);
    SDL_RenderPresent(renderer_);

    char title[160];
    std::snprintf(title, sizeof title, "NiGBA%s%s%s", gameTitle_.empty() ? "" : " - ", gameTitle_.c_str(),
                  fastForward_ ? "  [acelerado]" : "");
    SDL_SetWindowTitle(window_, title);
}

// ---------------------------------------------------------------- ações

void App::openRom(const std::string& path) {
    std::vector<uint8_t> rom;
    gba::CartridgeHeader header;
    try {
        rom = gba::readFile(path);
        header = gba::parseHeader(rom);
    } catch (const std::exception& e) {
        toast(std::string("Não deu para abrir: ") + e.what());
        return;
    }

    closeRom();
    auto console = std::make_unique<gba::Gba>();
    console->loadRom(std::move(rom));
    if (!options_.biosPath.empty()) {
        try { console->loadBios(gba::readFile(options_.biosPath)); }
        catch (const std::exception& e) { toast(std::string("BIOS ignorada: ") + e.what()); }
    }
    console->reset();
    console->setProbing(options_.probing);

    // Save ao lado da ROM, com o mesmo nome e extensão .sav. Um .sav de tamanho inesperado é preservado.
    savePath_ = gba::pathToUtf8(gba::utf8Path(path).replace_extension(".sav"));
    auto& backup = console->bus().backup();
    if (std::filesystem::exists(gba::utf8Path(savePath_))) {
        try {
            if (!backup.load(gba::readFile(savePath_))) {
                savePath_ = gba::pathToUtf8(gba::utf8Path(path).replace_extension(".NiGBA.sav"));
                toast("O .sav existente tem tamanho inesperado e foi preservado");
            }
        } catch (const std::exception& e) {
            toast(std::string("Save não carregado: ") + e.what());
        }
    }

    std::printf("Título: %s | Código: %s | Save: %s\n", header.title.c_str(), header.gameCode.c_str(),
                gba::Backup::name(backup.type()));
    for (const auto& [addr, len] : options_.dumps) diag::dumpMemory(*console, addr, len);
    if (options_.testPattern) diag::writeTestPattern(console->bus());
    std::fflush(stdout);

    console_ = std::move(console);
    romPath_ = path;
    gameTitle_ = header.title.empty() ? gba::pathToUtf8(gba::utf8Path(path).stem()) : header.title;
    paused_ = options_.testPattern; // com o padrão de teste, a CPU não roda
    stopped_ = false;
    frame_ = 0;
    probesPrinted_ = 0;
    if (audio_) SDL_ClearQueuedAudio(audio_);
    config_.addRecent(path);
    config_.save(configPath_);
}

void App::closeRom() {
    if (!console_) return;
    saveBackupNow();
    console_.reset();
    romPath_.clear();
    savePath_.clear();
    gameTitle_.clear();
    stopped_ = false;
    paused_ = false;
    if (audio_) SDL_ClearQueuedAudio(audio_);
}

void App::resetConsole() {
    if (!console_) return;
    saveBackupNow();
    console_->reset();
    stopped_ = false;
    paused_ = false;
    if (audio_) SDL_ClearQueuedAudio(audio_);
    toast("Reiniciado");
}

void App::saveBackupNow() {
    if (!console_ || savePath_.empty()) return;
    auto& backup = console_->bus().backup();
    if (!backup.dirty() || backup.data().empty()) return;
    try {
        gba::writeFileSafely(savePath_, backup.data());
        backup.clearDirty();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Aviso: save não gravado: %s\n", e.what());
    }
}

std::string App::statePath(int slot) const {
    return gba::pathToUtf8(gba::utf8Path(romPath_).replace_extension(".ss" + std::to_string(slot)));
}

bool App::stateExists(int slot) const {
    return console_ && std::filesystem::exists(gba::utf8Path(statePath(slot)));
}

void App::saveStateSlot(int slot) {
    if (!console_) return;
    try {
        gba::writeFileSafely(statePath(slot), console_->saveState());
        toast("Estado salvo no slot " + std::to_string(slot));
    } catch (const std::exception& e) {
        toast(std::string("Não deu para salvar o estado: ") + e.what());
    }
}

void App::loadStateSlot(int slot) {
    if (!console_) return;
    if (!stateExists(slot)) { toast("O slot " + std::to_string(slot) + " está vazio"); return; }
    try {
        console_->loadState(gba::readFile(statePath(slot)));
        stopped_ = false;
        if (audio_) SDL_ClearQueuedAudio(audio_);
        toast("Estado do slot " + std::to_string(slot) + " carregado");
    } catch (const std::exception& e) {
        toast(std::string("Não deu para carregar: ") + e.what());
    }
}

void App::setScale(int scale) {
    config_.scale = std::clamp(scale, 1, 8);
    if (fullscreen_) toggleFullscreen();
    SDL_SetWindowSize(window_, gba::kScreenWidth * config_.scale,
                      gba::kScreenHeight * config_.scale + static_cast<int>(menuBarHeight_));
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void App::toggleFullscreen() {
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(window_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void App::toast(std::string message) {
    toast_ = std::move(message);
    toastUntil_ = ImGui::GetCurrentContext() ? ImGui::GetTime() + 2.5 : 2.5;
}
