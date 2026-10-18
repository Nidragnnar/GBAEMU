#pragma once
// Barramento de memória do GBA.
// Mapa (GBATEK, "GBA Memory Map"):
//   0x00000000  BIOS      16 KB
//   0x02000000  EWRAM    256 KB  (espelhada a cada 256 KB)
//   0x03000000  IWRAM     32 KB  (espelhada a cada 32 KB)
//   0x04000000  I/O        1 KB  (registradores de hardware)
//   0x05000000  Paleta     1 KB  (espelhada)
//   0x06000000  VRAM      96 KB  (espelho de 128 KB; 0x18000-0x1FFFF repete 0x10000-0x17FFF)
//   0x07000000  OAM        1 KB  (espelhada)
//   0x08000000  ROM   até 32 MB  (3 janelas: 0x08, 0x0A, 0x0C)
//   0x0E000000  Save (SRAM ou Flash, barramento de 8 bits) — ver backup.hpp

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "backup.hpp"
#include "state.hpp"

namespace gba {

// Bits de interrupção (registradores IE e IF).
namespace irq {
constexpr uint16_t VBlank = 1 << 0;
constexpr uint16_t HBlank = 1 << 1;
constexpr uint16_t VCount = 1 << 2;
constexpr uint16_t Timer0 = 1 << 3; // Timer0 << n para o timer n
constexpr uint16_t Dma0   = 1 << 8; // Dma0 << n para o canal n
constexpr uint16_t Keypad = 1 << 12;
} // namespace irq

// Sonda de diagnóstico: acessos que costumam revelar emuladores imprecisos.
enum class ProbeKind : uint8_t {
    BiosRead,        // leitura de dados na BIOS
    OpenBusRead,     // leitura de endereço não mapeado
    WriteOnlyIoRead, // leitura de registrador que só aceita escrita
    UnusedIoRead,    // leitura de endereço de I/O sem registrador
    TimerRead,       // leitura de contador de timer (medição de tempo)
    WaitcntWrite,    // configuração de waitstates
};
struct Probe { ProbeKind kind; uint32_t addr; uint32_t value; };

class Bus {
public:
    Bus();
    Bus(const Bus&) = delete;            // as referências internas apontam para a própria memória
    Bus& operator=(const Bus&) = delete;

    void loadBios(std::vector<uint8_t> data);
    // Sem BIOS real: grava em 0x18 o tratador de IRQ da BIOS original, que chama o do jogo.
    void installHleBiosStub();
    void loadRom(std::vector<uint8_t> data); // também detecta o tipo de save
    Backup& backup() { return backup_; }
    void serialize(StateArchive& ar);

    // Busca de instruções (não passa pela sonda).
    uint16_t fetch16(uint32_t addr) const;
    uint32_t fetch32(uint32_t addr) const;

    // Proteção da BIOS: fora dela, ler a BIOS devolve a última instrução que a BIOS buscou.
    void setPcInBios(bool inBios) { pcInBios_ = inBios; }
    void setBiosOpcode(uint32_t opcode) { biosOpcode_ = opcode; }
    uint32_t biosOpcode() const { return biosOpcode_; }

    void setProbing(bool on) { probing_ = on; }
    bool hasProbes() const { return !probes_.empty(); }
    std::vector<Probe> takeProbes() { return std::exchange(probes_, {}); }

    uint8_t  read8(uint32_t addr) const;
    uint16_t read16(uint32_t addr) const;
    uint32_t read32(uint32_t addr) const;

    void write8(uint32_t addr, uint8_t value);
    void write16(uint32_t addr, uint16_t value);
    void write32(uint32_t addr, uint32_t value);

    // Acesso direto para o PPU (evita passar pelo barramento a cada pixel).
    const std::array<uint8_t, 96 * 1024>& vram() const { return vram_; }
    const std::array<uint8_t, 1024>& palette() const { return palette_; }
    const std::array<uint8_t, 1024>& oam() const { return oam_; }
    uint16_t ioRead16(uint32_t offset) const;

    // Escrita direta em I/O pelo hardware (ignora as proteções aplicadas às escritas da CPU).
    void ioWrite16Raw(uint32_t offset, uint16_t value);
    void requestInterrupt(uint16_t bits);
    uint32_t ioRead32(uint32_t offset) const;

    // Avisado depois de cada byte que a CPU grava em I/O (DMA e PPU reagem a alguns registradores).
    void setIoWriteHook(std::function<void(uint32_t offset)> hook) { ioWriteHook_ = std::move(hook); }
    bool irqPending() const; // IME ligado e algum bit em IE & IF

    // KEYINPUT (0x04000130): bits em 0 = botão pressionado. Padrão 0x03FF = nada pressionado.
    void setKeyInput(uint16_t value) { keyInput_ = value & 0x03FF; }

private:
    // Retorna ponteiro para o byte da região, ou nullptr se não mapeado.
    // `writable` informa se a região aceita escrita da CPU.
    const uint8_t* resolve(uint32_t addr, bool& writable) const;
    uint8_t* resolveMut(uint32_t addr);

    static uint32_t vramOffset(uint32_t addr);
    void ioWrite8(uint32_t offset, uint8_t value); // escrita da CPU em I/O, com as regras de cada registrador

    // As memórias somam ~400 KB: ficam no heap para o console poder ser criado em qualquer lugar
    // (a pilha do Windows tem só 1 MB). As referências abaixo mantêm o acesso direto e simples.
    struct Memory {
        std::array<uint8_t, 16 * 1024>  bios{};
        std::array<uint8_t, 256 * 1024> ewram{};
        std::array<uint8_t, 32 * 1024>  iwram{};
        std::array<uint8_t, 1024>       io{};
        std::array<uint8_t, 1024>       palette{};
        std::array<uint8_t, 96 * 1024>  vram{};
        std::array<uint8_t, 1024>       oam{};
    };
    std::unique_ptr<Memory> memory_ = std::make_unique<Memory>();
    std::array<uint8_t, 16 * 1024>&  bios_ = memory_->bios;
    std::array<uint8_t, 256 * 1024>& ewram_ = memory_->ewram;
    std::array<uint8_t, 32 * 1024>&  iwram_ = memory_->iwram;
    std::array<uint8_t, 1024>&       io_ = memory_->io;
    std::array<uint8_t, 1024>&       palette_ = memory_->palette;
    std::array<uint8_t, 96 * 1024>&  vram_ = memory_->vram;
    std::array<uint8_t, 1024>&       oam_ = memory_->oam;
    std::vector<uint8_t>            rom_;
    mutable Backup                  backup_;
    bool isEepromAddress(uint32_t addr) const;
    const uint8_t* fastPointer(uint32_t addr, uint32_t size) const;
    uint16_t                        keyInput_ = 0x03FF;
    bool                            probing_ = false;
    mutable bool                    fetching_ = false;
    bool                            pcInBios_ = false;
    mutable uint32_t                biosOpcode_ = 0xE129F000; // valor depois da inicialização (GBATEK)
    mutable uint32_t                lastFetch_ = 0;           // "open bus": o que acabou de passar pelo barramento
    uint8_t openBus(uint32_t addr) const { return static_cast<uint8_t>(lastFetch_ >> ((addr & 3) * 8)); }
    uint8_t readIo(uint32_t offset) const;
    mutable std::vector<Probe>      probes_;
    void probeRead(uint32_t addr, uint8_t value) const;
    uint8_t readRaw8(uint32_t addr) const;
    std::function<void(uint32_t)>   ioWriteHook_;
};

} // namespace gba
