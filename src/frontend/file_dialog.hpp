#pragma once
// Diálogo nativo "Abrir arquivo". No Windows usa o da própria API; nos outros sistemas, por enquanto,
// não há diálogo (dá para arrastar a ROM para a janela).

#include <optional>
#include <string>

struct SDL_Window;

std::optional<std::string> openRomDialog(SDL_Window* owner); // caminho em UTF-8, ou nada se cancelado
