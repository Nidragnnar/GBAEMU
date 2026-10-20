#include "patch.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace gba {

namespace {

// Leitor sequencial do patch, com verificação de limites (um patch corrompido não pode ler além do fim).
class Reader {
public:
    Reader(const std::vector<uint8_t>& data, size_t end) : data_(data), end_(end) {}
    bool atEnd() const { return pos_ >= end_; }
    size_t pos() const { return pos_; }
    uint8_t byte() {
        if (pos_ >= end_) throw PatchError("o patch está incompleto ou corrompido");
        return data_[pos_++];
    }
    uint32_t bigEndian(int bytes) {
        uint32_t v = 0;
        for (int i = 0; i < bytes; ++i) v = (v << 8) | byte();
        return v;
    }
    // Número de tamanho variável dos formatos UPS e BPS.
    uint64_t varint() {
        uint64_t value = 0, shift = 1;
        for (int guard = 0; guard < 10; ++guard) {
            const uint8_t x = byte();
            value += (x & 0x7F) * shift;
            if (x & 0x80) return value;
            shift <<= 7;
            value += shift;
        }
        throw PatchError("o patch está corrompido (número inválido)");
    }

private:
    const std::vector<uint8_t>& data_;
    size_t end_;
    size_t pos_ = 0;
};

uint32_t readLe32(const std::vector<uint8_t>& d, size_t at) {
    return static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) | (static_cast<uint32_t>(d[at + 2]) << 16) |
           (static_cast<uint32_t>(d[at + 3]) << 24);
}

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

// Confere os três CRC32 do final de um patch UPS/BPS: o do patch (integridade), o da ROM de origem
// (versão certa do jogo) e, depois de aplicar, o do resultado.
void checkFooter(const std::vector<uint8_t>& patch, const std::vector<uint8_t>& rom) {
    const size_t n = patch.size();
    if (crc32(patch.data(), n - 4) != readLe32(patch, n - 4)) throw PatchError("o arquivo do patch está corrompido (CRC32 não confere)");
    const uint32_t expected = readLe32(patch, n - 12);
    const uint32_t actual = crc32(rom.data(), rom.size());
    if (expected != actual) {
        throw PatchError("este patch foi feito para outra versão da ROM (CRC32 esperado " + hex(expected) + ", a ROM tem " +
                         hex(actual) + ")");
    }
}

void checkResult(const std::vector<uint8_t>& patch, const std::vector<uint8_t>& out) {
    if (crc32(out.data(), out.size()) != readLe32(patch, patch.size() - 8)) {
        throw PatchError("o resultado do patch não confere (CRC32); o patch pode estar corrompido");
    }
}

std::vector<uint8_t> applyIps(const std::vector<uint8_t>& rom, const std::vector<uint8_t>& patch) {
    std::vector<uint8_t> out = rom;
    Reader r(patch, patch.size());
    for (int i = 0; i < 5; ++i) r.byte(); // "PATCH"
    while (true) {
        const uint32_t offset = r.bigEndian(3);
        if (offset == 0x454F46) { // "EOF"
            if (patch.size() - r.pos() >= 3) out.resize(r.bigEndian(3)); // extensão: tamanho final da ROM
            return out;
        }
        uint32_t size = r.bigEndian(2);
        if (size == 0) { // repetição (RLE): quantidade de 16 bits e um byte
            size = r.bigEndian(2);
            const uint8_t value = r.byte();
            if (out.size() < offset + size) out.resize(offset + size);
            std::memset(out.data() + offset, value, size);
        } else {
            if (out.size() < offset + size) out.resize(offset + size);
            for (uint32_t i = 0; i < size; ++i) out[offset + i] = r.byte();
        }
        if (out.size() > 64u * 1024 * 1024) throw PatchError("o patch IPS gera uma ROM grande demais");
    }
}

std::vector<uint8_t> applyUps(const std::vector<uint8_t>& rom, const std::vector<uint8_t>& patch) {
    if (patch.size() < 18) throw PatchError("o patch UPS está incompleto");
    checkFooter(patch, rom);
    Reader r(patch, patch.size() - 12);
    for (int i = 0; i < 4; ++i) r.byte(); // "UPS1"
    const uint64_t sourceSize = r.varint();
    const uint64_t targetSize = r.varint();
    if (sourceSize != rom.size()) throw PatchError("este patch foi feito para uma ROM de outro tamanho");
    if (targetSize > 64u * 1024 * 1024) throw PatchError("o patch UPS gera uma ROM grande demais");

    std::vector<uint8_t> out(static_cast<size_t>(targetSize), 0);
    std::memcpy(out.data(), rom.data(), std::min(rom.size(), out.size()));
    uint64_t pos = 0;
    while (!r.atEnd()) {
        pos += r.varint(); // bytes iguais pulados
        while (true) {     // bytes diferentes, guardados como XOR, até um zero
            const uint8_t x = r.byte();
            if (pos < out.size()) out[pos] ^= x;
            ++pos;
            if (x == 0) break;
        }
    }
    checkResult(patch, out);
    return out;
}

std::vector<uint8_t> applyBps(const std::vector<uint8_t>& rom, const std::vector<uint8_t>& patch) {
    if (patch.size() < 19) throw PatchError("o patch BPS está incompleto");
    checkFooter(patch, rom);
    Reader r(patch, patch.size() - 12);
    for (int i = 0; i < 4; ++i) r.byte(); // "BPS1"
    const uint64_t sourceSize = r.varint();
    const uint64_t targetSize = r.varint();
    const uint64_t metadataSize = r.varint();
    for (uint64_t i = 0; i < metadataSize; ++i) r.byte();
    if (sourceSize != rom.size()) throw PatchError("este patch foi feito para uma ROM de outro tamanho");
    if (targetSize > 64u * 1024 * 1024) throw PatchError("o patch BPS gera uma ROM grande demais");

    std::vector<uint8_t> out(static_cast<size_t>(targetSize), 0);
    size_t outPos = 0;
    int64_t sourceRel = 0, targetRel = 0;
    const auto put = [&](uint8_t v) {
        if (outPos >= out.size()) throw PatchError("o patch BPS escreve além do tamanho final");
        out[outPos++] = v;
    };
    const auto signedOffset = [&r]() {
        const uint64_t d = r.varint();
        const int64_t magnitude = static_cast<int64_t>(d >> 1);
        return (d & 1) ? -magnitude : magnitude;
    };
    while (!r.atEnd()) {
        const uint64_t data = r.varint();
        const uint64_t length = (data >> 2) + 1;
        switch (data & 3) {
        case 0: // SourceRead: o mesmo trecho da ROM original
            for (uint64_t i = 0; i < length; ++i) {
                if (outPos >= rom.size()) throw PatchError("o patch BPS lê além da ROM original");
                put(rom[outPos]);
            }
            break;
        case 1: // TargetRead: bytes novos, guardados no patch
            for (uint64_t i = 0; i < length; ++i) put(r.byte());
            break;
        case 2: // SourceCopy: copia de outro ponto da ROM original
            sourceRel += signedOffset();
            for (uint64_t i = 0; i < length; ++i) {
                if (sourceRel < 0 || static_cast<uint64_t>(sourceRel) >= rom.size()) throw PatchError("o patch BPS lê além da ROM original");
                put(rom[static_cast<size_t>(sourceRel++)]);
            }
            break;
        default: // TargetCopy: repete algo já escrito no resultado (pode se sobrepor, como no LZ77)
            targetRel += signedOffset();
            for (uint64_t i = 0; i < length; ++i) {
                if (targetRel < 0 || static_cast<size_t>(targetRel) >= outPos) throw PatchError("o patch BPS está corrompido");
                put(out[static_cast<size_t>(targetRel++)]);
            }
            break;
        }
    }
    checkResult(patch, out);
    return out;
}

} // namespace

uint32_t crc32(const uint8_t* data, size_t size) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

PatchFormat detectPatchFormat(const std::vector<uint8_t>& patch) {
    const auto starts = [&patch](const char* magic) {
        const size_t n = std::strlen(magic);
        return patch.size() >= n && std::memcmp(patch.data(), magic, n) == 0;
    };
    if (starts("PATCH")) return PatchFormat::Ips;
    if (starts("UPS1")) return PatchFormat::Ups;
    if (starts("BPS1")) return PatchFormat::Bps;
    throw PatchError("o arquivo não é um patch IPS, UPS nem BPS");
}

const char* patchFormatName(PatchFormat format) {
    switch (format) {
    case PatchFormat::Ips: return "IPS";
    case PatchFormat::Ups: return "UPS";
    case PatchFormat::Bps: return "BPS";
    }
    return "?";
}

std::vector<uint8_t> applyPatch(const std::vector<uint8_t>& rom, const std::vector<uint8_t>& patch) {
    switch (detectPatchFormat(patch)) {
    case PatchFormat::Ips: return applyIps(rom, patch);
    case PatchFormat::Ups: return applyUps(rom, patch);
    case PatchFormat::Bps: return applyBps(rom, patch);
    }
    throw PatchError("formato de patch desconhecido");
}

} // namespace gba
