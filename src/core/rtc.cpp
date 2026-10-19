#include "rtc.hpp"

namespace gba {

namespace {
// Comandos (bits 4-6 do byte de comando) e quantos bytes de dados cada um tem.
constexpr int kReset = 0, kDateTime = 2, kControl = 4, kTime = 6;
constexpr int kCommandBytes[8] = {0, 0, 7, 0, 1, 0, 3, 0};

constexpr uint8_t kSck = 1, kSio = 2, kCs = 4;

uint8_t toBcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
bool isRead(uint8_t command) { return command & 0x80; }
int commandOf(uint8_t command) { return (command >> 4) & 7; }
} // namespace

Rtc::Rtc() : clock_([] { return std::time(nullptr); }) {}

void Rtc::write(uint32_t offset, uint16_t value) {
    switch (offset) {
    case 0xC4:
        // Só os pinos configurados como saída do GBA mudam com a escrita.
        pins_ = static_cast<uint8_t>((pins_ & ~direction_) | (value & direction_ & 0xF));
        updatePins();
        break;
    case 0xC6: direction_ = static_cast<uint8_t>(value & 0xF); break;
    case 0xC8: readable_ = value & 1; break;
    default: break;
    }
}

uint16_t Rtc::read(uint32_t offset) const {
    switch (offset) {
    case 0xC4: return pins_;
    case 0xC6: return direction_;
    case 0xC8: return readable_ ? 1 : 0;
    default: return 0;
    }
}

void Rtc::updatePins() {
    switch (transferStep_) {
    case 0: // espera SCK alto com CS baixo
        if ((pins_ & (kSck | kCs)) == kSck) transferStep_ = 1;
        break;
    case 1: // CS sobe: começa uma transferência
        if ((pins_ & (kSck | kCs)) == (kSck | kCs)) transferStep_ = 2;
        else if ((pins_ & (kSck | kCs)) != kSck) transferStep_ = 0;
        break;
    case 2:
        if (!(pins_ & kSck)) {
            // SCK baixo: o jogo está colocando um bit em SIO.
            bits_ = static_cast<uint8_t>((bits_ & ~(1u << bitsRead_)) | (((pins_ & kSio) >> 1) << bitsRead_));
        } else if (pins_ & kCs) {
            // SCK subiu com CS alto: o bit vale.
            if (!isRead(command_)) {
                if (++bitsRead_ == 8) processByte();
            } else {
                // Leitura: o relógio coloca o próximo bit em SIO (se SIO for entrada para o GBA).
                const uint8_t out = static_cast<uint8_t>(kSck | kCs | (outputBit() << 1));
                pins_ = static_cast<uint8_t>((pins_ & direction_) | (out & ~direction_ & 0xF));
                if (++bitsRead_ == 8) {
                    bitsRead_ = 0;
                    if (--bytesRemaining_ <= 0) {
                        commandActive_ = false;
                        command_ = 0;
                    }
                }
            }
        } else {
            // CS caiu: fim da transferência.
            bitsRead_ = 0;
            bytesRemaining_ = 0;
            commandActive_ = false;
            command_ = 0;
            transferStep_ = pins_ & kSck;
            pins_ = static_cast<uint8_t>((pins_ & direction_) | (kSck & ~direction_));
        }
        break;
    default:
        transferStep_ = 0;
        break;
    }
}

void Rtc::processByte() {
    --bytesRemaining_;
    if (!commandActive_) {
        // Primeiro byte: o comando. Os bits 0-3 precisam ser 0110.
        if ((bits_ & 0xF) == 0x6) {
            command_ = bits_;
            bytesRemaining_ = kCommandBytes[commandOf(command_)];
            commandActive_ = bytesRemaining_ > 0;
            switch (commandOf(command_)) {
            case kReset: control_ = 0; break;
            case kDateTime:
            case kTime: latchTime(); break;
            default: break;
            }
        }
    } else if (commandOf(command_) == kControl) {
        control_ = bits_; // o jogo configurou o relógio (por exemplo, modo 24 horas)
    }
    // Gravar data/hora é ignorado: o relógio segue sempre a hora do computador.
    bits_ = 0;
    bitsRead_ = 0;
    if (bytesRemaining_ <= 0) {
        commandActive_ = false;
        command_ = 0;
    }
}

uint8_t Rtc::outputBit() const {
    if (!commandActive_) return 0;
    uint8_t byte = 0;
    switch (commandOf(command_)) {
    case kControl: byte = control_; break;
    case kDateTime:
    case kTime: byte = time_[static_cast<size_t>(7 - bytesRemaining_)]; break; // TIME começa na hora (índice 4)
    default: break;
    }
    return static_cast<uint8_t>((byte >> bitsRead_) & 1);
}

void Rtc::latchTime() {
    const std::time_t now = clock_();
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    time_[0] = toBcd((t.tm_year + 1900) % 100);
    time_[1] = toBcd(t.tm_mon + 1);
    time_[2] = toBcd(t.tm_mday);
    time_[3] = toBcd(t.tm_wday);
    time_[4] = toBcd((control_ & 0x40) ? t.tm_hour : t.tm_hour % 12); // 24 h ou 12 h
    time_[5] = toBcd(t.tm_min);
    time_[6] = toBcd(t.tm_sec);
}

void Rtc::serialize(StateArchive& ar) {
    ar.value(pins_);
    ar.value(direction_);
    ar.value(readable_);
    ar.value(transferStep_);
    ar.value(bits_);
    ar.value(bitsRead_);
    ar.value(command_);
    ar.value(bytesRemaining_);
    ar.value(commandActive_);
    ar.value(control_);
    ar.value(time_);
}

} // namespace gba
