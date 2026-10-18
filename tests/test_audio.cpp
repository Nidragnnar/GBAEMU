// Testes do áudio: canais 1-4, filas A/B com DMA e timers.
#include <algorithm>
#include <cstdio>
#include <set>
#include <vector>

#include "core/gba.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FALHOU %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static std::vector<uint8_t> loopRom() {
    std::vector<uint8_t> rom(0x200, 0);
    rom[0] = 0xFE; rom[1] = 0xFF; rom[2] = 0xFF; rom[3] = 0xEA; // B .
    return rom;
}

struct Console {
    gba::Gba g;
    Console() { g.loadRom(loopRom()); g.reset(); }
    gba::Bus& bus() { return g.bus(); }
    void soundOn() {
        bus().write16(0x04000084, 0x0080);                 // liga o som
        bus().write16(0x04000080, 0xFF77);                 // canais 1-4 nos dois lados, volume máximo
        bus().write16(0x04000082, 0x0002);                 // canais 1-4 a 100%
    }
    std::vector<int16_t> run(int frames) {
        std::vector<int16_t> all;
        for (int i = 0; i < frames; ++i) {
            g.runFrame();
            const auto s = g.apu().takeSamples();
            all.insert(all.end(), s.begin(), s.end());
        }
        return all;
    }
};

static std::vector<int16_t> left(const std::vector<int16_t>& stereo) {
    std::vector<int16_t> l;
    for (size_t i = 0; i < stereo.size(); i += 2) l.push_back(stereo[i]);
    return l;
}

static int zeroCrossings(const std::vector<int16_t>& s) {
    int n = 0;
    for (size_t i = 1; i < s.size(); ++i) n += (s[i - 1] < 0) != (s[i] < 0);
    return n;
}

static void testSquareFrequency() {
    Console c;
    c.soundOn();
    c.bus().write16(0x04000068, 0xF080);                   // canal 2: volume 15, ciclo 50%
    c.bus().write16(0x0400006C, 0x8000 | 1024);            // 131072 / (2048 - 1024) = 128 Hz
    const auto l = left(c.run(60));                        // ~1 segundo
    const double seconds = static_cast<double>(l.size()) / gba::Apu::kSampleRate;
    const int crossings = zeroCrossings(l);                // 2 por ciclo
    const double hz = crossings / 2.0 / seconds;
    CHECK(hz > 120 && hz < 136);
    CHECK(*std::max_element(l.begin(), l.end()) > 1000);   // tem volume de verdade
}

static void testLengthAndStatus() {
    Console c;
    c.soundOn();
    c.bus().write16(0x04000068, 0xF080 | 0x3F);            // duração 64 - 63 = 1 passo de 1/256 s
    c.bus().write16(0x0400006C, 0xC000 | 1024);            // dispara com duração ligada
    c.g.apu().advance(512);
    CHECK(c.bus().read16(0x04000084) & 0x0002);            // canal 2 tocando
    c.run(1);
    CHECK(!(c.bus().read16(0x04000084) & 0x0002));         // acabou sozinho
}

static void testSweep() {
    Console c;
    c.soundOn();
    c.bus().write16(0x04000060, 0x0011);                   // sweep: período 1, deslocamento 1, subindo
    c.bus().write16(0x04000062, 0xF080);
    c.bus().write16(0x04000064, 0x8000 | 2000);            // 2000 + 1000 > 2047: estoura logo ao disparar
    c.g.apu().advance(512);
    CHECK(!(c.bus().read16(0x04000084) & 0x0001));

    c.bus().write16(0x04000064, 0x8000 | 100);             // agora sobe aos poucos
    c.run(2);
    CHECK((c.bus().ioRead16(0x064) & 0x7FF) > 100);        // a frequência foi atualizada no registrador
}

static void testNoise() {
    Console c;
    c.soundOn();
    c.bus().write16(0x04000078, 0xF000);                   // volume 15
    c.bus().write16(0x0400007C, 0x8000 | 0x0021);          // divisor 1, deslocamento 2
    const auto l = left(c.run(10));
    const std::set<int16_t> distinct(l.begin(), l.end());
    CHECK(distinct.size() > 20);                           // aleatório, não um tom fixo
    CHECK(zeroCrossings(l) > 200);
}

static void testWaveChannel() {
    Console c;
    c.soundOn();
    auto& bus = c.bus();
    bus.write16(0x04000070, 0x0040);                       // banco 1 tocando: a CPU escreve no banco 0
    for (uint32_t i = 0; i < 16; i += 2) bus.write16(0x04000090 + i, 0x00FF); // metade alta, metade baixa
    bus.write16(0x04000070, 0x0080);                       // liga o canal tocando o banco 0
    CHECK(bus.read16(0x04000090) == 0);                    // a CPU agora enxerga o banco 1 (vazio)
    bus.write16(0x04000072, 0x2000);                       // volume 100%
    bus.write16(0x04000074, 0x8000 | 1792);                // dispara
    const auto l = left(c.run(10));
    CHECK(zeroCrossings(l) > 100);
    CHECK(*std::max_element(l.begin(), l.end()) > 500);
}

static void testSoundOffIsSilent() {
    Console c;
    c.bus().write16(0x04000068, 0xF080);                   // som desligado: ignorado
    c.bus().write16(0x0400006C, 0x8000 | 1024);
    const auto s = c.run(5);
    CHECK(std::all_of(s.begin(), s.end(), [](int16_t v) { return v == 0; }));
    CHECK(s.size() > 2000);                                // mas as amostras continuam saindo
}

// O caminho da música do Pokémon: Timer 0 estoura a cada amostra, a fila A consome e o DMA 1 reabastece.
static void testFifoWithDmaAndTimer() {
    Console c;
    auto& bus = c.bus();
    for (uint32_t i = 0; i < 0x400; ++i) bus.write8(0x02000000 + i, (i / 16) % 2 ? 0xA0 : 0x60); // onda ±96
    bus.write16(0x04000084, 0x0080);
    bus.write16(0x04000082, 0x0B04);                       // fila A a 100%, nos dois lados, Timer 0, zera a fila
    bus.write32(0x040000BC, 0x02000000);                   // DMA 1: origem
    bus.write32(0x040000C0, 0x040000A0);                   //        destino: fila A
    bus.write32(0x040000C4, (0xB640u << 16));              // especial (som), repetir, 32 bits, liga
    bus.write16(0x04000100, 0xFE00);                       // Timer 0: estoura a cada 512 ciclos
    bus.write16(0x04000102, 0x0080);
    const auto l = left(c.run(4));
    CHECK(*std::max_element(l.begin(), l.end()) > 1000);
    CHECK(*std::min_element(l.begin(), l.end()) < -1000);
    CHECK(zeroCrossings(l) > 50);                          // a onda mudou de sinal várias vezes: a fila foi reabastecida
}

int main() {
    testSquareFrequency();
    testLengthAndStatus();
    testSweep();
    testNoise();
    testWaveChannel();
    testSoundOffIsSilent();
    testFifoWithDmaAndTimer();
    if (failures == 0) std::printf("Áudio: todos os testes passaram.\n");
    return failures == 0 ? 0 : 1;
}
