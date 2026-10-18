#include "apu.hpp"

#include <algorithm>
#include <utility>

namespace gba {

namespace {
// Registradores (deslocamento em relação a 0x04000000)
constexpr uint32_t kSound1L = 0x060, kSound1H = 0x062, kSound1X = 0x064;
constexpr uint32_t kSound2L = 0x068, kSound2H = 0x06C;
constexpr uint32_t kSound3L = 0x070, kSound3H = 0x072, kSound3X = 0x074;
constexpr uint32_t kSound4L = 0x078, kSound4H = 0x07C;
constexpr uint32_t kSoundCntL = 0x080, kSoundCntH = 0x082, kSoundCntX = 0x084;
constexpr uint32_t kWaveRam = 0x090;

constexpr int kFrameSequencerPeriod = 16777216 / 512; // 512 Hz
constexpr uint8_t kDuty[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 0},
};
// DAC dos canais com envelope: desligado se volume inicial = 0 e envelope decrescente.
bool dacOn(uint16_t envelopeReg) { return (envelopeReg & 0xF800) != 0; }
int squarePeriod(uint16_t freqReg) { return (2048 - (freqReg & 0x7FF)) * 16; }
int wavePeriod(uint16_t freqReg) { return (2048 - (freqReg & 0x7FF)) * 8; }
int noisePeriod(uint16_t reg) {
    const int r = reg & 7, s = (reg >> 4) & 0xF;
    return (r ? r * 16 : 8) << (s + 2); // 524288 Hz / r / 2^(s+1), com r = 0 valendo 0,5
}
} // namespace

void Apu::Envelope::trigger(uint16_t r) {
    volume = (r >> 12) & 0xF;
    increase = r & 0x0800;
    period = (r >> 8) & 7;
    timer = period;
}

void Apu::Envelope::tick() {
    if (period == 0) return;
    if (--timer > 0) return;
    timer = period;
    if (increase && volume < 15) ++volume;
    else if (!increase && volume > 0) --volume;
}

void Apu::reset() {
    square_ = {};
    wave_ = {};
    noise_ = {};
    for (auto& f : fifo_) { f.reset(); f.current = 0; }
    frameSequencerTimer_ = kFrameSequencerPeriod;
    frameSequencerStep_ = 0;
    sampleTimer_ = kCyclesPerSample;
    pendingCycles_ = 0;
    highPassL_ = highPassR_ = lastInL_ = lastInR_ = 0;
    samples_.clear();
}

// ---------------------------------------------------------------- escritas nos registradores

void Apu::onIoWrite(uint32_t offset) {
    switch (offset) {
    case 0x062: square_[0].length = 64 - (reg(kSound1H) & 0x3F); break;
    case 0x068: square_[1].length = 64 - (reg(kSound2L) & 0x3F); break;
    case 0x072: wave_.length = 256 - (reg(kSound3H) & 0xFF); break;
    case 0x078: noise_.length = 64 - (reg(kSound4L) & 0x3F); break;
    case 0x063: if (!dacOn(reg(kSound1H))) square_[0].enabled = false; break;
    case 0x069: if (!dacOn(reg(kSound2L))) square_[1].enabled = false; break;
    case 0x079: if (!dacOn(reg(kSound4L))) noise_.enabled = false; break;
    case 0x070:
        if (!(reg(kSound3L) & 0x80)) wave_.enabled = false; // DAC do canal 3
        refreshWaveRamView();                              // mudou o banco que está tocando
        break;
    // O bit 15 ("reiniciar") é só de escrita: dispara o canal e é apagado.
    case 0x065: if (reg(kSound1X) & 0x8000) { triggerSquare(0); clearRegBits(kSound1X, 0x8000); } break;
    case 0x06D: if (reg(kSound2H) & 0x8000) { triggerSquare(1); clearRegBits(kSound2H, 0x8000); } break;
    case 0x075: if (reg(kSound3X) & 0x8000) { triggerWave(); clearRegBits(kSound3X, 0x8000); } break;
    case 0x07D: if (reg(kSound4H) & 0x8000) { triggerNoise(); clearRegBits(kSound4H, 0x8000); } break;
    case 0x083: // reinício das filas A (bit 11) e B (bit 15), também só de escrita
        if (reg(kSoundCntH) & 0x0800) fifo_[0].reset();
        if (reg(kSoundCntH) & 0x8000) fifo_[1].reset();
        clearRegBits(kSoundCntH, 0x8800);
        break;
    case 0x084:
        if (!(reg(kSoundCntX) & 0x80)) { // som desligado: todos os canais param
            square_[0].enabled = square_[1].enabled = wave_.enabled = noise_.enabled = false;
        }
        break;
    default:
        if (offset >= kWaveRam && offset < kWaveRam + 16) {
            // A CPU escreve no banco que não está tocando.
            const int bank = (reg(kSound3L) & 0x40) ? 0 : 1;
            wave_.ram[static_cast<size_t>(bank * 16) + (offset - kWaveRam)] = rawByte(offset);
        } else if (offset >= 0x0A0 && offset < 0x0A8) {
            // A fila é só-escrita: a CPU não consegue lê-la, então pegamos o byte bruto que acabou de chegar.
            fifo_[offset < 0x0A4 ? 0 : 1].push(static_cast<int8_t>(rawByte(offset)));
        }
        break;
    }
}

void Apu::refreshWaveRamView() {
    const int bank = (reg(kSound3L) & 0x40) ? 0 : 1;
    for (uint32_t i = 0; i < 16; i += 2) {
        const size_t at = static_cast<size_t>(bank * 16) + i;
        bus_.ioWrite16Raw(kWaveRam + i, static_cast<uint16_t>(wave_.ram[at] | (wave_.ram[at + 1] << 8)));
    }
}

void Apu::triggerSquare(int index) {
    Square& sq = square_[static_cast<size_t>(index)];
    const uint16_t envReg = reg(index == 0 ? kSound1H : kSound2L);
    const uint16_t freqReg = reg(index == 0 ? kSound1X : kSound2H);
    sq.enabled = dacOn(envReg);
    if (sq.length == 0) sq.length = 64;
    sq.timer = squarePeriod(freqReg);
    sq.envelope.trigger(envReg);
    if (index == 0) {
        const uint16_t sweep = reg(kSound1L);
        const int period = (sweep >> 4) & 7, shift = sweep & 7;
        sq.shadowFreq = freqReg & 0x7FF;
        sq.sweepTimer = period ? period : 8;
        sq.sweepEnabled = period != 0 || shift != 0;
        if (shift && !sweepStep(false)) sq.enabled = false; // estouraria logo de cara
    }
}

void Apu::triggerWave() {
    wave_.enabled = (reg(kSound3L) & 0x80) != 0;
    if (wave_.length == 0) wave_.length = 256;
    wave_.timer = wavePeriod(reg(kSound3X));
    wave_.position = 0;
}

void Apu::triggerNoise() {
    const uint16_t envReg = reg(kSound4L);
    noise_.enabled = dacOn(envReg);
    if (noise_.length == 0) noise_.length = 64;
    noise_.timer = noisePeriod(reg(kSound4H));
    noise_.envelope.trigger(envReg);
    noise_.lfsr = (reg(kSound4H) & 0x08) ? 0x40 : 0x4000; // 7 ou 15 bits
}

// ---------------------------------------------------------------- tempo

bool Apu::sweepStep(bool writeBack) {
    Square& sq = square_[0];
    const uint16_t sweep = reg(kSound1L);
    const int shift = sweep & 7;
    const int delta = sq.shadowFreq >> shift;
    const int next = (sweep & 0x08) ? sq.shadowFreq - delta : sq.shadowFreq + delta;
    if (next > 2047) return false;
    if (writeBack && shift) {
        sq.shadowFreq = next;
        bus_.ioWrite16Raw(kSound1X, static_cast<uint16_t>((reg(kSound1X) & ~0x7FF) | next));
    }
    return true;
}

void Apu::frameSequencerStep() {
    const auto lengthTick = [](bool& enabled, int& length, bool lengthEnable) {
        if (lengthEnable && length > 0 && --length == 0) enabled = false;
    };
    if ((frameSequencerStep_ & 1) == 0) { // 256 Hz: duração
        lengthTick(square_[0].enabled, square_[0].length, reg(kSound1X) & 0x4000);
        lengthTick(square_[1].enabled, square_[1].length, reg(kSound2H) & 0x4000);
        lengthTick(wave_.enabled, wave_.length, reg(kSound3X) & 0x4000);
        lengthTick(noise_.enabled, noise_.length, reg(kSound4H) & 0x4000);
    }
    if (frameSequencerStep_ == 2 || frameSequencerStep_ == 6) { // 128 Hz: sweep
        Square& sq = square_[0];
        if (--sq.sweepTimer <= 0) {
            const int period = (reg(kSound1L) >> 4) & 7;
            sq.sweepTimer = period ? period : 8;
            if (sq.sweepEnabled && period && sq.enabled) {
                if (!sweepStep(true) || !sweepStep(false)) sq.enabled = false;
            }
        }
    }
    if (frameSequencerStep_ == 7) { // 64 Hz: envelope
        square_[0].envelope.tick();
        square_[1].envelope.tick();
        noise_.envelope.tick();
    }
    frameSequencerStep_ = (frameSequencerStep_ + 1) & 7;
}

void Apu::stepChannels(int cycles) {
    for (int i = 0; i < 2; ++i) {
        Square& sq = square_[static_cast<size_t>(i)];
        sq.timer -= cycles;
        while (sq.timer <= 0) {
            sq.timer += squarePeriod(reg(i == 0 ? kSound1X : kSound2H));
            sq.dutyStep = (sq.dutyStep + 1) & 7;
        }
    }
    wave_.timer -= cycles;
    while (wave_.timer <= 0) {
        wave_.timer += wavePeriod(reg(kSound3X));
        const int samples = (reg(kSound3L) & 0x20) ? 64 : 32; // 1 banco ou os 2 seguidos
        wave_.position = (wave_.position + 1) % samples;
    }
    noise_.timer -= cycles;
    while (noise_.timer <= 0) {
        noise_.timer += noisePeriod(reg(kSound4H));
        const bool carry = noise_.lfsr & 1;
        noise_.lfsr >>= 1;
        noise_.output = carry;
        if (carry) noise_.lfsr ^= (reg(kSound4H) & 0x08) ? 0x60 : 0x6000;
    }
}

// Os canais avançam de uma vez quando chega a hora de uma amostra (a cada 512 ciclos), porque o
// estado entre duas amostras não chega a ser ouvido.
void Apu::produceSamples() {
    while (sampleTimer_ <= 0) {
        // Leva os canais até o instante exato desta amostra.
        const int upToSample = pendingCycles_ + sampleTimer_;
        catchUp(upToSample);
        pendingCycles_ -= upToSample;
        sampleTimer_ += kCyclesPerSample;
        outputSample();
    }
}

void Apu::catchUp(int cycles) {
    if (cycles <= 0) return;
    if (!(reg(kSoundCntX) & 0x80)) return; // som desligado: canais parados
    stepChannels(cycles);
    frameSequencerTimer_ -= cycles;
    while (frameSequencerTimer_ <= 0) { frameSequencerTimer_ += kFrameSequencerPeriod; frameSequencerStep(); }
}

void Apu::onTimerOverflow(int timer, uint32_t count) {
    const uint16_t cnt = reg(kSoundCntH);
    for (int f = 0; f < 2; ++f) {
        const int selected = (cnt >> (f == 0 ? 10 : 14)) & 1;
        if (selected != timer) continue;
        Fifo& fifo = fifo_[static_cast<size_t>(f)];
        for (uint32_t i = 0; i < count; ++i) {
            if (fifo.count > 0) {
                fifo.current = fifo.data[static_cast<size_t>(fifo.readPos)];
                fifo.readPos = (fifo.readPos + 1) % 32;
                --fifo.count;
            }
        }
        if (fifo.count <= 16 && fifoRequest_) fifoRequest_(f);
    }
}

// ---------------------------------------------------------------- mixagem

void Apu::updateStatus() {
    const uint16_t status = static_cast<uint16_t>((square_[0].enabled ? 1 : 0) | (square_[1].enabled ? 2 : 0) |
                                                  (wave_.enabled ? 4 : 0) | (noise_.enabled ? 8 : 0));
    const uint16_t x = reg(kSoundCntX);
    if ((x & 0x0F) != status) bus_.ioWrite16Raw(kSoundCntX, static_cast<uint16_t>((x & ~0x0F) | status));
}

void Apu::outputSample() {
    updateStatus();
    double left = 0, right = 0;
    if (reg(kSoundCntX) & 0x80) {
        // Canais 1-4: amplitude de 0 a 15.
        int amp[4] = {0, 0, 0, 0};
        for (int i = 0; i < 2; ++i) {
            const Square& sq = square_[static_cast<size_t>(i)];
            const int duty = (reg(i == 0 ? kSound1H : kSound2L) >> 6) & 3;
            if (sq.enabled && kDuty[duty][sq.dutyStep]) amp[i] = sq.envelope.volume;
        }
        if (wave_.enabled) {
            const uint16_t l = reg(kSound3L), h = reg(kSound3H);
            const int bank = (l & 0x40) ? 1 : 0;
            const size_t byte = (static_cast<size_t>(bank * 16) + static_cast<size_t>(wave_.position / 2)) % 32;
            int sample = (wave_.position & 1) ? (wave_.ram[byte] & 0xF) : (wave_.ram[byte] >> 4);
            if (h & 0x8000) sample = sample * 3 / 4;                  // 75%
            else switch ((h >> 13) & 3) {
                case 0: sample = 0; break;                            // mudo
                case 2: sample >>= 1; break;                          // 50%
                case 3: sample >>= 2; break;                          // 25%
                default: break;                                       // 100%
            }
            amp[2] = sample;
        }
        if (noise_.enabled && noise_.output) amp[3] = noise_.envelope.volume;

        const uint16_t cntL = reg(kSoundCntL), cntH = reg(kSoundCntH);
        int psgL = 0, psgR = 0;
        for (int i = 0; i < 4; ++i) {
            if (cntL & (0x1000 << i)) psgL += amp[i];
            if (cntL & (0x0100 << i)) psgR += amp[i];
        }
        static constexpr double kPsgRatio[4] = {0.25, 0.5, 1.0, 0.0}; // 3 é proibido
        const double ratio = kPsgRatio[cntH & 3];
        left += psgL * (((cntL >> 4) & 7) + 1) * ratio;
        right += psgR * ((cntL & 7) + 1) * ratio;

        // Canais A/B: amostra de 8 bits com sinal, a 50% ou 100%.
        for (int f = 0; f < 2; ++f) {
            const double v = fifo_[static_cast<size_t>(f)].current * ((cntH & (f == 0 ? 0x04 : 0x08)) ? 4.0 : 2.0);
            if (cntH & (f == 0 ? 0x0200 : 0x2000)) left += v;
            if (cntH & (f == 0 ? 0x0100 : 0x1000)) right += v;
        }
    }

    // Filtro passa-alta (como o capacitor na saída do console): tira o nível contínuo e evita estalos.
    constexpr double kAlpha = 0.998;
    highPassL_ = left - lastInL_ + kAlpha * highPassL_;
    highPassR_ = right - lastInR_ + kAlpha * highPassR_;
    lastInL_ = left;
    lastInR_ = right;
    const auto toPcm = [](double v) { return static_cast<int16_t>(std::clamp(v * 16.0, -32768.0, 32767.0)); };
    samples_.push_back(toPcm(highPassL_));
    samples_.push_back(toPcm(highPassR_));
    // Se ninguém estiver consumindo (sem dispositivo de áudio), não deixa crescer sem limite.
    if (samples_.size() > static_cast<size_t>(kSampleRate) * 2) samples_.erase(samples_.begin(), samples_.begin() + kSampleRate);
}

} // namespace gba
