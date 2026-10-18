#pragma once
// Ferramentas de diagnóstico e engenharia reversa do frontend (saída no terminal).

#include <cstdint>
#include <string>
#include <vector>

#include "core/gba.hpp"

namespace diag {

void writeTestPattern(gba::Bus& bus);                         // degradê no modo 3 (valida barramento -> tela)
void dumpState(gba::Gba& console, uint64_t frame);            // F1
void dumpTrace(gba::Gba& console);                            // F2
void printNewProbes(const gba::Gba& console, size_t& printed);
void dumpMemory(gba::Gba& console, uint32_t addr, uint32_t len); // --dump

void traceText(const std::vector<uint8_t>& rom, const std::string& text); // --rastrear-texto
void whoCalls(const std::vector<uint8_t>& rom, uint32_t addr);            // --quem-chama
void callsTo(const std::vector<uint8_t>& rom, uint32_t addr);             // --chamadas-para
void branchesTo(const std::vector<uint8_t>& rom, uint32_t target);        // --desvios-para

} // namespace diag
