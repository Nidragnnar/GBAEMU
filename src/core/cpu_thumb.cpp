// Estado Thumb do ARM7TDMI: instruções de 16 bits (GBATEK, "THUMB Instruction Set").
// Durante a execução, r15 vale o endereço da instrução atual + 4.

#include <bit>

#include "alu.hpp"
#include "cpu.hpp"

namespace gba {

using alu::addWithCarry;
using alu::subWithCarry;

int Cpu::executeThumb(uint16_t instr) {
    switch (instr >> 13) {
    case 0:
        return ((instr >> 11) & 3) == 3 ? thumbAddSub(instr) : thumbShiftImm(instr);
    case 1:
        return thumbImmediate(instr);
    case 2:
        if ((instr >> 10) == 0b010000) return thumbAlu(instr);
        if ((instr >> 10) == 0b010001) return thumbHiRegister(instr);
        if ((instr >> 11) == 0b01001) return thumbPcLoad(instr);
        return (instr & (1 << 9)) ? thumbLoadStoreSigned(instr) : thumbLoadStoreReg(instr);
    case 3:
        return thumbLoadStoreImm(instr);
    case 4:
        return (instr & (1 << 12)) ? thumbSpLoadStore(instr) : thumbLoadStoreHalf(instr);
    case 5:
        if (!(instr & (1 << 12))) return thumbLoadAddress(instr);
        if ((instr >> 8) == 0b10110000) return thumbAddSp(instr);
        if ((instr & 0x0600) == 0x0400) return thumbPushPop(instr);
        return thumbUndefined();
    case 6:
        if (!(instr & (1 << 12))) return thumbMultiple(instr);
        if (((instr >> 8) & 0xF) == 0xF) { // formato 17: SWI
            if (hleBios_) return hleSwi(instr & 0xFF);
            raiseException(0x08, CpuMode::Supervisor, r_[15] - 2);
            return 3;
        }
        if (((instr >> 8) & 0xF) == 0xE) return thumbUndefined();
        return thumbCondBranch(instr);
    default:
        if (instr & (1 << 12)) return thumbLongBranch(instr);
        if (instr & (1 << 11)) return thumbUndefined(); // BLX só existe no ARMv5
        return thumbBranch(instr);
    }
}

int Cpu::thumbUndefined() {
    raiseException(0x04, CpuMode::Undefined, r_[15] - 2);
    return 3;
}

// Formato 1: LSL/LSR/ASR Rd, Rs, #imm
int Cpu::thumbShiftImm(uint16_t instr) {
    const uint32_t rd = instr & 7, rs = (instr >> 3) & 7;
    const auto s = shift((instr >> 11) & 3, r_[rs], (instr >> 6) & 0x1F, true);
    r_[rd] = s.value;
    setNZ(s.value);
    setFlag(flag::C, s.carry);
    return 1;
}

// Formato 2: ADD/SUB Rd, Rs, Rn  ou  ADD/SUB Rd, Rs, #imm3
int Cpu::thumbAddSub(uint16_t instr) {
    const uint32_t rd = instr & 7, rs = (instr >> 3) & 7, field = (instr >> 6) & 7;
    const uint32_t operand = (instr & (1 << 10)) ? field : r_[field];
    bool carry = false, overflow = false;
    const uint32_t result = (instr & (1 << 9))
        ? subWithCarry(r_[rs], operand, true, carry, overflow)
        : addWithCarry(r_[rs], operand, false, carry, overflow);
    r_[rd] = result;
    setNZ(result);
    setFlag(flag::C, carry);
    setFlag(flag::V, overflow);
    return 1;
}

// Formato 3: MOV/CMP/ADD/SUB Rd, #imm8
int Cpu::thumbImmediate(uint16_t instr) {
    const uint32_t op = (instr >> 11) & 3, rd = (instr >> 8) & 7, imm = instr & 0xFF;
    if (op == 0) { // MOV só altera N e Z
        r_[rd] = imm;
        setNZ(imm);
        return 1;
    }
    bool carry = false, overflow = false;
    const uint32_t result = op == 2
        ? addWithCarry(r_[rd], imm, false, carry, overflow)
        : subWithCarry(r_[rd], imm, true, carry, overflow);
    if (op != 1) r_[rd] = result; // CMP não grava
    setNZ(result);
    setFlag(flag::C, carry);
    setFlag(flag::V, overflow);
    return 1;
}

// Formato 4: operações da ALU entre registradores baixos
int Cpu::thumbAlu(uint16_t instr) {
    const uint32_t op = (instr >> 6) & 0xF, rs = (instr >> 3) & 7, rd = instr & 7;
    const uint32_t a = r_[rd], b = r_[rs];
    bool carry = cpsr_ & flag::C, overflow = cpsr_ & flag::V;
    bool write = true, arithmetic = false;
    int cycles = 1;
    uint32_t result = 0;

    switch (op) {
    case 0x0: result = a & b; break;                                                    // AND
    case 0x1: result = a ^ b; break;                                                    // EOR
    case 0x2: case 0x3: case 0x4: case 0x7: {                                           // LSL LSR ASR ROR
        static constexpr uint32_t kType[] = {0, 0, 0, 1, 2, 0, 0, 3};
        const auto s = shift(kType[op], a, b & 0xFF, false);
        result = s.value;
        carry = s.carry;
        cycles = 2;
        break;
    }
    case 0x5: result = addWithCarry(a, b, cpsr_ & flag::C, carry, overflow); arithmetic = true; break; // ADC
    case 0x6: result = subWithCarry(a, b, cpsr_ & flag::C, carry, overflow); arithmetic = true; break; // SBC
    case 0x8: result = a & b; write = false; break;                                     // TST
    case 0x9: result = subWithCarry(0, b, true, carry, overflow); arithmetic = true; break; // NEG
    case 0xA: result = subWithCarry(a, b, true, carry, overflow); write = false; arithmetic = true; break; // CMP
    case 0xB: result = addWithCarry(a, b, false, carry, overflow); write = false; arithmetic = true; break; // CMN
    case 0xC: result = a | b; break;                                                    // ORR
    case 0xD: result = a * b; cycles = 1 + alu::multiplyCycles(a); break;               // MUL
    case 0xE: result = a & ~b; break;                                                   // BIC
    default:  result = ~b; break;                                                       // MVN
    }

    if (write) r_[rd] = result;
    setNZ(result);
    setFlag(flag::C, carry);
    if (arithmetic) setFlag(flag::V, overflow);
    return cycles;
}

// Formato 5: ADD/CMP/MOV com registradores altos (r8-r15) e BX
int Cpu::thumbHiRegister(uint16_t instr) {
    const uint32_t op = (instr >> 8) & 3;
    const uint32_t rs = ((instr >> 3) & 7) | ((instr >> 3) & 8); // bit 6 = H2
    const uint32_t rd = (instr & 7) | ((instr >> 4) & 8);        // bit 7 = H1

    switch (op) {
    case 0: { // ADD (não altera flags)
        const uint32_t result = r_[rd] + r_[rs];
        if (rd == 15) { jumpTo(result, true); return 3; }
        r_[rd] = result;
        return 1;
    }
    case 1: { // CMP
        bool carry = false, overflow = false;
        const uint32_t result = subWithCarry(r_[rd], r_[rs], true, carry, overflow);
        setNZ(result);
        setFlag(flag::C, carry);
        setFlag(flag::V, overflow);
        return 1;
    }
    case 2: // MOV (não altera flags)
        if (rd == 15) { jumpTo(r_[rs], true); return 3; }
        r_[rd] = r_[rs];
        return 1;
    default: { // BX: bit 0 do destino escolhe Thumb (1) ou ARM (0)
        const uint32_t target = r_[rs];
        jumpTo(target, (target & 1) != 0);
        return 3;
    }
    }
}

// Formato 6: LDR Rd, [PC, #imm8×4]  (o PC é alinhado a 4 antes da soma)
int Cpu::thumbPcLoad(uint16_t instr) {
    const uint32_t rd = (instr >> 8) & 7;
    r_[rd] = bus_.read32((r_[15] & ~2u) + ((instr & 0xFF) << 2));
    return 3;
}

// Formato 7: STR/STRB/LDR/LDRB Rd, [Rb, Ro]
int Cpu::thumbLoadStoreReg(uint16_t instr) {
    const uint32_t rd = instr & 7, rb = (instr >> 3) & 7, ro = (instr >> 6) & 7;
    const bool load = instr & (1 << 11), byte = instr & (1 << 10);
    const uint32_t addr = r_[rb] + r_[ro];
    if (load) {
        r_[rd] = byte ? bus_.read8(addr) : loadWord(addr);
        return 3;
    }
    if (byte) bus_.write8(addr, static_cast<uint8_t>(r_[rd]));
    else bus_.write32(addr, r_[rd]);
    return 2;
}

// Formato 8: STRH/LDRH/LDSB/LDSH Rd, [Rb, Ro]
int Cpu::thumbLoadStoreSigned(uint16_t instr) {
    const uint32_t rd = instr & 7, rb = (instr >> 3) & 7, ro = (instr >> 6) & 7;
    const uint32_t addr = r_[rb] + r_[ro];
    switch ((instr >> 10) & 3) { // bit 11 = H, bit 10 = S
    case 0: bus_.write16(addr, static_cast<uint16_t>(r_[rd])); return 2; // STRH
    case 1: r_[rd] = loadSignedByte(addr); return 3;                     // LDSB
    case 2: r_[rd] = loadHalf(addr); return 3;                           // LDRH
    default: r_[rd] = loadSignedHalf(addr); return 3;                    // LDSH
    }
}

// Formato 9: STR/LDR/STRB/LDRB Rd, [Rb, #imm5]  (×4 para palavras)
int Cpu::thumbLoadStoreImm(uint16_t instr) {
    const uint32_t rd = instr & 7, rb = (instr >> 3) & 7, imm = (instr >> 6) & 0x1F;
    const bool byte = instr & (1 << 12), load = instr & (1 << 11);
    const uint32_t addr = r_[rb] + (byte ? imm : imm << 2);
    if (load) {
        r_[rd] = byte ? bus_.read8(addr) : loadWord(addr);
        return 3;
    }
    if (byte) bus_.write8(addr, static_cast<uint8_t>(r_[rd]));
    else bus_.write32(addr, r_[rd]);
    return 2;
}

// Formato 10: STRH/LDRH Rd, [Rb, #imm5×2]
int Cpu::thumbLoadStoreHalf(uint16_t instr) {
    const uint32_t rd = instr & 7, rb = (instr >> 3) & 7;
    const uint32_t addr = r_[rb] + (((instr >> 6) & 0x1F) << 1);
    if (instr & (1 << 11)) {
        r_[rd] = loadHalf(addr);
        return 3;
    }
    bus_.write16(addr, static_cast<uint16_t>(r_[rd]));
    return 2;
}

// Formato 11: STR/LDR Rd, [SP, #imm8×4]
int Cpu::thumbSpLoadStore(uint16_t instr) {
    const uint32_t rd = (instr >> 8) & 7;
    const uint32_t addr = r_[13] + ((instr & 0xFF) << 2);
    if (instr & (1 << 11)) {
        r_[rd] = loadWord(addr);
        return 3;
    }
    bus_.write32(addr, r_[rd]);
    return 2;
}

// Formato 12: ADD Rd, PC/SP, #imm8×4
int Cpu::thumbLoadAddress(uint16_t instr) {
    const uint32_t rd = (instr >> 8) & 7;
    const uint32_t base = (instr & (1 << 11)) ? r_[13] : (r_[15] & ~2u);
    r_[rd] = base + ((instr & 0xFF) << 2);
    return 1;
}

// Formato 13: ADD SP, #±imm7×4
int Cpu::thumbAddSp(uint16_t instr) {
    const uint32_t offset = (instr & 0x7F) << 2;
    r_[13] = (instr & (1 << 7)) ? r_[13] - offset : r_[13] + offset;
    return 1;
}

// Formato 14: PUSH {lista, LR} / POP {lista, PC}
int Cpu::thumbPushPop(uint16_t instr) {
    const bool pop = instr & (1 << 11), extra = instr & (1 << 8);
    const uint32_t list = instr & 0xFF;

    if (list == 0 && !extra) { // lista vazia: transfere o PC e move SP em 0x40
        if (pop) {
            const uint32_t value = bus_.read32(r_[13]);
            r_[13] += 0x40;
            jumpTo(value, true);
            return 5;
        }
        r_[13] -= 0x40;
        bus_.write32(r_[13], r_[15] + 2);
        return 2;
    }

    const int count = std::popcount(list) + (extra ? 1 : 0);
    if (pop) {
        uint32_t addr = r_[13];
        for (uint32_t i = 0; i < 8; ++i) {
            if (!(list & (1u << i))) continue;
            r_[i] = bus_.read32(addr);
            addr += 4;
        }
        if (extra) {
            const uint32_t value = bus_.read32(addr);
            addr += 4;
            r_[13] = addr;
            jumpTo(value, true); // ARMv4: POP {pc} não troca para ARM
            return count + 4;
        }
        r_[13] = addr;
        return count + 2;
    }

    uint32_t addr = r_[13] - static_cast<uint32_t>(count) * 4;
    r_[13] = addr;
    for (uint32_t i = 0; i < 8; ++i) {
        if (!(list & (1u << i))) continue;
        bus_.write32(addr, r_[i]);
        addr += 4;
    }
    if (extra) bus_.write32(addr, r_[14]);
    return count + 1;
}

// Formato 15: STMIA/LDMIA Rb!, {lista}
int Cpu::thumbMultiple(uint16_t instr) {
    const bool load = instr & (1 << 11);
    const uint32_t rb = (instr >> 8) & 7;
    const uint32_t list = instr & 0xFF;
    const uint32_t base = r_[rb];

    if (list == 0) { // lista vazia: transfere o PC e move a base 0x40
        if (load) {
            const uint32_t value = bus_.read32(base);
            r_[rb] = base + 0x40;
            jumpTo(value, true);
            return 5;
        }
        bus_.write32(base, r_[15] + 2);
        r_[rb] = base + 0x40;
        return 2;
    }

    const int count = std::popcount(list);
    const uint32_t finalBase = base + static_cast<uint32_t>(count) * 4;
    uint32_t addr = base;

    if (load) {
        for (uint32_t i = 0; i < 8; ++i) {
            if (!(list & (1u << i))) continue;
            r_[i] = bus_.read32(addr);
            addr += 4;
        }
        if (!(list & (1u << rb))) r_[rb] = finalBase; // se a base foi carregada, o valor lido prevalece
        return count + 2;
    }

    bool first = true;
    for (uint32_t i = 0; i < 8; ++i) {
        if (!(list & (1u << i))) continue;
        // Base na lista: se for o primeiro registrador grava o valor antigo, senão o novo.
        bus_.write32(addr, (i == rb && !first) ? finalBase : r_[i]);
        addr += 4;
        first = false;
    }
    r_[rb] = finalBase;
    return count + 1;
}

// Formato 16: B<cond> com deslocamento de 8 bits com sinal (×2)
int Cpu::thumbCondBranch(uint16_t instr) {
    if (!conditionPassed((instr >> 8) & 0xF)) return 1;
    const auto offset = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(instr & 0xFF)) * 2);
    jumpTo(r_[15] + offset, true);
    return 3;
}

// Formato 18: B incondicional com deslocamento de 11 bits com sinal (×2)
int Cpu::thumbBranch(uint16_t instr) {
    const auto offset = static_cast<uint32_t>(static_cast<int32_t>(static_cast<uint32_t>(instr) << 21) >> 20);
    jumpTo(r_[15] + offset, true);
    return 3;
}

// Formato 19: BL em duas metades. A primeira guarda a parte alta do deslocamento em LR;
// a segunda soma a parte baixa, salta e deixa em LR o endereço de retorno com bit 0 = 1.
int Cpu::thumbLongBranch(uint16_t instr) {
    const uint32_t offset = instr & 0x7FF;
    if (!(instr & (1 << 11))) {
        r_[14] = r_[15] + static_cast<uint32_t>(static_cast<int32_t>(offset << 21) >> 9);
        return 1;
    }
    const uint32_t returnAddr = r_[15] - 2; // instrução seguinte à segunda metade
    const uint32_t target = r_[14] + (offset << 1);
    r_[14] = returnAddr | 1;
    jumpTo(target, true);
    return 3;
}

} // namespace gba
