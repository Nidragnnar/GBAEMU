// Testes dos cheats: formatos raw, CodeBreaker e GameShark v1/v2, condições, erros e o arquivo .cht.

#include <cstdio>
#include <string>
#include <vector>

#include "core/cheats.hpp"
#include "core/gba.hpp"

static int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::printf("FALHOU %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

static gba::Gba& console() {
    static gba::Gba g;
    static bool ready = false;
    if (!ready) {
        std::vector<uint8_t> rom(0x200, 0);
        g.loadRom(rom);
        g.reset();
        ready = true;
    }
    return g;
}

static std::string errorOf(const std::string& code, gba::CheatFormat f) {
    try { gba::parseCheat(code, f); } catch (const gba::CheatError& e) { return e.what(); }
    return "";
}

static void testRaw() {
    auto& bus = console().bus();
    const auto ops = gba::parseCheat("02000000:AB\n02000010:BEEF\n02000020:12345678", gba::CheatFormat::Auto);
    CHECK(gba::detectCheatFormat("02000000:AB") == gba::CheatFormat::Raw);
    gba::applyCheat(ops, bus, 0);
    CHECK(bus.read8(0x02000000) == 0xAB && bus.read16(0x02000010) == 0xBEEF && bus.read32(0x02000020) == 0x12345678u);
    CHECK(errorOf("0200000:AB", gba::CheatFormat::Raw).find("linha 1") != std::string::npos);
    CHECK(errorOf("02000000:ABC", gba::CheatFormat::Raw).find("2, 4 ou 8") != std::string::npos);
}

static void testCodeBreaker() {
    auto& bus = console().bus();
    // Código mestre (0/1) é ignorado; 3 = 8 bits, 8 = 16 bits. Espaços e minúsculas são aceitos.
    const auto ops = gba::parseCheat("0000ABCD 000A\n1000ABCD 0007\n32000100 00FF\n82000102 c0de", gba::CheatFormat::Auto);
    CHECK(gba::detectCheatFormat("82000102 C0DE") == gba::CheatFormat::CodeBreaker);
    CHECK(ops.size() == 2);
    bus.write8(0x02000101, 0x55);                    // vizinho: um código de 8 bits não pode tocá-lo
    gba::applyCheat(ops, bus, 0);
    CHECK(bus.read8(0x02000100) == 0xFF && bus.read16(0x02000102) == 0xC0DE);
    CHECK(bus.read8(0x02000101) == 0x55);

    // 7 = "se igual": só grava se a condição valer.
    bus.write16(0x02000200, 5);
    gba::applyCheat(gba::parseCheat("72000200 0005\n82000202 1111", gba::CheatFormat::CodeBreaker), bus, 0);
    CHECK(bus.read16(0x02000202) == 0x1111);
    gba::applyCheat(gba::parseCheat("72000200 0006\n82000202 2222", gba::CheatFormat::CodeBreaker), bus, 0);
    CHECK(bus.read16(0x02000202) == 0x1111); // condição falsa: a linha seguinte foi pulada

    // D = "se os botões estiverem apertados" (A + B = 0x0003).
    const auto keys = gba::parseCheat("D0000020 0003\n82000300 7777", gba::CheatFormat::CodeBreaker);
    gba::applyCheat(keys, bus, 0x0001);              // só A: não
    CHECK(bus.read16(0x02000300) == 0);
    gba::applyCheat(keys, bus, 0x0003);              // A + B: sim
    CHECK(bus.read16(0x02000300) == 0x7777);

    // OR e AND de 16 bits.
    bus.write16(0x02000400, 0x00F0);
    gba::applyCheat(gba::parseCheat("22000400 000F\n62000400 00FC", gba::CheatFormat::CodeBreaker), bus, 0);
    CHECK(bus.read16(0x02000400) == 0x00FC);

    CHECK(errorOf("92000000 0001", gba::CheatFormat::CodeBreaker).find("criptografado") != std::string::npos);
    CHECK(errorOf("82000102 C0DE\n8200010 C0DE", gba::CheatFormat::CodeBreaker).find("linha 2") != std::string::npos);
}

static void testGameShark() {
    // A cifra e a decifra se desfazem para qualquer valor.
    bool roundTrip = true;
    for (uint32_t i = 0; i < 200; ++i) {
        uint32_t a = i * 0x9E3779B1u, v = ~i * 0x85EBCA77u;
        const uint32_t a0 = a, v0 = v;
        gba::gameSharkEncrypt(a, v);
        roundTrip = roundTrip && (a != a0 || v != v0);
        gba::gameSharkDecrypt(a, v);
        roundTrip = roundTrip && a == a0 && v == v0;
    }
    CHECK(roundTrip);

    // Um código cifrado de verdade: "escrever 1234 (16 bits) em 02000500".
    uint32_t a = 0x12000500, v = 0x00001234;
    gba::gameSharkEncrypt(a, v);
    char text[32];
    std::snprintf(text, sizeof text, "%08X %08X", a, v);
    CHECK(gba::detectCheatFormat(text) == gba::CheatFormat::GameShark);
    auto& bus = console().bus();
    gba::applyCheat(gba::parseCheat(text, gba::CheatFormat::Auto), bus, 0);
    CHECK(bus.read16(0x02000500) == 0x1234);
}

static void testCheatFile() {
    std::vector<gba::Cheat> list(2);
    list[0].name = "Dinheiro infinito";
    list[0].code = "82000102 C0DE\n32000100 00FF";
    list[0].format = gba::CheatFormat::CodeBreaker;
    list[1].name = "Desligado";
    list[1].code = "02000000:01";
    list[1].enabled = false;
    const auto back = gba::deserializeCheats(gba::serializeCheats(list));
    CHECK(back.size() == 2);
    CHECK(back[0].name == "Dinheiro infinito" && back[0].code == list[0].code && back[0].enabled && back[0].ops.size() == 2);
    CHECK(back[1].name == "Desligado" && !back[1].enabled && back[1].format == gba::CheatFormat::Auto);

    const auto broken = gba::deserializeCheats("[Quebrado]\nativo=1\ncodigo=XYZ\n");
    CHECK(broken.size() == 1 && !broken[0].enabled); // código inválido fica guardado, mas desligado
}

int main() {
    testRaw();
    testCodeBreaker();
    testGameShark();
    testCheatFile();
    if (failures == 0) std::printf("Cheats: todos os testes passaram.\n");
    return failures == 0 ? 0 : 1;
}
