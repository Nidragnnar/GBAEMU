// Roda as ROMs de teste do jsmolka (github.com/jsmolka/gba-tests) sem janela e confere se terminam na tela
// "All tests passed". Uso: gba_rom_tests <pasta com as ROMs>. ROMs ausentes são puladas, então este teste
// passa num computador sem elas; no GitHub Actions elas são baixadas antes (veja .github/workflows/ci.yml).
// As ROMs não ficam no repositório.

#include <cstdio>
#include <filesystem>
#include <string>

#include "core/cartridge.hpp"
#include "core/gba.hpp"

namespace {

// Impressão digital (FNV-1a) da imagem "All tests passed", que todas essas ROMs desenham igual.
constexpr uint64_t kAllTestsPassed = 0xCF4912C6BD29D1E5ull;

struct RomCase {
    const char* file;
    bool required; // false: só informa (ainda sem confirmação de que deve passar)
};

const RomCase kRoms[] = {
    {"arm.gba", true},     {"thumb.gba", true},    {"memory.gba", true}, {"nes.gba", true},
    {"unsafe.gba", true},  {"flash64.gba", true},  {"flash128.gba", true}, {"sram.gba", true},
    {"bios.gba", true},    {"none.gba", true},
};

uint64_t hashScreen(gba::Gba& g) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (const uint32_t px : g.ppu().framebuffer()) h = (h ^ px) * 0x100000001B3ull;
    return h;
}

uint64_t screenHash(const std::string& path) {
    gba::Gba g;
    g.loadRom(gba::readFile(path));
    g.reset();
    for (int i = 0; i < 300; ++i) g.runFrame(); // 5 s de GBA: sobra tempo para todas
    return hashScreen(g);
}

// Suíte do mGBA (suite.gba): entra numa categoria do menu e devolve a impressão digital da tela de placar.
uint64_t suiteCategoryHash(const std::string& path, int category) {
    gba::Gba g;
    g.loadRom(gba::readFile(path));
    g.reset();
    const auto frames = [&g](int n, uint16_t keys) {
        g.bus().setKeyInput(keys);
        for (int i = 0; i < n; ++i) g.runFrame();
    };
    const auto press = [&frames](int bit) { frames(4, static_cast<uint16_t>(0x3FF & ~(1u << bit))); frames(4, 0x3FF); };
    frames(60, 0x3FF);
    for (int i = 0; i < category; ++i) press(7); // baixo
    press(0);                                    // A: roda a categoria
    frames(1200, 0x3FF);
    return hashScreen(g);
}

// Categorias da suíte que o NiGBA já passa por inteiro, com a impressão digital da tela "N/N".
struct SuiteCase { int category; const char* name; uint64_t hash; };
const SuiteCase kSuite[] = {
    {0, "Memory", 0xE23FF7ECE4B5B0FAull},   {1, "I/O read", 0x64A29E0DB3647E96ull},
    {5, "Shifter", 0x4AE6B3C4F8314D1Eull},  {6, "Carry", 0xEA4F1BA8A09E1710ull},
    {8, "BIOS math", 0x993B9C33D4127E74ull}, {9, "DMA", 0x4DFFEDFE02D3BAD4ull},
    {10, "SIO regs", 0xE1E7A837EA77D779ull},
};

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path dir = argc > 1 ? argv[1] : "tests/roms";
    int failures = 0, ran = 0;
    for (const auto& rom : kRoms) {
        const auto path = dir / rom.file;
        if (!std::filesystem::exists(path)) {
            std::printf("  %-14s pulada (ROM ausente)\n", rom.file);
            continue;
        }
        ++ran;
        const bool passed = screenHash(gba::pathToUtf8(path)) == kAllTestsPassed;
        std::printf("  %-14s %s%s\n", rom.file, passed ? "passou" : "FALHOU", !passed && !rom.required ? " (informativo)" : "");
        if (!passed && rom.required) ++failures;
    }
    const auto suite = dir / "suite.gba";
    const bool printHashes = argc > 2 && std::string(argv[2]) == "--imprimir";
    if (std::filesystem::exists(suite)) {
        for (const auto& c : kSuite) {
            const uint64_t h = suiteCategoryHash(gba::pathToUtf8(suite), c.category);
            if (printHashes) { std::printf("  {%d, \"%s\", 0x%016llXull},\n", c.category, c.name, static_cast<unsigned long long>(h)); continue; }
            ++ran;
            const bool passed = h == c.hash;
            std::printf("  suite: %-10s %s\n", c.name, passed ? "passou (100%)" : "FALHOU");
            if (!passed) ++failures;
        }
    } else {
        std::printf("  suite.gba      pulada (ROM ausente)\n");
    }
    std::printf("ROMs de teste: %d rodadas, %d falha(s)\n", ran, failures);
    return failures == 0 ? 0 : 1;
}
