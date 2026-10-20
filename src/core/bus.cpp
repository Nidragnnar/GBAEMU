#include "bus.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace gba {

namespace {
// Registradores de I/O que só aceitam escrita (GBATEK, "GBA I/O Map").
bool isWriteOnlyIo(uint32_t off) {
    return (off >= 0x010 && off <= 0x047) || (off >= 0x04C && off <= 0x04F) || (off >= 0x054 && off <= 0x05F) ||
           (off >= 0x0A0 && off <= 0x0A7) || (off >= 0x0B0 && off <= 0x0B9) || (off >= 0x0BC && off <= 0x0C5) ||
           (off >= 0x0C8 && off <= 0x0D1) || (off >= 0x0D4 && off <= 0x0DD);
}
// Registradores que existem e podem ser lidos.
bool isReadableIo(uint32_t off) {
    // 08C-08F (entre o SOUNDBIAS e a memória de onda) é uma palavra inteira sem registrador: open bus.
    return off <= 0x00F || (off >= 0x048 && off <= 0x04B) || (off >= 0x050 && off <= 0x053) ||
           (off >= 0x060 && off <= 0x08B) || (off >= 0x090 && off <= 0x09F) ||
           (off >= 0x0BA && off <= 0x0BB) || (off >= 0x0C6 && off <= 0x0C7) ||
           (off >= 0x0D2 && off <= 0x0D3) || (off >= 0x0DE && off <= 0x0DF) || (off >= 0x100 && off <= 0x10F) ||
           (off >= 0x120 && off <= 0x12F) || (off >= 0x130 && off <= 0x135) || (off >= 0x140 && off <= 0x141) ||
           (off >= 0x150 && off <= 0x159) || (off >= 0x200 && off <= 0x20B) || (off >= 0x300 && off <= 0x301);
}
// Bits que existem e podem ser lidos em cada registrador de 16 bits (GBATEK). Bits fora da máscara
// leem 0: não existem no hardware ou são só de escrita. Registradores fora da tabela: todos os bits.
uint16_t ioReadMask(uint32_t halfOffset) {
    switch (halfOffset) {
    case 0x004: return 0xFF3F; // DISPSTAT: bits 6-7 não existem
    case 0x008: case 0x00A: return 0xDFFF; // BG0CNT/BG1CNT: o bit 13 só existe nos BGs afins (2 e 3)
    case 0x048: case 0x04A: return 0x3F3F; // WININ, WINOUT
    case 0x050: return 0x3FFF; // BLDCNT
    case 0x052: return 0x1F1F; // BLDALPHA
    // Som: comprimentos e "reiniciar" são só de escrita; vários bits não existem.
    case 0x060: return 0x007F; // SOUND1CNT_L
    case 0x062: return 0xFFC0; // SOUND1CNT_H
    case 0x064: return 0x4000; // SOUND1CNT_X
    case 0x068: return 0xFFC0; // SOUND2CNT_L
    case 0x06C: return 0x4000; // SOUND2CNT_H
    case 0x070: return 0x00E0; // SOUND3CNT_L
    case 0x072: return 0xE000; // SOUND3CNT_H
    case 0x074: return 0x4000; // SOUND3CNT_X
    case 0x078: return 0xFF00; // SOUND4CNT_L
    case 0x07C: return 0x40FF; // SOUND4CNT_H
    case 0x080: return 0xFF77; // SOUNDCNT_L: bits 3 e 7 não existem (o teste do Rogue confere isto)
    case 0x082: return 0x770F; // SOUNDCNT_H: bits de "reset dos FIFOs" são só de escrita
    case 0x084: return 0x008F; // SOUNDCNT_X
    case 0x088: return 0xC3FF; // SOUNDBIAS
    case 0x066: case 0x06A: case 0x06E: case 0x076: case 0x07A: case 0x07E: case 0x086:
    case 0x08A: case 0x08C: case 0x08E:
        return 0x0000;         // dentro da faixa de som, mas sem registrador
    case 0x0BA: case 0x0C6: case 0x0D2: return 0xF7E0; // DMA0-2 CNT_H
    case 0x0DE: return 0xFFE0; // DMA3 CNT_H
    case 0x102: case 0x106: case 0x10A: case 0x10E: return 0x00C7; // TMxCNT_H
    case 0x132: return 0xC3FF; // KEYCNT
    case 0x200: case 0x202: return 0x3FFF; // IE, IF
    case 0x204: return 0xDFFF; // WAITCNT
    case 0x206: case 0x20A: return 0x0000;
    case 0x208: return 0x0001; // IME
    default: return 0xFFFF;
    }
}

// Uma palavra de 32 bits com pelo menos uma parte legível.
bool wordHasReadable(uint32_t off) {
    const uint32_t base = off & ~3u;
    for (uint32_t i = 0; i < 4; ++i) if (isReadableIo(base + i)) return true;
    return false;
}
} // namespace

namespace {
inline uint16_t bytesToLe16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t bytesToLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline void le16ToBytes(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
inline void le32ToBytes(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24);
}
} // namespace

namespace {
constexpr uint32_t kMaxRomSize = 32u * 1024u * 1024u;
constexpr uint32_t kDispcnt = 0x000; // offset de DISPCNT na região de I/O
} // namespace

Bus::Bus() { updateWaitstates(); }

void Bus::resetTiming() {
    cycles_ = 0;
    dmaCountdown_ = -1;
    nextSequential_ = 0xFFFFFFFF;
    prefetchActive_ = false;
    prefetchCount_ = 0;
    prefetchProgress_ = 0;
    updateWaitstates();
}

void Bus::loadBios(std::vector<uint8_t> data) {
    if (data.size() != bios_.size()) {
        throw std::runtime_error("BIOS deve ter exatamente 16 KB");
    }
    std::copy(data.begin(), data.end(), bios_.begin());
}

void Bus::installHleBiosStub() {
    // Instruções da BIOS original nos mesmos endereços, para que a "última instrução buscada"
    // (devolvida pela proteção da BIOS) tenha os valores reais documentados no GBATEK.
    const auto put = [this](uint32_t addr, uint32_t v) {
        bios_[addr] = static_cast<uint8_t>(v);
        bios_[addr + 1] = static_cast<uint8_t>(v >> 8);
        bios_[addr + 2] = static_cast<uint8_t>(v >> 16);
        bios_[addr + 3] = static_cast<uint8_t>(v >> 24);
    };
    put(0x018, 0xEA000042); // b     0x128                  ; vetor de IRQ
    put(0x0E4, 0xE129F000); // (valor visto depois da inicialização)
    put(0x128, 0xE92D500F); // stmfd sp!, {r0-r3, r12, lr}
    put(0x12C, 0xE3A00301); // mov   r0, #0x04000000
    put(0x130, 0xE28FE000); // add   lr, pc, #0             ; retorno para o ldmfd
    put(0x134, 0xE510F004); // ldr   pc, [r0, #-4]          ; tratador do jogo em 0x03007FFC
    put(0x138, 0xE8BD500F); // ldmfd sp!, {r0-r3, r12, lr}
    put(0x13C, 0xE25EF004); // subs  pc, lr, #4             ; volta e restaura o CPSR
    put(0x144, 0xE55EC002); // (valor visto depois de uma IRQ)
    put(0x190, 0xE3A02004); // (valor visto depois de uma SWI)
}

void Bus::loadRom(std::vector<uint8_t> data) {
    if (data.empty() || data.size() > kMaxRomSize) {
        throw std::runtime_error("ROM vazia ou maior que 32 MB");
    }
    backup_.setType(Backup::detect(data));
    rom_ = std::move(data);
}

uint32_t Bus::vramOffset(uint32_t addr) {
    uint32_t off = addr & 0x1FFFF;          // espelho de 128 KB
    if (off >= 0x18000) off -= 0x8000;      // 0x18000-0x1FFFF -> 0x10000-0x17FFF
    return off;
}

const uint8_t* Bus::resolve(uint32_t addr, bool& writable) const {
    writable = true;
    switch (addr >> 24) {
    case 0x00:
        writable = false;
        return addr < bios_.size() ? &bios_[addr] : nullptr;
    case 0x02: return &ewram_[addr & 0x3FFFF];
    case 0x03: return &iwram_[addr & 0x7FFF];
    case 0x04: return (addr & 0xFFFFFF) < io_.size() ? &io_[addr & 0x3FF] : nullptr;
    case 0x05: return &palette_[addr & 0x3FF];
    case 0x06: return &vram_[vramOffset(addr)];
    case 0x07: return &oam_[addr & 0x3FF];
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
        writable = false;
        const uint32_t off = addr & 0x1FFFFFF;
        return off < rom_.size() ? &rom_[off] : nullptr;
    }
    default:
        writable = false;
        return nullptr;
    }
}

uint8_t* Bus::resolveMut(uint32_t addr) {
    bool writable = false;
    const uint8_t* p = resolve(addr, writable);
    return writable ? const_cast<uint8_t*>(p) : nullptr;
}

// ---------------------------------------------------------------- tempo

void Bus::updateWaitstates() {
    const uint16_t w = static_cast<uint16_t>(io_[0x204] | (io_[0x205] << 8));
    static constexpr uint8_t kN[4] = {4, 3, 2, 8}; // espera do primeiro acesso (não sequencial)
    for (int r = 0; r < 16; ++r) { n16_[r] = s16_[r] = n32_[r] = s32_[r] = 1; }
    n16_[0x2] = s16_[0x2] = 3; n32_[0x2] = s32_[0x2] = 6;     // EWRAM: barramento de 16 bits, 2 esperas
    for (int r : {0x5, 0x6}) { n32_[r] = s32_[r] = 2; }       // paleta e VRAM: barramento de 16 bits
    const auto rom = [this](int region, int n, int s) {      // cartucho: barramento de 16 bits
        for (int r : {region, region + 1}) {
            n16_[r] = static_cast<uint8_t>(1 + n);
            s16_[r] = static_cast<uint8_t>(1 + s);
            n32_[r] = static_cast<uint8_t>(1 + n + 1 + s);   // duas metades: N + S
            s32_[r] = static_cast<uint8_t>(2 * (1 + s));
        }
    };
    rom(0x8, kN[(w >> 2) & 3], (w & 0x0010) ? 1 : 2);        // WS0
    rom(0xA, kN[(w >> 5) & 3], (w & 0x0080) ? 1 : 4);        // WS1
    rom(0xC, kN[(w >> 8) & 3], (w & 0x0400) ? 1 : 8);        // WS2 (inclui a EEPROM)
    for (int r : {0xE, 0xF}) n16_[r] = s16_[r] = n32_[r] = s32_[r] = static_cast<uint8_t>(1 + kN[w & 3]); // SRAM
    prefetchEnabled_ = w & 0x4000;
    if (!prefetchEnabled_) { prefetchActive_ = false; prefetchCount_ = 0; prefetchProgress_ = 0; }
}

// O prefetch do cartucho lê as próximas halfwords enquanto a CPU faz outra coisa (acessa a RAM ou
// executa ciclos internos), ao ritmo de um acesso sequencial do cartucho, até encher 8 halfwords.
void Bus::advancePrefetch(int cycles) const {
    if (!prefetchActive_ || prefetchCount_ >= 8) return;
    const int region = static_cast<int>((prefetchHead_ >> 24) & 0xF);
    const int step = s16_[region];
    prefetchProgress_ += cycles;
    while (prefetchCount_ < 8 && prefetchProgress_ >= step) {
        prefetchProgress_ -= step;
        ++prefetchCount_;
    }
    if (prefetchCount_ >= 8) prefetchProgress_ = 0;
}

void Bus::fireDma() const {
    dmaCountdown_ = -1;
    if (dmaKickHook_) dmaKickHook_();
}

void Bus::idle(int cycles) const {
    if (cycles <= 0) return;
    if (dmaCountdown_ == 0) fireDma();
    cycles_ += cycles;
    advancePrefetch(cycles);
    nextSequential_ = 0xFFFFFFFF; // depois de ciclos internos, o próximo acesso é não sequencial
    countDownDma(cycles);
}

// Acesso da CPU: se um DMA imediato já deveria ter começado, ele roda antes; depois o acesso é cobrado
// e o tempo dele é descontado da espera de um DMA recém-ligado.
void Bus::account(uint32_t addr, int width, bool code) const {
    if (dmaCountdown_ == 0) fireDma();
    const int before = cycles_;
    accountCpu(addr, width, code);
    countDownDma(cycles_ - before);
}

void Bus::accountCpu(uint32_t addr, int width, bool code) const {
    const int region = (addr >> 28) ? 0x1 : static_cast<int>((addr >> 24) & 0xF); // >= 0x10000000: 1 ciclo
    const bool rom = region >= 0x8 && region <= 0xD;
    // Sequencial: o endereço seguinte ao do último acesso. No cartucho, cruzar um bloco de 128 KB
    // recomeça como não sequencial.
    // Mudar de região no meio de uma sequência também a quebra.
    const bool sequential = addr == nextSequential_ && !(rom && (addr & 0x1FFFF) == 0) &&
                            ((addr - static_cast<uint32_t>(width)) >> 24) == (addr >> 24);
    nextSequential_ = addr + static_cast<uint32_t>(width);
    const bool wide = width == 4;

    if (!rom) {
        const int cost = sequential ? (wide ? s32_[region] : s16_[region]) : (wide ? n32_[region] : n16_[region]);
        cycles_ += cost;
        advancePrefetch(cost);
        return;
    }
    if (code && prefetchEnabled_ && prefetchActive_ && addr == prefetchHead_) {
        // Busca de instrução que o prefetch já tinha (ou estava) lendo.
        const int needed = wide ? 2 : 1;
        prefetchHead_ = addr + static_cast<uint32_t>(width);
        if (prefetchCount_ >= needed) {
            // Já estava no buffer: 1 ciclo, e o prefetch segue lendo a próxima em paralelo.
            prefetchCount_ -= needed;
            cycles_ += 1;
            advancePrefetch(1);
        } else {
            // Ainda não chegou: espera o cartucho terminar. Durante a espera o prefetch está ocupado
            // com esta mesma instrução, então esse tempo não conta como progresso para a próxima.
            cycles_ += std::max(1, (needed - prefetchCount_) * s16_[region] - prefetchProgress_);
            prefetchCount_ = 0;
            prefetchProgress_ = 0;
        }
        return;
    }
    cycles_ += sequential ? (wide ? s32_[region] : s16_[region]) : (wide ? n32_[region] : n16_[region]);
    // Leitura de dados no cartucho com o prefetch no meio de uma halfword: ele precisa abortar a
    // leitura em andamento, o que custa 1 ciclo (ajustado contra a suíte de testes do mGBA).
    if (!code && prefetchActive_ && prefetchCount_ < 8 && prefetchProgress_ > 0) cycles_ += 1;
    if (code && prefetchEnabled_) {
        // Desvio (ou primeira busca): o buffer recomeça a partir da instrução seguinte.
        prefetchActive_ = true;
        prefetchHead_ = addr + static_cast<uint32_t>(width);
    } else {
        prefetchActive_ = false; // leitura de dados no cartucho interrompe o prefetch
    }
    prefetchCount_ = 0;
    prefetchProgress_ = 0;
}

void Bus::accountDma(uint32_t addr, bool word, bool sequential) const {
    const int region = (addr >> 28) ? 0x1 : static_cast<int>((addr >> 24) & 0xF);
    const bool rom = region >= 0x8 && region <= 0xD;
    if (rom && (addr & 0x1FFFF) == 0) sequential = false; // bloco de 128 KB do cartucho
    cycles_ += sequential ? (word ? s32_[region] : s16_[region]) : (word ? n32_[region] : n16_[region]);
}

uint32_t Bus::dmaLoad(uint32_t a, bool word, bool sequential) const {
    accountDma(a, word, sequential);
    return word ? read32(a) : read16(a);
}

void Bus::dmaStore(uint32_t a, uint32_t v, bool word, bool sequential) {
    accountDma(a, word, sequential);
    if (word) write32(a, v);
    else write16(a, static_cast<uint16_t>(v));
}

void Bus::serializeTiming(StateArchive& ar) {
    ar.value(cycles_);
    ar.value(nextSequential_);
    ar.value(prefetchActive_);
    ar.value(prefetchHead_);
    ar.value(prefetchCount_);
    ar.value(prefetchProgress_);
    if (ar.loading()) updateWaitstates();
}

void Bus::serializeDmaSchedule(StateArchive& ar) {
    ar.value(dmaCountdown_);
}

uint16_t Bus::fetch16(uint32_t addr) const {
    account(addr, 2, true);
    fetching_ = true;
    const uint16_t v = read16(addr);
    fetching_ = false;
    lastFetch_ = v | (static_cast<uint32_t>(v) << 16); // Thumb: a halfword aparece duplicada
    if ((addr >> 24) == 0 && addr < 0x4000) biosOpcode_ = lastFetch_;
    return v;
}

uint32_t Bus::fetch32(uint32_t addr) const {
    account(addr, 4, true);
    fetching_ = true;
    const uint32_t v = read32(addr);
    fetching_ = false;
    lastFetch_ = v;
    if ((addr >> 24) == 0 && addr < 0x4000) biosOpcode_ = v;
    return v;
}


void Bus::probeRead(uint32_t addr, uint8_t value) const {
    const uint32_t region = addr >> 24;
    ProbeKind kind;
    if (region == 0x00 && addr < 0x4000) {
        kind = ProbeKind::BiosRead;
    } else if (region == 0x01 || region >= 0x10 || (region == 0x00) || (region == 0x04 && (addr & 0xFFFFFF) >= 0x400)) {
        kind = ProbeKind::OpenBusRead;
    } else if (region == 0x04) {
        const uint32_t off = addr & 0x3FF;
        if (off >= 0x100 && off <= 0x10F && (off & 3) < 2) kind = ProbeKind::TimerRead;
        else if (isWriteOnlyIo(off)) kind = ProbeKind::WriteOnlyIoRead;
        else if (!isReadableIo(off)) kind = ProbeKind::UnusedIoRead;
        else return;
    } else {
        return;
    }
    if (probes_.size() < 256) probes_.push_back({kind, addr, value});
}

uint8_t Bus::peek8(uint32_t addr) const {
    if ((addr >> 24) == 0x00 && addr < bios_.size()) return bios_[addr];
    return readRaw8(addr);
}

uint8_t Bus::read8(uint32_t addr) const {
    const uint8_t value = readRaw8(addr);
    if (probing_ && !fetching_) probeRead(addr, value);
    return value;
}

// O EEPROM ocupa a região 0x0D inteira em ROMs de até 16 MB; em ROMs maiores, só o fim dela.
bool Bus::isEepromAddress(uint32_t addr) const {
    if ((addr >> 24) != 0x0D || !backup_.isEeprom()) return false;
    return rom_.size() <= 16u * 1024 * 1024 || (addr & 0x00FFFFFF) >= 0x00FFFF00;
}

uint8_t Bus::readRaw8(uint32_t addr) const {
    const uint32_t region = addr >> 24;
    switch (region) {
    case 0x00:
        if (addr >= 0x4000) return openBus(addr);
        // Só a própria BIOS lê a BIOS; de fora, vem a última instrução que ela buscou.
        if (fetching_ || pcInBios_) return bios_[addr];
        return static_cast<uint8_t>(biosOpcode_ >> ((addr & 3) * 8));
    case 0x01:
        return openBus(addr);
    case 0x04:
        if ((addr & 0xFFFFFF) >= io_.size()) return openBus(addr);
        return readIo(addr & 0x3FF);
    case 0x0E: case 0x0F:
        return backup_.read(addr);
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
        const uint32_t off = addr & 0x1FFFFFF;
        if (rtc_.readable() && isGpio(addr)) return static_cast<uint8_t>(rtc_.read(off & ~1u) >> ((off & 1) * 8));
        if (off < rom_.size()) return rom_[off];
        // Além do fim da ROM: o barramento devolve o próprio endereço/2.
        const uint16_t v = static_cast<uint16_t>((addr >> 1) & 0xFFFF);
        return (addr & 1) ? static_cast<uint8_t>(v >> 8) : static_cast<uint8_t>(v);
    }
    default:
        break;
    }
    bool writable = false;
    if (const uint8_t* p = resolve(addr, writable)) return *p;
    return openBus(addr);
}

// GBATEK, "GBA Unpredictable Things": registradores só-escrita ou inexistentes leem 0 quando a
// palavra de 32 bits tem alguma parte legível, e "open bus" quando a palavra inteira não tem.
// Registradores do cabo de link (SIO), cuja leitura depende do modo do cabo. Sem cabo conectado, os dados
// "recebidos" leem 0 e os bits de status ficam no estado de repouso. Valores conferidos pela suíte do mGBA.
uint16_t Bus::sioRead16(uint32_t halfOffset) const {
    const auto raw = [this](uint32_t o) { return static_cast<uint16_t>(io_[o] | (io_[o + 1] << 8)); };
    const uint16_t rcnt = raw(0x134), siocnt = raw(0x128);
    enum class Mode { Normal8, Normal32, Multi, Uart, Gpio, Joy };
    const Mode mode = (rcnt & 0x8000) ? ((rcnt & 0x4000) ? Mode::Joy : Mode::Gpio)
                                      : static_cast<Mode>((siocnt >> 12) & 3); // 0/1/2/3 = 8 bits, 32 bits, multi, UART
    const bool normal = mode == Mode::Normal8 || mode == Mode::Normal32;
    const uint16_t v = raw(halfOffset);
    switch (halfOffset) {
    case 0x120: case 0x122: return mode == Mode::Normal32 ? v : 0; // SIODATA32: só no modo de 32 bits
    case 0x124: case 0x126: return 0;                              // SIOMULTI2/3: dados recebidos
    case 0x128:                                                    // SIOCNT
        if (mode == Mode::Multi) return static_cast<uint16_t>((v & 0x4F83) | (v & 0x3000) | 0x000C); // SI/SD em 1
        if (mode == Mode::Uart) return static_cast<uint16_t>((v & 0x4F8F) | 0x3000 | 0x0020);       // recepção vazia
        return static_cast<uint16_t>((v & 0x4F8B) | (v & 0x3000) | 0x0004);                         // SI em 1
    case 0x12A: return mode == Mode::Uart ? 0 : v;                 // SIODATA8 / SIOMLT_SEND
    case 0x134:                                                    // RCNT: bits 9-13 não existem
        if (normal) return static_cast<uint16_t>(v & 0xC1F5);
        if (mode == Mode::Joy) return static_cast<uint16_t>(v & 0xC1FC);
        return static_cast<uint16_t>(v & 0xC1FF);
    case 0x140: return static_cast<uint16_t>(v & 0x0040);          // JOYCNT: bits 0-2 são "escreva 1 para limpar"
    case 0x150: case 0x152: case 0x154: case 0x156: return 0;      // JOY_RECV / JOY_TRANS
    default: return v;
    }
}

uint8_t Bus::readIo(uint32_t offset) const {
    if (offset >= 0x100 && offset <= 0x10F && timerSyncHook_) timerSyncHook_();
    if (offset == 0x130) return static_cast<uint8_t>(keyInput_);       // KEYINPUT vem do teclado,
    if (offset == 0x131) return static_cast<uint8_t>(keyInput_ >> 8);  // não da memória de I/O
    if ((offset >= 0x120 && offset < 0x12C) || offset == 0x134 || offset == 0x135 || (offset >= 0x140 && offset < 0x142) ||
        (offset >= 0x150 && offset < 0x158)) {
        return static_cast<uint8_t>(sioRead16(offset & ~1u) >> ((offset & 1) * 8));
    }
    if (isReadableIo(offset)) {
        const uint16_t mask = ioReadMask(offset & ~1u);
        return static_cast<uint8_t>(io_[offset] & ((offset & 1) ? (mask >> 8) : mask));
    }
    return wordHasReadable(offset) ? 0 : openBus(offset);
}

// Atalhos para as regiões mais acessadas (ROM, EWRAM, IWRAM): lê a halfword/palavra de uma vez,
// sem passar byte a byte pelo caminho geral. As demais regiões seguem pelo caminho completo.
const uint8_t* Bus::fastPointer(uint32_t addr, uint32_t size) const {
    switch (addr >> 24) {
    case 0x02: return &ewram_[addr & 0x3FFFF];
    case 0x03: return &iwram_[addr & 0x7FFF];
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: {
        const uint32_t off = addr & 0x1FFFFFF;
        if (rtc_.readable() && (addr >> 24) == 0x08 && off < 0xCA && off + size > 0xC4) return nullptr; // porta do RTC
        return off + size <= rom_.size() ? &rom_[off] : nullptr;
    }
    default: return nullptr; // 0x0D fica de fora: pode ser o EEPROM
    }
}

uint16_t Bus::read16(uint32_t addr) const {
    if (const uint8_t* p = fastPointer(addr & ~1u, 2)) return bytesToLe16(p);
    if (isEepromAddress(addr)) return backup_.eepromRead(); // um bit por halfword
    if ((addr >> 24) >= 0x0E) {
        // SRAM tem barramento de 8 bits: o byte se repete nas duas metades.
        const uint8_t b = read8(addr);
        return static_cast<uint16_t>(b * 0x0101);
    }
    addr &= ~1u; // o hardware força alinhamento
    return static_cast<uint16_t>(read8(addr) | (read8(addr + 1) << 8));
}

uint32_t Bus::read32(uint32_t addr) const {
    if (const uint8_t* p = fastPointer(addr & ~3u, 4)) return bytesToLe32(p);
    if ((addr >> 24) >= 0x0E) {
        const uint8_t b = read8(addr);
        return b * 0x01010101u;
    }
    addr &= ~3u;
    return static_cast<uint32_t>(read16(addr)) | (static_cast<uint32_t>(read16(addr + 2)) << 16);
}

void Bus::write8(uint32_t addr, uint8_t value) {
    const uint32_t region = addr >> 24;

    // Escritas de 8 bits têm regras especiais no GBA:
    if (region == 0x05) {
        // Paleta: o byte é gravado nas duas metades da halfword.
        const uint32_t off = (addr & 0x3FF) & ~1u;
        palette_[off] = palette_[off + 1] = value;
        return;
    }
    if (region == 0x07) return; // OAM ignora escritas de 8 bits
    if (region == 0x06) {
        // VRAM: só a área de background aceita; a área de sprites ignora.
        const uint16_t mode = ioRead16(kDispcnt) & 7;
        const uint32_t objStart = mode >= 3 ? 0x14000 : 0x10000;
        const uint32_t off = vramOffset(addr) & ~1u;
        if (off >= objStart) return;
        vram_[off] = vram_[off + 1] = value;
        return;
    }

    if (region == 0x04) {
        if ((addr & 0xFFFFFF) < io_.size()) ioWrite8(addr & 0x3FF, value);
        return;
    }
    if (region == 0x0E || region == 0x0F) {
        backup_.write(addr, value);
        return;
    }

    if (uint8_t* p = resolveMut(addr)) *p = value;
}

void Bus::ioWrite8(uint32_t offset, uint8_t value) {
    if (offset >= 0x100 && offset <= 0x10F && timerSyncHook_) timerSyncHook_();
    if (probing_ && (offset == 0x204 || offset == 0x205) && probes_.size() < 256) {
        probes_.push_back({ProbeKind::WaitcntWrite, 0x04000000 + offset, value});
    }
    switch (offset) {
    case 0x006: case 0x007: return;                       // VCOUNT: só leitura
    case 0x130: case 0x131: return;                       // KEYINPUT: só leitura
    case 0x084:                                           // SOUNDCNT_X: só o bit 7 (liga o som) é gravável
        io_[offset] = static_cast<uint8_t>((io_[offset] & 0x0F) | (value & 0x80));
        if (!(value & 0x80)) {
            // Som desligado: os registradores dos canais e o SOUNDCNT_L são zerados (GBATEK).
            for (uint32_t i = 0x060; i <= 0x081; ++i) io_[i] = 0;
        }
        break;
    case 0x085:
        break;
    case 0x004:                                           // DISPSTAT: bits 0-2 são do hardware
        io_[offset] = static_cast<uint8_t>((io_[offset] & 0x07) | (value & 0xF8));
        break;
    case 0x202: case 0x203:                               // IF: escrever 1 confirma (apaga)
        io_[offset] &= static_cast<uint8_t>(~value);
        break;
    default:
        // Com o som desligado, os registradores 060-081 ignoram escritas.
        if (offset >= 0x060 && offset <= 0x081 && !(io_[0x084] & 0x80)) return;
        io_[offset] = value;
        break;
    }
    if (offset == 0x204 || offset == 0x205) updateWaitstates();
    if (ioWriteHook_) ioWriteHook_(offset);
}

void Bus::ioWrite16Raw(uint32_t offset, uint16_t value) {
    offset &= 0x3FE;
    io_[offset] = static_cast<uint8_t>(value);
    io_[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void Bus::requestInterrupt(uint16_t bits) {
    ioWrite16Raw(0x202, static_cast<uint16_t>(ioRead16(0x202) | bits));
}

bool Bus::irqPending() const {
    return (ioRead16(0x208) & 1) && (ioRead16(0x200) & ioRead16(0x202) & 0x3FFF);
}

void Bus::write16(uint32_t addr, uint16_t value) {
    if (isGpio(addr)) { rtc_.write(addr & 0xFE, value); return; } // porta do relógio do cartucho
    if ((addr >> 24) == 0x02 || (addr >> 24) == 0x03) {
        le16ToBytes(const_cast<uint8_t*>(fastPointer(addr & ~1u, 2)), value);
        return;
    }
    if (isEepromAddress(addr)) { backup_.eepromWrite(value); return; }
    if ((addr >> 24) >= 0x0E) {
        // SRAM (8 bits): grava só o byte da halfword que cai no endereço exato.
        write8(addr, static_cast<uint8_t>(value >> ((addr & 1) * 8)));
        return;
    }
    addr &= ~1u;
    if ((addr >> 24) == 0x04) { // I/O: cada byte passa pelas regras do registrador
        write8(addr, static_cast<uint8_t>(value));
        write8(addr + 1, static_cast<uint8_t>(value >> 8));
        return;
    }
    // Para escritas de 16/32 bits não há duplicação; gravamos os bytes direto.
    if (uint8_t* p = resolveMut(addr)) {
        p[0] = static_cast<uint8_t>(value);
        if (uint8_t* q = resolveMut(addr + 1)) *q = static_cast<uint8_t>(value >> 8);
    }
}

void Bus::write32(uint32_t addr, uint32_t value) {
    if ((addr >> 24) == 0x02 || (addr >> 24) == 0x03) {
        le32ToBytes(const_cast<uint8_t*>(fastPointer(addr & ~3u, 4)), value);
        return;
    }
    if ((addr >> 24) >= 0x0E) {
        write8(addr, static_cast<uint8_t>(value >> ((addr & 3) * 8)));
        return;
    }
    addr &= ~3u;
    write16(addr, static_cast<uint16_t>(value));
    write16(addr + 2, static_cast<uint16_t>(value >> 16));
}

uint32_t Bus::ioRead32(uint32_t offset) const {
    offset &= 0x3FC;
    return static_cast<uint32_t>(ioRead16(offset)) | (static_cast<uint32_t>(ioRead16(offset + 2)) << 16);
}

uint16_t Bus::ioRead16(uint32_t offset) const {
    offset &= 0x3FE;
    return static_cast<uint16_t>(io_[offset] | (io_[offset + 1] << 8));
}

} // namespace gba

namespace gba {

void Bus::serialize(StateArchive& ar) {
    ar.value(bios_);   // inclui o tratador de IRQ HLE gravado na BIOS
    ar.value(ewram_);
    ar.value(iwram_);
    ar.value(io_);
    ar.value(palette_);
    ar.value(vram_);
    ar.value(oam_);
    ar.value(keyInput_);
    ar.value(pcInBios_);
    ar.value(biosOpcode_);
    ar.value(lastFetch_);
    backup_.serialize(ar);
    if (ar.loading()) probes_.clear();
    // O relógio do cartucho é serializado pelo Gba (a partir da versão 2 do formato).
}

} // namespace gba
