#include "cartridge.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace gba {

namespace {
std::string asciiField(const std::vector<uint8_t>& rom, size_t off, size_t len) {
    std::string s;
    for (size_t i = 0; i < len; ++i) {
        const char c = static_cast<char>(rom[off + i]);
        if (c == '\0') break;
        s.push_back((c >= 0x20 && c < 0x7F) ? c : '?');
    }
    return s;
}
} // namespace

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Não foi possível abrir: " + path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void writeFileSafely(const std::string& path, const std::vector<uint8_t>& data) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("Não foi possível gravar: " + tmp);
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!f) throw std::runtime_error("Falha ao gravar: " + tmp);
    }
    std::filesystem::rename(tmp, path); // substitui o arquivo antigo de uma vez
}

CartridgeHeader parseHeader(const std::vector<uint8_t>& rom) {
    if (rom.size() < 0xC0) throw std::runtime_error("ROM menor que o cabeçalho (192 bytes)");

    CartridgeHeader h;
    h.title = asciiField(rom, 0xA0, 12);
    h.gameCode = asciiField(rom, 0xAC, 4);
    h.makerCode = asciiField(rom, 0xB0, 2);
    h.version = rom[0xBC];
    h.checksum = rom[0xBD];
    h.fixedValueOk = rom[0xB2] == 0x96;

    // Complemento: -(soma de 0xA0..0xBC) - 0x19, truncado para 8 bits.
    uint8_t sum = 0;
    for (size_t i = 0xA0; i <= 0xBC; ++i) sum = static_cast<uint8_t>(sum - rom[i]);
    sum = static_cast<uint8_t>(sum - 0x19);
    h.checksumOk = sum == h.checksum;
    return h;
}

} // namespace gba
