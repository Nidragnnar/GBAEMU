#pragma once
// Os 4 canais de DMA (GBATEK, "GBA DMA Transfers").
// Registradores do canal n em 0x040000B0 + n*12: SAD (origem), DAD (destino), CNT_L (quantidade), CNT_H (controle).

#include <array>
#include <cstdint>

#include "bus.hpp"
#include "state.hpp"

namespace gba {

class Dma {
public:
    enum class Timing { Immediate = 0, VBlank = 1, HBlank = 2, Special = 3 };

    explicit Dma(Bus& bus) : bus_(bus) {}

    void reset();
    void onIoWrite(uint32_t offset); // detecta quando um canal é ligado
    void trigger(Timing timing);     // chamado pelo agendador no VBlank e no HBlank
    void soundRequest(int fifo);     // fila de som A (0) ou B (1) pedindo dados: DMA 1/2 no modo especial
    void serialize(StateArchive& ar) { for (auto& ch : channels_) ar.value(ch); }
    void serializeLatch(StateArchive& ar) { ar.value(latch_); } // versão 3 do formato (fica no final)
    void serializePending(StateArchive& ar) { ar.value(pending_); } // versão 6 (no final)
    void runPending();               // roda os DMAs imediatos que estavam esperando os 2 ciclos de partida

private:
    struct Channel {
        bool enabled = false;
        uint32_t src = 0, dst = 0, count = 0; // cópias internas, travadas ao ligar o canal
    };

    static uint32_t base(int n) { return 0x0B0 + static_cast<uint32_t>(n) * 12; }
    uint16_t control(int n) const { return bus_.ioRead16(base(n) + 10); }
    void latch(int n, bool reloadAll);
    void transfer(int n);
    void transferUnit(uint32_t src, uint32_t dst, bool word, bool sequential);

    Bus& bus_;
    std::array<Channel, 4> channels_{};
    uint32_t latch_ = 0; // último valor que passou pelo DMA
    uint8_t pending_ = 0; // canais imediatos ligados, esperando para começar
};

} // namespace gba
