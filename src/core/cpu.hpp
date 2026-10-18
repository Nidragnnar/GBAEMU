#pragma once
// CPU ARM7TDMI (ARMv4T): estados ARM (32 bits, cpu.cpp) e Thumb (16 bits, cpu_thumb.cpp).
// Referência: GBATEK, seções "ARM CPU Reference".

#include <array>
#include <cstdint>

#include "bus.hpp"
#include "state.hpp"

namespace gba {

enum class CpuMode : uint32_t {
    User = 0x10, Fiq = 0x11, Irq = 0x12, Supervisor = 0x13,
    Abort = 0x17, Undefined = 0x1B, System = 0x1F,
};

namespace flag {
constexpr uint32_t N = 1u << 31; // negativo
constexpr uint32_t Z = 1u << 30; // zero
constexpr uint32_t C = 1u << 29; // carry
constexpr uint32_t V = 1u << 28; // overflow
constexpr uint32_t I = 1u << 7;  // IRQ desabilitada
constexpr uint32_t F = 1u << 6;  // FIQ desabilitada
constexpr uint32_t T = 1u << 5;  // estado Thumb
} // namespace flag

class Cpu {
public:
    explicit Cpu(Bus& bus);

    // skipBios = true deixa a CPU no estado em que a BIOS entrega o controle ao jogo.
    void reset(bool skipBios);
    void jumpTo(uint32_t addr, bool thumb = false);

    // Executa uma instrução (ou entra numa IRQ pendente) e devolve os ciclos aproximados.
    int step();

    // r15 segue a semântica do pipeline: endereço da instrução atual + 8 (ARM) ou + 4 (Thumb).
    uint32_t reg(int i) const { return r_[i]; }
    void setReg(int i, uint32_t value);
    uint32_t cpsr() const { return cpsr_; }
    uint32_t spsr() const;
    CpuMode mode() const { return static_cast<CpuMode>(cpsr_ & 0x1F); }
    bool thumb() const { return (cpsr_ & flag::T) != 0; }
    uint32_t pc() const { return r_[15] - (thumb() ? 4 : 8); } // próxima instrução a executar

    // Rastro das últimas instruções executadas (diagnóstico), da mais antiga para a mais recente.
    struct TraceEntry { uint32_t pc = 0; uint32_t instr = 0; bool thumb = false; };
    static constexpr size_t kTraceSize = 64;
    std::array<TraceEntry, kTraceSize> trace() const;
    uint32_t lastPc() const { return trace_[(traceNext_ + kTraceSize - 1) % kTraceSize].pc; }

    // Para a CPU até chegar uma interrupção habilitada em IE (SWI 02h ou escrita em HALTCNT).
    void halt() { halted_ = true; }
    bool sleeping() const { return halted_ || waitActive_; }

    // Sem BIOS real, as chamadas SWI são emuladas em C++ (HLE) em vez de saltar para 0x08.
    void setHleBios(bool enabled) { hleBios_ = enabled; }
    void serialize(StateArchive& ar);

private:
    struct ShiftResult { uint32_t value; bool carry; };

    bool conditionPassed(uint32_t cond) const;
    ShiftResult shift(uint32_t type, uint32_t value, uint32_t amount, bool immediate) const;

    void switchMode(uint32_t newMode);
    void setCpsr(uint32_t value);
    void setSpsr(uint32_t value);
    void setNZ(uint32_t result);
    void setFlag(uint32_t mask, bool on) { cpsr_ = on ? (cpsr_ | mask) : (cpsr_ & ~mask); }
    void writeReg(uint32_t i, uint32_t value); // escrita em r15 recarrega o pipeline
    uint32_t readUserReg(uint32_t i) const;   // banco de usuário (LDM/STM com bit S)
    void writeUserReg(uint32_t i, uint32_t value);
    void raiseException(uint32_t vector, CpuMode newMode, uint32_t returnAddr);
    // BIOS em alto nível (hle_bios.cpp)
    int hleSwi(uint32_t number);
    void hleRegisterRamReset(uint32_t flags);
    void hleCpuSet();
    void hleCpuFastSet();
    void hleDiv(int32_t numerator, int32_t denominator);
    void hleDecompress(bool rle, bool vram);
    void hleIntrWait(bool discardOld, uint16_t flags);
    void hleSoftReset();
    void hleBgAffineSet();
    void hleObjAffineSet();

    uint32_t loadWord(uint32_t addr) const;
    uint32_t loadHalf(uint32_t addr) const;
    uint32_t loadSignedByte(uint32_t addr) const;
    uint32_t loadSignedHalf(uint32_t addr) const;

    int executeArm(uint32_t instr);
    int armBranchExchange(uint32_t instr);
    int armSwap(uint32_t instr);
    int armMultiply(uint32_t instr);
    int armMultiplyLong(uint32_t instr);
    int armHalfwordTransfer(uint32_t instr);
    int armMrs(uint32_t instr);
    int armMsr(uint32_t instr);
    int armDataProcessing(uint32_t instr);
    int armSingleTransfer(uint32_t instr);
    int armBlockTransfer(uint32_t instr);
    int armBranch(uint32_t instr);

    int executeThumb(uint16_t instr);
    int thumbShiftImm(uint16_t instr);       // formato 1
    int thumbAddSub(uint16_t instr);         // formato 2
    int thumbImmediate(uint16_t instr);      // formato 3
    int thumbAlu(uint16_t instr);            // formato 4
    int thumbHiRegister(uint16_t instr);     // formato 5
    int thumbPcLoad(uint16_t instr);         // formato 6
    int thumbLoadStoreReg(uint16_t instr);   // formato 7
    int thumbLoadStoreSigned(uint16_t instr);// formato 8
    int thumbLoadStoreImm(uint16_t instr);   // formato 9
    int thumbLoadStoreHalf(uint16_t instr);  // formato 10
    int thumbSpLoadStore(uint16_t instr);    // formato 11
    int thumbLoadAddress(uint16_t instr);    // formato 12
    int thumbAddSp(uint16_t instr);          // formato 13
    int thumbPushPop(uint16_t instr);        // formato 14
    int thumbMultiple(uint16_t instr);       // formato 15
    int thumbCondBranch(uint16_t instr);     // formato 16
    int thumbBranch(uint16_t instr);         // formato 18
    int thumbLongBranch(uint16_t instr);     // formato 19
    int thumbUndefined();

    Bus& bus_;

    std::array<uint32_t, 16> r_{};      // registradores visíveis no modo atual
    uint32_t cpsr_ = 0;
    std::array<uint32_t, 5> usrHi_{};   // r8-r12 de todos os modos exceto FIQ
    std::array<uint32_t, 5> fiqHi_{};   // r8-r12 do FIQ
    std::array<uint32_t, 6> bankSp_{};  // r13 por banco: usr/sys, fiq, irq, svc, abt, und
    std::array<uint32_t, 6> bankLr_{};  // r14 por banco
    std::array<uint32_t, 6> spsr_{};    // SPSR por banco (índice 0 não existe no hardware)

    std::array<uint32_t, 2> pipe_{};    // instruções já buscadas
    bool flushed_ = false;
    bool hleBios_ = false;
    bool halted_ = false;
    uint16_t intrWaitFlags_ = 0; // IntrWait em andamento: espera estes bits em [03007FF8]
    bool waitActive_ = false;    // dormindo agora (falso enquanto o tratador de IRQ roda no meio da espera)
    uint32_t waitResumePc_ = 0;  // ao voltar da IRQ para este ponto, a espera recomeça
    std::array<TraceEntry, kTraceSize> trace_{};
    size_t traceNext_ = 0;
};

} // namespace gba
