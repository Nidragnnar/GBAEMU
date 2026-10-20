#pragma once
// Patches de ROM (soft-patching): aplica um patch IPS, UPS ou BPS na memória, sem alterar o arquivo da ROM.
// É assim que traduções e hacks são distribuídos: o patch só tem as diferenças, não a ROM.
//
//   IPS ("PATCH"): o formato mais antigo. Registros (endereço, bytes) ou repetições; sem verificação.
//   UPS ("UPS1"):  diferenças por XOR, com o CRC32 da ROM de origem, do resultado e do próprio patch.
//   BPS ("BPS1"):  cópias da origem, do patch ou do próprio resultado; também com os três CRC32.
//
// Em UPS e BPS, uma ROM de origem diferente da esperada (outra versão do jogo, por exemplo) é detectada
// pelo CRC32 e gera um erro claro, em vez de um jogo corrompido.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace gba {

enum class PatchFormat { Ips, Ups, Bps };

struct PatchError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

uint32_t crc32(const uint8_t* data, size_t size);

// Identifica o formato pelo começo do arquivo. Lança PatchError se não for um patch conhecido.
PatchFormat detectPatchFormat(const std::vector<uint8_t>& patch);
const char* patchFormatName(PatchFormat format);

// Devolve a ROM com o patch aplicado. Lança PatchError com uma mensagem em português se algo estiver errado.
std::vector<uint8_t> applyPatch(const std::vector<uint8_t>& rom, const std::vector<uint8_t>& patch);

} // namespace gba
