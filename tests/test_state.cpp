// Testes de save state: a emulação depois de carregar tem de ser idêntica à original.
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "core/gba.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FALHOU %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

// Programa ARM na ROM: liga o modo 3, o som (canal 2) e o Timer 0, e fica desenhando na VRAM.
static std::vector<uint8_t> testRom(uint32_t variant = 0) {
    const uint32_t program[] = {
        0xE3A00301, // mov  r0, #0x04000000
        0xE3A01B01, // mov  r1, #0x400
        0xE3811003, // orr  r1, r1, #3
        0xE1C010B0, // strh r1, [r0]          ; DISPCNT = modo 3 + BG2
        0xE3A01080, // mov  r1, #0x80
        0xE1C018B4, // strh r1, [r0, #0x84]   ; liga o som
        0xE3A01CFF, // mov  r1, #0xFF00
        0xE3811077, // orr  r1, r1, #0x77
        0xE1C018B0, // strh r1, [r0, #0x80]   ; volume e canais
        0xE3A01002, // mov  r1, #2
        0xE1C018B2, // strh r1, [r0, #0x82]
        0xE3A01A0F, // mov  r1, #0xF000
        0xE3811080, // orr  r1, r1, #0x80
        0xE1C016B8, // strh r1, [r0, #0x68]   ; canal 2: volume 15, ciclo 50%
        0xE3A01902, // mov  r1, #0x8000
        0xE3811B01, // orr  r1, r1, #0x400
        0xE1C016BC, // strh r1, [r0, #0x6C]   ; dispara
        0xE2802C01, // add  r2, r0, #0x100
        0xE3A01080, // mov  r1, #0x80
        0xE1C210B2, // strh r1, [r2, #2]      ; liga o Timer 0
        0xE2833001, // laço: add r3, r3, #1
        0xE3A04406, //       mov r4, #0x06000000
        0xE20350FF, //       and r5, r3, #0xFF
        0xE1A05085, //       mov r5, r5, lsl #1
        0xE18430B5, //       strh r3, [r4, r5]
        0xE0060393, //       mul r6, r3, r3   ; 2 a 5 ciclos conforme o valor: varia a "sobra" de ciclos
        0xEAFFFFF8, //       b laço
    };
    std::vector<uint8_t> rom(0x400, 0);
    for (size_t i = 0; i < std::size(program); ++i) {
        for (int b = 0; b < 4; ++b) rom[i * 4 + static_cast<size_t>(b)] = static_cast<uint8_t>(program[i] >> (b * 8));
    }
    rom[0x3FF] = static_cast<uint8_t>(variant); // muda a impressão digital da ROM
    return rom;
}

struct Trace {
    std::vector<uint32_t> pixels;
    std::vector<int16_t> audio;
    uint32_t r3 = 0, timer = 0;
    bool operator==(const Trace&) const = default;
};

static Trace runAndRecord(gba::Gba& g, int frames) {
    Trace t;
    for (int i = 0; i < frames; ++i) {
        g.runFrame();
        const auto& fb = g.ppu().framebuffer();
        t.pixels.insert(t.pixels.end(), fb.begin(), fb.begin() + 240 * 4); // 4 linhas por quadro bastam
        const auto s = g.apu().takeSamples();
        t.audio.insert(t.audio.end(), s.begin(), s.end());
    }
    t.r3 = g.cpu().reg(3);
    t.timer = g.bus().read16(0x04000100);
    return t;
}

static void testRoundTrip() {
    gba::Gba g;
    g.loadRom(testRom());
    g.reset();
    runAndRecord(g, 30);
    const auto state = g.saveState();

    const Trace original = runAndRecord(g, 20);
    g.loadState(state);
    const Trace replay = runAndRecord(g, 20);
    CHECK(original == replay);
    CHECK(!original.audio.empty() && original.r3 != 0);

    // Num console recém-criado (mesma ROM): nada pode depender de estado fora do save state.
    gba::Gba fresh;
    fresh.loadRom(testRom());
    fresh.reset();
    fresh.loadState(state);
    const Trace fromFresh = runAndRecord(fresh, 20);
    CHECK(original == fromFresh);
}

static void testRejectsOtherGameAndCorruption() {
    gba::Gba g;
    g.loadRom(testRom());
    g.reset();
    runAndRecord(g, 5);
    const auto state = g.saveState();

    gba::Gba other;
    other.loadRom(testRom(1));                           // outro "jogo"
    other.reset();
    runAndRecord(other, 3);
    const auto before = other.saveState();
    bool threw = false;
    try { other.loadState(state); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    CHECK(other.saveState() == before);                  // nada mudou

    auto truncated = state;
    truncated.resize(truncated.size() / 2);              // arquivo cortado ao meio
    const auto mine = g.saveState();
    threw = false;
    try { g.loadState(truncated); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    CHECK(g.saveState() == mine);                        // o estado atual foi restaurado

    auto garbage = state;
    garbage[0] = 'X';                                    // assinatura errada
    threw = false;
    try { g.loadState(garbage); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

int main() {
    testRoundTrip();
    testRejectsOtherGameAndCorruption();
    if (failures == 0) std::printf("Save states: todos os testes passaram.\n");
    return failures == 0 ? 0 : 1;
}
