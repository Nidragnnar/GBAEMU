#include "frontend/app.hpp"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>

#include "core/cartridge.hpp"
#include "core/cheats.hpp"
#include "core/color.hpp"
#include "core/patch.hpp"
#include "core/png.hpp"
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
    if (gridTexture_) SDL_DestroyTexture(gridTexture_);
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
        if (isPatchFile(path)) { // patch arrastado com um jogo aberto: reabre o jogo com ele
            if (romPath_.empty()) toast("Abra o jogo primeiro e depois arraste o patch");
            else openRom(romPath_, path);
        } else {
            openRom(path);
        }
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
    else if (key == SDLK_F12) takeScreenshot();
    else if (key == SDLK_F1 && console_) diag::dumpState(*console_, frame_);
    else if (key == SDLK_F2 && console_) diag::dumpTrace(*console_);
}

// ---------------------------------------------------------------- emulação

bool App::runOneFrame(bool keepAudio) {
    try {
        console_->runFrame();
        if (!cheats_.empty()) { // cheats ligados: aplicados a cada quadro, como no GameShark
            const uint16_t pressed = static_cast<uint16_t>(~console_->bus().read16(0x04000130) & 0x03FF);
            for (const auto& c : cheats_) {
                if (c.enabled) gba::applyCheat(c.ops, console_->bus(), pressed);
            }
        }
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
    if (frame_ % kRewindInterval == 0) rewind_.push(console_->saveState()); // histórico do "voltar no tempo"
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
    if (rewinding_) {
        // Voltando no tempo: a cada 3 quadros de tela, a foto anterior (cada uma vale 10 quadros, então
        // o jogo volta a ~3x a velocidade). Sem som; sem fotos, fica parado na mais antiga.
        while (ran < 4 && timeDebt_ >= kFrameSeconds) {
            timeDebt_ -= kFrameSeconds;
            ++ran;
            if (++rewindTimer_ % 3 != 0) continue;
            if (auto state = rewind_.pop()) {
                console_->loadState(*state);
                console_->apu().takeSamples(); // descarta o som acumulado
                frame_ = frame_ >= kRewindInterval ? frame_ - kRewindInterval : 0;
            }
        }
    } else if (fastForward_) {
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
// Efeito de tela LCD: uma textura do tamanho da imagem na tela, branca, com linhas cinza na borda de cada
// pixel do GBA, desenhada por cima com mistura multiplicativa (branco não muda nada; cinza escurece).
// É refeita só quando o tamanho muda. Abaixo de 3x a grade cobriria demais a imagem, então não aparece.
void App::drawLcdGrid(const SDL_FRect& dst) {
    const int w = static_cast<int>(dst.w), h = static_cast<int>(dst.h);
    if (w < gba::kScreenWidth * 3 || h < gba::kScreenHeight * 3) return;
    if (!gridTexture_ || gridW_ != w || gridH_ != h) {
        if (gridTexture_) SDL_DestroyTexture(gridTexture_);
        gridTexture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (!gridTexture_) return;
        gridW_ = w;
        gridH_ = h;
        std::vector<bool> lineX(static_cast<size_t>(w)), lineY(static_cast<size_t>(h));
        for (int i = 1; i <= gba::kScreenWidth; ++i) lineX[static_cast<size_t>(std::min(w - 1, i * w / gba::kScreenWidth - 1))] = true;
        for (int i = 1; i <= gba::kScreenHeight; ++i) lineY[static_cast<size_t>(std::min(h - 1, i * h / gba::kScreenHeight - 1))] = true;
        std::vector<uint32_t> px(static_cast<size_t>(w) * static_cast<size_t>(h));
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int lines = (lineX[static_cast<size_t>(x)] ? 1 : 0) + (lineY[static_cast<size_t>(y)] ? 1 : 0);
                const uint32_t v = lines == 0 ? 0xFF : lines == 1 ? 0xC4 : 0x9C; // 77% / 61% de brilho
                px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = 0xFF000000u | (v << 16) | (v << 8) | v;
            }
        }
        SDL_UpdateTexture(gridTexture_, nullptr, px.data(), w * static_cast<int>(sizeof(uint32_t)));
        SDL_SetTextureBlendMode(gridTexture_, SDL_BLENDMODE_MOD);
    }
    const SDL_FRect area{dst.x, dst.y, static_cast<float>(w), static_cast<float>(h)};
    SDL_RenderCopyF(renderer_, gridTexture_, nullptr, &area);
}

// Voltar no tempo: segurar R (se o R não estiver mapeado para um botão do GBA) ou o gatilho esquerdo.
bool App::rewindHeld() const {
    if (capture_ != Capture::None) return false;
    const bool rUsed = std::find(config_.keys.begin(), config_.keys.end(), static_cast<int>(SDL_SCANCODE_R)) != config_.keys.end();
    if (!rUsed && !ImGui::GetIO().WantCaptureKeyboard) {
        const Uint8* state = SDL_GetKeyboardState(nullptr);
        const bool ctrl = state[SDL_SCANCODE_LCTRL] || state[SDL_SCANCODE_RCTRL]; // Ctrl+R é reiniciar
        if (state[SDL_SCANCODE_R] && !ctrl) return true;
    }
    for (SDL_GameController* pad : pads_) {
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) return true;
    }
    return false;
}

bool App::padFastForward() const {
    for (SDL_GameController* pad : pads_) {
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) return true;
    }
    return false;
}

void App::render() {
    const bool wasRewinding = rewinding_;
    rewinding_ = console_ && !stopped_ && !paused_ && rewindHeld();
    if (rewinding_ && !wasRewinding && audio_) SDL_ClearQueuedAudio(audio_);
    fastForward_ = !rewinding_ && capture_ == Capture::None &&
                   ((!ImGui::GetIO().WantCaptureKeyboard && SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_TAB] != 0) ||
                    padFastForward());

    SDL_SetRenderDrawColor(renderer_, 0xF3, 0xE6, 0xF7, 0xFF); // lavanda clarinho nas bordas
    SDL_RenderClear(renderer_);

    if (console_) {
        if (stopped_ || paused_) console_->ppu().renderFrame(); // mantém a imagem na tela
        const auto& frame = console_->ppu().framebuffer();
        const uint32_t* pixels = frame.data();
        if (config_.gbaColors) { // cores da tela do GBA original
            gba::applyGbaLcdColors(frame.data(), displayBuffer_.data(), frame.size());
            pixels = displayBuffer_.data();
        }
        SDL_UpdateTexture(texture_, nullptr, pixels, gba::kScreenWidth * static_cast<int>(sizeof(uint32_t)));
        SDL_SetTextureScaleMode(texture_, config_.smoothing ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        // Maior tamanho que cabe na área abaixo dos menus, mantendo a proporção 3:2.
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer_, &w, &h);
        const float top = fullscreen_ ? 0.0f : menuBarHeight_;
        const float areaH = static_cast<float>(h) - top;
        const float s = std::min(static_cast<float>(w) / gba::kScreenWidth, areaH / gba::kScreenHeight);
        SDL_FRect dst{(static_cast<float>(w) - gba::kScreenWidth * s) / 2, top + (areaH - gba::kScreenHeight * s) / 2,
                      gba::kScreenWidth * s, gba::kScreenHeight * s};
        SDL_RenderCopyF(renderer_, texture_, nullptr, &dst);
        if (config_.lcdGrid) drawLcdGrid(dst);
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

// Patch ao lado da ROM, com o mesmo nome: Jogo.gba + Jogo.bps / Jogo.ups / Jogo.ips. Vazio se não houver.
std::string App::findPatchFor(const std::string& romPath) const {
    for (const char* ext : {".bps", ".ups", ".ips"}) {
        const auto candidate = gba::utf8Path(romPath).replace_extension(ext);
        if (std::filesystem::exists(candidate)) return gba::pathToUtf8(candidate);
    }
    return "";
}

bool App::isPatchFile(const std::string& path) {
    std::string ext = gba::pathToUtf8(gba::utf8Path(path).extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".ips" || ext == ".ups" || ext == ".bps";
}

void App::openRom(const std::string& path, std::string patchPath, bool usePatch) {
    std::vector<uint8_t> rom;
    gba::CartridgeHeader header;
    try {
        rom = gba::readFile(path);
    } catch (const std::exception& e) {
        toast(std::string("Não deu para abrir: ") + e.what());
        return;
    }

    // Soft-patching: o patch é aplicado na memória; o arquivo da ROM nunca é alterado.
    if (!usePatch) patchPath.clear();
    else if (patchPath.empty()) patchPath = findPatchFor(path);
    patchName_.clear();
    if (!patchPath.empty()) {
        const std::string name = gba::pathToUtf8(gba::utf8Path(patchPath).filename());
        try {
            const auto patch = gba::readFile(patchPath);
            const auto format = gba::detectPatchFormat(patch);
            rom = gba::applyPatch(rom, patch);
            patchName_ = name;
            toast(std::string("Patch aplicado: ") + name + " (" + gba::patchFormatName(format) + ")");
        } catch (const std::exception& e) {
            toast("Patch " + name + " não aplicado: " + e.what());
        }
    }

    try {
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

    std::printf("Título: %s | Código: %s | Save: %s%s%s\n", header.title.c_str(), header.gameCode.c_str(),
                gba::Backup::name(backup.type()), patchName_.empty() ? "" : " | Patch: ", patchName_.c_str());
    for (const auto& [addr, len] : options_.dumps) diag::dumpMemory(*console, addr, len);
    if (options_.testPattern) diag::writeTestPattern(console->bus());
    std::fflush(stdout);

    console_ = std::move(console);
    rewind_.clear();
    romPath_ = path;
    loadCheats();
    gameTitle_ = header.title.empty() ? gba::pathToUtf8(gba::utf8Path(path).stem()) : header.title;
    paused_ = options_.testPattern; // com o padrão de teste, a CPU não roda
    stopped_ = false;
    frame_ = 0;
    probesPrinted_ = 0;
    if (audio_) SDL_ClearQueuedAudio(audio_);
    config_.addRecent(path);
    config_.save(configPath_);
}

// Cheats do jogo: arquivo .cht ao lado da ROM (Jogo.gba -> Jogo.cht).
std::string App::cheatsPath() const { return gba::pathToUtf8(gba::utf8Path(romPath_).replace_extension(".cht")); }

void App::loadCheats() {
    cheats_.clear();
    cheatError_.clear();
    try {
        if (std::filesystem::exists(gba::utf8Path(cheatsPath()))) {
            const auto bytes = gba::readFile(cheatsPath());
            cheats_ = gba::deserializeCheats(std::string(bytes.begin(), bytes.end()));
            if (!cheats_.empty()) toast(std::to_string(cheats_.size()) + " cheat(s) carregado(s)");
        }
    } catch (const std::exception& e) {
        toast(std::string("Não deu para ler os cheats: ") + e.what());
    }
}

void App::saveCheats() {
    try {
        if (cheats_.empty() && !std::filesystem::exists(gba::utf8Path(cheatsPath()))) return; // nada a guardar
        const std::string text = gba::serializeCheats(cheats_);
        gba::writeFileSafely(cheatsPath(), std::vector<uint8_t>(text.begin(), text.end()));
    } catch (const std::exception& e) {
        toast(std::string("Não deu para salvar os cheats: ") + e.what());
    }
}

// Captura de tela: PNG na resolução do GBA (240×160), numa pasta "Capturas" ao lado da ROM, com o nome
// do jogo e a data e hora: "Pokemon FireRed 2026-10-19 21-30-05.png".
void App::takeScreenshot() {
    if (!console_) return;
    try {
        const auto rom = gba::utf8Path(romPath_);
        const auto dir = rom.parent_path() / "Capturas";
        std::filesystem::create_directories(dir);

        const std::time_t now = std::time(nullptr);
        std::tm t{};
#ifdef _WIN32
        localtime_s(&t, &now);
#else
        localtime_r(&now, &t);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", &t);
        const std::string base = gba::pathToUtf8(rom.stem()) + " " + stamp;
        auto file = dir / gba::utf8Path(base + ".png");
        for (int n = 2; std::filesystem::exists(file); ++n) { // várias capturas no mesmo segundo
            file = dir / gba::utf8Path(base + " (" + std::to_string(n) + ").png");
        }

        const auto& frame = console_->ppu().framebuffer();
        gba::writeFileSafely(gba::pathToUtf8(file), gba::encodePng(frame.data(), 240, 160));
        toast("Captura salva: " + gba::pathToUtf8(file.filename()));
    } catch (const std::exception& e) {
        toast(std::string("Não deu para salvar a captura: ") + e.what());
    }
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
    rewind_.clear();
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
        rewind_.clear(); // não volta para antes do estado carregado
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
