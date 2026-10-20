// Testes de DMA e da renderização de backgrounds.
#include <cstdio>
#include <vector>

#include "core/color.hpp"
#include "core/gba.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FALHOU %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

using gba::Ppu;

constexpr uint16_t kRed = 0x001F, kGreen = 0x03E0, kBlue = 0x7C00, kYellow = 0x03FF;

static std::vector<uint8_t> loopRom() {
    std::vector<uint8_t> rom(0x200, 0);
    rom[0] = 0xFE; rom[1] = 0xFF; rom[2] = 0xFF; rom[3] = 0xEA; // B . (ARM)
    return rom;
}

struct Console {
    gba::Gba g;
    Console() { g.loadRom(loopRom()); g.reset(); }
    gba::Bus& bus() { return g.bus(); }
    uint32_t pixel(int x, int y) { return g.ppu().framebuffer()[static_cast<size_t>(y) * gba::kScreenWidth + x]; }
    void render() { g.ppu().renderFrame(); }
};

static uint32_t argb(uint16_t c) { return Ppu::bgr555ToArgb(c); }

// ------------------------------------------------------------------ DMA

static void testDmaImmediate() {
    Console c;
    auto& bus = c.bus();
    for (uint32_t i = 0; i < 4; ++i) bus.write32(0x02000000 + i * 4, 0x1000 + i);
    bus.write32(0x040000D4, 0x02000000);               // DMA3 SAD
    bus.write32(0x040000D8, 0x03000000);               // DMA3 DAD
    bus.write32(0x040000DC, 4u | (0xC400u << 16));     // 4 palavras, 32 bits, IRQ, liga
    bus.flushPendingDma();                             // o DMA imediato começa 2 ciclos depois de ligado
    CHECK(bus.read32(0x03000000) == 0x1000 && bus.read32(0x0300000C) == 0x1003);
    CHECK(bus.read32(0x03000010) == 0);
    CHECK(!(bus.read16(0x040000DE) & 0x8000));         // desligou sozinho
    CHECK(bus.read16(0x04000202) & (gba::irq::Dma0 << 3));
}

// O DMA imediato só começa 2 ciclos depois de ligado: até lá nada foi copiado. Ciclos que passam (um
// acesso da CPU, por exemplo) completam a espera e o DMA roda antes do acesso seguinte.
static void testImmediateDmaStartDelay() {
    Console c;
    auto& bus = c.bus();
    bus.write16(0x02000000, 0xCAFE);
    bus.write32(0x040000D4, 0x02000000);
    bus.write32(0x040000D8, 0x03000200);
    bus.write32(0x040000DC, 1u | (0x8000u << 16));     // 1 halfword, liga
    CHECK(bus.read16(0x03000200) == 0);                // ainda esperando os 2 ciclos
    bus.idle(1);
    CHECK(bus.read16(0x03000200) == 0);                // 1 ciclo: ainda não
    bus.idle(1);                                       // 2 ciclos: a espera acabou...
    bus.load16(0x03000000);                            // ...e o DMA roda antes do próximo acesso
    CHECK(bus.read16(0x03000200) == 0xCAFE);
}

static void testDmaFill() {
    Console c;
    auto& bus = c.bus();
    bus.write16(0x02000000, 0xBEEF);
    bus.write32(0x040000B0, 0x02000000);               // DMA0
    bus.write32(0x040000B4, 0x03000100);
    bus.write32(0x040000B8, 8u | (0x8100u << 16));     // origem fixa, 16 bits, liga
    bus.flushPendingDma();
    CHECK(bus.read16(0x03000100) == 0xBEEF && bus.read16(0x0300010E) == 0xBEEF);
    CHECK(bus.read16(0x03000110) == 0);
}

static void testDmaVBlankRepeat() {
    Console c;
    auto& bus = c.bus();
    bus.write32(0x02000000, 0xA);
    bus.write32(0x02000004, 0xB);
    bus.write32(0x040000BC, 0x02000000);               // DMA1
    bus.write32(0x040000C0, 0x03000200);
    // 1 palavra, destino incrementa e recarrega, repetir, 32 bits, no VBlank, liga
    bus.write32(0x040000C4, 1u | ((0x8000u | 0x1000 | 0x0400 | 0x0200 | 0x0060) << 16));
    CHECK(bus.read32(0x03000200) == 0);                // ainda não: espera o VBlank
    c.g.dma().trigger(gba::Dma::Timing::VBlank);
    CHECK(bus.read32(0x03000200) == 0xA);
    c.g.dma().trigger(gba::Dma::Timing::VBlank);
    CHECK(bus.read32(0x03000200) == 0xB);              // origem avançou, destino recarregou
    CHECK(bus.read32(0x03000204) == 0);
    CHECK(bus.read16(0x040000C6) & 0x8000);            // continua ligado
    c.g.dma().trigger(gba::Dma::Timing::HBlank);       // outro momento: não dispara
    CHECK(bus.read32(0x03000200) == 0xB);

    bus.write16(0x040000C6, 0);                        // desliga
    bus.write32(0x02000008, 0xC);
    c.g.dma().trigger(gba::Dma::Timing::VBlank);
    CHECK(bus.read32(0x03000200) == 0xB);
}

// ------------------------------------------------------------------ backgrounds

// Tile 1 (16 cores): em todas as linhas, pixel 0 = cor 1 e pixel 1 = cor 2.
static void setupTile0Bg(Console& c) {
    auto& bus = c.bus();
    bus.write16(0x05000000, kBlue);                    // cor de fundo (backdrop)
    bus.write16(0x05000000 + (16 + 1) * 2, kRed);      // paleta 1, cor 1
    bus.write16(0x05000000 + (16 + 2) * 2, kGreen);    // paleta 1, cor 2
    bus.write16(0x05000000 + (16 + 3) * 2, kYellow);   // paleta 1, cor 3
    for (uint32_t row = 0; row < 8; ++row) bus.write16(0x06000020 + row * 4, 0x0021);
    bus.write16(0x06004000, 0x1001);                   // mapa em 0x4000: tile 1, paleta 1
    bus.write16(0x04000008, 8 << 8);                   // BG0CNT: tiles em 0, mapa no bloco 8
    bus.write16(0x04000000, 0x0100);                   // modo 0, BG0 ligado
}

static void testTextBackground() {
    Console c;
    setupTile0Bg(c);
    c.render();
    CHECK(c.pixel(0, 0) == argb(kRed));
    CHECK(c.pixel(1, 0) == argb(kGreen));
    CHECK(c.pixel(2, 0) == argb(kBlue));               // cor 0 é transparente
    CHECK(c.pixel(0, 7) == argb(kRed));
    CHECK(c.pixel(8, 0) == argb(kBlue));               // tile 0 vazio

    c.bus().write16(0x06004000, 0x1401);               // espelhado na horizontal
    c.render();
    CHECK(c.pixel(7, 0) == argb(kRed) && c.pixel(6, 0) == argb(kGreen) && c.pixel(0, 0) == argb(kBlue));

    c.bus().write16(0x06004000, 0x1001);
    c.bus().write16(0x04000010, 1);                    // rolagem horizontal de 1 pixel
    c.render();
    CHECK(c.pixel(0, 0) == argb(kGreen));
    c.bus().write16(0x04000010, 255);                  // o mapa de 256 px dá a volta
    c.render();
    CHECK(c.pixel(1, 0) == argb(kRed));

    c.bus().write16(0x04000000, 0x0180);               // forced blank
    c.render();
    CHECK(c.pixel(0, 0) == 0xFFFFFFFFu);
}

static void testPriority() {
    Console c;
    setupTile0Bg(c);
    auto& bus = c.bus();
    for (uint32_t i = 0; i < 32; i += 2) bus.write16(0x06000040 + i, 0x3333); // tile 2: tudo cor 3
    bus.write16(0x06004800, 0x1002);                   // mapa do BG1 em 0x4800
    bus.write16(0x04000008, (8 << 8) | 1);             // BG0 prioridade 1
    bus.write16(0x0400000A, 9 << 8);                   // BG1 prioridade 0
    bus.write16(0x04000000, 0x0300);
    c.render();
    CHECK(c.pixel(0, 0) == argb(kYellow));             // BG1 na frente
    bus.write16(0x04000008, 8 << 8);                   // empate: BG0 vence
    c.render();
    CHECK(c.pixel(0, 0) == argb(kRed));
}

static void test256Colors() {
    Console c;
    auto& bus = c.bus();
    bus.write16(0x05000000 + 5 * 2, 0x1234);
    bus.write16(0x06000040, 0x0005);                   // tile 1 de 256 cores: primeiro pixel = 5
    bus.write16(0x06004000, 0x0001);
    bus.write16(0x04000008, (8 << 8) | 0x80);
    bus.write16(0x04000000, 0x0100);
    c.render();
    CHECK(c.pixel(0, 0) == argb(0x1234));
}

static void testBitmapMode4() {
    Console c;
    auto& bus = c.bus();
    bus.write16(0x05000006, kGreen);                   // cor 3
    bus.write16(0x06000000, 0x0003);                   // quadro 0, pixel (0,0) = 3
    bus.write16(0x0600A000, 0x0300);                   // quadro 1, pixel (1,0) = 3
    bus.write16(0x04000000, 0x0404);
    c.render();
    CHECK(c.pixel(0, 0) == argb(kGreen));
    bus.write16(0x04000000, 0x0414);                   // mostra o quadro 1
    c.render();
    CHECK(c.pixel(1, 0) == argb(kGreen) && c.pixel(0, 0) != argb(kGreen));
}

static void testAffineBackground() {
    Console c;
    auto& bus = c.bus();
    bus.write16(0x05000000, kBlue);
    bus.write16(0x05000000 + 5 * 2, kRed);
    bus.write16(0x05000000 + 6 * 2, kGreen);
    bus.write16(0x06000040, 0x0605);                   // tile 1: pixels 5, 6
    bus.write16(0x06004000, 0x0001);                   // mapa afim: 1 byte por tile
    bus.write16(0x0400000C, 8 << 8);                   // BG2CNT: 128×128, sem repetir
    bus.write16(0x04000020, 0x0100);                   // PA = 1.0
    bus.write16(0x04000026, 0x0100);                   // PD = 1.0
    bus.write16(0x04000000, 0x0402);                   // modo 2, BG2
    c.render();
    CHECK(c.pixel(0, 0) == argb(kRed) && c.pixel(1, 0) == argb(kGreen));
    CHECK(c.pixel(130, 0) == argb(kBlue));             // fora dos 128 px, sem repetição

    bus.write16(0x04000020, 0x0080);                   // PA = 0.5: zoom 2×
    c.render();
    CHECK(c.pixel(1, 0) == argb(kRed) && c.pixel(2, 0) == argb(kGreen));

    bus.write16(0x0400000C, (8 << 8) | 0x2000);        // com repetição
    bus.write16(0x04000020, 0x0100);
    c.render();
    CHECK(c.pixel(128, 0) == argb(kRed));
}

// Efeito comum nos jogos: um DMA de HBlank troca a rolagem a cada linha a partir de uma tabela.
static void testHBlankScrollEffect() {
    Console c;
    setupTile0Bg(c);
    auto& bus = c.bus();
    for (uint32_t i = 0; i < 160; ++i) bus.write16(0x02000000 + i * 2, static_cast<uint16_t>(i + 1));
    bus.write32(0x040000B0, 0x02000000);               // DMA0: tabela -> BG0HOFS
    bus.write32(0x040000B4, 0x04000010);
    // 1 unidade, destino fixo, repetir, 16 bits, no HBlank, liga
    bus.write32(0x040000B8, 1u | ((0x8000u | 0x2000 | 0x0200 | 0x0040) << 16));
    c.g.runFrame();
    CHECK(c.pixel(0, 0) == argb(kRed));                // linha 0: rolagem 0
    CHECK(c.pixel(0, 1) == argb(kGreen));              // linha 1: rolagem 1
    CHECK(c.pixel(0, 2) == argb(kBlue));               // linha 2: rolagem 2
}

// ------------------------------------------------------------------ sprites

static void setSprite(gba::Bus& bus, int i, uint16_t a0, uint16_t a1, uint16_t a2) {
    const uint32_t base = 0x07000000 + static_cast<uint32_t>(i) * 8;
    bus.write16(base, a0);
    bus.write16(base + 2, a1);
    bus.write16(base + 4, a2);
}

// Esconde todos os sprites e prepara: fundo azul, paleta de sprites, tile 1 com pixels 0 = cor 1, 1 = cor 2.
static void setupSprites(Console& c, uint16_t dispcnt) {
    auto& bus = c.bus();
    for (int i = 0; i < 128; ++i) setSprite(bus, i, 0x0200, 0, 0);
    bus.write16(0x05000000, kBlue);
    bus.write16(0x05000202, kRed);     // paleta de sprites, cor 1
    bus.write16(0x05000204, kGreen);   // cor 2
    bus.write16(0x06010020, 0x0021);   // tile 1, linha 0
    bus.write16(0x04000000, dispcnt);
}

static void testSpriteBasic() {
    Console c;
    setupSprites(c, 0x1040);                             // modo 0, sprites ligados, mapeamento 1D
    setSprite(c.bus(), 0, 20, 10, 1);                    // y = 20, x = 10, tile 1
    c.render();
    CHECK(c.pixel(10, 20) == argb(kRed));
    CHECK(c.pixel(11, 20) == argb(kGreen));
    CHECK(c.pixel(12, 20) == argb(kBlue));               // cor 0 é transparente
    CHECK(c.pixel(10, 21) == argb(kBlue));

    setSprite(c.bus(), 0, 20, 10 | 0x1000, 1);           // espelhado na horizontal
    c.render();
    CHECK(c.pixel(17, 20) == argb(kRed) && c.pixel(16, 20) == argb(kGreen));

    setSprite(c.bus(), 0, 20 | 0x0200, 10, 1);           // desligado
    c.render();
    CHECK(c.pixel(10, 20) == argb(kBlue));

    setSprite(c.bus(), 0, 20 | 0x0800, 10, 1);           // modo janela: não aparece como imagem
    c.render();
    CHECK(c.pixel(10, 20) == argb(kBlue));

    c.bus().write16(0x04000000, 0x0040);                 // sprites desligados no DISPCNT
    setSprite(c.bus(), 0, 20, 10, 1);
    c.render();
    CHECK(c.pixel(10, 20) == argb(kBlue));
}

static void testSpriteEdges() {
    Console c;
    setupSprites(c, 0x1040);
    c.bus().write16(0x06010022, 0x0100);                 // tile 1, linha 0: pixel 6 = cor 1
    setSprite(c.bus(), 0, 20, 0x1FC, 1);                 // x = -4: entra pela esquerda
    c.render();
    CHECK(c.pixel(2, 20) == argb(kRed));                 // coluna 6 do sprite cai na coluna 2 da tela

    c.bus().write16(0x06010034, 0x0001);                 // tile 1, linha 5: pixel 0 = cor 1
    setSprite(c.bus(), 0, 252, 10, 1);                   // y = 252 = -4: entra por cima
    c.render();
    CHECK(c.pixel(10, 1) == argb(kRed));                 // linha 5 do sprite cai na linha 1 da tela
}

static void testSpritePriority() {
    Console c;
    setupSprites(c, 0x1140);                             // BG0 e sprites
    auto& bus = c.bus();
    bus.write16(0x05000006, kYellow);                    // BG: cor 3
    for (uint32_t i = 0; i < 32; i += 2) bus.write16(0x06000040 + i, 0x3333); // tile 2 do BG: tudo cor 3
    bus.write16(0x06004000, 0x0002);
    bus.write16(0x04000008, 8 << 8);                     // BG0 prioridade 0
    setSprite(bus, 0, 0, 0, 1 | (1 << 10));              // sprite prioridade 1
    c.render();
    CHECK(c.pixel(0, 0) == argb(kYellow));               // BG na frente
    setSprite(bus, 0, 0, 0, 1);                          // prioridade 0: empate favorece o sprite
    c.render();
    CHECK(c.pixel(0, 0) == argb(kRed));
    CHECK(c.pixel(5, 0) == argb(kYellow));               // pixel transparente do sprite mostra o BG

    // Entre sprites: número menor vence no empate; prioridade melhor vence sempre.
    bus.write16(0x06010060, 0x0002);                     // tile 3: pixel 0 = cor 2
    bus.write16(0x04000000, 0x1040);
    setSprite(bus, 0, 50, 50, 1 | (1 << 10));
    setSprite(bus, 1, 50, 50, 3 | (1 << 10));
    c.render();
    CHECK(c.pixel(50, 50) == argb(kRed));
    setSprite(bus, 1, 50, 50, 3);
    c.render();
    CHECK(c.pixel(50, 50) == argb(kGreen));
}

static void testSpriteMappingAndColors() {
    Console c;
    setupSprites(c, 0x1040);
    auto& bus = c.bus();
    bus.write16(0x060100C0, 0x0001);                     // tile 6: pixel 0 = cor 1
    bus.write16(0x06010480, 0x0002);                     // tile 36: pixel 0 = cor 2
    setSprite(bus, 0, 100, 100 | 0x4000, 4);             // 16×16, tile inicial 4
    c.render();
    CHECK(c.pixel(100, 108) == argb(kRed));              // 1D: segunda linha de tiles começa em 4 + 2
    bus.write16(0x04000000, 0x1000);
    c.render();
    CHECK(c.pixel(100, 108) == argb(kGreen));            // 2D: segunda linha começa em 4 + 32

    bus.write16(0x0500020A, 0x1234);                     // cor 5
    bus.write16(0x06010100, 0x0005);                     // unidade 8: primeiro pixel = 5 (256 cores)
    setSprite(bus, 0, 30, 30, 8);
    bus.write16(0x07000000, 30 | 0x2000);                // 256 cores
    c.render();
    CHECK(c.pixel(30, 30) == argb(0x1234));
}

static void testAffineSprite() {
    Console c;
    setupSprites(c, 0x1040);
    auto& bus = c.bus();
    const auto matrix = [&bus](int16_t pa, int16_t pb, int16_t pc, int16_t pd) {
        bus.write16(0x07000006, static_cast<uint16_t>(pa));   // matriz 0
        bus.write16(0x0700000E, static_cast<uint16_t>(pb));
        bus.write16(0x07000016, static_cast<uint16_t>(pc));
        bus.write16(0x0700001E, static_cast<uint16_t>(pd));
    };
    matrix(0x100, 0, 0, 0x100);                          // identidade
    setSprite(bus, 0, 40 | 0x0100, 60, 1);
    c.render();
    CHECK(c.pixel(60, 40) == argb(kRed) && c.pixel(61, 40) == argb(kGreen));

    setSprite(bus, 0, 40 | 0x0300, 60, 1);               // tamanho duplo: área 16×16, sprite centralizado
    c.render();
    CHECK(c.pixel(64, 44) == argb(kRed) && c.pixel(60, 40) == argb(kBlue));

    matrix(0x80, 0, 0, 0x80);                            // zoom 2× na área dupla
    c.render();
    CHECK(c.pixel(60, 40) == argb(kRed) && c.pixel(61, 40) == argb(kRed));
    CHECK(c.pixel(62, 40) == argb(kGreen));
}

static void testSpritesInBitmapMode() {
    Console c;
    setupSprites(c, 0x1043);                             // modo 3 (sem BG2): só fundo e sprites
    auto& bus = c.bus();
    setSprite(bus, 0, 10, 10, 1);                        // tile < 512: área usada pelo bitmap
    c.render();
    CHECK(c.pixel(10, 10) == argb(kBlue));
    bus.write16(0x06014020, 0x0001);                     // tile 513
    setSprite(bus, 0, 10, 10, 513);
    c.render();
    CHECK(c.pixel(10, 10) == argb(kRed));
}

static void testSpriteLineBudget() {
    Console c;
    setupSprites(c, 0x1040);
    auto& bus = c.bus();
    // 18 sprites 64×64 fora da tela gastam 18 × 64 = 1152 dos 1210 ciclos da linha.
    for (int i = 0; i < 18; ++i) setSprite(bus, i, 0, static_cast<uint16_t>((-100 & 0x1FF) | 0xC000), 1);
    setSprite(bus, 18, 0, 10 | 0xC000, 1);               // o 19º não cabe: não é desenhado
    c.render();
    CHECK(c.pixel(10, 0) == argb(kBlue));
    setSprite(bus, 17, 0x0200, 0, 0);                    // com um a menos, cabe
    c.render();
    CHECK(c.pixel(10, 0) == argb(kRed));
}

// ------------------------------------------------------------------ janelas, blending e mosaico

// BG de cor sólida: tile `tile` inteiro com a cor `index` da paleta 0, mapa inteiro apontando para ele.
static void solidBg(Console& c, int bg, uint32_t screenBlock, uint32_t tile, uint8_t index, uint16_t color) {
    auto& bus = c.bus();
    bus.write16(0x05000000 + index * 2u, color);
    const uint16_t pair = static_cast<uint16_t>(index | (index << 4));
    for (uint32_t i = 0; i < 32; i += 2) bus.write16(0x06000000 + tile * 32 + i, static_cast<uint16_t>(pair | (pair << 8)));
    for (uint32_t i = 0; i < 0x800; i += 2) bus.write16(0x06000000 + screenBlock * 0x800 + i, static_cast<uint16_t>(tile));
    bus.write16(0x04000008 + static_cast<uint32_t>(bg) * 2, static_cast<uint16_t>(screenBlock << 8));
}

static uint16_t rgb(int r, int g, int b) { return static_cast<uint16_t>(r | (g << 5) | (b << 10)); }

static void testWindows() {
    Console c;
    auto& bus = c.bus();
    bus.write16(0x05000000, kBlue);
    solidBg(c, 0, 8, 1, 1, kRed);
    bus.write16(0x04000040, (10 << 8) | 20);           // WIN0: x 10..19
    bus.write16(0x04000044, (0 << 8) | 10);            //       y 0..9
    bus.write16(0x04000048, 0x0000);                   // dentro: nada
    bus.write16(0x0400004A, 0x003F);                   // fora: tudo
    bus.write16(0x04000000, 0x2100);                   // BG0 + WIN0
    c.render();
    CHECK(c.pixel(15, 5) == argb(kBlue));              // dentro da janela: BG0 escondido
    CHECK(c.pixel(25, 5) == argb(kRed) && c.pixel(15, 15) == argb(kRed));

    bus.write16(0x04000040, (230 << 8) | 5);           // início > fim: dá a volta pela borda
    c.render();
    CHECK(c.pixel(235, 5) == argb(kBlue) && c.pixel(2, 5) == argb(kBlue) && c.pixel(100, 5) == argb(kRed));

    // WIN0 tem prioridade sobre WIN1 onde se sobrepõem.
    bus.write16(0x04000040, (10 << 8) | 20);
    bus.write16(0x04000042, (0 << 8) | 240);           // WIN1: largura toda
    bus.write16(0x04000046, (0 << 8) | 160);
    bus.write16(0x04000048, 0x0100);                   // WIN0: nada; WIN1: BG0
    bus.write16(0x04000000, 0x6100);
    c.render();
    CHECK(c.pixel(15, 5) == argb(kBlue) && c.pixel(50, 50) == argb(kRed));
}

static void testObjWindow() {
    Console c;
    setupSprites(c, 0x9140);                           // BG0, sprites, janela de sprite, 1D
    auto& bus = c.bus();
    solidBg(c, 0, 8, 1, 3, kYellow);
    bus.write16(0x0400004A, 0x0001);                   // fora: BG0; janela de sprite: nada
    setSprite(bus, 0, 20 | 0x0800, 10, 1);             // sprite no modo janela (pixels 0 e 1 opacos)
    c.render();
    CHECK(c.pixel(10, 20) == argb(kBlue));             // dentro da forma do sprite: BG0 escondido
    CHECK(c.pixel(12, 20) == argb(kYellow));           // pixel transparente do sprite: fora da janela
}

static void testAlphaAndBrightness() {
    Console c;
    auto& bus = c.bus();
    solidBg(c, 0, 8, 1, 1, rgb(31, 0, 0));
    solidBg(c, 1, 9, 2, 2, rgb(0, 0, 31));
    bus.write16(0x0400000A, (9 << 8) | 1);             // BG1 atrás (prioridade 1)
    bus.write16(0x04000000, 0x0300);
    bus.write16(0x04000050, 0x0001 | (1 << 6) | (0x02 << 8)); // BG0 sobre BG1, mistura
    bus.write16(0x04000052, 0x0808);                   // EVA = EVB = 8/16
    c.render();
    CHECK(c.pixel(0, 0) == argb(rgb(15, 0, 15)));

    bus.write16(0x04000050, 0x0001 | (2 << 6));        // clarear BG0
    bus.write16(0x04000054, 16);
    c.render();
    CHECK(c.pixel(0, 0) == argb(rgb(31, 31, 31)));
    bus.write16(0x04000054, 8);
    c.render();
    CHECK(c.pixel(0, 0) == argb(rgb(31, 15, 15)));

    bus.write16(0x04000050, 0x0001 | (3 << 6));        // escurecer BG0
    c.render();
    CHECK(c.pixel(0, 0) == argb(rgb(16, 0, 0)));

    // Uma janela sem o bit de efeitos desliga o blending ali.
    bus.write16(0x04000040, 240);
    bus.write16(0x04000044, 160);
    bus.write16(0x04000048, 0x0003);                   // WIN0: BG0 e BG1, sem efeitos
    bus.write16(0x04000000, 0x2300);
    c.render();
    CHECK(c.pixel(0, 0) == argb(rgb(31, 0, 0)));
}

static void testSemiTransparentSprite() {
    Console c;
    setupSprites(c, 0x1140);
    auto& bus = c.bus();
    solidBg(c, 0, 8, 1, 3, rgb(0, 0, 31));
    bus.write16(0x05000202, rgb(31, 0, 0));            // sprite vermelho
    bus.write16(0x04000050, 0x0001 << 8);              // BG0 é segundo alvo; nenhum efeito escolhido
    bus.write16(0x04000052, 0x0808);
    setSprite(bus, 0, 20 | 0x0400, 10, 1);             // modo semitransparente
    c.render();
    CHECK(c.pixel(10, 20) == argb(rgb(15, 0, 15)));    // misturado mesmo assim
    setSprite(bus, 0, 20, 10, 1);                      // sprite normal: opaco
    c.render();
    CHECK(c.pixel(10, 20) == argb(rgb(31, 0, 0)));
}

static void testMosaic() {
    Console c;
    setupTile0Bg(c);                                   // linha 0..7: pixel 0 vermelho, pixel 1 verde
    auto& bus = c.bus();
    bus.write16(0x04000008, (8 << 8) | 0x40);          // BG0 com mosaico
    bus.write16(0x0400004C, 0x0003);                   // blocos de 4 pixels na horizontal
    c.render();
    CHECK(c.pixel(1, 0) == argb(kRed) && c.pixel(3, 0) == argb(kRed));
    CHECK(c.pixel(4, 0) == argb(kBlue));

    bus.write16(0x06000020 + 4, 0x0000);               // linha 1 do tile vazia
    bus.write16(0x0400004C, 0x0010);                   // blocos de 2 linhas na vertical
    c.render();
    CHECK(c.pixel(0, 1) == argb(kRed));                // a linha 1 repete a linha 0
}

// Cores da tela do GBA: preto continua preto, branco fica quase branco, cores puras perdem saturação,
// o brilho só cresce com o valor do canal, e a conversão em lote é igual à de cada cor.
static void testLcdColorCorrection() {
    const auto ch = [](uint32_t c, int shift) { return static_cast<int>((c >> shift) & 0xFF); };
    CHECK((gba::gbaLcdColor(0xFF000000u) & 0xFFFFFF) == 0);
    const uint32_t white = gba::gbaLcdColor(0xFFF8F8F8u);
    CHECK(ch(white, 16) > 230 && ch(white, 8) > 230 && ch(white, 0) > 230); // (252, 238, 242): levemente rosado, como a tela real
    const uint32_t red = gba::gbaLcdColor(0xFFF80000u);           // vermelho puro...
    CHECK(ch(red, 16) > 200 && ch(red, 8) > 20 && ch(red, 0) > 60); // ...ganha verde e azul (menos saturado)
    int last = -1;
    bool monotonic = true;
    for (uint32_t v = 0; v < 32; ++v) {
        const int g = ch(gba::gbaLcdColor(0xFF000000u | ((v << 3) << 8)), 8);
        monotonic = monotonic && g >= last;
        last = g;
    }
    CHECK(monotonic);
    std::vector<uint32_t> in = {0xFF102030u, 0xFFF80000u, 0xFF00F800u, 0xFFFFFFFFu}, out(in.size());
    gba::applyGbaLcdColors(in.data(), out.data(), in.size());
    bool same = true;
    for (size_t i = 0; i < in.size(); ++i) same = same && out[i] == gba::gbaLcdColor(in[i]);
    CHECK(same);
}

int main() {
    testDmaImmediate();
    testDmaFill();
    testImmediateDmaStartDelay();
    testLcdColorCorrection();
    testDmaVBlankRepeat();
    testTextBackground();
    testPriority();
    test256Colors();
    testBitmapMode4();
    testAffineBackground();
    testHBlankScrollEffect();
    testSpriteBasic();
    testSpriteEdges();
    testSpritePriority();
    testSpriteMappingAndColors();
    testAffineSprite();
    testSpritesInBitmapMode();
    testSpriteLineBudget();
    testWindows();
    testObjWindow();
    testAlphaAndBrightness();
    testSemiTransparentSprite();
    testMosaic();
    if (failures == 0) std::printf("Vídeo, sprites, efeitos e DMA: todos os testes passaram.\n");
    return failures == 0 ? 0 : 1;
}
