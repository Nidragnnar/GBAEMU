// Frontend SDL2: carrega a ROM, mostra o cabeçalho e roda o console quadro a quadro.
// Uso: gbaemu <rom.gba> [--bios gba_bios.bin] [--test-pattern] [--scale N] [--mudo] [--sonda] [--dump END:TAM]
// Com --test-pattern a CPU não roda; a tela mostra só o degradê.
//
// Controles: setas = direcional, Z = A, X = B, Enter = START, Backspace = SELECT,
//            A = L, S = R, Tab (segurar) = acelerar, F1 = diagnóstico, F2 = rastro da CPU, Esc = sair.

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/analysis.hpp"
#include "core/cartridge.hpp"
#include "core/gba.hpp"

#ifdef _WIN32
#define NOMINMAX // impede as macros min/max do windows.h de quebrar std::max
#include <windows.h>
#endif

namespace {

// Escreve um degradê direto na VRAM pelo barramento, como um jogo faria no modo 3.
// Serve para validar barramento -> PPU -> tela sem depender da CPU.
void writeTestPattern(gba::Bus& bus) {
    bus.write16(0x04000000, 0x0403); // DISPCNT: modo 3 + BG2 ligado
    for (int y = 0; y < gba::kScreenHeight; ++y) {
        for (int x = 0; x < gba::kScreenWidth; ++x) {
            const uint16_t r = static_cast<uint16_t>(x * 31 / (gba::kScreenWidth - 1));
            const uint16_t g = static_cast<uint16_t>(y * 31 / (gba::kScreenHeight - 1));
            const uint16_t b = 31 - r;
            bus.write16(0x06000000 + (y * gba::kScreenWidth + x) * 2,
                        static_cast<uint16_t>(r | (g << 5) | (b << 10)));
        }
    }
}

// KEYINPUT: bit em 0 = pressionado. Ordem dos bits: A B SELECT START → ← ↑ ↓ R L.
uint16_t readKeys() {
    static constexpr SDL_Scancode kMap[10] = {
        SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_RETURN,
        SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
        SDL_SCANCODE_S, SDL_SCANCODE_A,
    };
    const Uint8* state = SDL_GetKeyboardState(nullptr);
    uint16_t keys = 0x03FF;
    for (int i = 0; i < 10; ++i) {
        if (state[kMap[i]]) keys = static_cast<uint16_t>(keys & ~(1u << i));
    }
    return keys;
}

// F1: imprime o estado do vídeo, das interrupções e do DMA para diagnóstico.
void dumpState(gba::Gba& console, uint64_t frame) {
    auto& bus = console.bus();
    auto& cpu = console.cpu();
    const auto r16 = [&bus](uint32_t offset) { return static_cast<unsigned>(bus.read16(0x04000000 + offset)); };

    std::printf("\n===== diagnóstico (quadro %llu) =====\n", static_cast<unsigned long long>(frame));
    std::printf("CPU     PC=%08X CPSR=%08X modo=%02X %s, %llu instruções%s\n", cpu.pc(), cpu.cpsr(),
                cpu.cpsr() & 0x1F, cpu.thumb() ? "Thumb" : "ARM",
                static_cast<unsigned long long>(console.instructions()), cpu.sleeping() ? ", DORMINDO" : "");
    const unsigned dispcnt = r16(0x000);
    std::printf("DISPCNT=%04X (modo %u, forced blank %s, BGs %s%s%s%s, OBJ %s)\n", dispcnt, dispcnt & 7,
                (dispcnt & 0x80) ? "SIM" : "não", (dispcnt & 0x100) ? "0" : "-", (dispcnt & 0x200) ? "1" : "-",
                (dispcnt & 0x400) ? "2" : "-", (dispcnt & 0x800) ? "3" : "-", (dispcnt & 0x1000) ? "sim" : "não");
    std::printf("DISPSTAT=%04X VCOUNT=%u\n", r16(0x004), r16(0x006));
    std::printf("BG0CNT=%04X BG1CNT=%04X BG2CNT=%04X BG3CNT=%04X\n", r16(0x008), r16(0x00A), r16(0x00C), r16(0x00E));
    std::printf("BLDCNT=%04X BLDALPHA=%04X BLDY=%04X WININ=%04X WINOUT=%04X\n",
                r16(0x050), r16(0x052), r16(0x054), r16(0x048), r16(0x04A));
    std::printf("IE=%04X IF=%04X IME=%04X  [03007FF8]=%04X  tratador=[03007FFC]=%08X\n",
                r16(0x200), r16(0x202), r16(0x208), static_cast<unsigned>(bus.read16(0x03007FF8)),
                bus.read32(0x03007FFC));
    for (int n = 0; n < 4; ++n) {
        const uint32_t base = 0x040000B0 + static_cast<uint32_t>(n) * 12;
        std::printf("DMA%d SAD=%08X DAD=%08X CNT=%04X %04X\n", n, bus.read32(base), bus.read32(base + 4),
                    static_cast<unsigned>(bus.read16(base + 8)), static_cast<unsigned>(bus.read16(base + 10)));
    }

    std::printf("Paleta BG:");
    for (uint32_t i = 0; i < 16; ++i) std::printf(" %04X", static_cast<unsigned>(bus.read16(0x05000000 + i * 2)));
    std::printf("\n");
    const auto countNonZero = [&bus](uint32_t start, uint32_t size) {
        unsigned n = 0;
        for (uint32_t a = start; a < start + size; ++a) n += bus.read8(a) != 0;
        return n;
    };
    const unsigned whites = [&bus] {
        unsigned n = 0;
        for (uint32_t i = 0; i < 512; ++i) n += (bus.read16(0x05000000 + i * 2) & 0x7FFF) == 0x7FFF;
        return n;
    }();
    std::printf("Bytes não zero: VRAM BG %u/65536, VRAM OBJ %u/32768, paleta %u/1024 (cores brancas: %u/512), OAM %u/1024\n",
                countNonZero(0x06000000, 0x10000), countNonZero(0x06010000, 0x8000),
                countNonZero(0x05000000, 0x400), whites, countNonZero(0x07000000, 0x400));
    const auto& st = bus.backup().stats();
    std::printf("Save %s: %llu leituras, %llu escritas, identificação pedida %llu vez(es), respondida %llu vez(es)\n",
                gba::Backup::name(bus.backup().type()), static_cast<unsigned long long>(st.reads),
                static_cast<unsigned long long>(st.writes), static_cast<unsigned long long>(st.idRequests),
                static_cast<unsigned long long>(st.idReads));
    std::fflush(stdout);
}

// F2: registradores, últimas instruções executadas e o código em volta do PC.
void dumpTrace(gba::Gba& console) {
    auto& bus = console.bus();
    auto& cpu = console.cpu();
    std::printf("\n===== rastro =====\n");
    for (int i = 0; i < 16; ++i) {
        std::printf("r%-2d=%08X%s", i, cpu.reg(i), (i % 4 == 3) ? "\n" : "  ");
    }
    std::printf("CPSR=%08X\n\nÚltimas instruções (mais antiga primeiro):\n", cpu.cpsr());
    for (const auto& e : cpu.trace()) {
        if (e.thumb) std::printf("  %08X  T %04X\n", e.pc, e.instr);
        else std::printf("  %08X  A %08X\n", e.pc, e.instr);
    }
    const uint32_t pc = cpu.pc();
    const uint32_t start = (pc & ~1u) - 0x40;
    std::printf("\nMemória em volta do PC (halfwords):\n");
    for (uint32_t a = start; a < start + 0x80; a += 16) {
        std::printf("  %08X:", a);
        for (uint32_t k = 0; k < 16; k += 2) std::printf(" %04X", static_cast<unsigned>(bus.read16(a + k)));
        std::printf("\n");
    }
    std::fflush(stdout);
}

// --sonda: mostra os acessos suspeitos novos desde a última chamada.
void printNewProbes(const gba::Gba& console, size_t& printed) {
    static const char* const kNames[] = {
        "leitura na BIOS", "leitura de endereço não mapeado (open bus)", "leitura de registrador só-escrita",
        "leitura de I/O inexistente", "leitura de timer", "escrita em WAITCNT",
    };
    const auto& log = console.probeLog();
    for (; printed < log.size(); ++printed) {
        const auto& h = log[printed];
        std::printf("[sonda] quadro %-5llu PC %08X  %-44s %08X -> %02X\n", static_cast<unsigned long long>(h.frame),
                    h.pc, kNames[static_cast<int>(h.probe.kind)], h.probe.addr, h.probe.value);
    }
    std::fflush(stdout);
}

// Mostra halfwords da ROM em volta de um endereço (0x08xxxxxx).
void dumpRom(const std::vector<uint8_t>& rom, uint32_t center, uint32_t before, uint32_t after) {
    const uint32_t start = ((center - 0x08000000) & ~0xFu) - before;
    const uint32_t end = std::min<uint32_t>(static_cast<uint32_t>(rom.size()), (center - 0x08000000) + after);
    for (uint32_t off = start; off < end; off += 16) {
        std::printf("    %08X:", 0x08000000 + off);
        for (uint32_t k = 0; k < 16 && off + k + 1 < rom.size(); k += 2) {
            std::printf(" %04X", static_cast<unsigned>(rom[off + k] | (rom[off + k + 1] << 8)));
        }
        std::printf("\n");
    }
}

// Quem chama a função que começa em start (BL) e onde ela aparece como ponteiro (callbacks).
void printCallers(const std::vector<uint8_t>& rom, uint32_t start, int depth) {
    namespace an = gba::analysis;
    const std::string indent(static_cast<size_t>(depth) * 2, ' ');
    const auto callers = an::findThumbCallers(rom, start);
    const auto pointers = an::findWord(rom, start | 1);
    std::printf("%s  função em %08X: %zu chamada(s) BL, %zu ponteiro(s)\n", indent.c_str(), start,
                callers.size(), pointers.size());
    for (size_t i = 0; i < callers.size() && i < 6; ++i) {
        std::printf("%s  - BL em %08X\n", indent.c_str(), callers[i]);
        dumpRom(rom, callers[i], 0x80, 0x30);
    }
    for (size_t i = 0; i < pointers.size() && i < 6; ++i) {
        const uint32_t at = 0x08000000 + pointers[i];
        std::printf("%s  - ponteiro em %08X\n", indent.c_str(), at);
        dumpRom(rom, at, 0x60, 0x10);
        if (depth == 0) {
            if (const auto owner = an::findFunctionStart(rom, pointers[i])) printCallers(rom, *owner, depth + 1);
        }
    }
}

// --rastrear-texto: do texto até o código que decide exibi-lo.
void traceText(const std::vector<uint8_t>& rom, const std::string& text) {
    namespace an = gba::analysis;
    std::printf("\n===== rastreando \"%s\" =====\n", text.c_str());
    const auto encoded = an::encodePokemonText(text);
    if (encoded.empty()) { std::printf("texto com caractere sem equivalente na tabela do Pokémon\n"); return; }
    const auto hits = an::findBytes(rom, encoded);
    std::printf("texto encontrado em %zu lugar(es)\n", hits.size());
    for (size_t h = 0; h < hits.size() && h < 4; ++h) {
        const uint32_t textAddr = 0x08000000 + hits[h];
        const uint32_t start = an::findStringStart(rom, hits[h]);
        std::printf("\n[texto em %08X] início provável em %08X; bytes em volta:\n", textAddr, 0x08000000 + start);
        dumpRom(rom, 0x08000000 + start, 0x20, (hits[h] - start) + 0x40);

        // Ponteiros para qualquer endereço entre o início provável e a primeira letra.
        bool found = false;
        for (uint32_t addr = start; addr <= hits[h]; ++addr) {
            const auto refs = an::findWord(rom, 0x08000000 + addr);
            for (size_t r = 0; r < refs.size() && r < 6; ++r) {
                found = true;
                const uint32_t refAddr = 0x08000000 + refs[r];
                std::printf("\n  ponteiro para %08X em %08X; código antes dele:\n", 0x08000000 + addr, refAddr);
                dumpRom(rom, refAddr, 0x80, 0x10);
                if (const auto fn = an::findFunctionStart(rom, refs[r])) {
                    printCallers(rom, *fn, 0);
                } else {
                    std::printf("  (início da função não encontrado: talvez seja uma tabela)\n");
                }
            }
        }
        if (!found) std::printf("  nenhum ponteiro direto para o texto (pode estar numa tabela de textos)\n");
    }
    std::fflush(stdout);
}

// --quem-chama: a função que contém o endereço, quem a chama (BL) e onde ela é usada como ponteiro.
void whoCalls(const std::vector<uint8_t>& rom, uint32_t addr) {
    namespace an = gba::analysis;
    std::printf("\n===== quem chama %08X =====\n", addr);
    if (addr < 0x08000000 || addr - 0x08000000 >= rom.size()) { std::printf("fora da ROM\n"); return; }
    if (const auto start = an::findFunctionStart(rom, addr - 0x08000000, 0x1000)) {
        std::printf("início provável da função: %08X\n", *start);
        printCallers(rom, *start, 0);
    } else {
        std::printf("início da função não encontrado\n");
    }
    std::fflush(stdout);
}

// --chamadas-para: chamadas e ponteiros para um endereço exato (sem adivinhar o início da função).
void callsTo(const std::vector<uint8_t>& rom, uint32_t addr) {
    std::printf("\n===== chamadas para %08X =====\n", addr & ~1u);
    printCallers(rom, addr & ~1u, 0);
    std::fflush(stdout);
}

// --desvios-para: desvios que saltam para o endereço, o código antes deles e as funções chamadas ali.
void branchesTo(const std::vector<uint8_t>& rom, uint32_t target) {
    namespace an = gba::analysis;
    target &= ~1u;
    std::printf("\n===== desvios para %08X =====\n", target);
    const auto branches = an::findThumbBranchesTo(rom, target);
    std::printf("%zu desvio(s) encontrado(s)\n", branches.size());
    for (size_t b = 0; b < branches.size() && b < 6; ++b) {
        const uint32_t at = branches[b];
        std::printf("\n- desvio em %08X; código antes dele:\n", at);
        dumpRom(rom, at, 0x60, 0x10);
        // Funções chamadas (BL) nas instruções antes do desvio: é onde costuma estar o teste.
        const uint32_t from = at - 0x08000000 >= 0x60 ? at - 0x08000000 - 0x60 : 0;
        for (uint32_t off = from; off < at - 0x08000000; off += 2) {
            if (const auto t = an::thumbBlTarget(rom, off)) {
                std::printf("\n  função chamada em %08X -> %08X; início dela:\n", 0x08000000 + off, *t);
                if (*t >= 0x08000000 && *t - 0x08000000 < rom.size()) dumpRom(rom, *t, 0x0, 0xC0);
                off += 2; // BL ocupa duas halfwords
            }
        }
    }
    std::fflush(stdout);
}

// Grava o save se algo mudou. Erros são avisados mas não derrubam o emulador.
void saveBackup(gba::Backup& backup, const std::string& path) {
    if (!backup.dirty() || backup.data().empty()) return;
    try {
        gba::writeFileSafely(path, backup.data());
        backup.clearDirty();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Aviso: save não gravado: %s\n", e.what());
    }
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); // acentos corretos no console do Windows
#endif
    if (argc < 2) {
        std::fprintf(stderr, "Uso: %s <rom.gba> [--bios arquivo] [--test-pattern] [--scale N] [--mudo] [--sonda] [--dump END:TAM] [--rastrear-texto TEXTO] [--quem-chama END] [--chamadas-para END] [--desvios-para END]\n", argv[0]);
        return 1;
    }

    std::string romPath = argv[1], biosPath;
    bool testPattern = false;
    bool probing = false;
    bool mute = false;
    std::vector<std::pair<uint32_t, uint32_t>> dumps;
    std::vector<std::string> textsToTrace;
    std::vector<uint32_t> callersToFind;
    std::vector<uint32_t> exactCalls;
    std::vector<uint32_t> branchTargets;
    int scale = 3;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bios") && i + 1 < argc) biosPath = argv[++i];
        else if (!std::strcmp(argv[i], "--test-pattern")) testPattern = true;
        else if (!std::strcmp(argv[i], "--sonda")) probing = true;
        else if (!std::strcmp(argv[i], "--mudo")) mute = true;
        else if (!std::strcmp(argv[i], "--rastrear-texto") && i + 1 < argc) textsToTrace.emplace_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--desvios-para") && i + 1 < argc)
            branchTargets.push_back(static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16)));
        else if (!std::strcmp(argv[i], "--chamadas-para") && i + 1 < argc)
            exactCalls.push_back(static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16)));
        else if (!std::strcmp(argv[i], "--quem-chama") && i + 1 < argc)
            callersToFind.push_back(static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16)));
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) {
            // ENDERECO:TAMANHO em hexadecimal, por exemplo 080C2580:100
            const std::string arg = argv[++i];
            const size_t colon = arg.find(':');
            const uint32_t addr = static_cast<uint32_t>(std::strtoul(arg.substr(0, colon).c_str(), nullptr, 16));
            const uint32_t len = colon == std::string::npos ? 0x80u
                : static_cast<uint32_t>(std::strtoul(arg.substr(colon + 1).c_str(), nullptr, 16));
            dumps.emplace_back(addr & ~1u, std::min<uint32_t>(len, 0x1000));
        }
        else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) scale = std::max(1, std::atoi(argv[++i]));
    }

    gba::Gba console;
    try {
        auto rom = gba::readFile(romPath);
        for (const auto& text : textsToTrace) traceText(rom, text);
        for (const uint32_t addr : callersToFind) whoCalls(rom, addr);
        for (const uint32_t addr : exactCalls) callsTo(rom, addr);
        for (const uint32_t addr : branchTargets) branchesTo(rom, addr);
        const auto h = gba::parseHeader(rom);
        std::printf("Título:   %s\nCódigo:   %s\nEmpresa:  %s\nVersão:   %u\n"
                    "Checksum: %02X (%s)\nValor fixo 0x96: %s\n",
                    h.title.c_str(), h.gameCode.c_str(), h.makerCode.c_str(), h.version,
                    h.checksum, h.checksumOk ? "ok" : "INVÁLIDO", h.fixedValueOk ? "ok" : "INVÁLIDO");
        console.loadRom(std::move(rom));
        if (!biosPath.empty()) console.loadBios(gba::readFile(biosPath));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Erro: %s\n", e.what());
        return 1;
    }
    console.reset();
    console.setProbing(probing);
    for (const auto& [addr, len] : dumps) {
        std::printf("\n--dump %08X (%X bytes, halfwords):\n", addr, len);
        for (uint32_t a = addr; a < addr + len; a += 16) {
            std::printf("  %08X:", a);
            for (uint32_t k = 0; k < 16 && a + k < addr + len; k += 2) {
                std::printf(" %04X", static_cast<unsigned>(console.bus().read16(a + k)));
            }
            std::printf("\n");
        }
    }

    // Save ao lado da ROM, com o mesmo nome e extensão .sav.
    std::string savePath = std::filesystem::path(romPath).replace_extension(".sav").string();
    auto& backup = console.bus().backup();
    std::printf("Save:     %s", gba::Backup::name(backup.type()));
    if (std::filesystem::exists(savePath)) {
        try {
            if (backup.load(gba::readFile(savePath))) std::printf(" (carregado de %s)", savePath.c_str());
            else {
                // Nunca sobrescreve um save que não conseguimos ler: grava em outro arquivo.
                const std::string original = savePath;
                savePath = std::filesystem::path(romPath).replace_extension(".gbaemu.sav").string();
                std::printf(" (AVISO: %s tem tamanho inesperado e foi preservado; gravando em %s)",
                            original.c_str(), savePath.c_str());
            }
        } catch (const std::exception& e) {
            std::printf(" (AVISO: %s)", e.what());
        }
    }
    std::printf("\n");
    std::fflush(stdout);

    if (testPattern) writeTestPattern(console.bus());

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init falhou: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("gbaemu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       gba::kScreenWidth * scale, gba::kScreenHeight * scale, 0);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                         gba::kScreenWidth, gba::kScreenHeight);
    if (!win || !ren || !tex) {
        std::fprintf(stderr, "Falha ao criar janela: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Áudio a 32.768 Hz, estéreo, 16 bits (o SDL converte para a taxa da placa de som).
    SDL_AudioDeviceID audio = 0;
    if (!mute && SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want{};
        want.freq = gba::Apu::kSampleRate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        audio = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        if (audio) SDL_PauseAudioDevice(audio, 0);
    }
    if (!audio && !mute) std::fprintf(stderr, "Aviso: sem áudio (%s); usando o relógio do sistema.\n", SDL_GetError());

    // Ritmo: com áudio, o relógio é a placa de som (mantém ~50 ms de som na fila). Sem áudio, o relógio
    // do sistema. Assim a velocidade é a do GBA (59,73 quadros/s) em qualquer monitor.
    constexpr uint32_t kAudioTargetBytes = gba::Apu::kSampleRate * 2 * 2 / 20; // 50 ms
    constexpr double kFrameSeconds = static_cast<double>(gba::kCyclesPerFrame) / 16777216.0;
    const double perfFrequency = static_cast<double>(SDL_GetPerformanceFrequency());
    uint64_t lastTick = SDL_GetPerformanceCounter();
    double timeDebt = 0;

    size_t probesPrinted = 0;
    bool cpuRunning = !testPattern;
    bool running = true;
    uint64_t frame = 0;

    // Roda um quadro e entrega o som dele. Devolve false se a CPU parou.
    const auto runOneFrame = [&](bool keepAudio) {
        try {
            console.runFrame();
        } catch (const std::exception& e) {
            std::printf("CPU parou em %08X após %llu instruções: %s\n", console.cpu().pc(),
                        static_cast<unsigned long long>(console.instructions()), e.what());
            std::fflush(stdout);
            cpuRunning = false;
            return false;
        }
        ++frame;
        if (frame % 300 == 0) saveBackup(backup, savePath); // ~5 s: protege contra fechamento inesperado
        const auto samples = console.apu().takeSamples();
        if (audio && keepAudio && !samples.empty()) {
            SDL_QueueAudio(audio, samples.data(), static_cast<Uint32>(samples.size() * sizeof(int16_t)));
        }
        return true;
    };

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) running = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F1) dumpState(console, frame);
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F2) dumpTrace(console);
        }
        console.bus().setKeyInput(readKeys());
        const bool fastForward = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_TAB] != 0;

        const uint64_t now = SDL_GetPerformanceCounter();
        timeDebt = std::min(timeDebt + static_cast<double>(now - lastTick) / perfFrequency, 4 * kFrameSeconds);
        lastTick = now;

        int ran = 0;
        if (!cpuRunning) {
            console.ppu().renderFrame();
        } else if (fastForward) {
            for (; ran < 6 && cpuRunning; ++ran) runOneFrame(false); // acelerado: sem som
            timeDebt = 0;
        } else if (audio) {
            while (ran < 4 && cpuRunning && SDL_GetQueuedAudioSize(audio) < kAudioTargetBytes) { runOneFrame(true); ++ran; }
        } else {
            while (ran < 4 && cpuRunning && timeDebt >= kFrameSeconds) { runOneFrame(false); timeDebt -= kFrameSeconds; ++ran; }
        }
        if (ran == 0) SDL_Delay(1); // nada a fazer agora: não gasta CPU à toa

        if (probing) printNewProbes(console, probesPrinted);

        // Barra de título com o PC: mostra na hora se o jogo ficou preso em algum laço.
        char title[128];
        std::snprintf(title, sizeof title, "gbaemu - PC %08X - %llu instruções%s%s", console.cpu().pc(),
                      static_cast<unsigned long long>(console.instructions()), cpuRunning ? "" : " (parada)",
                      fastForward ? " [acelerado]" : "");
        SDL_SetWindowTitle(win, title);

        SDL_UpdateTexture(tex, nullptr, console.ppu().framebuffer().data(),
                          gba::kScreenWidth * static_cast<int>(sizeof(uint32_t)));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
    }

    saveBackup(backup, savePath);
    if (audio) SDL_CloseAudioDevice(audio);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
