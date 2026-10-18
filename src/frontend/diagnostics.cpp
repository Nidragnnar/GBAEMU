#include "frontend/diagnostics.hpp"

#include <algorithm>
#include <cstdio>

#include "core/analysis.hpp"

namespace diag {

namespace {
void dumpRom(const std::vector<uint8_t>& rom, uint32_t center, uint32_t before, uint32_t after);
void printCallers(const std::vector<uint8_t>& rom, uint32_t start, int depth);
} // namespace

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

namespace {
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

} // namespace

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


void dumpMemory(gba::Gba& console, uint32_t addr, uint32_t len) {
    std::printf("\n--dump %08X (%X bytes, halfwords):\n", addr, len);
    for (uint32_t a = addr; a < addr + len; a += 16) {
        std::printf("  %08X:", a);
        for (uint32_t k = 0; k < 16 && a + k < addr + len; k += 2) {
            std::printf(" %04X", static_cast<unsigned>(console.bus().read16(a + k)));
        }
        std::printf("\n");
    }
}

} // namespace diag
