// Janelas de depuração do NiGBA: CPU, memória, VRAM, paletas, sprites e registradores de I/O.
// Elas só leem o estado do console (com Bus::peek8 e ioRead16), sem efeitos colaterais na emulação.

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "frontend/app.hpp"

namespace {

const ImVec4 kAccent{0.86f, 0.42f, 0.64f, 1.0f};
const ImVec4 kDim{0.62f, 0.50f, 0.64f, 1.0f};

uint32_t argb(uint16_t c) {
    const uint32_t r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    const auto x8 = [](uint32_t v) { return (v << 3) | (v >> 2); };
    return 0xFF000000u | (x8(r) << 16) | (x8(g) << 8) | x8(b);
}

ImVec4 toVec4(uint16_t c) {
    return ImVec4(static_cast<float>(c & 0x1F) / 31.0f, static_cast<float>((c >> 5) & 0x1F) / 31.0f,
                  static_cast<float>((c >> 10) & 0x1F) / 31.0f, 1.0f);
}

ImTextureID textureId(SDL_Texture* t) { return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(t)); }

struct Region { const char* name; uint32_t base; uint32_t size; };
const Region kRegions[] = {
    {"BIOS", 0x00000000, 0x4000},     {"EWRAM", 0x02000000, 0x40000}, {"IWRAM", 0x03000000, 0x8000},
    {"I/O", 0x04000000, 0x400},       {"Paleta", 0x05000000, 0x400},  {"VRAM", 0x06000000, 0x18000},
    {"OAM", 0x07000000, 0x400},       {"ROM", 0x08000000, 0x2000000}, {"Save", 0x0E000000, 0x10000},
};

const char* modeName(uint32_t mode) {
    switch (mode & 0x1F) {
    case 0x10: return "User";
    case 0x11: return "FIQ";
    case 0x12: return "IRQ";
    case 0x13: return "Supervisor";
    case 0x17: return "Abort";
    case 0x1B: return "Undefined";
    case 0x1F: return "System";
    default: return "?";
    }
}

struct IoReg { uint32_t offset; const char* name; const char* description; };
struct IoGroup { const char* name; std::vector<IoReg> regs; };
const std::vector<IoGroup>& ioGroups() {
    static const std::vector<IoGroup> groups = {
        {"Vídeo", {{0x000, "DISPCNT", "modo, camadas ligadas, janelas"}, {0x004, "DISPSTAT", "VBlank/HBlank/VCOUNT e IRQs"},
                   {0x006, "VCOUNT", "linha atual"}, {0x008, "BG0CNT", ""}, {0x00A, "BG1CNT", ""}, {0x00C, "BG2CNT", ""},
                   {0x00E, "BG3CNT", ""}, {0x010, "BG0HOFS", "rolagem"}, {0x012, "BG0VOFS", ""}, {0x014, "BG1HOFS", ""},
                   {0x016, "BG1VOFS", ""}, {0x018, "BG2HOFS", ""}, {0x01A, "BG2VOFS", ""}, {0x01C, "BG3HOFS", ""},
                   {0x01E, "BG3VOFS", ""}, {0x040, "WIN0H", "janela 0: x1/x2"}, {0x042, "WIN1H", ""},
                   {0x044, "WIN0V", "janela 0: y1/y2"}, {0x046, "WIN1V", ""}, {0x048, "WININ", ""}, {0x04A, "WINOUT", ""},
                   {0x04C, "MOSAIC", ""}, {0x050, "BLDCNT", "efeito e alvos"}, {0x052, "BLDALPHA", "EVA/EVB"},
                   {0x054, "BLDY", "EVY"}}},
        {"Som", {{0x060, "SOUND1CNT_L", "sweep"}, {0x062, "SOUND1CNT_H", ""}, {0x064, "SOUND1CNT_X", ""},
                 {0x068, "SOUND2CNT_L", ""}, {0x06C, "SOUND2CNT_H", ""}, {0x070, "SOUND3CNT_L", ""},
                 {0x072, "SOUND3CNT_H", ""}, {0x074, "SOUND3CNT_X", ""}, {0x078, "SOUND4CNT_L", ""},
                 {0x07C, "SOUND4CNT_H", ""}, {0x080, "SOUNDCNT_L", "volume e canais 1-4"},
                 {0x082, "SOUNDCNT_H", "canais A/B"}, {0x084, "SOUNDCNT_X", "liga o som"}, {0x088, "SOUNDBIAS", ""}}},
        {"DMA", {{0x0B0, "DMA0SAD (baixo)", ""}, {0x0B2, "DMA0SAD (alto)", ""}, {0x0B4, "DMA0DAD (baixo)", ""},
                 {0x0B6, "DMA0DAD (alto)", ""}, {0x0B8, "DMA0CNT_L", "quantidade"}, {0x0BA, "DMA0CNT_H", "controle"},
                 {0x0BC, "DMA1SAD (baixo)", ""}, {0x0BE, "DMA1SAD (alto)", ""}, {0x0C0, "DMA1DAD (baixo)", ""},
                 {0x0C2, "DMA1DAD (alto)", ""}, {0x0C4, "DMA1CNT_L", ""}, {0x0C6, "DMA1CNT_H", ""},
                 {0x0C8, "DMA2SAD (baixo)", ""}, {0x0CA, "DMA2SAD (alto)", ""}, {0x0CC, "DMA2DAD (baixo)", ""},
                 {0x0CE, "DMA2DAD (alto)", ""}, {0x0D0, "DMA2CNT_L", ""}, {0x0D2, "DMA2CNT_H", ""},
                 {0x0D4, "DMA3SAD (baixo)", ""}, {0x0D6, "DMA3SAD (alto)", ""}, {0x0D8, "DMA3DAD (baixo)", ""},
                 {0x0DA, "DMA3DAD (alto)", ""}, {0x0DC, "DMA3CNT_L", ""}, {0x0DE, "DMA3CNT_H", ""}}},
        {"Timers", {{0x100, "TM0CNT_L", "contador"}, {0x102, "TM0CNT_H", "controle"}, {0x104, "TM1CNT_L", ""},
                    {0x106, "TM1CNT_H", ""}, {0x108, "TM2CNT_L", ""}, {0x10A, "TM2CNT_H", ""},
                    {0x10C, "TM3CNT_L", ""}, {0x10E, "TM3CNT_H", ""}}},
        {"Sistema", {{0x130, "KEYINPUT", "botões (0 = apertado)"}, {0x132, "KEYCNT", ""}, {0x200, "IE", "IRQs habilitadas"},
                     {0x202, "IF", "IRQs pendentes"}, {0x204, "WAITCNT", "waitstates"}, {0x208, "IME", "IRQs ligadas"},
                     {0x300, "POSTFLG/HALTCNT", ""}}},
    };
    return groups;
}

} // namespace

// ---------------------------------------------------------------- utilitários

SDL_Texture* App::debugTexture(SDL_Texture*& texture, int w, int h) {
    if (!texture) {
        texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    }
    return texture;
}

void App::stepFrame() {
    if (!console_ || stopped_) return;
    paused_ = true;
    runOneFrame(false);
}

void App::drawDebugWindows() {
    if (!console_) return;
    if (showCpu_) drawCpuWindow();
    if (showMemory_) drawMemoryWindow();
    if (showVram_) drawVramWindow();
    if (showPalette_) drawPaletteWindow();
    if (showSprites_) drawSpritesWindow();
    if (showIo_) drawIoWindow();
}

// ---------------------------------------------------------------- CPU

void App::drawCpuWindow() {
    ImGui::SetNextWindowSize(ImVec2(430, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CPU", &showCpu_)) { ImGui::End(); return; }
    auto& cpu = console_->cpu();

    if (ImGui::Button(paused_ ? "Continuar" : "Pausar")) togglePause();
    ImGui::SameLine();
    if (ImGui::Button("Avançar 1 quadro")) stepFrame();
    ImGui::SameLine();
    ImGui::TextDisabled("quadro %llu", static_cast<unsigned long long>(frame_));

    ImGui::Separator();
    const uint32_t cpsr = cpu.cpsr();
    ImGui::Text("PC %08X   %s   modo %s%s", cpu.pc(), cpu.thumb() ? "Thumb" : "ARM", modeName(cpsr),
                cpu.sleeping() ? "   (dormindo)" : "");
    const char flagNames[] = {'N', 'Z', 'C', 'V'};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        const bool on = cpsr & (1u << (31 - i));
        ImGui::TextColored(on ? kAccent : kDim, "%c", flagNames[i]);
    }
    ImGui::SameLine();
    ImGui::TextColored((cpsr & 0x80) ? kAccent : kDim, "  I");
    ImGui::SameLine();
    ImGui::TextColored((cpsr & 0x20) ? kAccent : kDim, "T");
    ImGui::SameLine();
    ImGui::TextDisabled("  CPSR %08X", cpsr);

    if (ImGui::BeginTable("##regs", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
        for (int i = 0; i < 16; ++i) {
            ImGui::TableNextColumn();
            ImGui::TextColored(kDim, "%s", i == 13 ? "sp " : i == 14 ? "lr " : i == 15 ? "pc " : ("r" + std::to_string(i) + (i < 10 ? " " : "")).c_str());
            ImGui::SameLine();
            ImGui::Text("%08X", cpu.reg(i));
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%llu instruções executadas", static_cast<unsigned long long>(console_->instructions()));

    ImGui::Separator();
    ImGui::Text("Últimas instruções");
    if (ImGui::BeginChild("##rastro", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        const auto trace = cpu.trace();
        for (auto it = trace.rbegin(); it != trace.rend(); ++it) { // mais recente primeiro
            if (it->pc == 0 && it->instr == 0) continue;
            if (it->thumb) ImGui::Text("%08X   T  %04X", it->pc, it->instr);
            else ImGui::Text("%08X   A  %08X", it->pc, it->instr);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------- memória

void App::drawMemoryWindow() {
    ImGui::SetNextWindowSize(ImVec2(620, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Memória", &showMemory_)) { ImGui::End(); return; }
    auto& bus = console_->bus();

    const Region& region = kRegions[memRegion_];
    ImGui::SetNextItemWidth(130);
    if (ImGui::BeginCombo("##regiao", region.name)) {
        for (int i = 0; i < static_cast<int>(std::size(kRegions)); ++i) {
            if (ImGui::Selectable(kRegions[i].name, i == memRegion_)) {
                memRegion_ = i;
                memAddress_ = kRegions[i].base;
                memJump_ = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    if (ImGui::InputScalar("Ir para", ImGuiDataType_U32, &memAddress_, nullptr, nullptr, "%08X",
                           ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
        for (int i = 0; i < static_cast<int>(std::size(kRegions)); ++i) { // acha a região do endereço digitado
            if (memAddress_ >= kRegions[i].base && memAddress_ < kRegions[i].base + kRegions[i].size) memRegion_ = i;
        }
        memJump_ = true;
    }
    const Region& r = kRegions[memRegion_];
    ImGui::SameLine();
    ImGui::TextDisabled("%08X - %08X", r.base, r.base + r.size - 1);

    if (ImGui::BeginChild("##hex", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const int rows = static_cast<int>(r.size / 16);
        if (memJump_ && memAddress_ >= r.base && memAddress_ < r.base + r.size) {
            ImGui::SetScrollY(static_cast<float>((memAddress_ - r.base) / 16) * lineH);
            memJump_ = false;
        }
        ImGuiListClipper clipper;
        clipper.Begin(rows, lineH);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const uint32_t addr = r.base + static_cast<uint32_t>(row) * 16;
                char line[128];
                int n = std::snprintf(line, sizeof line, "%08X  ", addr);
                char ascii[17] = {};
                for (uint32_t k = 0; k < 16; ++k) {
                    const uint8_t v = bus.peek8(addr + k);
                    n += std::snprintf(line + n, sizeof line - static_cast<size_t>(n), "%02X%s", v, k == 7 ? "  " : " ");
                    ascii[k] = (v >= 0x20 && v < 0x7F) ? static_cast<char>(v) : '.';
                }
                const bool selected = memAddress_ >= addr && memAddress_ < addr + 16;
                if (selected) ImGui::TextColored(kAccent, "%s %s", line, ascii);
                else ImGui::Text("%s %s", line, ascii);
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------- VRAM

void App::drawVramWindow() {
    ImGui::SetNextWindowSize(ImVec2(560, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("VRAM", &showVram_)) { ImGui::End(); return; }
    auto& bus = console_->bus();

    ImGui::Checkbox("256 cores", &vram8bpp_);
    ImGui::SameLine();
    ImGui::Checkbox("Paleta dos sprites", &vramObjPalette_);
    if (!vram8bpp_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderInt("Paleta", &vramPaletteBank_, 0, 15);
    }

    // 96 KB de tiles, 32 por linha. 16 cores: 32 bytes por tile (3072 tiles); 256 cores: 64 bytes (1536 tiles).
    constexpr int kTilesPerRow = 32;
    const int bytesPerTile = vram8bpp_ ? 64 : 32;
    const int tiles = 0x18000 / bytesPerTile;
    const int rows = tiles / kTilesPerRow;
    const int w = kTilesPerRow * 8, h = 0x18000 / 32 / kTilesPerRow * 8; // textura com espaço para o caso maior
    SDL_Texture* tex = debugTexture(vramTexture_, w, h);
    if (!tex) { ImGui::End(); return; }

    std::vector<uint32_t> pixels(static_cast<size_t>(w) * h, 0xFFF3E6F7u);
    const uint32_t paletteBase = vramObjPalette_ ? 0x05000200 : 0x05000000;
    const auto color = [&](uint32_t index) {
        const uint32_t a = paletteBase + index * 2;
        return argb(static_cast<uint16_t>(bus.peek8(a) | (bus.peek8(a + 1) << 8)));
    };
    for (int t = 0; t < tiles; ++t) {
        const int tx = (t % kTilesPerRow) * 8, ty = (t / kTilesPerRow) * 8;
        const uint32_t base = 0x06000000 + static_cast<uint32_t>(t * bytesPerTile);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                uint32_t index;
                if (vram8bpp_) {
                    index = bus.peek8(base + static_cast<uint32_t>(y * 8 + x));
                } else {
                    const uint8_t pair = bus.peek8(base + static_cast<uint32_t>(y * 4 + x / 2));
                    const uint32_t nibble = (x & 1) ? (pair >> 4) : (pair & 0xF);
                    index = nibble ? static_cast<uint32_t>(vramPaletteBank_) * 16 + nibble : 0;
                }
                pixels[static_cast<size_t>(ty + y) * w + tx + x] = color(index);
            }
        }
    }
    SDL_UpdateTexture(tex, nullptr, pixels.data(), w * static_cast<int>(sizeof(uint32_t)));

    ImGui::TextDisabled("Backgrounds: 06000000-0600FFFF  |  Sprites: 06010000-06017FFF");
    if (ImGui::BeginChild("##tiles", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        constexpr float kZoom = 2.0f;
        const float shownH = static_cast<float>(rows * 8);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Image(textureId(tex), ImVec2(static_cast<float>(w) * kZoom, shownH * kZoom), ImVec2(0, 0),
                     ImVec2(1, shownH / static_cast<float>(h)));
        if (ImGui::IsItemHovered()) {
            const ImVec2 m = ImGui::GetMousePos();
            const int tx = static_cast<int>((m.x - origin.x) / (8 * kZoom));
            const int ty = static_cast<int>((m.y - origin.y) / (8 * kZoom));
            const int tile = ty * kTilesPerRow + tx;
            if (tx >= 0 && tx < kTilesPerRow && tile >= 0 && tile < tiles) {
                const uint32_t addr = 0x06000000 + static_cast<uint32_t>(tile * bytesPerTile);
                ImGui::SetTooltip("Tile %d\n%08X\n%s", addr < 0x06010000 ? tile : static_cast<int>((addr - 0x06010000) / 32),
                                  addr, addr < 0x06010000 ? "área dos backgrounds" : "área dos sprites (número do tile na OAM)");
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------- paletas

void App::drawPaletteWindow() {
    ImGui::SetNextWindowSize(ImVec2(620, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Paletas", &showPalette_)) { ImGui::End(); return; }
    auto& bus = console_->bus();
    for (int half = 0; half < 2; ++half) {
        if (half) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(kAccent, "%s", half ? "Sprites (05000200)" : "Backgrounds (05000000)");
        for (int i = 0; i < 256; ++i) {
            const uint32_t addr = 0x05000000 + static_cast<uint32_t>(half * 0x200 + i * 2);
            const uint16_t c = static_cast<uint16_t>((bus.peek8(addr) | (bus.peek8(addr + 1) << 8)) & 0x7FFF);
            if (i % 16) ImGui::SameLine(0, 2);
            ImGui::PushID(half * 256 + i);
            ImGui::ColorButton("##cor", toVec4(c), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(16, 16));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Paleta %d, cor %d (índice %d)\n%08X = %04X\nR %d  G %d  B %d (0-31)", i / 16, i % 16, i,
                                  addr, c, c & 0x1F, (c >> 5) & 0x1F, (c >> 10) & 0x1F);
            }
            ImGui::PopID();
        }
        ImGui::EndGroup();
    }
    ImGui::TextDisabled("A cor 0 de cada paleta de 16 é transparente (exceto a cor 0 dos backgrounds: o fundo).");
    ImGui::End();
}

// ---------------------------------------------------------------- sprites

void App::drawSpritesWindow() {
    ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sprites", &showSprites_)) { ImGui::End(); return; }
    auto& bus = console_->bus();
    ImGui::Checkbox("Só os visíveis", &spritesOnlyVisible_);

    static constexpr int kWidth[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static constexpr int kHeight[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
    const auto half = [&bus](uint32_t a) { return static_cast<uint16_t>(bus.peek8(a) | (bus.peek8(a + 1) << 8)); };
    const bool oneD = bus.ioRead16(0x000) & 0x0040;

    // Miniaturas: cada sprite desenhado (sem rotação) numa célula de 64×64 de uma textura 1024×512.
    SDL_Texture* atlas = debugTexture(spriteTexture_, 1024, 512);
    std::vector<uint32_t> pixels(1024u * 512u, 0x00000000u);
    for (int i = 0; i < 128; ++i) {
        const uint32_t base = 0x07000000 + static_cast<uint32_t>(i) * 8;
        const uint16_t a0 = half(base), a1 = half(base + 2), a2 = half(base + 4);
        const int shape = a0 >> 14;
        if (shape == 3) continue;
        const int w = kWidth[shape][a1 >> 14], h = kHeight[shape][a1 >> 14];
        const bool c256 = a0 & 0x2000;
        const uint32_t tile = a2 & 0x3FF, bank = a2 >> 12;
        const uint32_t stride = oneD ? static_cast<uint32_t>(w / 8) * (c256 ? 2 : 1) : 32;
        const int cx = (i % 16) * 64, cy = (i / 16) * 64;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const uint32_t row = static_cast<uint32_t>(y / 8) * stride;
                uint32_t index;
                if (c256) {
                    const uint32_t unit = (tile + row + static_cast<uint32_t>(x / 8) * 2) & 0x3FF;
                    index = bus.peek8(0x06010000 + unit * 32 + static_cast<uint32_t>((y & 7) * 8 + (x & 7)));
                } else {
                    const uint32_t unit = (tile + row + static_cast<uint32_t>(x / 8)) & 0x3FF;
                    const uint8_t pair = bus.peek8(0x06010000 + unit * 32 + static_cast<uint32_t>((y & 7) * 4 + (x & 7) / 2));
                    const uint32_t nibble = (x & 1) ? (pair >> 4) : (pair & 0xF);
                    index = nibble ? bank * 16 + nibble : 0;
                }
                if (index) pixels[static_cast<size_t>(cy + y) * 1024 + cx + x] = argb(half(0x05000200 + index * 2));
            }
        }
    }
    if (atlas) {
        SDL_SetTextureBlendMode(atlas, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(atlas, nullptr, pixels.data(), 1024 * static_cast<int>(sizeof(uint32_t)));
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##oam", 9, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* col : {"#", "Imagem", "X", "Y", "Tamanho", "Tile", "Paleta", "Prior.", "Tipo"}) ImGui::TableSetupColumn(col);
        ImGui::TableHeadersRow();
        for (int i = 0; i < 128; ++i) {
            const uint32_t base = 0x07000000 + static_cast<uint32_t>(i) * 8;
            const uint16_t a0 = half(base), a1 = half(base + 2), a2 = half(base + 4);
            const bool affine = a0 & 0x0100;
            const bool hidden = !affine && (a0 & 0x0200);
            const int shape = a0 >> 14;
            if (shape == 3 || (spritesOnlyVisible_ && hidden)) continue;
            const int w = kWidth[shape][a1 >> 14], h = kHeight[shape][a1 >> 14];
            int x = a1 & 0x1FF;
            if (x >= 256) x -= 512;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(hidden ? kDim : kAccent, "%d", i);
            ImGui::TableNextColumn();
            if (atlas) {
                const float scale = 32.0f / static_cast<float>(std::max(w, h));
                const float u0 = static_cast<float>((i % 16) * 64) / 1024.0f, v0 = static_cast<float>((i / 16) * 64) / 512.0f;
                ImGui::Image(textureId(atlas), ImVec2(static_cast<float>(w) * scale, static_cast<float>(h) * scale), ImVec2(u0, v0),
                             ImVec2(u0 + static_cast<float>(w) / 1024.0f, v0 + static_cast<float>(h) / 512.0f));
            }
            ImGui::TableNextColumn(); ImGui::Text("%d", x);
            ImGui::TableNextColumn(); ImGui::Text("%d", a0 & 0xFF);
            ImGui::TableNextColumn(); ImGui::Text("%dx%d", w, h);
            ImGui::TableNextColumn(); ImGui::Text("%d", a2 & 0x3FF);
            ImGui::TableNextColumn(); ImGui::Text("%s", (a0 & 0x2000) ? "256" : std::to_string(a2 >> 12).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", (a2 >> 10) & 3);
            ImGui::TableNextColumn();
            const int mode = (a0 >> 10) & 3;
            ImGui::Text("%s%s%s", hidden ? "oculto" : affine ? "afim" : "normal",
                        mode == 1 ? ", transp." : mode == 2 ? ", janela" : "", (a0 & 0x1000) ? ", mosaico" : "");
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---------------------------------------------------------------- registradores de I/O

void App::drawIoWindow() {
    ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Registradores de I/O", &showIo_)) { ImGui::End(); return; }
    auto& bus = console_->bus();
    if (ImGui::BeginTabBar("##io")) {
        for (const auto& group : ioGroups()) {
            if (!ImGui::BeginTabItem(group.name)) continue;
            if (ImGui::BeginTable("##regs", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Endereço");
                ImGui::TableSetupColumn("Nome");
                ImGui::TableSetupColumn("Valor");
                ImGui::TableSetupColumn("O que é");
                ImGui::TableHeadersRow();
                for (const auto& reg : group.regs) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextDisabled("%08X", 0x04000000 + reg.offset);
                    ImGui::TableNextColumn(); ImGui::Text("%s", reg.name);
                    // Valor guardado internamente (inclusive de registradores só-escrita, que a CPU não consegue ler).
                    ImGui::TableNextColumn(); ImGui::TextColored(kAccent, "%04X", bus.ioRead16(reg.offset));
                    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", reg.description);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
