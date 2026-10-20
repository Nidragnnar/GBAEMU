#pragma once
// Cores da tela do GBA. A tela original (sem luz de fundo) mostrava cores mais escuras e menos saturadas
// que um monitor, e os jogos eram pintados com cores fortes para compensar. Esta correção simula a curva
// de brilho e a mistura de cores da tela do GBA, com a fórmula de Near (higan/bsnes), a referência usada
// pela comunidade.

#include <cstddef>
#include <cstdint>

namespace gba {

// Uma cor 0xAARRGGBB (canais de 8 bits vindos dos 5 bits do GBA) convertida para a aparência da tela do GBA.
uint32_t gbaLcdColor(uint32_t argb);

// Converte uma imagem inteira (usa uma tabela pré-calculada com as 32.768 cores do GBA).
void applyGbaLcdColors(const uint32_t* in, uint32_t* out, size_t count);

} // namespace gba
