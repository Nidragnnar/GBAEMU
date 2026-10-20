#pragma once
// Cheats: códigos que gravam valores na memória do jogo a cada quadro, como os aparelhos GameShark,
// Action Replay e CodeBreaker faziam.
//
// Formatos aceitos:
//   Raw (VBA)          "02024284:63" (8 bits), ":0063" (16 bits), ":00000063" (32 bits)
//   CodeBreaker        "TAAAAAAA VVVV" sem criptografia. Tipos: 3 (8 bits), 8 (16 bits), 2 (OR), 6 (AND),
//                      7 (se igual), A (se diferente), D (se botões);
//                      0, 1 e 9 (código mestre, gancho e semente) são ignorados.
//   GameShark v1/v2    "XXXXXXXX YYYYYYYY" criptografado (TEA). Tipos: 0 (8 bits), 1 (16 bits), 2 (32 bits),
//                      D (se igual); o código mestre (F...) é ignorado.
// O Action Replay / GameShark v3 ainda não é suportado.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace gba {

class Bus;

enum class CheatFormat { Auto, Raw, CodeBreaker, GameShark };

struct CheatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Uma operação já decodificada.
struct CheatOp {
    enum class Kind : uint8_t { Write8, Write16, Write32, Or16, And16, IfEqual16, IfNotEqual16, IfKeys };
    Kind kind;
    uint32_t address;
    uint32_t value;
};

struct Cheat {
    std::string name;
    std::string code;          // como o usuário digitou (é o que fica salvo)
    CheatFormat format = CheatFormat::Auto;
    bool enabled = true;
    std::vector<CheatOp> ops;  // decodificado de "code"
    uint32_t hook = 0;         // gancho do código mestre (CodeBreaker tipo 1); 0 = nenhum
};

// Decodifica um código. Lança CheatError dizendo a linha e o problema.
std::vector<CheatOp> parseCheat(const std::string& code, CheatFormat format);
CheatFormat detectCheatFormat(const std::string& code);
// Gancho do CodeBreaker ("1AAAAAAA ...", no código mestre): os códigos rodam quando a CPU executa o endereço
// 08AAAAAA, como o aparelho fazia. Muitos códigos de Pokémon dependem disso. 0 se não houver.
uint32_t findCheatHook(const std::string& code, CheatFormat format);
const char* cheatFormatName(CheatFormat format);

// GameShark v1/v2: decifra (e cifra, para os testes) um par endereço/valor.
void gameSharkDecrypt(uint32_t& address, uint32_t& value);
void gameSharkEncrypt(uint32_t& address, uint32_t& value);

// Executa as operações de um cheat. keysPressed: bits do KEYINPUT, 1 = apertado (para o tipo D).
void applyCheat(const std::vector<CheatOp>& ops, Bus& bus, uint16_t keysPressed);

// Lista de cheats de um jogo, salva num arquivo .cht ao lado da ROM.
std::string serializeCheats(const std::vector<Cheat>& cheats);
std::vector<Cheat> deserializeCheats(const std::string& text); // códigos inválidos ficam desligados

} // namespace gba
