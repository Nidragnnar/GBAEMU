// Testes dos patches IPS, UPS e BPS: monta patches de verdade (com CRC32), aplica e confere byte a byte.

#include <cstdio>
#include <string>
#include <vector>

#include "core/patch.hpp"

static int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::printf("FALHOU %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

using Bytes = std::vector<uint8_t>;

static Bytes rom16() {
    Bytes r(16);
    for (int i = 0; i < 16; ++i) r[i] = static_cast<uint8_t>(i);
    return r;
}

static void putVarint(Bytes& o, uint64_t v) { // inverso do número variável de UPS/BPS
    while (true) {
        const uint8_t x = v & 0x7F;
        v >>= 7;
        if (v == 0) { o.push_back(static_cast<uint8_t>(0x80 | x)); return; }
        o.push_back(x);
        --v;
    }
}
static void putLe32(Bytes& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
static void putFooter(Bytes& patch, const Bytes& source, const Bytes& target) {
    putLe32(patch, gba::crc32(source.data(), source.size()));
    putLe32(patch, gba::crc32(target.data(), target.size()));
    putLe32(patch, gba::crc32(patch.data(), patch.size()));
}
static std::string errorOf(const Bytes& rom, const Bytes& patch) {
    try { gba::applyPatch(rom, patch); } catch (const gba::PatchError& e) { return e.what(); }
    return "";
}

static void testCrc32() {
    const std::string s = "123456789";
    CHECK(gba::crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size()) == 0xCBF43926u); // valor de referência
}

static void testIps() {
    Bytes p = {'P', 'A', 'T', 'C', 'H'};
    p.insert(p.end(), {0x00, 0x00, 0x02, 0x00, 0x03, 0xA1, 0xA2, 0xA3}); // 3 bytes no endereço 2
    p.insert(p.end(), {0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x04, 0xAA}); // repetição: 4 x AA no endereço 10
    p.insert(p.end(), {0x00, 0x00, 0x14, 0x00, 0x02, 0xB1, 0xB2});       // além do fim: a ROM cresce
    p.insert(p.end(), {'E', 'O', 'F'});
    const Bytes out = gba::applyPatch(rom16(), p);
    CHECK(gba::detectPatchFormat(p) == gba::PatchFormat::Ips);
    CHECK(out.size() == 22);
    CHECK(out[1] == 1 && out[2] == 0xA1 && out[4] == 0xA3 && out[5] == 5);
    CHECK(out[9] == 9 && out[10] == 0xAA && out[13] == 0xAA && out[14] == 14);
    CHECK(out[16] == 0 && out[20] == 0xB1 && out[21] == 0xB2); // o espaço novo é preenchido com zero

    Bytes t = {'P', 'A', 'T', 'C', 'H', 'E', 'O', 'F', 0x00, 0x00, 0x08}; // extensão: tamanho final 8
    CHECK(gba::applyPatch(rom16(), t).size() == 8);
    CHECK(!errorOf(rom16(), {'P', 'A', 'T', 'C', 'H', 0x00, 0x00}).empty()); // incompleto
}

// Codificador UPS mínimo: trechos iguais são pulados; diferentes, guardados como XOR até um zero.
static Bytes makeUps(const Bytes& source, const Bytes& target) {
    Bytes p = {'U', 'P', 'S', '1'};
    putVarint(p, source.size());
    putVarint(p, target.size());
    const auto at = [](const Bytes& b, size_t i) -> uint8_t { return i < b.size() ? b[i] : 0; };
    size_t pos = 0, last = 0;
    while (pos < target.size()) {
        if ((at(source, pos) ^ at(target, pos)) == 0) { ++pos; continue; }
        putVarint(p, pos - last);
        while (pos < target.size() && (at(source, pos) ^ at(target, pos)) != 0) { p.push_back(at(source, pos) ^ at(target, pos)); ++pos; }
        p.push_back(0);
        ++pos;
        last = pos;
    }
    putFooter(p, source, target);
    return p;
}

static void testUps() {
    const Bytes rom = rom16();
    Bytes target = rom;
    target[3] = 0x33; target[4] = 0x44; target[12] = 0xCC;
    target.push_back(0x77); target.push_back(0x88); // e cresce 2 bytes
    const Bytes patch = makeUps(rom, target);
    CHECK(gba::detectPatchFormat(patch) == gba::PatchFormat::Ups);
    CHECK(gba::applyPatch(rom, patch) == target);

    Bytes otherVersion = rom;
    otherVersion[0] = 0xFF; // outra versão da ROM: o CRC32 de origem não confere
    CHECK(errorOf(otherVersion, patch).find("outra versão da ROM") != std::string::npos);

    Bytes corrupted = patch;
    corrupted[6] ^= 1; // um bit trocado no meio do patch
    CHECK(errorOf(rom, corrupted).find("corrompido") != std::string::npos);
}

static void testBps() {
    const Bytes rom = rom16();
    Bytes p = {'B', 'P', 'S', '1'};
    putVarint(p, 16);   // tamanho da origem
    putVarint(p, 13);   // tamanho do resultado
    putVarint(p, 0);    // sem metadados
    putVarint(p, ((4 - 1) << 2) | 0);                        // SourceRead: 4 bytes da origem
    putVarint(p, ((3 - 1) << 2) | 2); putVarint(p, 8 << 1);  // SourceCopy: 3 bytes a partir da origem[8]
    putVarint(p, ((2 - 1) << 2) | 1);                        // TargetRead: 2 bytes novos ("XY")
    p.push_back('X'); p.push_back('Y');
    putVarint(p, ((4 - 1) << 2) | 3); putVarint(p, 7 << 1);  // TargetCopy: 4 bytes a partir do resultado[7];
                                                             // como se sobrepõe ao que escreve, repete "XY"
    const Bytes target = {0, 1, 2, 3, 8, 9, 10, 'X', 'Y', 'X', 'Y', 'X', 'Y'};
    putFooter(p, rom, target);
    CHECK(gba::detectPatchFormat(p) == gba::PatchFormat::Bps);
    CHECK(gba::applyPatch(rom, p) == target);

    Bytes otherVersion = rom;
    otherVersion[15] = 0;
    CHECK(errorOf(otherVersion, p).find("outra versão da ROM") != std::string::npos);
}

static void testUnknownFormat() {
    CHECK(errorOf(rom16(), {'N', 'O', 'P', 'E'}).find("não é um patch") != std::string::npos);
}

int main() {
    testCrc32();
    testIps();
    testUps();
    testBps();
    testUnknownFormat();
    if (failures == 0) std::printf("Patches (IPS, UPS, BPS): todos os testes passaram.\n");
    return failures == 0 ? 0 : 1;
}
