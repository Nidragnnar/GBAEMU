#pragma once
// Preferências do usuário, guardadas em gbaemu.ini (texto simples, "chave=valor").

#include <array>
#include <string>
#include <vector>

struct Config {
    std::vector<std::string> recentRoms; // mais recente primeiro
    int scale = 3;
    bool mute = false;
    int volume = 80;                     // 0-100
    int stateSlot = 1;                   // 1-4
    // Teclas (scancodes do SDL) na ordem dos bits do KEYINPUT: A B SELECT START → ← ↑ ↓ R L.
    std::array<int, 10> keys{};

    Config();
    void addRecent(const std::string& path);
    static Config load(const std::string& file);
    void save(const std::string& file) const;

    static constexpr size_t kMaxRecent = 8;
};
