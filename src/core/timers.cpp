#include "timers.hpp"

namespace gba {

namespace {
constexpr uint32_t kPrescalerShift[4] = {0, 6, 8, 10}; // 1, 64, 256, 1024 ciclos por incremento
constexpr uint16_t kCascade = 1 << 2;
constexpr uint16_t kIrq = 1 << 6;
constexpr uint16_t kEnable = 1 << 7;
} // namespace

void Timers::reset() {
    timers_ = {};
    anyEnabled_ = false;
    for (int n = 0; n < 4; ++n) publish(n);
}

void Timers::onIoWrite(uint32_t offset) {
    if (offset < 0x100 || offset > 0x10F) return;
    const int n = static_cast<int>((offset - 0x100) / 4);
    const uint32_t base = 0x100 + static_cast<uint32_t>(n) * 4;
    Timer& t = timers_[n];

    if (offset == base || offset == base + 1) {
        // A CPU escreveu em CNT_L: isso define a recarga, não o contador.
        const uint16_t written = bus_.ioRead16(base);
        t.reload = offset == base
            ? static_cast<uint16_t>((t.reload & 0xFF00) | (written & 0x00FF))
            : static_cast<uint16_t>((t.reload & 0x00FF) | (written & 0xFF00));
        publish(n); // o que a CPU lê continua sendo o contador
        return;
    }
    if (offset == base + 2) {
        t.control = bus_.ioRead16(base + 2);
        const bool enable = t.control & kEnable;
        if (enable && !t.enabled) { // ao ligar, o contador começa na recarga
            t.counter = t.reload;
            t.prescalerAccum = 0;
            publish(n);
        }
        t.enabled = enable;
        anyEnabled_ = timers_[0].enabled || timers_[1].enabled || timers_[2].enabled || timers_[3].enabled;
    }
}

void Timers::advanceEnabled(int cycles) {
    uint32_t previousOverflows = 0;
    for (int n = 0; n < 4; ++n) {
        Timer& t = timers_[n];
        const uint16_t control = t.control;
        uint32_t overflows = 0;

        if (t.enabled) {
            uint32_t ticks;
            if (n > 0 && (control & kCascade)) {
                ticks = previousOverflows; // cascata: conta os estouros do timer anterior
            } else {
                const uint32_t shift = kPrescalerShift[control & 3];
                t.prescalerAccum += static_cast<uint32_t>(cycles);
                ticks = t.prescalerAccum >> shift;
                t.prescalerAccum &= (1u << shift) - 1;
            }

            while (ticks > 0) {
                const uint32_t untilOverflow = 0x10000 - t.counter;
                if (ticks < untilOverflow) {
                    t.counter += ticks;
                    break;
                }
                ticks -= untilOverflow;
                t.counter = t.reload; // estourou: recarrega
                ++overflows;
            }
            if (overflows && (control & kIrq)) bus_.requestInterrupt(static_cast<uint16_t>(irq::Timer0 << n));
            if (overflows && n < 2 && overflowHook_) overflowHook_(n, overflows);
            if (ticks || overflows) publish(n);
        }
        previousOverflows = overflows;
    }
}

} // namespace gba
