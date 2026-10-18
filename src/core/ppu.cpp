#include "ppu.hpp"

#include <algorithm>

namespace gba {

namespace {
// Registradores (deslocamento em relação a 0x04000000).
constexpr uint32_t kDispcnt = 0x000;
constexpr uint32_t kDispstat = 0x004;
constexpr uint32_t kVcount = 0x006;
constexpr uint32_t kBgCnt = 0x008;   // + 2 × bg
constexpr uint32_t kBgHofs = 0x010;  // + 4 × bg
constexpr uint32_t kBgVofs = 0x012;  // + 4 × bg
constexpr uint32_t kAffine = 0x020;  // PA, PB, PC, PD, X, Y de BG2; BG3 em + 0x10
constexpr uint32_t kWin0H = 0x040;   // WIN1H em 0x042
constexpr uint32_t kWin0V = 0x044;   // WIN1V em 0x046
constexpr uint32_t kWinIn = 0x048;
constexpr uint32_t kWinOut = 0x04A;
constexpr uint32_t kMosaic = 0x04C;
constexpr uint32_t kBldCnt = 0x050;
constexpr uint32_t kBldAlpha = 0x052;
constexpr uint32_t kBldY = 0x054;

constexpr uint32_t kBgVramLimit = 0x10000; // nos modos 0-2, os backgrounds só enxergam os primeiros 64 KB

int32_t signExtend28(uint32_t v) { return static_cast<int32_t>(v << 4) >> 4; }
} // namespace

uint32_t Ppu::bgr555ToArgb(uint16_t c) {
    // 5 bits por canal -> 8 bits, replicando os bits altos para cobrir 0..255.
    const uint32_t r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    const auto x8 = [](uint32_t v) { return (v << 3) | (v >> 2); };
    return 0xFF000000u | (x8(r) << 16) | (x8(g) << 8) | x8(b);
}

uint16_t Ppu::paletteColor(uint32_t index) const {
    const auto& pal = bus_.palette();
    return static_cast<uint16_t>((pal[index * 2] | (pal[index * 2 + 1] << 8)) & 0x7FFF);
}

uint16_t Ppu::objPaletteColor(uint32_t index) const {
    const auto& pal = bus_.palette(); // a paleta dos sprites é a segunda metade (0x200)
    return static_cast<uint16_t>((pal[0x200 + index * 2] | (pal[0x201 + index * 2] << 8)) & 0x7FFF);
}

uint8_t Ppu::vram8(uint32_t offset) const {
    const auto& vram = bus_.vram();
    return offset < vram.size() ? vram[offset] : 0;
}

// ---------------------------------------------------------------- temporização

// DISPSTAT: bit 0 VBlank, bit 1 HBlank, bit 2 VCOUNT = LYC,
//           bits 3-5 habilitam as IRQs correspondentes, bits 8-15 = LYC.
void Ppu::startLine(int line) {
    line_ = line;
    uint16_t dispstat = io(kDispstat) & ~0x0007;
    if (line >= kScreenHeight && line < kTotalLines - 1) dispstat |= 0x0001; // VBlank (a flag cai na linha 227)
    if (line == (dispstat >> 8)) {
        dispstat |= 0x0004;
        if (dispstat & 0x0020) bus_.requestInterrupt(irq::VCount);
    }
    bus_.ioWrite16Raw(kDispstat, dispstat);
    bus_.ioWrite16Raw(kVcount, static_cast<uint16_t>(line));

    if (line == kScreenHeight) {
        if (dispstat & 0x0008) bus_.requestInterrupt(irq::VBlank);
        reloadAffine(0); // os pontos de referência afins voltam ao valor dos registradores a cada quadro
        reloadAffine(1);
    }
}

void Ppu::startHBlank() {
    const uint16_t dispstat = io(kDispstat) | 0x0002;
    bus_.ioWrite16Raw(kDispstat, dispstat);
    if (dispstat & 0x0010) bus_.requestInterrupt(irq::HBlank);

    // A linha é desenhada com os registradores como estavam durante o HDraw. Mudanças feitas
    // no HBlank (efeitos de rolagem por linha) valem a partir da linha seguinte, como no hardware.
    if (line_ < kScreenHeight) {
        renderLine(line_);
        advanceAffine();
    }
}

void Ppu::onIoWrite(uint32_t offset) {
    if (offset >= 0x028 && offset <= 0x02F) reloadAffine(0);
    else if (offset >= 0x038 && offset <= 0x03F) reloadAffine(1);
}

void Ppu::reloadAffine(int index) {
    const uint32_t base = kAffine + static_cast<uint32_t>(index) * 0x10;
    refX_[index] = signExtend28(bus_.ioRead32(base + 0x8));
    refY_[index] = signExtend28(bus_.ioRead32(base + 0xC));
}

void Ppu::advanceAffine() {
    for (int i = 0; i < 2; ++i) {
        const uint32_t base = kAffine + static_cast<uint32_t>(i) * 0x10;
        refX_[i] += static_cast<int16_t>(io(base + 0x2)); // PB
        refY_[i] += static_cast<int16_t>(io(base + 0x6)); // PD
    }
}

void Ppu::renderFrame() {
    reloadAffine(0);
    reloadAffine(1);
    for (int line = 0; line < kScreenHeight; ++line) {
        renderLine(line);
        advanceAffine();
    }
}

// ---------------------------------------------------------------- composição

void Ppu::renderLine(int line) {
    uint32_t* row = &fb_[static_cast<size_t>(line) * kScreenWidth];
    const uint16_t dispcnt = io(kDispcnt);
    if (dispcnt & 0x0080) { // forced blank: o hardware mostra branco
        std::fill(row, row + kScreenWidth, 0xFFFFFFFFu);
        return;
    }

    // Quais backgrounds existem em cada modo: 0 = nenhum, 1 = tiles, 2 = afim, 3 = bitmap.
    static constexpr int kKind[8][4] = {
        {1, 1, 1, 1}, {1, 1, 2, 0}, {0, 0, 2, 2},
        {0, 0, 3, 0}, {0, 0, 3, 0}, {0, 0, 3, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0},
    };
    const int mode = dispcnt & 7;

    // Mosaico (MOSAIC): blocos de N×M pixels repetem o pixel do canto superior esquerdo.
    const uint16_t mosaic = io(kMosaic);
    const int bgMosaicW = (mosaic & 0xF) + 1, bgMosaicH = ((mosaic >> 4) & 0xF) + 1;

    std::array<Line, 4> layers;
    std::array<int, 4> order{}; // backgrounds ativos, do que fica na frente para o de trás
    std::array<int, 4> priority{};
    int active = 0;
    for (int bg = 0; bg < 4; ++bg) {
        priority[bg] = io(kBgCnt + bg * 2) & 3;
        const int kind = kKind[mode][bg];
        if (kind == 0 || !(dispcnt & (0x100 << bg))) continue;
        const bool useMosaic = io(kBgCnt + bg * 2) & 0x0040;
        const int srcLine = useMosaic ? line - line % bgMosaicH : line;
        if (kind == 1) renderTextBg(bg, srcLine, layers[bg]);
        else if (kind == 2) renderAffineBg(bg, layers[bg]); // TODO: mosaico vertical em BG afim
        else renderBitmapBg(mode, srcLine, layers[bg]);
        if (useMosaic && bgMosaicW > 1) {
            for (int x = 0; x < kScreenWidth; ++x) layers[bg][x] = layers[bg][x - x % bgMosaicW];
        }
        order[active++] = bg;
    }
    // Prioridade menor fica na frente; empate: o de número menor fica na frente.
    std::stable_sort(order.begin(), order.begin() + active, [&priority](int a, int b) {
        return priority[a] < priority[b];
    });

    ObjLine sprites;
    if (dispcnt & 0x1000) renderSprites(line, sprites);

    // Camadas: 0-3 = BGs, 4 = sprites, 5 = cor de fundo.
    constexpr int kObjLayer = 4, kBackdropLayer = 5;
    const uint16_t backdrop = paletteColor(0);
    for (int x = 0; x < kScreenWidth; ++x) {
        const uint8_t allowed = windowMask(x, line, sprites);
        const ObjPixel& obj = sprites.pixels[x];
        const bool objVisible = obj.color != kTransparent && (allowed & 0x10);

        // As duas camadas visíveis de cima: a da frente e a que está logo atrás dela.
        uint16_t color[2] = {backdrop, backdrop};
        int layer[2] = {kBackdropLayer, kBackdropLayer};
        bool semi[2] = {false, false};
        int found = 0;
        const auto take = [&](uint16_t c, int l, bool s) {
            if (found < 2) { color[found] = c; layer[found] = l; semi[found] = s; ++found; }
        };
        bool objTaken = !objVisible;
        for (int i = 0; i < active && found < 2; ++i) {
            const int bg = order[i];
            // Sprite com prioridade igual ou menor que a do background fica na frente dele.
            if (!objTaken && obj.priority <= priority[bg]) { take(obj.color, kObjLayer, obj.semiTransparent); objTaken = true; }
            if (found < 2 && (allowed & (1 << bg)) && layers[bg][x] != kTransparent) take(layers[bg][x], bg, false);
        }
        if (!objTaken) take(obj.color, kObjLayer, obj.semiTransparent);
        take(backdrop, kBackdropLayer, false);

        row[x] = bgr555ToArgb(applyEffects(color[0], layer[0], semi[0], color[1], layer[1], allowed & 0x20));
    }
}

// Janelas: WIN0 tem prioridade sobre WIN1, que tem sobre a janela de sprites; o resto é "fora".
// Devolve os bits liberados: 0-3 BGs, 4 sprites, 5 efeitos. Sem janelas ligadas, tudo liberado.
uint8_t Ppu::windowMask(int x, int line, const ObjLine& sprites) const {
    const uint16_t dispcnt = io(kDispcnt);
    if (!(dispcnt & 0xE000)) return 0x3F;
    const auto inside = [](int v, int start, int end) {
        // Início maior que o fim: a janela "dá a volta" pela borda.
        return start <= end ? (v >= start && v < end) : (v >= start || v < end);
    };
    const uint16_t winIn = io(kWinIn), winOut = io(kWinOut);
    for (int w = 0; w < 2; ++w) {
        if (!(dispcnt & (0x2000 << w))) continue;
        const uint16_t h = io(kWin0H + w * 2), v = io(kWin0V + w * 2);
        if (inside(x, h >> 8, h & 0xFF) && inside(line, v >> 8, v & 0xFF)) {
            return static_cast<uint8_t>((winIn >> (w * 8)) & 0x3F);
        }
    }
    if ((dispcnt & 0x8000) && sprites.window[x]) return static_cast<uint8_t>((winOut >> 8) & 0x3F);
    return static_cast<uint8_t>(winOut & 0x3F);
}

// Blending (BLDCNT): bits 0-5 primeiro alvo, 6-7 efeito (1 mistura, 2 clarear, 3 escurecer), 8-13 segundo alvo.
uint16_t Ppu::applyEffects(uint16_t top, int topLayer, bool topSemi, uint16_t below, int belowLayer, bool effects) const {
    if (!effects) return top;
    const uint16_t bldcnt = io(kBldCnt);
    const bool belowIsSecond = bldcnt & (0x100 << belowLayer);
    const auto channel = [](uint16_t c, int shift) { return static_cast<int>((c >> shift) & 0x1F); };
    const auto compose = [&](auto fn) {
        uint16_t out = 0;
        for (const int shift : {0, 5, 10}) out = static_cast<uint16_t>(out | (std::min(31, fn(shift)) << shift));
        return out;
    };
    const auto alpha = [&] {
        const uint16_t bld = io(kBldAlpha);
        const int eva = std::min(16, bld & 0x1F), evb = std::min(16, (bld >> 8) & 0x1F);
        return compose([&](int sh) { return (channel(top, sh) * eva + channel(below, sh) * evb) >> 4; });
    };

    // Sprites semitransparentes sempre se misturam com um segundo alvo logo atrás, qualquer que seja o efeito.
    if (topSemi && belowIsSecond) return alpha();
    if (!(bldcnt & (1 << topLayer))) return top;

    const int evy = std::min(16, io(kBldY) & 0x1F);
    switch ((bldcnt >> 6) & 3) {
    case 1: return belowIsSecond ? alpha() : top;
    case 2: return compose([&](int sh) { const int c = channel(top, sh); return c + (((31 - c) * evy) >> 4); });
    case 3: return compose([&](int sh) { const int c = channel(top, sh); return c - ((c * evy) >> 4); });
    default: return top;
    }
}

// ---------------------------------------------------------------- sprites

// OAM: 128 entradas de 8 bytes (GBATEK, "LCD OBJ - OAM Attributes").
//   attr0: Y (0-7), afim (8), tamanho duplo / desligado (9), modo (10-11), mosaico (12), 256 cores (13), forma (14-15)
//   attr1: X com sinal (0-8), matriz afim (9-13) ou espelhamentos (12, 13), tamanho (14-15)
//   attr2: tile (0-9), prioridade (10-11), paleta (12-15)
// As matrizes afins (PA, PB, PC, PD) ocupam o 4º halfword de grupos de 4 entradas.
void Ppu::renderSprites(int line, ObjLine& objLine) const {
    auto& out = objLine.pixels;
    static constexpr int kWidth[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static constexpr int kHeight[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

    const uint16_t dispcnt = io(kDispcnt);
    const bool oneDimensional = dispcnt & 0x0040;
    const bool bitmapMode = (dispcnt & 7) >= 3;
    // Tempo disponível para desenhar sprites numa linha; esgotado, os sprites seguintes somem.
    int budget = (dispcnt & 0x0020) ? 954 : 1210;

    const uint16_t mosaic = io(kMosaic);
    const int objMosaicW = ((mosaic >> 8) & 0xF) + 1, objMosaicH = ((mosaic >> 12) & 0xF) + 1;

    const auto& oam = bus_.oam();
    const auto half = [&oam](uint32_t off) { return static_cast<uint16_t>(oam[off] | (oam[off + 1] << 8)); };

    for (int i = 0; i < 128; ++i) {
        const uint32_t base = static_cast<uint32_t>(i) * 8;
        const uint16_t attr0 = half(base), attr1 = half(base + 2), attr2 = half(base + 4);
        const bool affine = attr0 & 0x0100;
        if (!affine && (attr0 & 0x0200)) continue;          // desligado
        const int mode = (attr0 >> 10) & 3;
        const int shape = attr0 >> 14;
        if (mode == 3 || shape == 3) continue;               // combinações proibidas

        const int size = attr1 >> 14;
        const int width = kWidth[shape][size], height = kHeight[shape][size];
        const bool doubleSize = affine && (attr0 & 0x0200);  // área de desenho com o dobro do tamanho
        const int boxW = doubleSize ? width * 2 : width;
        const int boxH = doubleSize ? height * 2 : height;

        int top = attr0 & 0xFF;
        if (top + boxH > 256) top -= 256;                    // Y dá a volta: sprite saindo por cima
        if (line < top || line >= top + boxH) continue;
        int left = attr1 & 0x1FF;
        if (left >= 256) left -= 512;                        // X tem 9 bits com sinal

        const int cost = affine ? 10 + 2 * boxW : boxW;
        if (budget < cost) break;
        budget -= cost;
        const bool windowSprite = mode == 2;                 // só define a forma da janela de sprite
        const bool semiTransparent = mode == 1;
        const bool useMosaic = attr0 & 0x1000;

        const uint32_t tile = attr2 & 0x3FF;
        if (bitmapMode && tile < 512) continue;              // nos modos bitmap, metade da VRAM de sprites é do quadro
        const uint8_t priority = static_cast<uint8_t>((attr2 >> 10) & 3);
        const uint32_t paletteBank = attr2 >> 12;
        const bool color256 = attr0 & 0x2000;

        int32_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            const uint32_t group = ((attr1 >> 9) & 0x1F) * 32;
            pa = static_cast<int16_t>(half(group + 6));
            pb = static_cast<int16_t>(half(group + 14));
            pc = static_cast<int16_t>(half(group + 22));
            pd = static_cast<int16_t>(half(group + 30));
        }
        const bool hflip = !affine && (attr1 & 0x1000);
        const bool vflip = !affine && (attr1 & 0x2000);
        // Distância (em unidades de 32 bytes) entre uma linha de tiles do sprite e a seguinte.
        const uint32_t tilesPerRow = static_cast<uint32_t>(width / 8) * (color256 ? 2 : 1);
        const uint32_t rowStride = oneDimensional ? tilesPerRow : 32;
        // Mosaico: usa o pixel do canto do bloco (em coordenadas da tela), sem sair do sprite.
        const int sy = useMosaic ? std::max(0, (line - line % objMosaicH) - top) : line - top;

        for (int bxScreen = 0; bxScreen < boxW; ++bxScreen) {
            const int sx = left + bxScreen;
            if (sx < 0 || sx >= kScreenWidth) continue;
            // Um sprite anterior (número menor) com prioridade igual ou melhor já ocupa o pixel.
            if (!windowSprite && out[sx].color != kTransparent && out[sx].priority <= priority) continue;
            const int bx = useMosaic ? std::max(0, (sx - sx % objMosaicW) - left) : bxScreen;

            int tx, ty;
            if (affine) {
                const int32_t dx = bx - boxW / 2, dy = sy - boxH / 2; // relativo ao centro
                tx = ((pa * dx + pb * dy) >> 8) + width / 2;
                ty = ((pc * dx + pd * dy) >> 8) + height / 2;
                if (tx < 0 || ty < 0 || tx >= width || ty >= height) continue;
            } else {
                tx = hflip ? width - 1 - bx : bx;
                ty = vflip ? height - 1 - sy : sy;
            }

            const uint32_t row = static_cast<uint32_t>(ty / 8) * rowStride;
            uint16_t color;
            if (color256) {
                const uint32_t unit = (tile + row + static_cast<uint32_t>(tx / 8) * 2) & 0x3FF;
                const uint8_t index = vram8(0x10000 + unit * 32 + static_cast<uint32_t>((ty & 7) * 8 + (tx & 7)));
                if (!index) continue;
                color = objPaletteColor(index);
            } else {
                const uint32_t unit = (tile + row + static_cast<uint32_t>(tx / 8)) & 0x3FF;
                const uint8_t pair = vram8(0x10000 + unit * 32 + static_cast<uint32_t>((ty & 7) * 4 + (tx & 7) / 2));
                const uint8_t index = (tx & 1) ? (pair >> 4) : (pair & 0xF);
                if (!index) continue;
                color = objPaletteColor(paletteBank * 16 + index);
            }
            if (windowSprite) objLine.window[sx] = true;
            else out[sx] = {color, priority, semiTransparent};
        }
    }
}

// ---------------------------------------------------------------- backgrounds

// Background de tiles (modos 0 e 1). Mapa de entradas de 16 bits:
// bits 0-9 tile, bit 10 espelho horizontal, bit 11 espelho vertical, bits 12-15 paleta (16 cores).
void Ppu::renderTextBg(int bg, int line, Line& out) const {
    const uint16_t cnt = io(kBgCnt + bg * 2);
    const uint32_t charBase = ((cnt >> 2) & 3) * 0x4000u;
    const uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800u;
    const bool color256 = cnt & 0x0080;
    const int width = (cnt & 0x4000) ? 512 : 256;
    const int height = (cnt & 0x8000) ? 512 : 256;
    const int hofs = io(kBgHofs + bg * 4) & 0x1FF;
    const int vofs = io(kBgVofs + bg * 4) & 0x1FF;

    const int y = (line + vofs) & (height - 1);
    const int tileRow = y / 8;
    int cachedCol = -1;
    uint16_t entry = 0;
    for (int x = 0; x < kScreenWidth; ++x) {
        const int px = (x + hofs) & (width - 1);
        const int tileCol = px / 8;
        if (tileCol != cachedCol) { // a entrada do mapa só muda a cada 8 pixels
            cachedCol = tileCol;
            // O mapa é dividido em blocos de 32×32 tiles (2 KB), lado a lado.
            const uint32_t block = static_cast<uint32_t>((tileRow / 32) * (width / 256) + tileCol / 32);
            const uint32_t entryAddr = screenBase + block * 0x800 + static_cast<uint32_t>((tileRow % 32) * 32 + tileCol % 32) * 2;
            entry = static_cast<uint16_t>(vram8(entryAddr) | (vram8(entryAddr + 1) << 8));
        }

        const uint32_t tile = entry & 0x3FF;
        int tx = px & 7, ty = y & 7;
        if (entry & 0x0400) tx = 7 - tx;
        if (entry & 0x0800) ty = 7 - ty;

        out[x] = kTransparent;
        if (color256) {
            const uint32_t addr = charBase + tile * 64 + static_cast<uint32_t>(ty * 8 + tx);
            if (addr >= kBgVramLimit) continue;
            const uint8_t index = vram8(addr);
            if (index) out[x] = paletteColor(index);
        } else {
            const uint32_t addr = charBase + tile * 32 + static_cast<uint32_t>(ty * 4 + tx / 2);
            if (addr >= kBgVramLimit) continue;
            const uint8_t pair = vram8(addr);
            const uint8_t index = (tx & 1) ? (pair >> 4) : (pair & 0xF); // pixel da esquerda no nibble baixo
            if (index) out[x] = paletteColor((entry >> 12) * 16u + index);
        }
    }
}

// Background afim (rotação/escala, modos 1 e 2). Mapa de 1 byte por tile, tiles sempre de 256 cores.
void Ppu::renderAffineBg(int bg, Line& out) const {
    const int i = bg - 2;
    const uint16_t cnt = io(kBgCnt + bg * 2);
    const uint32_t charBase = ((cnt >> 2) & 3) * 0x4000u;
    const uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800u;
    const int size = 128 << (cnt >> 14);
    const bool wrap = cnt & 0x2000;
    const uint32_t base = kAffine + static_cast<uint32_t>(i) * 0x10;
    const int32_t pa = static_cast<int16_t>(io(base + 0x0));
    const int32_t pc = static_cast<int16_t>(io(base + 0x4));

    for (int x = 0; x < kScreenWidth; ++x) {
        int32_t tx = (refX_[i] + pa * x) >> 8;
        int32_t ty = (refY_[i] + pc * x) >> 8;
        out[x] = kTransparent;
        if (wrap) {
            tx &= size - 1;
            ty &= size - 1;
        } else if (tx < 0 || ty < 0 || tx >= size || ty >= size) {
            continue;
        }
        const uint32_t mapAddr = screenBase + static_cast<uint32_t>((ty / 8) * (size / 8) + tx / 8);
        const uint32_t addr = charBase + vram8(mapAddr) * 64u + static_cast<uint32_t>((ty & 7) * 8 + (tx & 7));
        if (addr >= kBgVramLimit) continue;
        const uint8_t index = vram8(addr);
        if (index) out[x] = paletteColor(index);
    }
}

// Modos de bitmap: 3 (240×160, cor direta), 4 (240×160, 256 cores, 2 quadros), 5 (160×128, cor direta, 2 quadros).
void Ppu::renderBitmapBg(int mode, int line, Line& out) const {
    const uint32_t frame = (io(kDispcnt) & 0x0010) ? 0xA000 : 0;
    for (int x = 0; x < kScreenWidth; ++x) {
        out[x] = kTransparent;
        if (mode == 3) {
            const uint32_t addr = static_cast<uint32_t>(line * kScreenWidth + x) * 2;
            out[x] = static_cast<uint16_t>((vram8(addr) | (vram8(addr + 1) << 8)) & 0x7FFF);
        } else if (mode == 4) {
            const uint8_t index = vram8(frame + static_cast<uint32_t>(line * kScreenWidth + x));
            if (index) out[x] = paletteColor(index);
        } else if (x < 160 && line < 128) {
            const uint32_t addr = frame + static_cast<uint32_t>(line * 160 + x) * 2;
            out[x] = static_cast<uint16_t>((vram8(addr) | (vram8(addr + 1) << 8)) & 0x7FFF);
        }
    }
}

} // namespace gba
