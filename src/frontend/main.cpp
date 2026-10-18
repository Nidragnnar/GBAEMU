// gbaemu: emulador de Game Boy Advance.
// Uso: gbaemu [rom.gba] [--bios arquivo] [--scale N] [--mudo] [--test-pattern] [--sonda]
//             [--dump END:TAM] [--rastrear-texto TEXTO] [--quem-chama END] [--chamadas-para END]
//             [--desvios-para END]
// Sem ROM, abre a tela inicial (dá para escolher ou arrastar um jogo).

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/cartridge.hpp"
#include "frontend/app.hpp"
#include "frontend/diagnostics.hpp"

#ifdef _WIN32
#define NOMINMAX // impede as macros min/max do windows.h de quebrar std::max
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); // acentos corretos no terminal do Windows
#endif
    Options options;
    std::vector<std::string> textsToTrace;
    std::vector<uint32_t> whoCalls, callsTo, branchesTo;
    const auto hex = [](const char* s) { return static_cast<uint32_t>(std::strtoul(s, nullptr, 16)); };

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (!std::strcmp(a, "--bios") && hasValue) options.biosPath = argv[++i];
        else if (!std::strcmp(a, "--scale") && hasValue) options.scale = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(a, "--mudo")) options.mute = true;
        else if (!std::strcmp(a, "--test-pattern")) options.testPattern = true;
        else if (!std::strcmp(a, "--sonda")) options.probing = true;
        else if (!std::strcmp(a, "--rastrear-texto") && hasValue) textsToTrace.emplace_back(argv[++i]);
        else if (!std::strcmp(a, "--quem-chama") && hasValue) whoCalls.push_back(hex(argv[++i]));
        else if (!std::strcmp(a, "--chamadas-para") && hasValue) callsTo.push_back(hex(argv[++i]));
        else if (!std::strcmp(a, "--desvios-para") && hasValue) branchesTo.push_back(hex(argv[++i]));
        else if (!std::strcmp(a, "--dump") && hasValue) {
            // ENDERECO:TAMANHO em hexadecimal, por exemplo 080C2580:100
            const std::string arg = argv[++i];
            const size_t colon = arg.find(':');
            const uint32_t addr = hex(arg.substr(0, colon).c_str());
            const uint32_t len = colon == std::string::npos ? 0x80u : hex(arg.substr(colon + 1).c_str());
            options.dumps.emplace_back(addr & ~1u, std::min<uint32_t>(len, 0x1000));
        } else if (a[0] != '-') {
            options.romPath = a;
        } else {
            std::fprintf(stderr, "Opção desconhecida: %s\n", a);
        }
    }

    // Ferramentas de engenharia reversa: rodam sobre a ROM antes de abrir a janela.
    if (!options.romPath.empty() && (!textsToTrace.empty() || !whoCalls.empty() || !callsTo.empty() || !branchesTo.empty())) {
        try {
            const auto rom = gba::readFile(options.romPath);
            for (const auto& t : textsToTrace) diag::traceText(rom, t);
            for (const uint32_t addr : whoCalls) diag::whoCalls(rom, addr);
            for (const uint32_t addr : callsTo) diag::callsTo(rom, addr);
            for (const uint32_t addr : branchesTo) diag::branchesTo(rom, addr);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Erro: %s\n", e.what());
            return 1;
        }
    }

    App app(std::move(options));
    return app.run();
}
