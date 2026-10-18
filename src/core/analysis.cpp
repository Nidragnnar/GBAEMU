#include "analysis.hpp"

#include <algorithm>

namespace gba::analysis {

std::vector<uint8_t> encodePokemonText(const std::string& text) {
    std::vector<uint8_t> out;
    for (const char c : text) {
        if (c >= 'A' && c <= 'Z') out.push_back(static_cast<uint8_t>(0xBB + (c - 'A')));
        else if (c >= 'a' && c <= 'z') out.push_back(static_cast<uint8_t>(0xD5 + (c - 'a')));
        else if (c >= '0' && c <= '9') out.push_back(static_cast<uint8_t>(0xA1 + (c - '0')));
        else if (c == ' ') out.push_back(0x00);
        else if (c == '!') out.push_back(0xAB);
        else if (c == '?') out.push_back(0xAC);
        else if (c == '.') out.push_back(0xAD);
        else if (c == '-') out.push_back(0xAE);
        else if (c == ',') out.push_back(0xB8);
        else if (c == '/') out.push_back(0xBA);
        else if (c == ':') out.push_back(0xF0);
        else if (c == '\'') out.push_back(0xB4);
        else return {};
    }
    return out;
}

uint32_t findStringStart(const std::vector<uint8_t>& rom, uint32_t offset, uint32_t maxDistance) {
    uint32_t start = offset;
    while (start > 0 && offset - start < maxDistance && rom[start - 1] != 0xFF) --start;
    return start;
}

std::vector<uint32_t> findBytes(const std::vector<uint8_t>& rom, const std::vector<uint8_t>& needle) {
    std::vector<uint32_t> hits;
    if (needle.empty()) return hits;
    auto it = rom.begin();
    while ((it = std::search(it, rom.end(), needle.begin(), needle.end())) != rom.end()) {
        hits.push_back(static_cast<uint32_t>(it - rom.begin()));
        ++it;
    }
    return hits;
}

std::vector<uint32_t> findWord(const std::vector<uint8_t>& rom, uint32_t value) {
    std::vector<uint32_t> hits;
    for (uint32_t i = 0; i + 4 <= rom.size(); i += 4) {
        const uint32_t w = rom[i] | (rom[i + 1] << 8) | (rom[i + 2] << 16) | (static_cast<uint32_t>(rom[i + 3]) << 24);
        if (w == value) hits.push_back(i);
    }
    return hits;
}

namespace {
uint16_t half(const std::vector<uint8_t>& rom, uint32_t off) {
    return static_cast<uint16_t>(rom[off] | (rom[off + 1] << 8));
}
} // namespace

std::optional<uint32_t> thumbBlTarget(const std::vector<uint8_t>& rom, uint32_t offset) {
    if (offset + 4 > rom.size()) return std::nullopt;
    const uint16_t hi = half(rom, offset), lo = half(rom, offset + 2);
    if ((hi & 0xF800) != 0xF000 || (lo & 0xF800) != 0xF800) return std::nullopt;
    const int32_t high = static_cast<int32_t>(static_cast<uint32_t>(hi & 0x7FF) << 21) >> 9; // com sinal, × 4096
    const uint32_t addr = 0x08000000 + offset;
    return addr + 4 + static_cast<uint32_t>(high) + ((lo & 0x7FFu) << 1);
}

std::vector<uint32_t> findThumbCallers(const std::vector<uint8_t>& rom, uint32_t target) {
    std::vector<uint32_t> callers;
    for (uint32_t off = 0; off + 4 <= rom.size(); off += 2) {
        if (const auto t = thumbBlTarget(rom, off); t && *t == target) callers.push_back(0x08000000 + off);
    }
    return callers;
}

std::vector<uint32_t> findThumbBranchesTo(const std::vector<uint8_t>& rom, uint32_t target, uint32_t range) {
    std::vector<uint32_t> hits;
    const uint32_t center = target - 0x08000000;
    const uint32_t from = center > range ? (center - range) & ~1u : 0;
    const uint32_t to = std::min<uint32_t>(static_cast<uint32_t>(rom.size()) - 2, center + range);
    for (uint32_t off = from; off <= to; off += 2) {
        const uint16_t op = half(rom, off);
        const uint32_t pc = 0x08000000 + off + 4;
        uint32_t dest = 0;
        if ((op & 0xF000) == 0xD000 && (op & 0x0F00) < 0x0E00) {        // Bxx (condições 0-13)
            dest = pc + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(op & 0xFF)) * 2);
        } else if ((op & 0xF800) == 0xE000) {                           // B
            dest = pc + static_cast<uint32_t>(static_cast<int32_t>(static_cast<uint32_t>(op) << 21) >> 20);
        } else {
            continue;
        }
        if (dest == target) hits.push_back(0x08000000 + off);
    }
    return hits;
}

std::optional<uint32_t> findFunctionStart(const std::vector<uint8_t>& rom, uint32_t offset, uint32_t maxDistance) {
    offset &= ~1u;
    for (uint32_t back = 0; back <= maxDistance && back <= offset; back += 2) {
        if ((half(rom, offset - back) & 0xFF00) == 0xB500) return 0x08000000 + offset - back; // push {..., lr}
    }
    return std::nullopt;
}

} // namespace gba::analysis
