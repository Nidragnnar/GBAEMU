#include "backup.hpp"

#include <algorithm>
#include <cstring>

namespace gba {

namespace {
constexpr uint8_t kErased = 0xFF; // memória apagada lê 0xFF

bool contains(const std::vector<uint8_t>& rom, const char* text) {
    const size_t len = std::strlen(text);
    return std::search(rom.begin(), rom.end(), text, text + len) != rom.end();
}
} // namespace

Backup::Type Backup::detect(const std::vector<uint8_t>& rom) {
    if (contains(rom, "FLASH1M_V")) return Type::Flash128K;
    if (contains(rom, "FLASH512_V") || contains(rom, "FLASH_V")) return Type::Flash64K;
    if (contains(rom, "SRAM_V") || contains(rom, "SRAM_F_V")) return Type::Sram;
    if (contains(rom, "EEPROM_V")) return Type::Eeprom;
    return Type::Sram; // sem assinatura (homebrew): SRAM é o palpite mais seguro
}

const char* Backup::name(Type type) {
    switch (type) {
    case Type::Sram: return "SRAM 32 KB";
    case Type::Flash64K: return "Flash 64 KB";
    case Type::Flash128K: return "Flash 128 KB";
    case Type::Eeprom: return "EEPROM (512 bytes ou 8 KB)";
    default: return "nenhum";
    }
}

void Backup::setType(Type type) {
    type_ = type;
    size_t size = 0;
    switch (type) {
    case Type::Sram: size = 32 * 1024; break;
    case Type::Flash64K: size = 64 * 1024; break;
    case Type::Flash128K: size = 128 * 1024; break;
    case Type::Eeprom: size = 8 * 1024; break; // encolhe para 512 se o jogo usar endereços de 6 bits
    default: break;
    }
    data_.assign(size, kErased);
    stage_ = Stage::Ready;
    idMode_ = eraseArmed_ = writeArmed_ = bankArmed_ = false;
    bank_ = 0;
    dirty_ = false;
    eepromAddrBits_ = 0;
    eepromBits_.clear();
    eepromReply_.clear();
    eepromReplyPos_ = 0;
}

bool Backup::load(const std::vector<uint8_t>& saved) {
    if (type_ == Type::Eeprom && (saved.size() == 512 || saved.size() == 8 * 1024)) {
        setEepromSize(saved.size() == 512 ? 6 : 14); // o tamanho do arquivo revela o modelo
        data_ = saved;
        dirty_ = false;
        return true;
    }
    if (saved.size() != data_.size() || data_.empty()) return false;
    data_ = saved;
    dirty_ = false;
    return true;
}

uint8_t Backup::read(uint32_t addr) const {
    ++stats_.reads;
    const uint32_t a = addr & 0xFFFF;
    switch (type_) {
    case Type::Sram:
        return data_[a & 0x7FFF];
    case Type::Flash64K:
    case Type::Flash128K:
        if (idMode_ && a < 2) {
            // Flash 128 KB: Macronix MX29L010 (C2h, 09h). Flash 64 KB: Panasonic MN63F805MNP (32h, 1Bh).
            ++stats_.idReads;
            if (type_ == Type::Flash128K) return a == 0 ? 0xC2 : 0x09;
            return a == 0 ? 0x32 : 0x1B;
        }
        return data_[bank_ * 0x10000 + a];
    default:
        return 0xFF;
    }
}

void Backup::write(uint32_t addr, uint8_t value) {
    ++stats_.writes;
    if (type_ == Type::Sram) {
        data_[addr & 0x7FFF] = value;
        dirty_ = true;
    } else if (isFlash()) {
        flashWrite(addr & 0xFFFF, value);
    }
}

void Backup::flashWrite(uint32_t a, uint8_t value) {
    if (writeArmed_) { // gravação de um byte (a gravação real é instantânea aqui)
        writeArmed_ = false;
        data_[bank_ * 0x10000 + a] = value;
        dirty_ = true;
        return;
    }
    if (bankArmed_) {
        bankArmed_ = false;
        if (a == 0) bank_ = value & 1;
        return;
    }

    switch (stage_) {
    case Stage::Ready:
        if (a == 0x5555 && value == 0xAA) stage_ = Stage::GotAA;
        else if (value == 0xF0) idMode_ = false; // alguns chips aceitam "sair" sem o prefixo
        return;
    case Stage::GotAA:
        stage_ = (a == 0x2AAA && value == 0x55) ? Stage::Got55 : Stage::Ready;
        return;
    case Stage::Got55:
        stage_ = Stage::Ready;
        break;
    }

    if (eraseArmed_) {
        eraseArmed_ = false;
        if (a == 0x5555 && value == 0x10) {          // apagar o chip inteiro
            std::fill(data_.begin(), data_.end(), kErased);
            dirty_ = true;
        } else if (value == 0x30) {                  // apagar o setor de 4 KB que contém o endereço
            const auto start = data_.begin() + bank_ * 0x10000 + (a & 0xF000);
            std::fill(start, start + 0x1000, kErased);
            dirty_ = true;
        }
        return;
    }
    if (a != 0x5555) return;

    switch (value) {
    case 0x90: idMode_ = true; ++stats_.idRequests; break; // entrar no modo de identificação
    case 0xF0: idMode_ = false; break;   // sair do modo de identificação
    case 0x80: eraseArmed_ = true; break;
    case 0xA0: writeArmed_ = true; break;
    case 0xB0: bankArmed_ = type_ == Type::Flash128K; break;
    default: break;
    }
}

// ---------------------------------------------------------------- EEPROM (GBATEK, "GBA Cart EEPROM")

void Backup::setEepromSize(int addrBits) {
    if (eepromAddrBits_ == addrBits) return;
    eepromAddrBits_ = addrBits;
    data_.resize(addrBits == 6 ? 512 : 8 * 1024, kErased);
}

// O jogo sempre manda um comando inteiro numa única transferência por DMA. Os comprimentos possíveis:
// pedido de leitura 9 (6 bits de endereço) ou 17 (14 bits); gravação 73 ou 81.
void Backup::eepromDmaHint(uint32_t units) {
    if (type_ != Type::Eeprom || eepromAddrBits_ != 0) return;
    if (units == 9 || units == 73) setEepromSize(6);
    else if (units == 17 || units == 81) setEepromSize(14);
}

void Backup::eepromWrite(uint16_t value) {
    eepromBits_.push_back(static_cast<uint8_t>(value & 1));
    eepromProcess();
}

void Backup::eepromProcess() {
    const int addrBits = eepromAddrBits_ ? eepromAddrBits_ : 14;
    const size_t n = eepromBits_.size();
    if (n < 2) return;
    const auto address = [&] {
        uint32_t a = 0;
        for (int i = 0; i < addrBits; ++i) a = (a << 1) | eepromBits_[2 + static_cast<size_t>(i)];
        return (a & 0x3FF) * 8; // blocos de 8 bytes; no modelo de 8 KB só os 10 bits baixos contam
    };
    const bool read = eepromBits_[0] == 1 && eepromBits_[1] == 1;
    const bool write = eepromBits_[0] == 1 && eepromBits_[1] == 0;

    if (read && n == static_cast<size_t>(2 + addrBits + 1)) {
        // Resposta: 4 bits descartáveis e os 64 bits do bloco, do bit mais alto do primeiro byte em diante.
        const uint32_t base = address() % data_.size();
        eepromReply_.assign(4, 0);
        for (uint32_t i = 0; i < 8; ++i) {
            for (int bit = 7; bit >= 0; --bit) eepromReply_.push_back(static_cast<uint8_t>((data_[base + i] >> bit) & 1));
        }
        eepromReplyPos_ = 0;
        eepromBits_.clear();
    } else if (write && n == static_cast<size_t>(2 + addrBits + 64 + 1)) {
        const uint32_t base = address() % data_.size();
        for (uint32_t i = 0; i < 8; ++i) {
            uint8_t byte = 0;
            for (int bit = 0; bit < 8; ++bit) byte = static_cast<uint8_t>((byte << 1) | eepromBits_[2 + static_cast<size_t>(addrBits) + i * 8 + static_cast<size_t>(bit)]);
            data_[base + i] = byte;
        }
        dirty_ = true;
        eepromBits_.clear();
    } else if (!read && !write) {
        eepromBits_.clear(); // comando desconhecido: descarta
    }
}

uint16_t Backup::eepromRead() {
    if (eepromReplyPos_ < eepromReply_.size()) return eepromReply_[eepromReplyPos_++];
    return 1; // sem leitura pendente: "pronto" (a gravação aqui é instantânea)
}

} // namespace gba

namespace gba {

void Backup::serialize(StateArchive& ar) {
    Type type = type_;
    ar.value(type);
    if (ar.loading() && type != type_) throw std::runtime_error("save state de outro tipo de cartucho");
    ar.value(stage_);
    ar.value(idMode_);
    ar.value(eraseArmed_);
    ar.value(writeArmed_);
    ar.value(bankArmed_);
    ar.value(bank_);
    int addrBits = eepromAddrBits_;
    ar.value(addrBits);
    if (ar.loading() && addrBits) setEepromSize(addrBits);
    ar.vector(eepromBits_, 256);
    ar.vector(eepromReply_, 256);
    uint64_t replyPos = eepromReplyPos_;
    ar.value(replyPos);
    eepromReplyPos_ = static_cast<size_t>(replyPos);
}

} // namespace gba
