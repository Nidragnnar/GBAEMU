#include "color.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace gba {

namespace {

// Fórmula de Near (higan): a tela do GBA tem gama ~4,0 e mistura um pouco os canais entre si; o
// resultado é reconvertido para a gama de um monitor (2,2) e escurecido para 255/280.
uint32_t computeLcdColor(int r5, int g5, int b5) {
    constexpr double kLcdGamma = 4.0, kOutGamma = 2.2;
    const double lr = std::pow(r5 / 31.0, kLcdGamma);
    const double lg = std::pow(g5 / 31.0, kLcdGamma);
    const double lb = std::pow(b5 / 31.0, kLcdGamma);
    const auto channel = [&](double mixed) {
        const double v = std::pow(mixed / 255.0, 1.0 / kOutGamma) * (255.0 * 255.0 / 280.0);
        return static_cast<uint32_t>(std::clamp(std::lround(v), 0L, 255L));
    };
    const uint32_t r = channel(0 * lb + 50 * lg + 255 * lr);
    const uint32_t g = channel(30 * lb + 230 * lg + 10 * lr);
    const uint32_t b = channel(220 * lb + 10 * lg + 50 * lr);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

const std::array<uint32_t, 32768>& table() {
    static const std::array<uint32_t, 32768> t = [] {
        std::array<uint32_t, 32768> out{};
        for (int i = 0; i < 32768; ++i) out[static_cast<size_t>(i)] = computeLcdColor(i & 31, (i >> 5) & 31, (i >> 10) & 31);
        return out;
    }();
    return t;
}

inline uint32_t index15(uint32_t argb) { // os 5 bits de cada canal, no formato do GBA (azul no alto)
    return ((argb >> 19) & 0x1F) | (((argb >> 11) & 0x1F) << 5) | (((argb >> 3) & 0x1F) << 10);
}

} // namespace

uint32_t gbaLcdColor(uint32_t argb) { return table()[index15(argb)]; }

void applyGbaLcdColors(const uint32_t* in, uint32_t* out, size_t count) {
    const auto& t = table();
    for (size_t i = 0; i < count; ++i) out[i] = t[index15(in[i])];
}

} // namespace gba
