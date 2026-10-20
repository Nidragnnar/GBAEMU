#include "dma.hpp"

namespace gba {

namespace {
// CNT_H
constexpr uint16_t kRepeat = 1 << 9;
constexpr uint16_t kWord = 1 << 10;
constexpr uint16_t kIrq = 1 << 14;
constexpr uint16_t kEnable = 1 << 15;

// Faixa de endereços que cada canal enxerga (o canal 0 não alcança o cartucho).
constexpr uint32_t kSrcMask[4] = {0x07FFFFFF, 0x0FFFFFFF, 0x0FFFFFFF, 0x0FFFFFFF};
constexpr uint32_t kDstMask[4] = {0x07FFFFFF, 0x07FFFFFF, 0x07FFFFFF, 0x0FFFFFFF};
constexpr uint32_t kMaxCount[4] = {0x4000, 0x4000, 0x4000, 0x10000};
} // namespace

void Dma::reset() { channels_ = {}; latch_ = 0; }

void Dma::latch(int n, bool reloadAll) {
    Channel& ch = channels_[n];
    const uint32_t count = bus_.ioRead16(base(n) + 8) & (kMaxCount[n] - 1);
    ch.count = count == 0 ? kMaxCount[n] : count; // quantidade 0 significa o máximo
    if (reloadAll) {
        ch.src = bus_.ioRead32(base(n)) & kSrcMask[n];
        ch.dst = bus_.ioRead32(base(n) + 4) & kDstMask[n];
    }
}

void Dma::onIoWrite(uint32_t offset) {
    if (offset < 0x0B0 || offset > 0x0DF) return;
    const int n = static_cast<int>((offset - 0x0B0) / 12);
    if (offset != base(n) + 11) return; // só interessa o byte alto de CNT_H, onde fica o "ligar"

    const uint16_t cnt = control(n);
    Channel& ch = channels_[n];
    const bool wasEnabled = ch.enabled;
    ch.enabled = (cnt & kEnable) != 0;
    if (!ch.enabled || wasEnabled) return;

    latch(n, true); // ligou agora: trava origem, destino e quantidade
    if (static_cast<Timing>((cnt >> 12) & 3) == Timing::Immediate) transfer(n);
}

void Dma::trigger(Timing timing) {
    for (int n = 0; n < 4; ++n) { // canal de número menor tem prioridade
        if (channels_[n].enabled && static_cast<Timing>((control(n) >> 12) & 3) == timing) transfer(n);
    }
}

// Modo especial dos canais 1 e 2: a cada pedido da fila de som, 4 palavras de 32 bits vão para a fila,
// com o destino fixo, qualquer que seja a configuração (GBATEK, "Sound DMA").
void Dma::soundRequest(int fifo) {
    const uint32_t fifoAddr = fifo == 0 ? 0x040000A0 : 0x040000A4;
    for (int n = 1; n <= 2; ++n) {
        Channel& ch = channels_[n];
        const uint16_t cnt = control(n);
        if (!ch.enabled || static_cast<Timing>((cnt >> 12) & 3) != Timing::Special || (ch.dst & ~3u) != fifoAddr) continue;
        const uint32_t srcControl = (cnt >> 7) & 3;
        const uint32_t srcStep = srcControl == 1 ? 0u - 4u : srcControl == 2 ? 0u : 4u;
        for (int i = 0; i < 4; ++i) {
            transferUnit(ch.src, fifoAddr, true);
            ch.src += srcStep;
        }
        if (cnt & kIrq) bus_.requestInterrupt(static_cast<uint16_t>(irq::Dma0 << n));
        if (!(cnt & kRepeat)) {
            ch.enabled = false;
            bus_.ioWrite16Raw(base(n) + 10, static_cast<uint16_t>(cnt & ~kEnable));
        }
        return;
    }
}

// Uma unidade de transferência. O DMA tem um registrador interno de passagem: cada valor lido fica nele.
// Se a origem é uma região que o DMA não consegue ler (a BIOS, ou o cartucho para o canal 0, cujo endereço
// cai na BIOS pela máscara), o que vai para o destino é o valor que ficou nesse registrador.
void Dma::transferUnit(uint32_t src, uint32_t dst, bool word) {
    const bool readable = src >= 0x02000000;
    if (word) {
        if (readable) latch_ = bus_.load32(src & ~3u);
        bus_.store32(dst & ~3u, latch_);
    } else {
        if (readable) {
            const uint32_t v = bus_.load16(src & ~1u);
            latch_ = v | (v << 16); // a halfword aparece nas duas metades
        }
        bus_.store16(dst & ~1u, static_cast<uint16_t>(latch_ >> ((dst & 2) * 8)));
    }
}

void Dma::transfer(int n) {
    Channel& ch = channels_[n];
    const uint16_t cnt = control(n);
    const bool word = cnt & kWord;
    const uint32_t unit = word ? 4 : 2;
    const uint32_t dstControl = (cnt >> 5) & 3; // 0 incrementa, 1 decrementa, 2 fixo, 3 incrementa e recarrega
    const uint32_t srcControl = (cnt >> 7) & 3; // 0 incrementa, 1 decrementa, 2 fixo
    const auto step = [unit](uint32_t control) -> uint32_t {
        return control == 1 ? 0u - unit : control == 2 ? 0u : unit;
    };
    bus_.idle(2); // o DMA leva 2 ciclos internos para começar a transferência
    // Lendo do cartucho (ROM), a origem sempre avança: o cartucho tem um contador de endereço próprio que
    // incrementa a cada leitura sequencial, então "fixo" e "decrementando" não têm efeito ali.
    const bool fromRom = (ch.src >> 24) >= 0x08 && (ch.src >> 24) <= 0x0D;
    const uint32_t srcStep = fromRom ? step(0) : step(srcControl);
    const uint32_t dstStep = step(dstControl == 3 ? 0 : dstControl);

    if ((ch.dst >> 24) == 0x0D || (ch.src >> 24) == 0x0D) bus_.backup().eepromDmaHint(ch.count);
    for (uint32_t i = 0; i < ch.count; ++i) {
        transferUnit(ch.src, ch.dst, word);
        ch.src += srcStep;
        ch.dst += dstStep;
    }
    // TODO: a CPU fica parada durante o DMA (2 ciclos por unidade + waitstates); ainda não contabilizado.

    if (cnt & kIrq) bus_.requestInterrupt(static_cast<uint16_t>(irq::Dma0 << n));

    const auto timing = static_cast<Timing>((cnt >> 12) & 3);
    if ((cnt & kRepeat) && timing != Timing::Immediate) {
        latch(n, false);                                            // recarrega a quantidade
        if (dstControl == 3) ch.dst = bus_.ioRead32(base(n) + 4) & kDstMask[n];
    } else {
        ch.enabled = false;
        bus_.ioWrite16Raw(base(n) + 10, static_cast<uint16_t>(cnt & ~kEnable)); // hardware desliga o canal
    }
}

} // namespace gba
