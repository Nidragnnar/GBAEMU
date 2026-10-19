// Funções da BIOS do GBA emuladas em C++ (HLE), usadas quando nenhuma BIOS real é fornecida.
// Referência: GBATEK, "BIOS Functions". Os parâmetros chegam em r0-r3, como na BIOS original.

#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "cpu.hpp"

namespace gba {

namespace {
// Tabela de seno da BIOS: 256 passos por volta, valores em 1.14 (0x4000 = 1.0).
const std::array<int32_t, 256>& sineTable() {
    static const std::array<int32_t, 256> table = [] {
        std::array<int32_t, 256> t{};
        for (int i = 0; i < 256; ++i) t[i] = static_cast<int32_t>(std::lround(std::sin(i * 2.0 * 3.14159265358979323846 / 256.0) * 16384.0));
        return t;
    }();
    return table;
}

// Multiplicação de 32 bits com estouro "dando a volta", como no ARM (em C++, estouro com sinal é indefinido).
int32_t mul32(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }
int32_t shl32(int32_t a, int n) { return static_cast<int32_t>(static_cast<uint32_t>(a) << n); }

// ArcTan da BIOS: uma aproximação por polinômio em ponto fixo (1.14), com as constantes da própria BIOS.
// Os jogos (e as ROMs de teste) dependem dos valores exatos dela, inclusive dos que ficam em r1 e r3.
int16_t biosArcTan(int32_t i, int32_t& r1, int32_t& r3) {
    const int32_t a = -(mul32(i, i) >> 14);
    int32_t b = (mul32(0xA9, a) >> 14) + 0x390;
    for (const int32_t c : {0x91C, 0xFB6, 0x16AA, 0x2081, 0x3651, 0xA2F9}) b = (mul32(b, a) >> 14) + c;
    r1 = a;
    r3 = b;
    return static_cast<int16_t>(mul32(i, b) >> 16);
}

// ArcTan2 da BIOS: escolhe o octante e usa a ArcTan acima. 0x10000 = uma volta.
int32_t biosArcTan2(int32_t x, int32_t y, int32_t& r1) {
    int32_t r3 = 0;
    const auto atanOf = [&](int32_t num, int32_t den) {
        const int32_t n = shl32(num, 14);
        const int32_t q = den == -1 ? static_cast<int32_t>(0u - static_cast<uint32_t>(n)) : n / den; // sem estouro em C++
        return static_cast<int32_t>(biosArcTan(q, r1, r3));
    };
    if (y == 0) return x >= 0 ? 0 : 0x8000;
    if (x == 0) return y >= 0 ? 0x4000 : 0xC000;
    if (y >= 0) {
        if (x >= 0) {
            if (x >= y) return atanOf(y, x);
        } else if (-x >= y) {
            return atanOf(y, x) + 0x8000;
        }
        return 0x4000 - atanOf(x, y);
    }
    if (x <= 0) {
        if (-x > -y) return atanOf(y, x) + 0x8000;
    } else if (x >= -y) {
        return atanOf(y, x) + 0x10000;
    }
    return 0xC000 - atanOf(x, y);
}

[[noreturn]] void fail(const char* what, uint32_t number, uint32_t pc) {
    char msg[128];
    std::snprintf(msg, sizeof msg, "SWI 0x%02X %s (em %08X)", static_cast<unsigned>(number), what, pc);
    throw std::runtime_error(msg);
}
} // namespace

int Cpu::hleSwi(uint32_t number) {
    // A BIOS real termina toda SWI no mesmo ponto; a proteção da BIOS passa a mostrar esta instrução.
    bus_.setBiosOpcode(0xE3A02004);
    switch (number) {
    case 0x00: hleSoftReset(); return 3;
    case 0x01: hleRegisterRamReset(r_[0]); return 3;
    case 0x02: halt(); return 3;                                       // Halt
    case 0x04: hleIntrWait(r_[0] != 0, static_cast<uint16_t>(r_[1])); return 3;
    case 0x05: hleIntrWait(true, irq::VBlank); return 3;               // VBlankIntrWait
    case 0x09: {                                                        // ArcTan: tan em 1.14 -> ângulo
        int32_t a = 0, b = 0;
        r_[0] = static_cast<uint32_t>(static_cast<int32_t>(biosArcTan(static_cast<int32_t>(r_[0]), a, b)));
        r_[1] = static_cast<uint32_t>(a);
        r_[3] = static_cast<uint32_t>(b);
        return 3;
    }
    case 0x0A: {                                                        // ArcTan2(x, y): 0..0xFFFF = uma volta
        int32_t a = static_cast<int32_t>(r_[1]);
        r_[0] = static_cast<uint16_t>(biosArcTan2(static_cast<int32_t>(r_[0]), static_cast<int32_t>(r_[1]), a));
        r_[1] = static_cast<uint32_t>(a);
        r_[3] = 0x170;
        return 3;
    }
    case 0x0E: hleBgAffineSet(); return 3;
    case 0x0F: hleObjAffineSet(); return 3;
    case 0x19: {                                                        // SoundBias: nível 0 ou 0x200
        const uint16_t bias = bus_.ioRead16(0x088);
        bus_.write16(0x04000088, static_cast<uint16_t>((bias & ~0x3FF) | (r_[0] ? 0x200 : 0)));
        return 3;
    }
    case 0x06: hleDiv(static_cast<int32_t>(r_[0]), static_cast<int32_t>(r_[1])); return 3;
    case 0x07: hleDiv(static_cast<int32_t>(r_[1]), static_cast<int32_t>(r_[0])); return 3; // DivArm: ordem invertida
    case 0x08: { // Sqrt: raiz quadrada inteira de r0 (sem sinal)
        uint32_t value = r_[0], root = 0, bit = 1u << 30;
        while (bit > value) bit >>= 2;
        while (bit) {
            if (value >= root + bit) { value -= root + bit; root = (root >> 1) + bit; }
            else root >>= 1;
            bit >>= 2;
        }
        r_[0] = root;
        return 3;
    }
    case 0x0B: hleCpuSet(); return 3;
    case 0x0C: hleCpuFastSet(); return 3;
    case 0x11: hleDecompress(false, false); return 3; // LZ77UnCompWram
    case 0x12: hleDecompress(false, true); return 3;  // LZ77UnCompVram
    case 0x14: hleDecompress(true, false); return 3;  // RLUnCompWram
    case 0x15: hleDecompress(true, true); return 3;   // RLUnCompVram
    case 0x03: halt(); return 3;                      // Stop: tratado como Halt (acorda com uma IRQ)
    case 0x0D: r_[0] = 0xBAAE187F; r_[1] = 1; r_[3] = 0x4000; return 3; // GetBiosChecksum (valores da BIOS do GBA)
    case 0x10: hleBitUnPack(); return 3;
    case 0x13: hleHuffman(); return 3;                // HuffUnComp
    case 0x16: hleDiffUnFilter(false, false); return 3; // Diff8bitUnFilterWram
    case 0x17: hleDiffUnFilter(false, true); return 3;  // Diff8bitUnFilterVram
    case 0x18: hleDiffUnFilter(true, false); return 3;  // Diff16bitUnFilter
    case 0x1F: hleMidiKey2Freq(); return 3;
    case 0x25: r_[0] = 1; return 3;                   // MultiBoot: não há cabo de link; devolve "falhou"
    case 0x26: hleSoftReset(); return 3;              // HardReset: recomeça o jogo (sem a animação da BIOS)
    case 0x27: halt(); return 3;                      // CustomHalt
    // Driver de som da própria BIOS (raro: a maioria dos jogos traz o seu). Sem ele o jogo segue, só
    // sem esse som. 0x2A devolve uma lista de endereços: aqui, nenhum.
    case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
    case 0x28: case 0x29: case 0x2A:
        return 3;
    default:
        fail("ainda não implementada sem BIOS", number, pc());
    }
}

// SWI 04h/05h: liga IME e dorme até o tratador do jogo marcar uma das interrupções em [03007FF8].
// A espera em si acontece em Cpu::step(). Com discardOld, ignora marcações que já estavam lá.
void Cpu::hleIntrWait(bool discardOld, uint16_t flags) {
    bus_.write16(0x04000208, 1);
    const uint16_t check = bus_.read16(0x03007FF8);
    if (discardOld) {
        bus_.write16(0x03007FF8, static_cast<uint16_t>(check & ~flags));
    } else if (check & flags) {
        bus_.write16(0x03007FF8, static_cast<uint16_t>(check & ~flags)); // já tinha chegado: volta direto
        return;
    }
    intrWaitFlags_ = flags;
    waitActive_ = true;
}

// SWI 00h: limpa a área das pilhas e recomeça o jogo (A+B+START+SELECT no Pokémon).
// O byte em 03007FFA escolhe recomeçar na ROM (0) ou na EWRAM (outro valor).
void Cpu::hleSoftReset() {
    const bool fromRam = bus_.read8(0x03007FFA) != 0;
    for (uint32_t a = 0x03007E00; a < 0x03008000; a += 4) bus_.write32(a, 0);
    reset(true);
    bus_.setBiosOpcode(0xE129F000);
    jumpTo(fromRam ? 0x02000000 : 0x08000000);
}

// Matriz de rotação/escala: A = cos·sx, B = -sin·sx, C = sin·sy, D = cos·sy (8.8).
namespace {
struct Affine { int32_t pa, pb, pc, pd; };
Affine affineMatrix(int16_t sx, int16_t sy, uint16_t angle) {
    const auto& sine = sineTable();
    const uint32_t theta = angle >> 8; // a BIOS só usa o byte alto do ângulo
    const int32_t sin = sine[theta];
    const int32_t cos = sine[(theta + 64) & 0xFF];
    return {(sx * cos) >> 14, -((sx * sin) >> 14), (sy * sin) >> 14, (sy * cos) >> 14};
}
} // namespace

// SWI 0Fh: r0 = origem (sx, sy, ângulo; 8 bytes cada), r1 = destino, r2 = quantidade,
// r3 = distância em bytes entre PA, PB, PC e PD no destino (2 para BG, 8 para a OAM).
void Cpu::hleObjAffineSet() {
    uint32_t src = r_[0], dst = r_[1];
    const uint32_t stride = r_[3];
    for (uint32_t i = 0; i < r_[2]; ++i, src += 8) {
        const Affine m = affineMatrix(static_cast<int16_t>(bus_.read16(src)), static_cast<int16_t>(bus_.read16(src + 2)),
                                      bus_.read16(src + 4));
        for (const int32_t v : {m.pa, m.pb, m.pc, m.pd}) {
            bus_.write16(dst, static_cast<uint16_t>(v));
            dst += stride;
        }
    }
}

// SWI 0Eh: r0 = origem (20 bytes cada), r1 = destino (PA, PB, PC, PD, X, Y; 16 bytes cada), r2 = quantidade.
// X/Y posicionam o ponto (origX, origY) da imagem no ponto (dispX, dispY) da tela.
void Cpu::hleBgAffineSet() {
    uint32_t src = r_[0], dst = r_[1];
    for (uint32_t i = 0; i < r_[2]; ++i, src += 20, dst += 16) {
        const auto origX = static_cast<int32_t>(bus_.read32(src));
        const auto origY = static_cast<int32_t>(bus_.read32(src + 4));
        const int32_t dispX = static_cast<int16_t>(bus_.read16(src + 8));
        const int32_t dispY = static_cast<int16_t>(bus_.read16(src + 10));
        const Affine m = affineMatrix(static_cast<int16_t>(bus_.read16(src + 12)), static_cast<int16_t>(bus_.read16(src + 14)),
                                      bus_.read16(src + 16));
        bus_.write16(dst, static_cast<uint16_t>(m.pa));
        bus_.write16(dst + 2, static_cast<uint16_t>(m.pb));
        bus_.write16(dst + 4, static_cast<uint16_t>(m.pc));
        bus_.write16(dst + 6, static_cast<uint16_t>(m.pd));
        bus_.write32(dst + 8, static_cast<uint32_t>(origX - (m.pa * dispX + m.pb * dispY)));
        bus_.write32(dst + 12, static_cast<uint32_t>(origY - (m.pc * dispX + m.pd * dispY)));
    }
}

// SWI 01h: cada bit de r0 apaga uma área.
void Cpu::hleRegisterRamReset(uint32_t flags) {
    const auto clear = [this](uint32_t start, uint32_t size) {
        for (uint32_t a = start; a < start + size; a += 4) bus_.write32(a, 0);
    };
    if (flags & 0x01) clear(0x02000000, 0x40000);   // EWRAM 256 KB
    if (flags & 0x02) clear(0x03000000, 0x7E00);    // IWRAM, exceto os últimos 0x200 (pilhas)
    if (flags & 0x04) clear(0x05000000, 0x400);     // paleta
    if (flags & 0x08) clear(0x06000000, 0x18000);   // VRAM
    if (flags & 0x10) clear(0x07000000, 0x400);     // OAM
    if (flags & 0x20) clear(0x04000120, 0x40);      // registradores seriais
    if (flags & 0x40) clear(0x04000060, 0x50);      // registradores de som
    if (flags & 0x80) {                             // demais registradores
        clear(0x04000000, 0x60);
        clear(0x040000B0, 0x70);
        clear(0x04000160, 0x1A0);                   // até 0x2FF: não toca em POSTFLG/HALTCNT (0x300),
    }                                               // senão a própria limpeza poria a CPU para dormir
    bus_.write16(0x04000000, 0x0080); // a BIOS sempre deixa DISPCNT em "forced blank"
}

// SWI 06h: r0 / r1 com sinal. Devolve r0 = quociente, r1 = resto, r3 = |quociente|.
void Cpu::hleDiv(int32_t numerator, int32_t denominator) {
    if (denominator == 0) {
        // Divisão por zero: a BIOS real só termina para alguns numeradores; para os outros, trava. Aqui
        // sempre devolvemos o mesmo resultado definido que o mGBA usa (sinal do numerador, resto = numerador).
        r_[0] = numerator < 0 ? 0xFFFFFFFFu : 1u;
        r_[1] = static_cast<uint32_t>(numerator);
        r_[3] = 1;
        return;
    }
    // INT32_MIN / -1 estoura em C++; no hardware o resultado é o próprio INT32_MIN.
    const bool overflow = numerator == INT32_MIN && denominator == -1;
    const int32_t quotient = overflow ? INT32_MIN : numerator / denominator;
    const int32_t remainder = overflow ? 0 : numerator % denominator; // resto tem o sinal do numerador
    r_[0] = static_cast<uint32_t>(quotient);
    r_[1] = static_cast<uint32_t>(remainder);
    r_[3] = quotient < 0 ? 0u - static_cast<uint32_t>(quotient) : static_cast<uint32_t>(quotient);
}

// SWI 0Bh: r0 = origem, r1 = destino, r2 = quantidade (bits 0-20),
// bit 24 = preencher (repete o primeiro valor), bit 26 = unidades de 32 bits (senão 16).
void Cpu::hleCpuSet() {
    if (!(r_[0] & 0x0E000000)) return; // a BIOS ignora pedidos com origem dentro dela mesma
    const uint32_t control = r_[2];
    const uint32_t count = control & 0x1FFFFF;
    const bool fill = control & (1u << 24);
    const bool word = control & (1u << 26);
    uint32_t src = r_[0], dst = r_[1];

    if (word) {
        // Origem e destino vão sem alinhar: o barramento alinha nas regiões normais, mas a SRAM (8 bits)
        // usa o endereço exato (lê o byte dele repetido; grava o byte "girado" para aquela posição).
        const uint32_t fillValue = bus_.read32(src);
        for (uint32_t i = 0; i < count; ++i, dst += 4) {
            bus_.write32(dst, fill ? fillValue : bus_.read32(src));
            if (!fill) src += 4;
        }
    } else {
        // A BIOS lê com LDRH sem alinhar a origem: num endereço ímpar, o ARM7 gira a halfword em 8 bits e
        // a STRH grava só os 16 bits de baixo, ou seja, sobra um byte (o teste da suíte do mGBA confere).
        const auto readHalf = [this](uint32_t a) { return static_cast<uint16_t>(bus_.read16(a) >> ((a & 1) * 8)); };
        const uint16_t fillValue = readHalf(src);
        for (uint32_t i = 0; i < count; ++i, dst += 2) {
            bus_.write16(dst, fill ? fillValue : readHalf(src));
            if (!fill) src += 2;
        }
    }
}

// SWI 0Ch: como CpuSet, mas sempre em palavras e a quantidade é arredondada para múltiplo de 8.
void Cpu::hleCpuFastSet() {
    if (!(r_[0] & 0x0E000000)) return; // a BIOS ignora pedidos com origem dentro dela mesma
    const uint32_t control = r_[2];
    const uint32_t count = ((control & 0x1FFFFF) + 7) & ~7u;
    const bool fill = control & (1u << 24);
    uint32_t src = r_[0], dst = r_[1]; // sem alinhar, pelo mesmo motivo da CpuSet

    const uint32_t fillValue = bus_.read32(src);
    for (uint32_t i = 0; i < count; ++i, dst += 4) {
        bus_.write32(dst, fill ? fillValue : bus_.read32(src));
        if (!fill) src += 4;
    }
}

// SWI 11h/12h (LZ77) e 14h/15h (RLE). r0 = dados comprimidos, r1 = destino.
// Cabeçalho: bits 4-7 = tipo (a BIOS não confere), bits 8-31 = tamanho descomprimido (0 = nada a fazer).
// As versões "Vram" gravam em halfwords, porque escrita de 8 bits na VRAM é duplicada pelo hardware.
void Cpu::hleDecompress(bool rle, bool vram) {
    uint32_t src = r_[0];
    if (!(src & 0x0E000000)) return; // a BIOS ignora pedidos com origem dentro dela mesma
    const uint32_t header = bus_.read32(src);
    src += 4;
    const uint32_t size = header >> 8;
    if (size == 0) return;

    const uint32_t dst = r_[1];
    std::vector<uint8_t> out;
    out.reserve(size);

    if (rle) {
        while (out.size() < size) {
            const uint8_t flag = bus_.read8(src++);
            if (flag & 0x80) { // bloco repetido: um byte, (flag & 0x7F) + 3 vezes
                const uint8_t value = bus_.read8(src++);
                for (int n = (flag & 0x7F) + 3; n > 0 && out.size() < size; --n) out.push_back(value);
            } else {           // bloco literal: (flag & 0x7F) + 1 bytes
                for (int n = (flag & 0x7F) + 1; n > 0 && out.size() < size; --n) out.push_back(bus_.read8(src++));
            }
        }
    } else {
        while (out.size() < size) {
            const uint8_t flags = bus_.read8(src++); // 8 blocos, do bit 7 para o bit 0
            for (int bit = 7; bit >= 0 && out.size() < size; --bit) {
                if (!(flags & (1 << bit))) {
                    out.push_back(bus_.read8(src++));
                    continue;
                }
                // Referência: 4 bits de tamanho (+3) e 12 bits de distância (+1) para trás.
                const uint8_t b0 = bus_.read8(src++), b1 = bus_.read8(src++);
                const uint32_t length = (b0 >> 4) + 3u;
                const uint32_t distance = (((b0 & 0xFu) << 8) | b1) + 1u;
                for (uint32_t n = 0; n < length && out.size() < size; ++n) {
                    const uint32_t pos = static_cast<uint32_t>(out.size());
                    // A BIOS copia de dentro do próprio destino: antes do início, lê a memória que já estava lá.
                    out.push_back(distance <= pos ? out[pos - distance] : bus_.read8(dst + pos - distance));
                }
            }
        }
    }

    if (vram) {
        for (uint32_t i = 0; i < out.size(); i += 2) {
            const uint16_t hi = i + 1 < out.size() ? out[i + 1] : 0;
            bus_.write16(dst + i, static_cast<uint16_t>(out[i] | (hi << 8)));
        }
    } else {
        for (uint32_t i = 0; i < out.size(); ++i) bus_.write8(dst + i, out[i]);
    }
}

// SWI 10h: expande dados de 1/2/4/8 bits por unidade para 1/2/4/8/16/32 bits, somando um deslocamento.
// r0 = origem, r1 = destino (gravado em palavras de 32 bits), r2 = informações:
//   u16 tamanho da origem em bytes, u8 bits por unidade na origem, u8 bits por unidade no destino,
//   u32 deslocamento (bit 31 = somar também às unidades zero).
void Cpu::hleBitUnPack() {
    uint32_t src = r_[0], dst = r_[1];
    const uint32_t info = r_[2];
    const uint32_t length = bus_.read16(info);
    const uint32_t srcWidth = bus_.read8(info + 2), dstWidth = bus_.read8(info + 3);
    const uint32_t offsetWord = bus_.read32(info + 4);
    const uint32_t offset = offsetWord & 0x7FFFFFFF;
    const bool offsetZeros = offsetWord & 0x80000000;
    if (srcWidth == 0 || dstWidth == 0 || srcWidth > 8 || dstWidth > 32 || 8 % srcWidth || 32 % dstWidth) return;

    uint32_t out = 0, outBits = 0;
    const uint32_t srcMask = (1u << srcWidth) - 1;
    const uint32_t dstMask = dstWidth == 32 ? 0xFFFFFFFFu : (1u << dstWidth) - 1;
    for (uint32_t i = 0; i < length; ++i) {
        const uint8_t byte = bus_.read8(src++);
        for (uint32_t bit = 0; bit < 8; bit += srcWidth) {
            uint32_t unit = (byte >> bit) & srcMask;
            if (unit || offsetZeros) unit += offset;
            out |= (unit & dstMask) << outBits;
            outBits += dstWidth;
            if (outBits == 32) {
                bus_.write32(dst, out);
                dst += 4;
                out = 0;
                outBits = 0;
            }
        }
    }
}

// SWI 13h: descompressão Huffman. r0 = origem, r1 = destino (palavras de 32 bits).
// Cabeçalho: bits 0-3 = bits por valor (4 ou 8), bits 4-7 = 2, bits 8-31 = tamanho descomprimido.
// Depois: tamanho da árvore (byte; a tabela ocupa (n+1)*2 bytes contando esse byte), os nós, e a
// sequência de bits em palavras de 32 bits, do bit 31 para o 0 (0 = filho 0, 1 = filho 1).
// Nó: bits 0-5 = deslocamento até os filhos, bit 6 = filho 1 é valor, bit 7 = filho 0 é valor.
void Cpu::hleHuffman() {
    uint32_t src = r_[0];
    if (!(src & 0x0E000000)) return; // origem dentro da BIOS: ignorado, como na BIOS real
    const uint32_t header = bus_.read32(src);
    const uint32_t size = header >> 8;
    const uint32_t valueBits = header & 0xF;
    if (size == 0 || (valueBits != 4 && valueBits != 8)) return;
    const uint32_t treeStart = src + 4;
    const uint32_t root = treeStart + 1;
    uint32_t stream = treeStart + (bus_.read8(treeStart) + 1u) * 2;

    uint32_t dst = r_[1], out = 0, outBits = 0, written = 0;
    uint32_t node = root;
    uint8_t nodeValue = bus_.read8(node);
    while (written < size) {
        const uint32_t word = bus_.read32(stream);
        stream += 4;
        for (int bit = 31; bit >= 0 && written < size; --bit) {
            const uint32_t direction = (word >> bit) & 1;
            const uint32_t child = (node & ~1u) + (nodeValue & 0x3Fu) * 2 + 2 + direction;
            const bool isValue = nodeValue & (direction ? 0x40 : 0x80);
            if (!isValue) {
                node = child;
                nodeValue = bus_.read8(node);
                continue;
            }
            out |= (bus_.read8(child) & ((1u << valueBits) - 1)) << outBits;
            outBits += valueBits;
            if (outBits == 32) {
                bus_.write32(dst, out);
                dst += 4;
                written += 4;
                out = 0;
                outBits = 0;
            }
            node = root;
            nodeValue = bus_.read8(node);
        }
    }
}

// SWI 16h/17h/18h: desfaz o "filtro de diferença" (cada valor guardado como a diferença para o anterior).
// Cabeçalho: bits 0-3 = 1 (8 bits) ou 2 (16 bits), bits 4-7 = 8, bits 8-31 = tamanho em bytes.
// A versão "Vram" de 8 bits grava em halfwords, porque escrita de 8 bits na VRAM é duplicada.
void Cpu::hleDiffUnFilter(bool sixteenBit, bool vram) {
    uint32_t src = r_[0], dst = r_[1];
    if (!(src & 0x0E000000)) return;
    const uint32_t size = bus_.read32(src) >> 8;
    src += 4;
    if (sixteenBit) {
        uint16_t value = 0;
        for (uint32_t i = 0; i < size; i += 2) {
            value = static_cast<uint16_t>(value + bus_.read16(src + i));
            bus_.write16(dst + i, value);
        }
        return;
    }
    uint8_t value = 0;
    uint16_t pending = 0;
    for (uint32_t i = 0; i < size; ++i) {
        value = static_cast<uint8_t>(value + bus_.read8(src + i));
        if (!vram) {
            bus_.write8(dst + i, value);
        } else if (i & 1) {
            bus_.write16(dst + i - 1, static_cast<uint16_t>(pending | (value << 8)));
        } else {
            pending = value;
            if (i + 1 == size) bus_.write16(dst + i, pending);
        }
    }
}

// SWI 1Fh: frequência de uma nota MIDI para o driver de som.
// r0 = endereço de uma "WaveData" (frequência de base em +4), r1 = nota, r2 = ajuste fino (1/256 de nota).
// r0 = frequência / 2^((180 - nota - ajuste/256) / 12)
void Cpu::hleMidiKey2Freq() {
    const double base = static_cast<double>(bus_.read32(r_[0] + 4));
    const double exponent = (180.0 - static_cast<double>(r_[1]) - static_cast<double>(r_[2]) / 256.0) / 12.0;
    r_[0] = static_cast<uint32_t>(base / std::pow(2.0, exponent));
}

} // namespace gba
