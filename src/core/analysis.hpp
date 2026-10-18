#pragma once
// Ferramentas de engenharia reversa sobre a ROM (usadas pelo frontend com --rastrear-texto).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gba::analysis {

// Converte texto ASCII para a tabela de caracteres dos jogos de Pokémon da 3ª geração.
// Devolve vazio se houver um caractere sem equivalente.
std::vector<uint8_t> encodePokemonText(const std::string& text);

// Início provável de um texto do Pokémon que contém offset: logo depois do terminador 0xFF anterior.
// Os textos costumam começar com códigos de controle (cor, fonte) antes das letras.
uint32_t findStringStart(const std::vector<uint8_t>& rom, uint32_t offset, uint32_t maxDistance = 0x80);

// Todas as posições (deslocamento na ROM) onde a sequência aparece.
std::vector<uint32_t> findBytes(const std::vector<uint8_t>& rom, const std::vector<uint8_t>& needle);

// Palavras de 32 bits alinhadas iguais a value (ponteiros em "literal pools" e tabelas).
std::vector<uint32_t> findWord(const std::vector<uint8_t>& rom, uint32_t value);

// Destino de um BL Thumb (duas halfwords) em addr, se houver um ali.
std::optional<uint32_t> thumbBlTarget(const std::vector<uint8_t>& rom, uint32_t offset);

// Endereços (0x08xxxxxx) de todos os BL Thumb que chamam target.
std::vector<uint32_t> findThumbCallers(const std::vector<uint8_t>& rom, uint32_t target);

// Desvios Thumb (B incondicional e Bxx condicional) que saltam para target, a até range bytes de distância.
std::vector<uint32_t> findThumbBranchesTo(const std::vector<uint8_t>& rom, uint32_t target, uint32_t range = 0x1000);

// Procura para trás, a partir de offset, o PUSH {..., lr} que provavelmente abre a função.
std::optional<uint32_t> findFunctionStart(const std::vector<uint8_t>& rom, uint32_t offset, uint32_t maxDistance = 0x800);

} // namespace gba::analysis
