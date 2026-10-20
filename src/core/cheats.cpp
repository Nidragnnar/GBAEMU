#include "cheats.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include "bus.hpp"

namespace gba {

namespace {

// Chaves do GameShark v1/v2 (TEA, 32 rodadas).
constexpr uint32_t kSeeds[4] = {0x09F4FBBD, 0x9681884A, 0x352027E9, 0xF3DEE5A7};
constexpr uint32_t kDelta = 0x9E3779B9;

struct Line {
    int number;       // linha no texto original (para as mensagens de erro)
    std::string text; // só os dígitos hexadecimais (e ':' no formato raw), em maiúsculas
};

std::vector<Line> splitLines(const std::string& code) {
    std::vector<Line> lines;
    std::istringstream in(code);
    std::string raw;
    int number = 0;
    while (std::getline(in, raw)) {
        ++number;
        std::string clean;
        for (const char c : raw) {
            if (std::isxdigit(static_cast<unsigned char>(c))) clean += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            else if (c == ':') clean += c;
            else if (c == '#' || c == ';') break; // comentário até o fim da linha
        }
        if (!clean.empty()) lines.push_back({number, clean});
    }
    return lines;
}

uint32_t hex(const std::string& s) { return static_cast<uint32_t>(std::stoul(s, nullptr, 16)); }

[[noreturn]] void fail(const Line& line, const std::string& what) {
    throw CheatError("linha " + std::to_string(line.number) + ": " + what);
}

std::string typeName(uint32_t t) {
    const char digits[] = "0123456789ABCDEF";
    return std::string(1, digits[t & 0xF]);
}

std::vector<CheatOp> parseRaw(const std::vector<Line>& lines) {
    std::vector<CheatOp> ops;
    for (const auto& line : lines) {
        const auto colon = line.text.find(':');
        if (colon != 8) fail(line, "o formato raw é AAAAAAAA:VV (endereço de 8 dígitos, dois-pontos e o valor)");
        const std::string value = line.text.substr(9);
        const uint32_t address = hex(line.text.substr(0, 8));
        if (value.size() == 2) ops.push_back({CheatOp::Kind::Write8, address, hex(value)});
        else if (value.size() == 4) ops.push_back({CheatOp::Kind::Write16, address, hex(value)});
        else if (value.size() == 8) ops.push_back({CheatOp::Kind::Write32, address, hex(value)});
        else fail(line, "o valor deve ter 2, 4 ou 8 dígitos");
    }
    return ops;
}

std::vector<CheatOp> parseCodeBreaker(const std::vector<Line>& lines) {
    std::vector<CheatOp> ops;
    for (const auto& line : lines) {
        if (line.text.size() != 12) fail(line, "o CodeBreaker tem 12 dígitos por linha (TAAAAAAA VVVV)");
        const uint32_t type = hex(line.text.substr(0, 1));
        const uint32_t address = hex(line.text.substr(1, 7));
        const uint32_t value = hex(line.text.substr(8, 4));
        switch (type) {
        case 0x0: case 0x1: break; // código mestre (identificação do jogo) e gancho: não precisam aqui
        case 0x2: ops.push_back({CheatOp::Kind::Or16, address, value}); break;
        case 0x3: ops.push_back({CheatOp::Kind::Write8, address, value & 0xFF}); break;
        case 0x6: ops.push_back({CheatOp::Kind::And16, address, value}); break;
        case 0x7: ops.push_back({CheatOp::Kind::IfEqual16, address, value}); break;
        case 0x8: ops.push_back({CheatOp::Kind::Write16, address, value}); break;
        case 0x9: fail(line, "CodeBreaker criptografado (tipo 9) ainda não é suportado");
        case 0xA: ops.push_back({CheatOp::Kind::IfNotEqual16, address, value}); break;
        case 0xD: ops.push_back({CheatOp::Kind::IfKeys, address, value}); break;
        default: fail(line, "o tipo " + typeName(type) + " do CodeBreaker ainda não é suportado");
        }
    }
    return ops;
}

std::vector<CheatOp> parseGameShark(const std::vector<Line>& lines) {
    std::vector<CheatOp> ops;
    for (const auto& line : lines) {
        if (line.text.size() != 16) fail(line, "o GameShark tem 16 dígitos por linha (XXXXXXXX YYYYYYYY)");
        uint32_t address = hex(line.text.substr(0, 8));
        uint32_t value = hex(line.text.substr(8, 8));
        gameSharkDecrypt(address, value);
        if (address == 0xDEADFACE) fail(line, "a troca de chaves do GameShark (DEADFACE) ainda não é suportada");
        const uint32_t type = address >> 28;
        const uint32_t target = address & 0x0FFFFFFF;
        switch (type) {
        case 0x0: ops.push_back({CheatOp::Kind::Write8, target, value & 0xFF}); break;
        case 0x1: ops.push_back({CheatOp::Kind::Write16, target, value & 0xFFFF}); break;
        case 0x2: ops.push_back({CheatOp::Kind::Write32, target, value}); break;
        case 0xD: ops.push_back({CheatOp::Kind::IfEqual16, target, value & 0xFFFF}); break;
        case 0xF: break; // código mestre / gancho do aparelho
        default:
            fail(line, "o tipo " + typeName(type) + " do GameShark ainda não é suportado (é um código v3 / Action Replay?)");
        }
    }
    return ops;
}

} // namespace

void gameSharkDecrypt(uint32_t& address, uint32_t& value) {
    uint32_t sum = kDelta * 32;
    for (int round = 0; round < 32; ++round) {
        value -= ((address << 4) + kSeeds[2]) ^ (address + sum) ^ ((address >> 5) + kSeeds[3]);
        address -= ((value << 4) + kSeeds[0]) ^ (value + sum) ^ ((value >> 5) + kSeeds[1]);
        sum -= kDelta;
    }
}

void gameSharkEncrypt(uint32_t& address, uint32_t& value) {
    uint32_t sum = 0;
    for (int round = 0; round < 32; ++round) {
        sum += kDelta;
        address += ((value << 4) + kSeeds[0]) ^ (value + sum) ^ ((value >> 5) + kSeeds[1]);
        value += ((address << 4) + kSeeds[2]) ^ (address + sum) ^ ((address >> 5) + kSeeds[3]);
    }
}

CheatFormat detectCheatFormat(const std::string& code) {
    const auto lines = splitLines(code);
    if (lines.empty()) return CheatFormat::Raw;
    if (lines[0].text.find(':') != std::string::npos) return CheatFormat::Raw;
    if (lines[0].text.size() == 12) return CheatFormat::CodeBreaker;
    return CheatFormat::GameShark;
}

uint32_t findCheatHook(const std::string& code, CheatFormat format) {
    if (format == CheatFormat::Auto) format = detectCheatFormat(code);
    if (format != CheatFormat::CodeBreaker) return 0;
    for (const auto& line : splitLines(code)) {
        if (line.text.size() == 12 && line.text[0] == '1') return 0x08000000u | (hex(line.text.substr(1, 7)) & 0x01FFFFFFu);
    }
    return 0;
}

const char* cheatFormatName(CheatFormat format) {
    switch (format) {
    case CheatFormat::Auto: return "Automático";
    case CheatFormat::Raw: return "Raw (VBA)";
    case CheatFormat::CodeBreaker: return "CodeBreaker";
    case CheatFormat::GameShark: return "GameShark v1/v2";
    }
    return "?";
}

std::vector<CheatOp> parseCheat(const std::string& code, CheatFormat format) {
    const auto lines = splitLines(code);
    if (lines.empty()) throw CheatError("o código está vazio");
    if (format == CheatFormat::Auto) format = detectCheatFormat(code);
    switch (format) {
    case CheatFormat::Raw: return parseRaw(lines);
    case CheatFormat::CodeBreaker: return parseCodeBreaker(lines);
    case CheatFormat::GameShark: return parseGameShark(lines);
    case CheatFormat::Auto: break;
    }
    throw CheatError("formato desconhecido");
}

void applyCheat(const std::vector<CheatOp>& ops, Bus& bus, uint16_t keysPressed) {
    for (size_t i = 0; i < ops.size(); ++i) {
        const CheatOp& op = ops[i];
        bool skipNext = false;
        switch (op.kind) {
        case CheatOp::Kind::Write8: bus.write8(op.address, static_cast<uint8_t>(op.value)); break;
        case CheatOp::Kind::Write16: bus.write16(op.address, static_cast<uint16_t>(op.value)); break;
        case CheatOp::Kind::Write32: bus.write32(op.address, op.value); break;
        case CheatOp::Kind::Or16: bus.write16(op.address, static_cast<uint16_t>(bus.read16(op.address) | op.value)); break;
        case CheatOp::Kind::And16: bus.write16(op.address, static_cast<uint16_t>(bus.read16(op.address) & op.value)); break;
        // Condições: se não forem verdadeiras, a linha seguinte não é executada.
        case CheatOp::Kind::IfEqual16: skipNext = bus.read16(op.address) != op.value; break;
        case CheatOp::Kind::IfNotEqual16: skipNext = bus.read16(op.address) == op.value; break;
        case CheatOp::Kind::IfKeys: skipNext = (keysPressed & op.value) != op.value; break;
        }
        if (skipNext) ++i;
    }
}

std::string serializeCheats(const std::vector<Cheat>& cheats) {
    std::ostringstream out;
    out << "# Cheats do NiGBA\n";
    for (const auto& c : cheats) {
        out << "\n[" << c.name << "]\n";
        out << "ativo=" << (c.enabled ? 1 : 0) << "\n";
        out << "formato=" << static_cast<int>(c.format) << "\n";
        std::istringstream lines(c.code);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) out << "codigo=" << line << "\n";
        }
    }
    return out.str();
}

std::vector<Cheat> deserializeCheats(const std::string& text) {
    std::vector<Cheat> cheats;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            cheats.push_back({});
            cheats.back().name = line.substr(1, line.size() - 2);
            continue;
        }
        if (cheats.empty()) continue;
        Cheat& c = cheats.back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "ativo") c.enabled = value == "1";
        else if (key == "formato") c.format = static_cast<CheatFormat>(std::clamp(std::atoi(value.c_str()), 0, 3));
        else if (key == "codigo") c.code += (c.code.empty() ? "" : "\n") + value;
    }
    for (auto& c : cheats) {
        try {
            c.ops = parseCheat(c.code, c.format);
            c.hook = findCheatHook(c.code, c.format);
        } catch (const CheatError&) {
            c.enabled = false; // código inválido fica guardado, mas desligado
        }
    }
    return cheats;
}

} // namespace gba
