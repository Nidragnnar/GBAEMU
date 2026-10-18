#pragma once
// Áudio do GBA (GBATEK, "GBA Sound Controller").
//   Canais 1-4: os do Game Boy (2 ondas quadradas, onda programável, ruído), com duração, envelope e sweep.
//   Canais A/B: amostras de 8 bits em filas de 32 bytes, reabastecidas por DMA e consumidas nos estouros
//   do Timer 0 ou 1. É por eles que a maioria dos jogos toca música.
// Saída: amostras estéreo de 16 bits a 32.768 Hz.

#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "bus.hpp"

namespace gba {

class Apu {
public:
    static constexpr int kSampleRate = 32768;
    static constexpr int kCyclesPerSample = 16777216 / kSampleRate; // 512

    explicit Apu(Bus& bus) : bus_(bus) {}

    void reset();
    void onIoWrite(uint32_t offset);
    // Chamado a cada instrução: só acumula; os canais avançam quando chega a hora de uma amostra.
    void advance(int cycles) {
        pendingCycles_ += cycles;
        sampleTimer_ -= cycles;
        if (sampleTimer_ <= 0) produceSamples();
    }
    void onTimerOverflow(int timer, uint32_t count);
    // Chamado quando a fila A (0) ou B (1) fica com 16 bytes ou menos: o DMA de som deve reabastecê-la.
    void setFifoRequest(std::function<void(int fifo)> request) { fifoRequest_ = std::move(request); }

    // Amostras geradas desde a última chamada (esquerda, direita, esquerda, ...).
    std::vector<int16_t> takeSamples() { return std::exchange(samples_, {}); }

private:
    struct Envelope {
        int volume = 0, period = 0, timer = 0;
        bool increase = false;
        void trigger(uint16_t reg);
        void tick();
    };
    struct Square {
        bool enabled = false;
        int length = 0, dutyStep = 0, timer = 0;
        Envelope envelope;
        // Sweep (só o canal 1)
        int sweepTimer = 0, shadowFreq = 0;
        bool sweepEnabled = false;
    };
    struct Wave {
        bool enabled = false;
        int length = 0, timer = 0, position = 0;
        std::array<uint8_t, 32> ram{}; // dois bancos de 16 bytes (32 amostras de 4 bits cada)
    };
    struct Noise {
        bool enabled = false, output = false;
        int length = 0, timer = 0;
        uint16_t lfsr = 0;
        Envelope envelope;
    };
    struct Fifo {
        std::array<int8_t, 32> data{};
        int readPos = 0, writePos = 0, count = 0;
        int8_t current = 0; // amostra tocando agora
        void reset() { readPos = writePos = count = 0; }
        void push(int8_t v) { if (count < 32) { data[writePos] = v; writePos = (writePos + 1) % 32; ++count; } }
    };

    uint16_t reg(uint32_t offset) const { return bus_.ioRead16(offset); }
    uint8_t rawByte(uint32_t offset) const { return static_cast<uint8_t>(reg(offset & ~1u) >> ((offset & 1) * 8)); }
    void clearRegBits(uint32_t offset, uint16_t bits) { bus_.ioWrite16Raw(offset, static_cast<uint16_t>(reg(offset) & ~bits)); }

    void triggerSquare(int index);
    void triggerWave();
    void triggerNoise();
    bool sweepStep(bool writeBack); // calcula a nova frequência; false se estourou (desliga o canal)
    void frameSequencerStep();
    void stepChannels(int cycles);
    void catchUp(int cycles);
    void produceSamples();
    void outputSample();
    void refreshWaveRamView();      // o que a CPU enxerga em 0x090-0x09F é o banco que não está tocando
    void updateStatus();

    Bus& bus_;
    std::array<Square, 2> square_{};
    Wave wave_{};
    Noise noise_{};
    std::array<Fifo, 2> fifo_{};
    int frameSequencerTimer_ = 0, frameSequencerStep_ = 0, sampleTimer_ = 0;
    int pendingCycles_ = 0; // ciclos ainda não aplicados aos canais
    double highPassL_ = 0, highPassR_ = 0, lastInL_ = 0, lastInR_ = 0; // remove o nível contínuo da saída
    std::vector<int16_t> samples_;
    std::function<void(int)> fifoRequest_;
};

} // namespace gba
