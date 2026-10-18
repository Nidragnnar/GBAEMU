// Testes do núcleo sem dependências externas. Saída != 0 indica falha.
#include <cstdio>
#include <vector>

#include "core/analysis.hpp"
#include "core/bus.hpp"
#include "core/cartridge.hpp"
#include "core/ppu.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FALHOU %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static std::vector<uint8_t> fakeRom() {
    std::vector<uint8_t> rom(0x200, 0);
    const char title[] = "POKEMON EMER";
    for (int i = 0; i < 12; ++i) rom[0xA0 + i] = static_cast<uint8_t>(title[i]);
    rom[0xAC] = 'B'; rom[0xAD] = 'P'; rom[0xAE] = 'E'; rom[0xAF] = 'E';
    rom[0xB0] = '0'; rom[0xB1] = '1';
    rom[0xB2] = 0x96;
    uint8_t sum = 0;
    for (int i = 0xA0; i <= 0xBC; ++i) sum = static_cast<uint8_t>(sum - rom[i]);
    rom[0xBD] = static_cast<uint8_t>(sum - 0x19);
    rom[0x100] = 0x78; rom[0x101] = 0x56; rom[0x102] = 0x34; rom[0x103] = 0x12;
    return rom;
}

static void flashCommand(gba::Bus& bus, uint8_t cmd) {
    bus.write8(0x0E005555, 0xAA);
    bus.write8(0x0E002AAA, 0x55);
    bus.write8(0x0E005555, cmd);
}

static void testBackup() {
    // Detecção pela assinatura na ROM
    auto rom = fakeRom();
    const char sig[] = "FLASH1M_V103";
    for (size_t i = 0; i < sizeof sig - 1; ++i) rom[0x150 + i] = static_cast<uint8_t>(sig[i]);
    CHECK(gba::Backup::detect(rom) == gba::Backup::Type::Flash128K);
    CHECK(gba::Backup::detect(fakeRom()) == gba::Backup::Type::Sram);

    gba::Bus bus;
    bus.loadRom(rom);
    CHECK(bus.backup().type() == gba::Backup::Type::Flash128K);
    CHECK(bus.read8(0x0E000000) == 0xFF);             // vazia

    // Identificação (o que o Pokémon faz na inicialização)
    flashCommand(bus, 0x90);
    CHECK(bus.read8(0x0E000000) == 0xC2);             // Macronix
    CHECK(bus.read8(0x0E000001) == 0x09);             // MX29L010 (128 KB)
    flashCommand(bus, 0xF0);
    CHECK(bus.read8(0x0E000000) == 0xFF);

    // Gravação de um byte
    flashCommand(bus, 0xA0);
    bus.write8(0x0E001234, 0x5A);
    CHECK(bus.read8(0x0E001234) == 0x5A);
    CHECK(bus.backup().dirty());
    bus.write8(0x0E001235, 0x77);                     // sem o comando: ignorado
    CHECK(bus.read8(0x0E001235) == 0xFF);

    // Troca de banco: o segundo banco é independente
    flashCommand(bus, 0xB0);
    bus.write8(0x0E000000, 1);
    CHECK(bus.read8(0x0E001234) == 0xFF);
    flashCommand(bus, 0xA0);
    bus.write8(0x0E001234, 0x11);
    flashCommand(bus, 0xB0);
    bus.write8(0x0E000000, 0);
    CHECK(bus.read8(0x0E001234) == 0x5A);
    CHECK(bus.backup().data()[0x11234] == 0x11);

    // Apagar setor de 4 KB (só o setor do endereço)
    flashCommand(bus, 0xA0);
    bus.write8(0x0E002000, 0x22);
    flashCommand(bus, 0x80);
    bus.write8(0x0E005555, 0xAA);
    bus.write8(0x0E002AAA, 0x55);
    bus.write8(0x0E001000, 0x30);
    CHECK(bus.read8(0x0E001234) == 0xFF);
    CHECK(bus.read8(0x0E002000) == 0x22);

    // Apagar o chip inteiro
    flashCommand(bus, 0x80);
    flashCommand(bus, 0x10);
    CHECK(bus.read8(0x0E002000) == 0xFF);
    CHECK(bus.backup().data()[0x11234] == 0xFF);

    // Carregar um save: só aceita o tamanho certo
    std::vector<uint8_t> saved(128 * 1024, 0);
    saved[3] = 0x99;
    CHECK(bus.backup().load(saved));
    CHECK(bus.read8(0x0E000003) == 0x99);
    CHECK(!bus.backup().load(std::vector<uint8_t>(1000, 0)));
}

static void testAnalysis() {
    namespace an = gba::analysis;
    const auto enc = an::encodePokemonText("Ab 1!");
    CHECK((enc == std::vector<uint8_t>{0xBB, 0xD6, 0x00, 0xA2, 0xAB}));
    CHECK(an::encodePokemonText("~").empty());

    std::vector<uint8_t> rom(0x400, 0);
    const auto put16 = [&rom](uint32_t off, uint16_t v) { rom[off] = static_cast<uint8_t>(v); rom[off + 1] = static_cast<uint8_t>(v >> 8); };
    put16(0x200, 0xB510);                       // push {r4, lr}: início da função
    put16(0x100, 0xF000); put16(0x102, 0xF87E); // BL para frente  -> 08000200
    put16(0x300, 0xF7FF); put16(0x302, 0xFF7E); // BL para trás    -> 08000200
    CHECK(an::thumbBlTarget(rom, 0x100) == 0x08000200u);
    CHECK(an::thumbBlTarget(rom, 0x300) == 0x08000200u);
    CHECK((an::findThumbCallers(rom, 0x08000200) == std::vector<uint32_t>{0x08000100, 0x08000300}));
    CHECK(an::findFunctionStart(rom, 0x240) == 0x08000200u);

    rom[0x3F0] = 0x01; rom[0x3F1] = 0x02; rom[0x3F2] = 0x00; rom[0x3F3] = 0x08; // ponteiro 08000201
    CHECK((an::findWord(rom, 0x08000201) == std::vector<uint32_t>{0x3F0}));
    CHECK((an::findBytes(rom, {0x10, 0xB5}) == std::vector<uint32_t>{0x200}));

    // Desvios para 08000200: BEQ curto e B incondicional
    put16(0x1F0, 0xD006);                       // beq: 0x1F0 + 4 + 6*2 = 0x200
    put16(0x210, 0xE7F6);                       // b:   0x210 + 4 - 20  = 0x200
    CHECK((an::findThumbBranchesTo(rom, 0x08000200) == std::vector<uint32_t>{0x080001F0, 0x08000210}));

    // Texto com códigos de controle antes das letras: o início é logo depois do 0xFF anterior.
    std::vector<uint8_t> text = {0xBB, 0xFF, 0xFC, 0x01, 0x02, 0xC3, 0xE2, 0xFF};
    CHECK(an::findStringStart(text, 5) == 2);
}

int main() {
    // Cabeçalho
    auto rom = fakeRom();
    auto h = gba::parseHeader(rom);
    CHECK(h.title == "POKEMON EMER");
    CHECK(h.gameCode == "BPEE");
    CHECK(h.checksumOk);
    CHECK(h.fixedValueOk);
    rom[0xA0] ^= 1;
    CHECK(!gba::parseHeader(rom).checksumOk);

    gba::Bus bus;
    bus.loadRom(fakeRom());

    // ROM: leitura little-endian, espelhos e escrita ignorada
    CHECK(bus.read32(0x08000100) == 0x12345678u);
    CHECK(bus.read32(0x0A000100) == 0x12345678u);
    CHECK(bus.read32(0x0C000100) == 0x12345678u);
    bus.write32(0x08000100, 0);
    CHECK(bus.read32(0x08000100) == 0x12345678u);
    CHECK(bus.read16(0x08000102) == 0x1234);
    CHECK(bus.read16(0x08000103) == 0x1234); // alinhamento forçado
    CHECK(bus.read16(0x08100000) == ((0x08100000 >> 1) & 0xFFFF)); // open bus da ROM

    // EWRAM / IWRAM e espelhos
    bus.write32(0x02000010, 0xDEADBEEF);
    CHECK(bus.read32(0x02040010) == 0xDEADBEEFu);
    bus.write16(0x03000020, 0xBEEF);
    CHECK(bus.read16(0x03008020) == 0xBEEF);

    // VRAM: espelho 0x18000-0x1FFFF -> 0x10000-0x17FFF
    bus.write16(0x06010000, 0x1111);
    CHECK(bus.read16(0x06018000) == 0x1111);
    CHECK(bus.read16(0x06030000) == 0x1111);

    // Escritas de 8 bits
    bus.write8(0x05000001, 0xAB);              // paleta duplica
    CHECK(bus.read16(0x05000000) == 0xABAB);
    bus.write16(0x07000000, 0x1234);
    bus.write8(0x07000000, 0xFF);              // OAM ignora
    CHECK(bus.read16(0x07000000) == 0x1234);
    bus.write8(0x06000004, 0x5A);              // VRAM BG duplica
    CHECK(bus.read16(0x06000004) == 0x5A5A);
    bus.write16(0x06010002, 0x0000);
    bus.write8(0x06010002, 0x77);              // VRAM OBJ (modo 0) ignora
    CHECK(bus.read16(0x06010002) == 0x0000);

    // SRAM 8 bits
    bus.write8(0x0E000005, 0x42);
    CHECK(bus.read8(0x0E000005) == 0x42);
    CHECK(bus.read16(0x0E000005) == 0x4242);

    // BIOS só leitura
    bus.write32(0x00000000, 0xFFFFFFFF);
    CHECK(bus.read32(0x00000100) == 0xE129F000u); // de fora, a BIOS é protegida
    CHECK(bus.fetch32(0x00000000) == 0);           // o conteúdo não mudou
    CHECK(bus.read32(0x00000100) == 0);            // e a busca vira a nova "última instrução"

    // KEYINPUT: nenhum botão pressionado por padrão, e escrita da CPU não altera
    CHECK(bus.read16(0x04000130) == 0x03FF);
    bus.write16(0x04000130, 0);
    CHECK(bus.read16(0x04000130) == 0x03FF);
    bus.setKeyInput(0x03FE); // A pressionado
    CHECK(bus.read16(0x04000130) == 0x03FE);

    // PPU modo 3
    CHECK(gba::Ppu::bgr555ToArgb(0x7FFF) == 0xFFFFFFFFu);
    CHECK(gba::Ppu::bgr555ToArgb(0x001F) == 0xFFFF0000u); // vermelho
    CHECK(gba::Ppu::bgr555ToArgb(0x7C00) == 0xFF0000FFu); // azul
    bus.write16(0x04000000, 0x0403);
    bus.write16(0x06000000, 0x03E0); // pixel (0,0) verde
    gba::Ppu ppu(bus);
    ppu.renderFrame();
    CHECK(ppu.framebuffer()[0] == 0xFF00FF00u);

    testBackup();
    testAnalysis();
    if (failures == 0) std::printf("Todos os testes passaram.\n");
    return failures == 0 ? 0 : 1;
}
