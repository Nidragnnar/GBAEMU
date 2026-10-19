#pragma once
// Relógio do cartucho (RTC Seiko S-3511), ligado à porta GPIO da ROM (GBATEK, "GBA Cart Real-Time Clock").
// Usado pelos Pokémon Ruby/Sapphire/Emerald (e hacks deles), Boktai e outros.
//
// Porta GPIO, nos endereços da ROM:
//   080000C4  dados: bit 0 = SCK (relógio), bit 1 = SIO (dado), bit 2 = CS (seleção)
//   080000C6  direção de cada pino: 1 = o GBA escreve, 0 = o GBA lê
//   080000C8  bit 0 = 1: a CPU passa a enxergar a porta nesses endereços (senão vê a ROM normal)
//
// Protocolo: CS sobe, chega um byte de comando (bit menos significativo primeiro: bits 0-3 = 0110,
// bits 4-6 = comando, bit 7 = leitura), depois os bytes de dados, um bit por pulso de SCK.

#include <array>
#include <cstdint>
#include <ctime>
#include <functional>

#include "state.hpp"

namespace gba {

class Rtc {
public:
    Rtc();

    // Escrita da CPU num dos registradores da porta (deslocamento 0xC4, 0xC6 ou 0xC8).
    void write(uint32_t offset, uint16_t value);
    // O que a CPU lê em 0xC4/0xC6/0xC8 quando a porta está legível.
    uint16_t read(uint32_t offset) const;
    bool readable() const { return readable_; }

    // Hora usada pelo relógio (padrão: a do computador). Os testes usam uma hora fixa.
    void setClock(std::function<std::time_t()> clock) { clock_ = std::move(clock); }

    void serialize(StateArchive& ar);

private:
    void updatePins();     // o jogo mudou os pinos: avança o protocolo
    void processByte();    // terminou de chegar um byte do jogo
    uint8_t outputBit() const;
    void latchTime();      // copia a hora atual para os 7 bytes de data/hora (BCD)

    std::function<std::time_t()> clock_;

    uint8_t pins_ = 0;       // estado dos 4 pinos
    uint8_t direction_ = 0;  // 1 = saída do GBA
    bool readable_ = false;

    int transferStep_ = 0;   // 0 = parado, 1 = SCK alto, 2 = transferindo
    uint8_t bits_ = 0;       // byte sendo montado
    int bitsRead_ = 0;
    uint8_t command_ = 0;
    int bytesRemaining_ = 0;
    bool commandActive_ = false;
    uint8_t control_ = 0x40; // bit 6 = modo 24 horas; bit 7 (falha de energia) desligado
    std::array<uint8_t, 7> time_{}; // ano, mês, dia, dia da semana, hora, minuto, segundo (BCD)
};

} // namespace gba
