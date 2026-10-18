#include "gba.hpp"

namespace gba {

Gba::Gba() {
    timers_.setOverflowHook([this](int timer, uint32_t count) { apu_.onTimerOverflow(timer, count); });
    apu_.setFifoRequest([this](int fifo) { dma_.soundRequest(fifo); });
    bus_.setIoWriteHook([this](uint32_t offset) {
        dma_.onIoWrite(offset);
        ppu_.onIoWrite(offset);
        timers_.onIoWrite(offset);
        apu_.onIoWrite(offset);
        if (offset == 0x301 && !(bus_.read8(0x04000301) & 0x80)) cpu_.halt(); // HALTCNT (modo Stop ignorado)
    });
}

void Gba::reset() {
    if (!hasBios_) bus_.installHleBiosStub();
    cpu_.reset(!hasBios_);
    cpu_.setHleBios(!hasBios_);
    dma_.reset();
    timers_.reset();
    apu_.reset();
    cycleBalance_ = 0;
}

void Gba::runCycles(int cycles) {
    // Uma instrução pode ultrapassar a fatia; a diferença é descontada da próxima.
    cycleBalance_ += cycles;
    while (cycleBalance_ > 0) {
        const int spent = cpu_.step();
        timers_.advance(spent);
        apu_.advance(spent);
        if (bus_.hasProbes()) collectProbes();
        cycleBalance_ -= spent;
        ++instructions_;
    }
}

void Gba::collectProbes() {
    const uint32_t pc = cpu_.lastPc();
    for (const Probe& p : bus_.takeProbes()) {
        // Halfwords e words geram um acesso por byte; agrupa pelo endereço alinhado.
        const uint32_t key = p.kind == ProbeKind::WaitcntWrite ? p.addr : (p.addr & ~3u);
        if (probeLog_.size() < 500 && probeSeen_.insert({pc, static_cast<int>(p.kind), key}).second) {
            probeLog_.push_back({pc, p, frames_});
        }
    }
}

void Gba::runFrame() {
    ++frames_;
    for (int line = 0; line < kTotalLines; ++line) {
        ppu_.startLine(line);
        if (line == kScreenHeight) dma_.trigger(Dma::Timing::VBlank);
        runCycles(kHDrawCycles);
        ppu_.startHBlank();
        if (line < kScreenHeight) dma_.trigger(Dma::Timing::HBlank); // DMA de HBlank só nas linhas visíveis
        runCycles(kHBlankCycles);
    }
}

} // namespace gba
