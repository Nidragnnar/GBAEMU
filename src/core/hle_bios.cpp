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
        const double angle = std::atan(static_cast<int16_t>(r_[0]) / 16384.0);
        r_[0] = static_cast<uint32_t>(static_cast<int32_t>(std::lround(angle / (3.14159265358979323846 / 2) * 0x4000)));
        return 3;
    }
    case 0x0A: {                                                        // ArcTan2(x, y): 0..0xFFFF = uma volta
        const double angle = std::atan2(static_cast<int16_t>(r_[1]), static_cast<int16_t>(r_[0]));
        r_[0] = static_cast<uint16_t>(std::lround(angle / (2 * 3.14159265358979323846) * 0x10000) & 0xFFFF);
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
    if (denominator == 0) fail("Div: divisão por zero (a BIOS real trava aqui)", 0x06, pc());
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
        src &= ~3u; dst &= ~3u;
        const uint32_t fillValue = bus_.read32(src);
        for (uint32_t i = 0; i < count; ++i, dst += 4) {
            bus_.write32(dst, fill ? fillValue : bus_.read32(src));
            if (!fill) src += 4;
        }
    } else {
        src &= ~1u; dst &= ~1u;
        const uint16_t fillValue = bus_.read16(src);
        for (uint32_t i = 0; i < count; ++i, dst += 2) {
            bus_.write16(dst, fill ? fillValue : bus_.read16(src));
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
    uint32_t src = r_[0] & ~3u, dst = r_[1] & ~3u;

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

} // namespace gba
