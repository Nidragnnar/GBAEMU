#pragma once
// Histórico para "voltar no tempo": guarda os últimos N estados do console (save states) e devolve do
// mais recente para o mais antigo.
//
// Para caber na memória, só o estado mais recente fica inteiro. Cada um dos anteriores é guardado como a
// diferença (XOR) para o estado seguinte, com as sequências de zeros comprimidas: entre duas fotos
// próximas quase toda a memória do console é igual, então cada diferença ocupa poucos KB em vez de ~550.
// Voltar um passo = aplicar a diferença mais nova ao estado atual. O mais antigo pode ser descartado
// sem afetar os outros, porque a reconstrução sempre parte do mais recente.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace gba {

class RewindBuffer {
public:
    explicit RewindBuffer(size_t capacity) : capacity_(capacity) {}

    void push(std::vector<uint8_t> state);   // guarda um estado novo (descarta o mais antigo se lotar)
    std::optional<std::vector<uint8_t>> pop(); // devolve o mais recente e o remove; vazio se não houver
    void clear();

    size_t size() const { return newest_ ? diffs_.size() + 1 : 0; }
    size_t capacity() const { return capacity_; }
    size_t memoryUsed() const;               // bytes ocupados (para mostrar na interface)

private:
    size_t capacity_;
    std::optional<std::vector<uint8_t>> newest_;
    std::deque<std::vector<uint8_t>> diffs_; // diffs_.back() leva do mais recente ao anterior
};

// Expostos para os testes: diferença comprimida entre dois blocos do mesmo tamanho, e a volta.
std::vector<uint8_t> compressXorDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);
void applyXorDiff(std::vector<uint8_t>& target, const std::vector<uint8_t>& diff);

} // namespace gba
