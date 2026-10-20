#include "gba.hpp"

#include <algorithm>

#include <cstring>
#include <stdexcept>

namespace gba {

Gba::Gba() {
    timers_.setOverflowHook([this](int timer, uint32_t count) { apu_.onTimerOverflow(timer, count); });
    apu_.setFifoRequest([this](int fifo) { dma_.soundRequest(fifo); });
    bus_.setTimerSyncHook([this] { syncTimers(); });
    bus_.setDmaKickHook([this] { dma_.runPending(); });
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
    bus_.resetTiming();
    cycleBalance_ = 0;
}

void Gba::runCycles(int cycles) {
    // Uma instrução pode ultrapassar a fatia; a diferença é descontada da próxima.
    cycleBalance_ += cycles;
    while (cycleBalance_ > 0) {
        // Tempo de cada passo: ciclos internos da CPU + o que o barramento cobrou pelos acessos dela
        // (buscas, leituras, gravações) e por qualquer DMA que tenha rodado nesse meio tempo.
        if (hookAddress_ && cpu_.pc() == hookAddress_ && hookFn_) hookFn_(); // gancho dos cheats
        bus_.idle(cpu_.step()); // ciclos internos: o prefetch do cartucho continua lendo durante eles
        const int spent = std::max(1, bus_.takeCycles());
        timers_.advance(spent - timersSynced_); // o que já foi adiantado no meio da instrução não conta de novo
        timersSynced_ = 0;
        apu_.advance(spent);
        if (bus_.hasProbes()) collectProbes();
        cycleBalance_ -= spent;
        ++instructions_;
    }
}

// Põe os timers em dia no meio de uma instrução (antes de a CPU ler ou escrever um registrador deles).
void Gba::syncTimers() {
    const int pending = bus_.pendingCycles();
    if (pending > timersSynced_) {
        timers_.advance(pending - timersSynced_);
        timersSynced_ = pending;
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
constexpr uint32_t kStateVersion = 6; // 2: relógio; 3: registrador do DMA; 4: tempo do barramento; 5: atraso dos timers;
                                      // 6: DMA imediato esperando para começar
} // namespace

void Gba::serialize(StateArchive& ar) {
    char magic[8];
    std::memcpy(magic, kStateMagic, sizeof magic);
    ar.value(magic);
    if (ar.loading() && std::memcmp(magic, kStateMagic, sizeof magic) != 0) {
        throw std::runtime_error("o arquivo não é um save state do NiGBA");
    }
    uint32_t version = kStateVersion;
    ar.value(version);
    if (ar.loading() && (version < 1 || version > kStateVersion)) {
        throw std::runtime_error("save state de outra versão do NiGBA");
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
    // Campos novos sempre no final: assim, cada versão antiga é o começo exato da nova.
    if (version >= 2) bus_.rtc().serialize(ar);
    if (version >= 3) dma_.serializeLatch(ar);
    if (version >= 4) bus_.serializeTiming(ar);
    else if (ar.loading()) bus_.resetTiming();
    if (version >= 5) timers_.serializeStartDelays(ar);
    if (version >= 6) {
        bus_.serializeDmaSchedule(ar);
        dma_.serializePending(ar);
    }
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
