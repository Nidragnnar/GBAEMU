#pragma once
// Codificador de PNG mínimo, sem bibliotecas externas: imagem RGB de 8 bits, dados comprimidos com
// "deflate" em blocos armazenados (sem compressão de fato). Uma tela do GBA (240×160) fica com ~115 KB,
// e qualquer programa de imagens abre normalmente.

#include <cstdint>
#include <vector>

namespace gba {

// pixels: w × h valores 0xAARRGGBB (o formato da tela do NiGBA); o canal alfa é ignorado.
std::vector<uint8_t> encodePng(const uint32_t* pixels, int width, int height);

} // namespace gba
