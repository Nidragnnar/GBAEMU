// Testes de save state: a emulação depois de carregar tem de ser idêntica à original.
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "core/gba.hpp"
#include "core/rewind.hpp"

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

// ------------------------------------------------------------------ voltar no tempo (rewind)

static void testXorDiffRoundTrip() {
    std::vector<uint8_t> a(5000), b(5000);
    for (size_t i = 0; i < a.size(); ++i) a[i] = b[i] = static_cast<uint8_t>(i * 7);
    b[0] ^= 1; b[100] = 0xFF; b[101] = 0x00; b[4999] ^= 0x80;     // poucas diferenças, inclusive nas pontas
    for (size_t i = 2000; i < 2300; ++i) b[i] = static_cast<uint8_t>(~b[i]); // e um trecho inteiro diferente
    const auto diff = gba::compressXorDiff(a, b);
    CHECK(diff.size() < 400);                                   // bem menor que os 5000 bytes
    std::vector<uint8_t> back = b;
    gba::applyXorDiff(back, diff);
    CHECK(back == a);
    CHECK(gba::compressXorDiff(a, a).size() < 8);               // iguais: quase nada
    std::vector<uint8_t> other(10, 1);                          // tamanhos diferentes: guarda inteiro
    std::vector<uint8_t> t = a;
    gba::applyXorDiff(t, gba::compressXorDiff(other, a));
    CHECK(t == other);
}

static void testRewindBufferOrderAndCapacity() {
    gba::RewindBuffer buf(3);
    for (uint8_t i = 1; i <= 5; ++i) buf.push(std::vector<uint8_t>(64, i));
    CHECK(buf.size() == 3);
    for (uint8_t expected : {5, 4, 3}) {                        // do mais recente para o mais antigo
        auto s = buf.pop();
        CHECK(s && s->size() == 64 && (*s)[0] == expected && (*s)[63] == expected);
    }
    CHECK(!buf.pop());                                          // os dois mais antigos foram descartados
    buf.push({9});
    buf.clear();
    CHECK(buf.size() == 0 && !buf.pop());
}

// Com o console de verdade: guarda uma foto a cada 10 quadros e confere que a volta reconstrói cada
// estado exatamente, e que carregar a foto devolve o console àquele momento.
static void testRewindWithConsole() {
    gba::Gba g;
    g.loadRom(testRom());
    g.reset();
    gba::RewindBuffer buf(20);
    std::vector<std::vector<uint8_t>> expected;
    for (int i = 0; i < 12; ++i) {
        for (int f = 0; f < 10; ++f) g.runFrame();
        expected.push_back(g.saveState());
        buf.push(expected.back());
    }
    CHECK(buf.memoryUsed() < expected[0].size() * 2);          // 12 fotos em menos que o espaço de 2
    for (int i = 11; i >= 0; --i) {
        auto s = buf.pop();
        CHECK(s && *s == expected[static_cast<size_t>(i)]);
    }
    g.loadState(expected[3]);                                   // voltar para a 4ª foto...
    g.runFrame();
    gba::Gba fresh;                                             // ...e seguir dali dá o mesmo que um console
    fresh.loadRom(testRom());                                   // que nunca saiu desse caminho
    fresh.loadState(expected[3]);
    fresh.runFrame();
    CHECK(g.saveState() == fresh.saveState());
}

int main() {
    testRoundTrip();
    testRejectsOtherGameAndCorruption();
    testXorDiffRoundTrip();
    testRewindBufferOrderAndCapacity();
    testRewindWithConsole();
    if (failures == 0) std::printf("Save states: todos os testes passaram.\n");
    return failures == 0 ? 0 : 1;
}
