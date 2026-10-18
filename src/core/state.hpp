#pragma once
// Save states: uma única função serialize() por componente serve para gravar e para ler,
// então os campos nunca ficam fora de ordem entre as duas operações.

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace gba {

class StateArchive {
public:
    static StateArchive writer() { return StateArchive(false, {}); }
    static StateArchive reader(std::vector<uint8_t> data) { return StateArchive(true, std::move(data)); }

    bool loading() const { return loading_; }
    const std::vector<uint8_t>& data() const { return data_; }

    // Tipos simples e structs sem ponteiros (copiados byte a byte).
    template <class T>
    void value(T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "use bytes()/vector() para tipos não triviais");
        bytes(&v, sizeof v);
    }

    void bytes(void* p, size_t n) {
        if (loading_) {
            if (pos_ + n > data_.size()) throw std::runtime_error("save state incompleto ou corrompido");
            std::memcpy(p, data_.data() + pos_, n);
            pos_ += n;
        } else {
            const auto* b = static_cast<const uint8_t*>(p);
            data_.insert(data_.end(), b, b + n);
        }
    }

    // Vetor de tamanho variável: grava o tamanho antes dos dados.
    void vector(std::vector<uint8_t>& v, size_t maxSize = 64 * 1024 * 1024) {
        uint64_t size = v.size();
        value(size);
        if (loading_) {
            if (size > maxSize) throw std::runtime_error("save state corrompido");
            v.resize(static_cast<size_t>(size));
        }
        if (size) bytes(v.data(), static_cast<size_t>(size));
    }

    bool atEnd() const { return pos_ == data_.size(); }

private:
    StateArchive(bool loading, std::vector<uint8_t> data) : loading_(loading), data_(std::move(data)) {}
    bool loading_;
    std::vector<uint8_t> data_;
    size_t pos_ = 0;
};

} // namespace gba
