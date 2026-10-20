// Testes dos patches IPS, UPS e BPS: monta patches de verdade (com CRC32), aplica e confere byte a byte.

#include <cstdio>
#include <string>
#include <vector>

#include "core/patch.hpp"
#include "core/png.hpp"

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

// PNG: confere a assinatura, os CRCs de cada "chunk" e reconstrói os pixels dos blocos armazenados.
static uint32_t be32(const Bytes& b, size_t at) {
    return (uint32_t{b[at]} << 24) | (uint32_t{b[at + 1]} << 16) | (uint32_t{b[at + 2]} << 8) | b[at + 3];
}
static void testPng() {
    const int w = 7, h = 3;
    std::vector<uint32_t> px(w * h);
    for (int i = 0; i < w * h; ++i) px[i] = 0xFF000000u | (static_cast<uint32_t>(i * 10) << 16) | (0x80u << 8) | static_cast<uint32_t>(255 - i);
    const Bytes png = gba::encodePng(px.data(), w, h);
    CHECK(png.size() > 8 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');

    Bytes idat;
    size_t pos = 8;
    bool sawEnd = false;
    while (pos + 12 <= png.size()) {
        const uint32_t len = be32(png, pos);
        const std::string type(png.begin() + static_cast<long>(pos + 4), png.begin() + static_cast<long>(pos + 8));
        CHECK(gba::crc32(png.data() + pos + 4, len + 4) == be32(png, pos + 8 + len)); // CRC de cada chunk
        if (type == "IHDR") CHECK(be32(png, pos + 8) == static_cast<uint32_t>(w) && be32(png, pos + 12) == static_cast<uint32_t>(h));
        if (type == "IDAT") idat.insert(idat.end(), png.begin() + static_cast<long>(pos + 8), png.begin() + static_cast<long>(pos + 8 + len));
        if (type == "IEND") sawEnd = true;
        pos += 12 + len;
    }
    CHECK(sawEnd);
    // zlib: 2 bytes de cabeçalho, blocos armazenados (tipo, tamanho, ~tamanho, dados) e o Adler-32.
    Bytes raw;
    size_t z = 2;
    bool last = false;
    while (!last && z + 5 <= idat.size()) {
        last = idat[z] & 1;
        const uint32_t len = idat[z + 1] | (idat[z + 2] << 8);
        CHECK(((idat[z + 3] | (idat[z + 4] << 8)) ^ 0xFFFFu) == len);
        raw.insert(raw.end(), idat.begin() + static_cast<long>(z + 5), idat.begin() + static_cast<long>(z + 5 + len));
        z += 5 + len;
    }
    CHECK(raw.size() == static_cast<size_t>(h * (1 + w * 3)));
    bool pixelsOk = raw.size() == static_cast<size_t>(h * (1 + w * 3));
    for (int y = 0; pixelsOk && y < h; ++y) {
        pixelsOk = raw[static_cast<size_t>(y * (1 + w * 3))] == 0; // filtro "nenhum"
        for (int x = 0; pixelsOk && x < w; ++x) {
            const size_t at = static_cast<size_t>(y * (1 + w * 3) + 1 + x * 3);
            const uint32_t p = px[static_cast<size_t>(y * w + x)];
            pixelsOk = raw[at] == ((p >> 16) & 0xFF) && raw[at + 1] == ((p >> 8) & 0xFF) && raw[at + 2] == (p & 0xFF);
        }
    }
    CHECK(pixelsOk);
}

int main() {
    testCrc32();
    testIps();
    testUps();
    testBps();
    testUnknownFormat();
    testPng();
    if (failures == 0) std::printf("Patches (IPS, UPS, BPS) e PNG: todos os testes passaram.\n");
    return failures == 0 ? 0 : 1;
}
