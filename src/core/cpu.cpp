#include "cpu.hpp"

#include <bit>
#include <stdexcept>

#include "alu.hpp"

namespace gba {

using alu::addWithCarry;
using alu::multiplyCycles;
using alu::ror;
using alu::subWithCarry;

namespace {

int bankOf(uint32_t mode) {
    switch (mode & 0x1F) {
    case 0x11: return 1; // FIQ
    case 0x12: return 2; // IRQ
    case 0x13: return 3; // Supervisor
    case 0x17: return 4; // Abort
    case 0x1B: return 5; // Undefined
    default:   return 0; // User / System
    }
}

} // namespace

Cpu::Cpu(Bus& bus) : bus_(bus) { reset(true); }

void Cpu::reset(bool skipBios) {
    r_.fill(0);
    usrHi_.fill(0);
    fiqHi_.fill(0);
    bankSp_.fill(0);
    bankLr_.fill(0);
    spsr_.fill(0);

    if (skipBios) {
        // Estado deixado pela BIOS oficial antes de saltar para o cartucho.
        cpsr_ = static_cast<uint32_t>(CpuMode::System);
        r_[13] = 0x03007F00;
        bankSp_[2] = 0x03007FA0; // IRQ
        bankSp_[3] = 0x03007FE0; // Supervisor
        jumpTo(0x08000000);
    } else {
        cpsr_ = static_cast<uint32_t>(CpuMode::Supervisor) | flag::I | flag::F;
        jumpTo(0x00000000);
    }
    halted_ = false;
    intrWaitFlags_ = 0;
    waitActive_ = false;
}

void Cpu::jumpTo(uint32_t addr, bool thumb) {
    if (thumb) {
        cpsr_ |= flag::T;
        addr &= ~1u;
        pipe_[0] = bus_.fetch16(addr);
        pipe_[1] = bus_.fetch16(addr + 2);
        r_[15] = addr + 4;
    } else {
        cpsr_ &= ~flag::T;
        addr &= ~3u;
        pipe_[0] = bus_.fetch32(addr);
        pipe_[1] = bus_.fetch32(addr + 4);
        r_[15] = addr + 8;
    }
    flushed_ = true;
}

int Cpu::step() {
    // Enquanto dorme, a CPU não executa nada; devolve alguns ciclos para o tempo andar.
    constexpr int kIdleCycles = 16;
    if (halted_) {
        if (!(bus_.ioRead16(0x200) & bus_.ioRead16(0x202) & 0x3FFF)) return kIdleCycles;
        halted_ = false; // acorda com qualquer interrupção habilitada em IE, mesmo com IME desligado
    }
    // IntrWait da BIOS: o tratador do jogo marca em [03007FF8] as interrupções que recebeu.
    // Quando o tratador termina e a execução volta ao ponto da SWI, a espera recomeça.
    if (intrWaitFlags_ && !waitActive_ && pc() == waitResumePc_ && mode() != CpuMode::Irq) waitActive_ = true;
    if (waitActive_) {
        const uint16_t check = bus_.read16(0x03007FF8);
        if (check & intrWaitFlags_) {
            bus_.write16(0x03007FF8, static_cast<uint16_t>(check & ~intrWaitFlags_));
            intrWaitFlags_ = 0;
            waitActive_ = false;
            // Na BIOS real a IRQ é atendida dentro da SWI, que depois ainda executa o próprio
            // retorno: a última instrução buscada pela BIOS volta a ser a do fim da SWI.
            bus_.setBiosOpcode(0xE3A02004);
        } else if (bus_.irqPending()) {
            waitActive_ = false; // suspende a espera enquanto o tratador roda
            waitResumePc_ = pc();
            raiseException(0x18, CpuMode::Irq, pc() + 4);
            return 3;
        } else {
            return kIdleCycles;
        }
    }

    // IRQ: atendida entre instruções se o CPSR permitir. Ao voltar com "subs pc, lr, #4",
    // a CPU retoma exatamente na instrução que seria executada agora.
    if (!(cpsr_ & flag::I) && bus_.irqPending()) {
        raiseException(0x18, CpuMode::Irq, pc() + 4);
        return 3;
    }

    trace_[traceNext_] = {pc(), thumb() ? (pipe_[0] & 0xFFFF) : pipe_[0], thumb()};
    traceNext_ = (traceNext_ + 1) % kTraceSize;

    // Como no ARM7, a busca da instrução em PC+8 (ARM) ou PC+4 (Thumb) acontece durante a
    // execução da atual. Se a instrução desviar, o que foi buscado é descartado.
    bus_.setPcInBios(pc() < 0x4000); // só código da BIOS enxerga o conteúdo da BIOS

    if (thumb()) {
        const auto instr = static_cast<uint16_t>(pipe_[0]);
        pipe_[0] = pipe_[1];
        pipe_[1] = bus_.fetch16(r_[15]);
        flushed_ = false;
        const int cycles = executeThumb(instr);
        if (!flushed_) r_[15] += 2;
        return cycles;
    }

    const uint32_t instr = pipe_[0];
    pipe_[0] = pipe_[1];
    pipe_[1] = bus_.fetch32(r_[15]);
    flushed_ = false;

    int cycles = 1;
    if (conditionPassed(instr >> 28)) cycles = executeArm(instr);
    if (!flushed_) r_[15] += 4;
    return cycles;
}

std::array<Cpu::TraceEntry, Cpu::kTraceSize> Cpu::trace() const {
    std::array<TraceEntry, kTraceSize> ordered{};
    for (size_t i = 0; i < kTraceSize; ++i) ordered[i] = trace_[(traceNext_ + i) % kTraceSize];
    return ordered;
}

void Cpu::setReg(int i, uint32_t value) {
    if (i == 15) jumpTo(value, thumb());
    else r_[i] = value;
}

void Cpu::writeReg(uint32_t i, uint32_t value) {
    if (i == 15) jumpTo(value, thumb());
    else r_[i] = value;
}

// ---------------------------------------------------------------- modos e PSRs

void Cpu::switchMode(uint32_t newMode) {
    const int oldBank = bankOf(cpsr_);
    const int newBank = bankOf(newMode);
    if (oldBank != newBank) {
        bankSp_[oldBank] = r_[13];
        bankLr_[oldBank] = r_[14];
        r_[13] = bankSp_[newBank];
        r_[14] = bankLr_[newBank];
        if ((oldBank == 1) != (newBank == 1)) { // entrando ou saindo do FIQ troca r8-r12
            auto& save = oldBank == 1 ? fiqHi_ : usrHi_;
            auto& load = newBank == 1 ? fiqHi_ : usrHi_;
            for (int i = 0; i < 5; ++i) {
                save[i] = r_[8 + i];
                r_[8 + i] = load[i];
            }
        }
    }
    cpsr_ = (cpsr_ & ~0x1Fu) | (newMode & 0x1F);
}

void Cpu::setCpsr(uint32_t value) {
    switchMode(value & 0x1F);
    cpsr_ = value;
}

uint32_t Cpu::spsr() const {
    const int bank = bankOf(cpsr_);
    return bank == 0 ? cpsr_ : spsr_[bank]; // User/System não têm SPSR
}

void Cpu::setSpsr(uint32_t value) {
    const int bank = bankOf(cpsr_);
    if (bank != 0) spsr_[bank] = value;
}

void Cpu::setNZ(uint32_t result) {
    setFlag(flag::N, (result >> 31) != 0);
    setFlag(flag::Z, result == 0);
}

uint32_t Cpu::readUserReg(uint32_t i) const {
    if (i < 8 || i == 15) return r_[i];
    const int bank = bankOf(cpsr_);
    if (i < 13) return bank == 1 ? usrHi_[i - 8] : r_[i];
    if (bank == 0) return r_[i];
    return i == 13 ? bankSp_[0] : bankLr_[0];
}

void Cpu::writeUserReg(uint32_t i, uint32_t value) {
    if (i < 8 || i == 15) { r_[i] = value; return; }
    const int bank = bankOf(cpsr_);
    if (i < 13) { (bank == 1 ? usrHi_[i - 8] : r_[i]) = value; return; }
    if (bank == 0) { r_[i] = value; return; }
    (i == 13 ? bankSp_[0] : bankLr_[0]) = value;
}

void Cpu::raiseException(uint32_t vector, CpuMode newMode, uint32_t returnAddr) {
    const uint32_t oldCpsr = cpsr_;
    switchMode(static_cast<uint32_t>(newMode));
    spsr_[bankOf(cpsr_)] = oldCpsr;
    r_[14] = returnAddr;
    cpsr_ |= flag::I;
    jumpTo(vector, false); // exceções sempre entram em estado ARM
}

// ---------------------------------------------------------------- utilitários

// Leituras com as peculiaridades do ARM7 para endereços desalinhados.
uint32_t Cpu::loadWord(uint32_t addr) const { return ror(bus_.read32(addr), (addr & 3) * 8); }
uint32_t Cpu::loadHalf(uint32_t addr) const { return ror(bus_.read16(addr), (addr & 1) * 8); }
uint32_t Cpu::loadSignedByte(uint32_t addr) const { return alu::signExtend8(bus_.read8(addr)); }
uint32_t Cpu::loadSignedHalf(uint32_t addr) const {
    // LDRSH em endereço ímpar vira leitura de byte com sinal.
    return (addr & 1) ? loadSignedByte(addr) : alu::signExtend16(bus_.read16(addr));
}

bool Cpu::conditionPassed(uint32_t cond) const {
    const bool n = cpsr_ & flag::N, z = cpsr_ & flag::Z;
    const bool c = cpsr_ & flag::C, v = cpsr_ & flag::V;
    switch (cond) {
    case 0x0: return z;               // EQ
    case 0x1: return !z;              // NE
    case 0x2: return c;               // CS/HS
    case 0x3: return !c;              // CC/LO
    case 0x4: return n;               // MI
    case 0x5: return !n;              // PL
    case 0x6: return v;               // VS
    case 0x7: return !v;              // VC
    case 0x8: return c && !z;         // HI
    case 0x9: return !c || z;         // LS
    case 0xA: return n == v;          // GE
    case 0xB: return n != v;          // LT
    case 0xC: return !z && n == v;    // GT
    case 0xD: return z || n != v;     // LE
    case 0xE: return true;            // AL
    default:  return false;           // NV: nunca executa no ARMv4
    }
}

Cpu::ShiftResult Cpu::shift(uint32_t type, uint32_t value, uint32_t amount, bool immediate) const {
    const bool c = cpsr_ & flag::C;
    switch (type) {
    case 0: // LSL
        if (amount == 0) return {value, c};
        if (amount < 32) return {value << amount, ((value >> (32 - amount)) & 1) != 0};
        if (amount == 32) return {0, (value & 1) != 0};
        return {0, false};
    case 1: // LSR (imediato 0 significa 32)
        if (immediate && amount == 0) amount = 32;
        if (amount == 0) return {value, c};
        if (amount < 32) return {value >> amount, ((value >> (amount - 1)) & 1) != 0};
        if (amount == 32) return {0, (value >> 31) != 0};
        return {0, false};
    case 2: // ASR (imediato 0 significa 32)
        if (immediate && amount == 0) amount = 32;
        if (amount == 0) return {value, c};
        if (amount < 32) {
            return {static_cast<uint32_t>(static_cast<int32_t>(value) >> amount),
                    ((value >> (amount - 1)) & 1) != 0};
        }
        return {(value >> 31) ? 0xFFFFFFFFu : 0u, (value >> 31) != 0};
    default: // ROR (imediato 0 significa RRX)
        if (immediate && amount == 0) return {(c ? 1u << 31 : 0u) | (value >> 1), (value & 1) != 0};
        if (amount == 0) return {value, c};
        amount &= 31;
        if (amount == 0) return {value, (value >> 31) != 0};
        return {ror(value, amount), ((value >> (amount - 1)) & 1) != 0};
    }
}

// ---------------------------------------------------------------- decodificação ARM

int Cpu::executeArm(uint32_t instr) {
    // A ordem importa: vários formatos compartilham bits com "data processing".
    if ((instr & 0x0FFFFFF0) == 0x012FFF10) return armBranchExchange(instr);
    if ((instr & 0x0FB00FF0) == 0x01000090) return armSwap(instr);
    if ((instr & 0x0FC000F0) == 0x00000090) return armMultiply(instr);
    if ((instr & 0x0F8000F0) == 0x00800090) return armMultiplyLong(instr);
    if ((instr & 0x0E000090) == 0x00000090 && (instr & 0x60)) return armHalfwordTransfer(instr);
    if ((instr & 0x0FBF0FFF) == 0x010F0000) return armMrs(instr);
    if ((instr & 0x0FB0FFF0) == 0x0120F000) return armMsr(instr);
    if ((instr & 0x0FB0F000) == 0x0320F000) return armMsr(instr);
    if ((instr & 0x0C000000) == 0x00000000) return armDataProcessing(instr);
    if ((instr & 0x0E000010) == 0x06000010) {
        raiseException(0x04, CpuMode::Undefined, r_[15] - 4);
        return 3;
    }
    if ((instr & 0x0C000000) == 0x04000000) return armSingleTransfer(instr);
    if ((instr & 0x0E000000) == 0x08000000) return armBlockTransfer(instr);
    if ((instr & 0x0E000000) == 0x0A000000) return armBranch(instr);
    if ((instr & 0x0F000000) == 0x0F000000) { // SWI
        if (hleBios_) return hleSwi((instr >> 16) & 0xFF); // no modo ARM o número fica nos bits 16-23
        raiseException(0x08, CpuMode::Supervisor, r_[15] - 4);
        return 3;
    }
    // Coprocessador: o GBA não tem nenhum, então é instrução indefinida.
    raiseException(0x04, CpuMode::Undefined, r_[15] - 4);
    return 3;
}

int Cpu::armBranchExchange(uint32_t instr) {
    const uint32_t target = r_[instr & 0xF];
    jumpTo(target, (target & 1) != 0); // bit 0 escolhe Thumb
    return 3;
}

int Cpu::armBranch(uint32_t instr) {
    const int32_t offset = static_cast<int32_t>(instr << 8) >> 6; // 24 bits com sinal, ×4
    if (instr & (1 << 24)) r_[14] = r_[15] - 4;                    // BL guarda o retorno
    jumpTo(r_[15] + static_cast<uint32_t>(offset));
    return 3;
}

int Cpu::armDataProcessing(uint32_t instr) {
    const bool immediate = instr & (1 << 25);
    const bool setFlags = instr & (1 << 20);
    const uint32_t opcode = (instr >> 21) & 0xF;
    const uint32_t rn = (instr >> 16) & 0xF;
    const uint32_t rd = (instr >> 12) & 0xF;

    int cycles = 1;
    uint32_t rnValue = r_[rn];
    uint32_t op2;
    bool carry = cpsr_ & flag::C;

    if (immediate) {
        const uint32_t rotate = ((instr >> 8) & 0xF) * 2;
        op2 = ror(instr & 0xFF, rotate);
        if (rotate) carry = (op2 >> 31) != 0;
    } else {
        const uint32_t rm = instr & 0xF;
        const uint32_t type = (instr >> 5) & 3;
        if (instr & (1 << 4)) {
            // Shift por registrador: 1 ciclo extra, e o PC é lido 4 bytes adiante.
            const uint32_t amount = r_[(instr >> 8) & 0xF] & 0xFF;
            const uint32_t rmValue = r_[rm] + (rm == 15 ? 4 : 0);
            if (rn == 15) rnValue += 4;
            const auto s = shift(type, rmValue, amount, false);
            op2 = s.value;
            carry = s.carry;
            ++cycles;
        } else {
            const auto s = shift(type, r_[rm], (instr >> 7) & 0x1F, true);
            op2 = s.value;
            carry = s.carry;
        }
    }

    bool overflow = cpsr_ & flag::V;
    const bool carryIn = cpsr_ & flag::C;
    bool writeResult = true;
    uint32_t result = 0;

    switch (opcode) {
    case 0x0: result = rnValue & op2; break;                                            // AND
    case 0x1: result = rnValue ^ op2; break;                                            // EOR
    case 0x2: result = subWithCarry(rnValue, op2, true, carry, overflow); break;        // SUB
    case 0x3: result = subWithCarry(op2, rnValue, true, carry, overflow); break;        // RSB
    case 0x4: result = addWithCarry(rnValue, op2, false, carry, overflow); break;       // ADD
    case 0x5: result = addWithCarry(rnValue, op2, carryIn, carry, overflow); break;     // ADC
    case 0x6: result = subWithCarry(rnValue, op2, carryIn, carry, overflow); break;     // SBC
    case 0x7: result = subWithCarry(op2, rnValue, carryIn, carry, overflow); break;     // RSC
    case 0x8: result = rnValue & op2; writeResult = false; break;                       // TST
    case 0x9: result = rnValue ^ op2; writeResult = false; break;                       // TEQ
    case 0xA: result = subWithCarry(rnValue, op2, true, carry, overflow); writeResult = false; break; // CMP
    case 0xB: result = addWithCarry(rnValue, op2, false, carry, overflow); writeResult = false; break; // CMN
    case 0xC: result = rnValue | op2; break;                                            // ORR
    case 0xD: result = op2; break;                                                      // MOV
    case 0xE: result = rnValue & ~op2; break;                                           // BIC
    default:  result = ~op2; break;                                                     // MVN
    }

    if (writeResult && rd == 15) {
        // "MOVS pc, lr" e afins: retorno de exceção, restaura o CPSR a partir do SPSR.
        if (setFlags) setCpsr(spsr());
        jumpTo(result, thumb());
        return cycles + 2;
    }

    if (setFlags) {
        setNZ(result);
        setFlag(flag::C, carry);
        setFlag(flag::V, overflow); // nas lógicas, overflow manteve o valor anterior
    }
    if (writeResult) r_[rd] = result;
    return cycles;
}

int Cpu::armMrs(uint32_t instr) {
    const bool useSpsr = instr & (1 << 22);
    r_[(instr >> 12) & 0xF] = useSpsr ? spsr() : cpsr_;
    return 1;
}

int Cpu::armMsr(uint32_t instr) {
    const bool useSpsr = instr & (1 << 22);
    const uint32_t value = (instr & (1 << 25))
        ? ror(instr & 0xFF, ((instr >> 8) & 0xF) * 2)
        : r_[instr & 0xF];

    uint32_t mask = 0;
    if (instr & (1 << 19)) mask |= 0xFF000000; // campo f (flags)
    if (instr & (1 << 16)) mask |= 0x000000FF; // campo c (controle)

    if (useSpsr) {
        setSpsr((spsr() & ~mask) | (value & mask));
    } else {
        if (mode() == CpuMode::User) mask &= 0xFF000000; // usuário só altera flags
        mask &= ~flag::T;                                 // MSR não troca ARM/Thumb
        setCpsr((cpsr_ & ~mask) | (value & mask));
    }
    return 1;
}

int Cpu::armMultiply(uint32_t instr) {
    const uint32_t rd = (instr >> 16) & 0xF, rn = (instr >> 12) & 0xF;
    const uint32_t rs = (instr >> 8) & 0xF, rm = instr & 0xF;
    const bool accumulate = instr & (1 << 21);

    uint32_t result = r_[rm] * r_[rs];
    if (accumulate) result += r_[rn];
    r_[rd] = result;
    if (instr & (1 << 20)) setNZ(result); // C fica sem significado no ARMv4; mantemos
    return 1 + multiplyCycles(r_[rs]) + (accumulate ? 1 : 0);
}

int Cpu::armMultiplyLong(uint32_t instr) {
    const uint32_t rdHi = (instr >> 16) & 0xF, rdLo = (instr >> 12) & 0xF;
    const uint32_t rs = (instr >> 8) & 0xF, rm = instr & 0xF;
    const bool isSigned = instr & (1 << 22);
    const bool accumulate = instr & (1 << 21);

    uint64_t result = isSigned
        ? static_cast<uint64_t>(int64_t{static_cast<int32_t>(r_[rm])} * static_cast<int32_t>(r_[rs]))
        : uint64_t{r_[rm]} * r_[rs];
    if (accumulate) result += (uint64_t{r_[rdHi]} << 32) | r_[rdLo];

    r_[rdLo] = static_cast<uint32_t>(result);
    r_[rdHi] = static_cast<uint32_t>(result >> 32);
    if (instr & (1 << 20)) {
        setFlag(flag::N, (result >> 63) != 0);
        setFlag(flag::Z, result == 0);
    }
    return 2 + multiplyCycles(r_[rs]) + (accumulate ? 1 : 0);
}

int Cpu::armSwap(uint32_t instr) {
    const uint32_t rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF, rm = instr & 0xF;
    const uint32_t addr = r_[rn];
    const uint32_t source = r_[rm];
    if (instr & (1 << 22)) {
        const uint8_t old = bus_.read8(addr);
        bus_.write8(addr, static_cast<uint8_t>(source));
        r_[rd] = old;
    } else {
        const uint32_t old = loadWord(addr);
        bus_.write32(addr, source);
        r_[rd] = old;
    }
    return 4;
}

int Cpu::armHalfwordTransfer(uint32_t instr) {
    const bool pre = instr & (1 << 24), up = instr & (1 << 23);
    const bool immediate = instr & (1 << 22), wb = instr & (1 << 21), load = instr & (1 << 20);
    const uint32_t rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
    const uint32_t sh = (instr >> 5) & 3;

    const uint32_t offset = immediate ? (((instr >> 4) & 0xF0) | (instr & 0xF)) : r_[instr & 0xF];
    const uint32_t base = r_[rn];
    const uint32_t offsetAddr = up ? base + offset : base - offset;
    const uint32_t addr = pre ? offsetAddr : base;
    const bool writeback = (!pre || wb) && rn != 15;

    if (!load) { // STRH
        bus_.write16(addr, static_cast<uint16_t>(r_[rd] + (rd == 15 ? 4 : 0)));
        if (writeback) r_[rn] = offsetAddr;
        return 2;
    }

    const uint32_t value = sh == 1 ? loadHalf(addr) : sh == 2 ? loadSignedByte(addr) : loadSignedHalf(addr);
    if (writeback) r_[rn] = offsetAddr; // antes da carga: se rn == rd, a carga prevalece
    writeReg(rd, value);
    return rd == 15 ? 5 : 3;
}

int Cpu::armSingleTransfer(uint32_t instr) {
    const bool registerOffset = instr & (1 << 25);
    const bool pre = instr & (1 << 24), up = instr & (1 << 23);
    const bool byte = instr & (1 << 22), wb = instr & (1 << 21), load = instr & (1 << 20);
    const uint32_t rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;

    uint32_t offset = instr & 0xFFF;
    if (registerOffset) {
        offset = shift((instr >> 5) & 3, r_[instr & 0xF], (instr >> 7) & 0x1F, true).value;
    }
    const uint32_t base = r_[rn];
    const uint32_t offsetAddr = up ? base + offset : base - offset;
    const uint32_t addr = pre ? offsetAddr : base;
    const bool writeback = (!pre || wb) && rn != 15;

    if (!load) {
        const uint32_t value = r_[rd] + (rd == 15 ? 4 : 0); // STR do PC grava endereço + 12
        if (byte) bus_.write8(addr, static_cast<uint8_t>(value));
        else bus_.write32(addr, value);
        if (writeback) r_[rn] = offsetAddr;
        return 2;
    }

    const uint32_t value = byte ? bus_.read8(addr) : loadWord(addr);
    if (writeback) r_[rn] = offsetAddr;
    writeReg(rd, value);
    return rd == 15 ? 5 : 3;
}

int Cpu::armBlockTransfer(uint32_t instr) {
    const bool pre = instr & (1 << 24), up = instr & (1 << 23);
    const bool psrOrUser = instr & (1 << 22), wb = instr & (1 << 21), load = instr & (1 << 20);
    const uint32_t rn = (instr >> 16) & 0xF;
    uint32_t list = instr & 0xFFFF;

    int count = std::popcount(list);
    if (list == 0) { // ARMv4: lista vazia transfere só o PC e move a base 0x40
        list = 1u << 15;
        count = 16;
    }
    const uint32_t size = static_cast<uint32_t>(count) * 4;
    const uint32_t base = r_[rn];
    const uint32_t finalBase = up ? base + size : base - size;
    // Os registradores sempre vão em ordem crescente de endereço.
    uint32_t addr = up ? (pre ? base + 4 : base) : (pre ? base - size : base - size + 4);

    const bool loadsPc = load && (list & (1u << 15));
    const bool userBank = psrOrUser && !loadsPc;

    if (load) {
        if (wb) r_[rn] = finalBase; // se rn estiver na lista, o valor carregado prevalece
        for (uint32_t i = 0; i < 16; ++i) {
            if (!(list & (1u << i))) continue;
            const uint32_t value = bus_.read32(addr);
            addr += 4;
            if (i == 15) {
                if (psrOrUser) setCpsr(spsr()); // LDM com ^ e PC: retorno de exceção
                jumpTo(value, thumb());
            } else if (userBank) {
                writeUserReg(i, value);
            } else {
                r_[i] = value;
            }
        }
        return count + 2 + (loadsPc ? 2 : 0);
    }

    bool first = true;
    for (uint32_t i = 0; i < 16; ++i) {
        if (!(list & (1u << i))) continue;
        uint32_t value;
        if (i == 15) value = r_[15] + 4;
        else value = userBank ? readUserReg(i) : r_[i];
        // Base na lista: se for o primeiro registrador grava o valor antigo, senão o novo.
        if (i == rn && !first && wb) value = finalBase;
        bus_.write32(addr, value);
        addr += 4;
        first = false;
    }
    if (wb) r_[rn] = finalBase;
    return count + 1;
}

} // namespace gba

namespace gba {

void Cpu::serialize(StateArchive& ar) {
    ar.value(r_);
    ar.value(cpsr_);
    ar.value(usrHi_);
    ar.value(fiqHi_);
    ar.value(bankSp_);
    ar.value(bankLr_);
    ar.value(spsr_);
    ar.value(pipe_);
    ar.value(flushed_);
    ar.value(hleBios_);
    ar.value(halted_);
    ar.value(intrWaitFlags_);
    ar.value(waitActive_);
    ar.value(waitResumePc_);
}

} // namespace gba
