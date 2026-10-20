#include "png.hpp"

#include <algorithm>
#include <cstring>

#include "patch.hpp" // crc32

namespace gba {

namespace {

void putBe32(std::vector<uint8_t>& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
}

// Um "chunk" do PNG: tamanho, tipo (4 letras), dados e o CRC32 do tipo + dados.
void putChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    putBe32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    putBe32(out, crc32(out.data() + start, out.size() - start));
}

uint32_t adler32(const std::vector<uint8_t>& data) {
    uint32_t a = 1, b = 0;
    for (const uint8_t byte : data) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

// Fluxo zlib com blocos "armazenados" do deflate (até 65535 bytes cada, sem compressão).
std::vector<uint8_t> zlibStored(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out = {0x78, 0x01}; // cabeçalho zlib: deflate, janela de 32 KB
    size_t pos = 0;
    do {
        const size_t len = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + len == raw.size();
        out.push_back(last ? 1 : 0); // BFINAL + BTYPE = 00 (armazenado)
        out.push_back(static_cast<uint8_t>(len));
        out.push_back(static_cast<uint8_t>(len >> 8));
        out.push_back(static_cast<uint8_t>(~len));
        out.push_back(static_cast<uint8_t>(~len >> 8));
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
    } while (pos < raw.size());
    putBe32(out, adler32(raw));
    return out;
}

} // namespace

std::vector<uint8_t> encodePng(const uint32_t* pixels, int width, int height) {
    // Linhas da imagem: um byte de filtro (0 = nenhum) seguido de R, G, B de cada pixel.
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 3));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        for (int x = 0; x < width; ++x) {
            const uint32_t p = pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
            raw.push_back(static_cast<uint8_t>(p >> 16));
            raw.push_back(static_cast<uint8_t>(p >> 8));
            raw.push_back(static_cast<uint8_t>(p));
        }
    }

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> header;
    putBe32(header, static_cast<uint32_t>(width));
    putBe32(header, static_cast<uint32_t>(height));
    header.insert(header.end(), {8, 2, 0, 0, 0}); // 8 bits, RGB, deflate, filtro padrão, sem entrelaçamento
    putChunk(png, "IHDR", header);
    putChunk(png, "IDAT", zlibStored(raw));
    putChunk(png, "IEND", {});
    return png;
}

} // namespace gba
