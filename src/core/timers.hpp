#pragma once
// Os 4 timers (GBATEK, "GBA Timers"). Timer n em 0x04000100 + n*4:
//   CNT_L: escrever define o valor de recarga; ler devolve o contador atual.
//   CNT_H: bits 0-1 prescaler (1/64/256/1024 ciclos), bit 2 cascata, bit 6 IRQ, bit 7 liga.

#include <array>
#include <cstdint>
#include <functional>
#include <utility>

#include "bus.hpp"
#include "state.hpp"

namespace gba {

class Timers {
public:
    explicit Timers(Bus& bus) : bus_(bus) {}

    void reset();
    void onIoWrite(uint32_t offset);
    void serialize(StateArchive& ar) { for (auto& t : timers_) ar.value(t); ar.value(anyEnabled_); }
    void serializeStartDelays(StateArchive& ar) { ar.value(startDelay_); } // versão 5 do formato (no final)
    // Chamado depois de cada instrução com os ciclos gastos (o caso comum, tudo desligado, é imediato).
    void advance(int cycles) { if (anyEnabled_) advanceEnabled(cycles); }
    // Avisado quando o Timer 0 ou 1 estoura (as filas de som A/B consomem amostras nesse momento).
    void setOverflowHook(std::function<void(int timer, uint32_t count)> hook) { overflowHook_ = std::move(hook); }

private:
    struct Timer {
        bool enabled = false;
        uint16_t reload = 0;
        uint32_t counter = 0;
        uint32_t prescalerAccum = 0; // ciclos acumulados até o próximo incremento
        uint16_t control = 0;        // cópia de CNT_H (evita ler o registrador a cada instrução)
    };

    // Ciclos que faltam para cada timer começar a contar depois de ligado. Fica fora da struct Timer
    // porque ela é gravada inteira no save state: um campo novo lá mudaria o formato no meio do arquivo.
    std::array<int, 4> startDelay_{};

    void advanceEnabled(int cycles);
    void publish(int n) { bus_.ioWrite16Raw(0x100 + static_cast<uint32_t>(n) * 4, static_cast<uint16_t>(timers_[n].counter)); }

    Bus& bus_;
    std::array<Timer, 4> timers_{};
    bool anyEnabled_ = false;
    std::function<void(int, uint32_t)> overflowHook_;
};

} // namespace gba
