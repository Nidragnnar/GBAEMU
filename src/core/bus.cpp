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
    return off <= 0x00F || (off >= 0x048 && off <= 0x04B) || (off >= 0x050 && off <= 0x053) ||
           (off >= 0x060 && off <= 0x09F) || (off >= 0x0BA && off <= 0x0BB) || (off >= 0x0C6 && off <= 0x0C7) ||
           (off >= 0x0D2 && off <= 0x0D3) || (off >= 0x0DE && off <= 0x0DF) || (off >= 0x100 && off <= 0x10F) ||
           (off >= 0x120 && off <= 0x12F) || (off >= 0x130 && off <= 0x135) || (off >= 0x140 && off <= 0x141) ||
           (off >= 0x150 && off <= 0x159) || (off >= 0x200 && off <= 0x20B) || (off >= 0x300 && off <= 0x301);
}
// Bits que existem e podem ser lidos em cada registrador de 16 bits (GBATEK). Bits fora da máscara
// leem 0: não existem no hardware ou são só de escrita. Registradores fora da tabela: todos os bits.
uint16_t ioReadMask(uint32_t halfOffset) {
    switch (halfOffset) {
    case 0x004: return 0xFF3F; // DISPSTAT: bits 6-7 não existem
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
inline uint16_t load16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t load32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline void store16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
inline void store32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24);
}
} // namespace

namespace {
constexpr uint32_t kMaxRomSize = 32u * 1024u * 1024u;
constexpr uint32_t kDispcnt = 0x000; // offset de DISPCNT na região de I/O
} // namespace

Bus::Bus() = default;

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

uint16_t Bus::fetch16(uint32_t addr) const {
    fetching_ = true;
    const uint16_t v = read16(addr);
    fetching_ = false;
    lastFetch_ = v | (static_cast<uint32_t>(v) << 16); // Thumb: a halfword aparece duplicada
    if ((addr >> 24) == 0 && addr < 0x4000) biosOpcode_ = lastFetch_;
    return v;
}

uint32_t Bus::fetch32(uint32_t addr) const {
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
uint8_t Bus::readIo(uint32_t offset) const {
    if (offset == 0x130) return static_cast<uint8_t>(keyInput_);       // KEYINPUT vem do teclado,
    if (offset == 0x131) return static_cast<uint8_t>(keyInput_ >> 8);  // não da memória de I/O
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
    if (const uint8_t* p = fastPointer(addr & ~1u, 2)) return load16(p);
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
    if (const uint8_t* p = fastPointer(addr & ~3u, 4)) return load32(p);
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
        store16(const_cast<uint8_t*>(fastPointer(addr & ~1u, 2)), value);
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
        store32(const_cast<uint8_t*>(fastPointer(addr & ~3u, 4)), value);
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
