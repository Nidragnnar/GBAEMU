#include "frontend/config.hpp"

#include <SDL.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "core/cartridge.hpp"

std::array<int, 10> Config::defaultKeys() {
    return {SDL_SCANCODE_Z,     SDL_SCANCODE_X,    SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_RETURN,
            SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT, SDL_SCANCODE_UP,        SDL_SCANCODE_DOWN,
            SDL_SCANCODE_S,     SDL_SCANCODE_A};
}

// Pela posição, como no GBA: o A fica à direita (B no Xbox, círculo no PlayStation) e o B embaixo.
std::array<int, 10> Config::defaultPadButtons() {
    return {SDL_CONTROLLER_BUTTON_B,          SDL_CONTROLLER_BUTTON_A,         SDL_CONTROLLER_BUTTON_BACK,
            SDL_CONTROLLER_BUTTON_START,      SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
            SDL_CONTROLLER_BUTTON_DPAD_UP,   SDL_CONTROLLER_BUTTON_DPAD_DOWN,  SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER};
}

Config::Config() : keys(defaultKeys()), padButtons(defaultPadButtons()) {}

void Config::addRecent(const std::string& path) {
    recentRoms.erase(std::remove(recentRoms.begin(), recentRoms.end(), path), recentRoms.end());
    recentRoms.insert(recentRoms.begin(), path);
    if (recentRoms.size() > kMaxRecent) recentRoms.resize(kMaxRecent);
}

Config Config::load(const std::string& file) {
    Config c;
    std::ifstream in(gba::utf8Path(file));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        try {
            if (key == "recente") { if (c.recentRoms.size() < kMaxRecent) c.recentRoms.push_back(value); }
            else if (key == "escala") c.scale = std::clamp(std::stoi(value), 1, 8);
            else if (key == "mudo") c.mute = value == "1";
            else if (key == "volume") c.volume = std::clamp(std::stoi(value), 0, 100);
            else if (key == "slot") c.stateSlot = std::clamp(std::stoi(value), 1, 4);
            else if (key == "teclas" || key == "controle") {
                auto& target = key == "teclas" ? c.keys : c.padButtons;
                std::stringstream ss(value);
                std::string item;
                for (size_t i = 0; i < target.size() && std::getline(ss, item, ','); ++i) target[i] = std::stoi(item);
            }
        } catch (const std::exception&) {
            // valor inválido: mantém o padrão
        }
    }
    return c;
}

void Config::save(const std::string& file) const {
    std::ostringstream out;
    out << "# Preferências do NiGBA\n";
    out << "escala=" << scale << "\nmudo=" << (mute ? 1 : 0) << "\nvolume=" << volume << "\nslot=" << stateSlot << "\n";
    out << "teclas=";
    for (size_t i = 0; i < keys.size(); ++i) out << (i ? "," : "") << keys[i];
    out << "\ncontrole=";
    for (size_t i = 0; i < padButtons.size(); ++i) out << (i ? "," : "") << padButtons[i];
    out << "\n";
    for (const auto& r : recentRoms) out << "recente=" << r << "\n";
    const std::string text = out.str();
    try {
        gba::writeFileSafely(file, std::vector<uint8_t>(text.begin(), text.end()));
    } catch (const std::exception&) {
        // sem permissão de escrita: as preferências só não ficam salvas
    }
}
