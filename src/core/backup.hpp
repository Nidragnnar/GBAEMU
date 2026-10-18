#pragma once
// Memória de save do cartucho na região 0x0E000000 (GBATEK, "GBA Cart Backup").
// SRAM é acessada diretamente; Flash é um chip que recebe comandos (identificar, gravar, apagar).

#include <cstdint>
#include <string>
#include <vector>
#include "state.hpp"

namespace gba {

class Backup {
public:
    enum class Type { None, Sram, Flash64K, Flash128K, Eeprom };

    // Jogos oficiais carregam na ROM uma assinatura da biblioteca de save usada.
    static Type detect(const std::vector<uint8_t>& rom);
    static const char* name(Type type);

    void setType(Type type);
    Type type() const { return type_; }

    uint8_t read(uint32_t addr) const;
    void write(uint32_t addr, uint8_t value);

    // EEPROM: acesso serial, um bit por halfword, sempre por DMA na região 0x0D000000.
    bool isEeprom() const { return type_ == Type::Eeprom; }
    uint16_t eepromRead();             // próximo bit da resposta (1 = pronto, quando não há leitura pendente)
    void eepromWrite(uint16_t value);  // próximo bit do comando (bit 0)
    void eepromDmaHint(uint32_t units); // tamanho de uma transferência por DMA: revela 512 bytes ou 8 KB

    // Conteúdo para o arquivo .sav.
    const std::vector<uint8_t>& data() const { return data_; }
    bool load(const std::vector<uint8_t>& saved); // false se o tamanho não bate com o tipo
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    // Estado do protocolo (comandos em andamento). O conteúdo do save fica de fora: ele pertence ao cartucho.
    void serialize(StateArchive& ar);

    // Contadores para diagnóstico.
    struct Stats { uint64_t reads = 0, writes = 0, idRequests = 0, idReads = 0; };
    const Stats& stats() const { return stats_; }

private:
    bool isFlash() const { return type_ == Type::Flash64K || type_ == Type::Flash128K; }
    void flashWrite(uint32_t addr, uint8_t value);

    Type type_ = Type::None;
    std::vector<uint8_t> data_;
    bool dirty_ = false;

    // Máquina de comandos da Flash: AA em 5555, 55 em 2AAA, depois o comando em 5555.
    enum class Stage { Ready, GotAA, Got55 } stage_ = Stage::Ready;
    bool idMode_ = false;     // leituras em 0 e 1 devolvem fabricante e dispositivo
    bool eraseArmed_ = false; // recebeu 0x80: o próximo comando é um apagamento
    bool writeArmed_ = false; // recebeu 0xA0: a próxima escrita grava um byte
    bool bankArmed_ = false;  // recebeu 0xB0: a próxima escrita em 0 escolhe o banco de 64 KB
    uint32_t bank_ = 0;

    // EEPROM
    int eepromAddrBits_ = 0;           // 6 (512 bytes) ou 14 (8 KB); 0 = ainda não sabemos
    std::vector<uint8_t> eepromBits_;  // comando sendo recebido
    std::vector<uint8_t> eepromReply_; // resposta de leitura ainda não entregue
    size_t eepromReplyPos_ = 0;
    void setEepromSize(int addrBits);
    void eepromProcess();
    mutable Stats stats_;
};

} // namespace gba
