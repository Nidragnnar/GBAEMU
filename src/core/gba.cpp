#include "gba.hpp"

#include <cstring>
#include <stdexcept>

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

void Gba::loadRom(std::vector<uint8_t> rom) {
    // Impressão digital da ROM (FNV-1a de 64 bits): garante que um save state só volte no mesmo jogo.
    uint64_t hash = 0xCBF29CE484222325ull;
    for (const uint8_t b : rom) hash = (hash ^ b) * 0x100000001B3ull;
    romHash_ = hash;
    bus_.loadRom(std::move(rom));
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

namespace gba {

namespace {
constexpr char kStateMagic[8] = {'G', 'B', 'A', 'E', 'M', 'U', 'S', 'T'};
constexpr uint32_t kStateVersion = 1;
} // namespace

void Gba::serialize(StateArchive& ar) {
    char magic[8];
    std::memcpy(magic, kStateMagic, sizeof magic);
    ar.value(magic);
    if (ar.loading() && std::memcmp(magic, kStateMagic, sizeof magic) != 0) {
        throw std::runtime_error("o arquivo não é um save state do gbaemu");
    }
    uint32_t version = kStateVersion;
    ar.value(version);
    if (ar.loading() && version != kStateVersion) {
        throw std::runtime_error("save state de outra versão do gbaemu");
    }
    uint64_t hash = romHash_;
    ar.value(hash);
    if (ar.loading() && hash != romHash_) throw std::runtime_error("save state de outro jogo");

    bus_.serialize(ar);
    cpu_.serialize(ar);
    ppu_.serialize(ar);
    dma_.serialize(ar);
    timers_.serialize(ar);
    apu_.serialize(ar);
    ar.value(cycleBalance_);
    ar.value(instructions_);
    ar.value(frames_);
    if (ar.loading() && !ar.atEnd()) throw std::runtime_error("save state com dados sobrando (corrompido?)");
}

std::vector<uint8_t> Gba::saveState() {
    auto ar = StateArchive::writer();
    serialize(ar);
    return ar.data();
}

void Gba::loadState(const std::vector<uint8_t>& data) {
    const std::vector<uint8_t> backup = saveState(); // se der errado no meio, volta a este ponto
    try {
        auto ar = StateArchive::reader(data);
        serialize(ar);
    } catch (...) {
        auto restore = StateArchive::reader(backup);
        serialize(restore);
        throw;
    }
}

} // namespace gba
