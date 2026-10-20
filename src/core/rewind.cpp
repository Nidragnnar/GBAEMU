#include "rewind.hpp"

#include <stdexcept>

namespace gba {

namespace {

// Formato da diferença: 1 byte de tipo, depois os dados.
//   tipo 0: XOR entre dois blocos do mesmo tamanho, como pares (zeros pulados, n bytes, os n bytes)
//   tipo 1: o bloco anterior inteiro (só se os tamanhos forem diferentes, o que não deveria acontecer)
constexpr uint8_t kXor = 0, kFull = 1;

void putVarint(std::vector<uint8_t>& out, size_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

size_t getVarint(const std::vector<uint8_t>& in, size_t& pos) {
    size_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (pos >= in.size()) throw std::runtime_error("histórico de rewind corrompido");
        const uint8_t byte = in[pos++];
        v |= static_cast<size_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return v;
    }
    throw std::runtime_error("histórico de rewind corrompido");
}

} // namespace

std::vector<uint8_t> compressXorDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::vector<uint8_t> out;
    if (a.size() != b.size()) {
        out.push_back(kFull);
        out.insert(out.end(), a.begin(), a.end());
        return out;
    }
    out.push_back(kXor);
    const size_t n = a.size();
    size_t i = 0;
    while (i < n) {
        size_t zeros = 0;
        while (i + zeros < n && a[i + zeros] == b[i + zeros]) ++zeros;
        i += zeros;
        size_t literals = 0;
        while (i + literals < n && a[i + literals] != b[i + literals]) ++literals;
        putVarint(out, zeros);
        putVarint(out, literals);
        for (size_t k = 0; k < literals; ++k) out.push_back(static_cast<uint8_t>(a[i + k] ^ b[i + k]));
        i += literals;
    }
    return out;
}

void applyXorDiff(std::vector<uint8_t>& target, const std::vector<uint8_t>& diff) {
    if (diff.empty()) throw std::runtime_error("histórico de rewind corrompido");
    if (diff[0] == kFull) {
        target.assign(diff.begin() + 1, diff.end());
        return;
    }
    size_t pos = 1, i = 0;
    while (pos < diff.size()) {
        i += getVarint(diff, pos);
        const size_t literals = getVarint(diff, pos);
        if (i + literals > target.size() || pos + literals > diff.size()) throw std::runtime_error("histórico de rewind corrompido");
        for (size_t k = 0; k < literals; ++k) target[i + k] ^= diff[pos + k];
        pos += literals;
        i += literals;
    }
}

void RewindBuffer::push(std::vector<uint8_t> state) {
    if (capacity_ == 0) return;
    if (newest_) {
        diffs_.push_back(compressXorDiff(*newest_, state)); // leva do novo de volta ao que era o mais recente
        if (diffs_.size() + 1 > capacity_) diffs_.pop_front(); // descarta o mais antigo
    }
    newest_ = std::move(state);
}

std::optional<std::vector<uint8_t>> RewindBuffer::pop() {
    if (!newest_) return std::nullopt;
    std::vector<uint8_t> result = std::move(*newest_);
    if (diffs_.empty()) {
        newest_.reset();
    } else {
        std::vector<uint8_t> previous = result;
        applyXorDiff(previous, diffs_.back());
        diffs_.pop_back();
        newest_ = std::move(previous);
    }
    return result;
}

void RewindBuffer::clear() {
    newest_.reset();
    diffs_.clear();
}

size_t RewindBuffer::memoryUsed() const {
    size_t total = newest_ ? newest_->size() : 0;
    for (const auto& d : diffs_) total += d.size();
    return total;
}

} // namespace gba
