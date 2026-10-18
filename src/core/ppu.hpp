#pragma once
// PPU (vídeo): temporização das linhas (VCOUNT, DISPSTAT, VBlank/HBlank) e renderização linha a linha.
// Backgrounds: modos 0-2 (tiles e afins) e 3-5 (bitmap). Sprites normais e afins.
// Efeitos: janelas (2 retangulares, janela de sprites e "fora"), blending (transparência, clarear,
// escurecer, sprites semitransparentes) e mosaico.
// Referência: GBATEK, "LCD Video Controller".

#include <array>
#include <cstdint>
#include <memory>

#include "bus.hpp"
#include "state.hpp"

namespace gba {

constexpr int kScreenWidth = 240;
constexpr int kScreenHeight = 160;
constexpr int kTotalLines = 228;       // 160 visíveis + 68 de VBlank
constexpr int kHDrawCycles = 960;      // parte visível da linha
constexpr int kHBlankCycles = 272;     // 1232 ciclos por linha no total
constexpr int kCyclesPerFrame = kTotalLines * (kHDrawCycles + kHBlankCycles); // 280.896

class Ppu {
public:
    explicit Ppu(Bus& bus) : bus_(bus) {}

    // Chamados pelo agendador no início de cada linha e no início do HBlank.
    void startLine(int line);
    void startHBlank();
    void onIoWrite(uint32_t offset); // escrita nos pontos de referência afins recarrega os internos
    void serialize(StateArchive& ar) { ar.value(line_); ar.value(refX_); ar.value(refY_); ar.value(fb_); }

    // Renderiza o quadro inteiro de uma vez (usado quando a CPU não está rodando).
    void renderFrame();
    const std::array<uint32_t, kScreenWidth * kScreenHeight>& framebuffer() const { return fb_; }

    static uint32_t bgr555ToArgb(uint16_t c);

private:
    // Uma linha de um background: cor BGR555, ou kTransparent onde não há pixel.
    using Line = std::array<uint16_t, kScreenWidth>;
    static constexpr uint16_t kTransparent = 0x8000;

    // Uma linha de sprites: cor e prioridade do sprite visível em cada pixel.
    struct ObjPixel { uint16_t color = kTransparent; uint8_t priority = 4; bool semiTransparent = false; };
    struct ObjLine {
        std::array<ObjPixel, kScreenWidth> pixels{};
        std::array<bool, kScreenWidth> window{}; // pixels cobertos por sprites no modo "janela de sprite"
    };

    void renderLine(int line);
    void renderSprites(int line, ObjLine& out) const;
    uint16_t objPaletteColor(uint32_t index) const;
    void renderTextBg(int bg, int line, Line& out) const;
    uint8_t windowMask(int x, int line, const ObjLine& sprites) const; // camadas e efeitos liberados no pixel
    uint16_t applyEffects(uint16_t top, int topLayer, bool topSemi, uint16_t below, int belowLayer, bool effects) const;
    void renderAffineBg(int bg, Line& out) const;
    void renderBitmapBg(int mode, int line, Line& out) const;
    void reloadAffine(int index);
    void advanceAffine(); // depois de cada linha visível, soma PB/PD aos pontos de referência

    uint16_t paletteColor(uint32_t index) const;
    uint8_t vram8(uint32_t offset) const;
    uint16_t io(uint32_t offset) const { return bus_.ioRead16(offset); }

    Bus& bus_;
    int line_ = 0;
    std::array<int32_t, 2> refX_{}, refY_{}; // pontos de referência internos de BG2 e BG3 (8.8 com sinal)
    // A imagem (150 KB) fica no heap, pelo mesmo motivo das memórias do barramento.
    std::unique_ptr<std::array<uint32_t, kScreenWidth * kScreenHeight>> fbStorage_ =
        std::make_unique<std::array<uint32_t, kScreenWidth * kScreenHeight>>();
    std::array<uint32_t, kScreenWidth * kScreenHeight>& fb_ = *fbStorage_;
};

} // namespace gba
