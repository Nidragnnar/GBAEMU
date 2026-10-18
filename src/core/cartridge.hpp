#pragma once
// Leitura do cabeçalho da ROM (GBATEK, "GBA Cartridge Header", 0x00-0xBF).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gba {

struct CartridgeHeader {
    std::string title;      // 0xA0, 12 bytes ASCII
    std::string gameCode;   // 0xAC, 4 bytes (ex.: "BPEE" = Pokémon Emerald EUA)
    std::string makerCode;  // 0xB0, 2 bytes
    uint8_t version = 0;    // 0xBC
    uint8_t checksum = 0;   // 0xBD, valor gravado na ROM
    bool checksumOk = false;
    bool fixedValueOk = false; // 0xB2 deve ser 0x96
};

// Caminhos são sempre UTF-8 (no Windows, o SDL entrega os argumentos e os arquivos arrastados assim).
std::filesystem::path utf8Path(const std::string& utf8);
std::string pathToUtf8(const std::filesystem::path& path);

std::vector<uint8_t> readFile(const std::string& path);
// Grava num arquivo temporário e renomeia: uma falha no meio nunca corrompe o arquivo existente.
void writeFileSafely(const std::string& path, const std::vector<uint8_t>& data);
CartridgeHeader parseHeader(const std::vector<uint8_t>& rom);

} // namespace gba
