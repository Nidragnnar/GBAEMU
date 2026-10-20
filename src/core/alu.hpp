#pragma once
// Operações aritméticas compartilhadas pelos estados ARM e Thumb (uso interno do núcleo).

#include <cstdint>

namespace gba::alu {

inline uint32_t ror(uint32_t v, uint32_t n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

inline uint32_t addWithCarry(uint32_t a, uint32_t b, bool carryIn, bool& carry, bool& overflow) {
    const uint64_t wide = uint64_t{a} + b + (carryIn ? 1 : 0);
    const auto result = static_cast<uint32_t>(wide);
    carry = (wide >> 32) != 0;
    overflow = ((~(a ^ b) & (a ^ result)) >> 31) != 0;
    return result;
}

// a - b - (carryIn ? 0 : 1). No ARM, carry em subtração significa "não houve empréstimo".
inline uint32_t subWithCarry(uint32_t a, uint32_t b, bool carryIn, bool& carry, bool& overflow) {
    const uint64_t borrow = carryIn ? 0 : 1;
    const auto result = static_cast<uint32_t>(uint64_t{a} - b - borrow);
    carry = uint64_t{a} >= uint64_t{b} + borrow;
    overflow = (((a ^ b) & (a ^ result)) >> 31) != 0;
    return result;
}

// Ciclos internos da multiplicação: dependem de quantos bytes altos do multiplicador são 0 ou todos 1.
// Ciclos internos do multiplicador (8 bits por ciclo, parando cedo). Sem sinal (UMULL/UMLAL), só para
// quando os bits restantes são zero; com sinal (MUL/MLA/SMULL/SMLAL), também quando são todos 1.
inline int multiplyCyclesUnsigned(uint32_t rs) {
    if ((rs >> 8) == 0) return 1;
    if ((rs >> 16) == 0) return 2;
    if ((rs >> 24) == 0) return 3;
    return 4;
}
inline int multiplyCycles(uint32_t rs) {
    if ((rs >> 8) == 0 || (rs >> 8) == 0x00FFFFFF) return 1;
    if ((rs >> 16) == 0 || (rs >> 16) == 0xFFFF) return 2;
    if ((rs >> 24) == 0 || (rs >> 24) == 0xFF) return 3;
    return 4;
}

inline uint32_t signExtend8(uint32_t v) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v))); }
inline uint32_t signExtend16(uint32_t v) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))); }

} // namespace gba::alu
