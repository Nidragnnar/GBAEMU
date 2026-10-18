#pragma once
// O console completo: barramento, CPU, DMA, timers, PPU e áudio, com o agendador que avança linha a linha.

#include <cstdint>
#include <set>
#include <tuple>
#include <vector>

#include "apu.hpp"
#include "bus.hpp"
#include "cpu.hpp"
#include "dma.hpp"
#include "ppu.hpp"
#include "timers.hpp"

namespace gba {

class Gba {
public:
    Gba();

    void loadRom(std::vector<uint8_t> rom) { bus_.loadRom(std::move(rom)); }
    void loadBios(std::vector<uint8_t> bios) { bus_.loadBios(std::move(bios)); hasBios_ = true; }
    void reset();

    // Executa um quadro inteiro (228 linhas, 280.896 ciclos).
    void runFrame();

    Bus& bus() { return bus_; }
    Cpu& cpu() { return cpu_; }
    Ppu& ppu() { return ppu_; }
    Dma& dma() { return dma_; }
    Timers& timers() { return timers_; }
    Apu& apu() { return apu_; }
    uint64_t instructions() const { return instructions_; }

    // Sonda: cada combinação (instrução, tipo, endereço) é registrada uma vez.
    struct ProbeHit { uint32_t pc; Probe probe; uint64_t frame; };
    void setProbing(bool on) { bus_.setProbing(on); }
    const std::vector<ProbeHit>& probeLog() const { return probeLog_; }

private:
    void runCycles(int cycles);

    Bus bus_;
    Cpu cpu_{bus_};
    Ppu ppu_{bus_};
    Dma dma_{bus_};
    Timers timers_{bus_};
    Apu apu_{bus_};
    bool hasBios_ = false;
    int64_t cycleBalance_ = 0; // ciclos que sobraram ou faltaram da fatia anterior
    uint64_t instructions_ = 0;
    uint64_t frames_ = 0;
    std::vector<ProbeHit> probeLog_;
    std::set<std::tuple<uint32_t, int, uint32_t>> probeSeen_;
    void collectProbes();
};

} // namespace gba
