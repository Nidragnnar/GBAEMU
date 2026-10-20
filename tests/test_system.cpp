// Testes do sistema: interrupções, tratador de IRQ da BIOS (HLE) e temporização do vídeo.
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <iterator>
#include <vector>

#include "core/gba.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FALHOU %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

// ROM mínima: na entrada (0x08000000) só um laço infinito "B ." em ARM.
static std::vector<uint8_t> loopRom() {
    std::vector<uint8_t> rom(0x200, 0);
    rom[0] = 0xFE; rom[1] = 0xFF; rom[2] = 0xFF; rom[3] = 0xEA; // EAFFFFFE
    return rom;
}

static void writeWords(gba::Bus& bus, uint32_t addr, std::initializer_list<uint32_t> words) {
    for (uint32_t w : words) { bus.write32(addr, w); addr += 4; }
}

static void testIoRules() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();

    bus.ioWrite16Raw(0x004, 0x0003);          // hardware: VBlank + HBlank
    bus.write16(0x04000004, 0xFFF8);          // CPU não consegue mudar os bits 0-2
    CHECK(bus.read16(0x04000004) == 0xFF3B);   // e os bits 6-7 não existem
    bus.write16(0x04000006, 0x1234);          // VCOUNT é só leitura
    CHECK(bus.read16(0x04000006) == 0);

    bus.requestInterrupt(gba::irq::VBlank | gba::irq::HBlank);
    bus.write16(0x04000202, gba::irq::VBlank); // escrever 1 em IF confirma só aquele bit
    CHECK(bus.read16(0x04000202) == gba::irq::HBlank);

    CHECK(!bus.irqPending());                  // IME e IE desligados
    bus.write16(0x04000200, gba::irq::HBlank);
    CHECK(!bus.irqPending());
    bus.write16(0x04000208, 1);
    CHECK(bus.irqPending());
}

static void testIrqThroughBiosStub() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    auto& cpu = g.cpu();

    // Tratador do jogo em ARM: marca r5, confirma a IRQ em IF e volta com BX lr.
    constexpr uint32_t kHandler = 0x02000000;
    writeWords(bus, kHandler, {
        0xE3A05077,   // mov  r5, #0x77
        0xE3A01301,   // mov  r1, #0x04000000
        0xE2811C02,   // add  r1, r1, #0x200
        0xE3A02001,   // mov  r2, #1
        0xE1C120B2,   // strh r2, [r1, #2]   ; IF = 1 (confirma o VBlank)
        0xE12FFF1E}); // bx   lr
    bus.write32(0x03007FFC, kHandler);  // onde a BIOS procura o tratador do jogo

    // Programa principal em Thumb: laço infinito.
    bus.write16(0x02000100, 0xE7FE);
    cpu.jumpTo(0x02000100, true);
    cpu.setReg(1, 0x1111);

    bus.write16(0x04000200, gba::irq::VBlank); // IE
    bus.write16(0x04000208, 1);                // IME
    bus.requestInterrupt(gba::irq::VBlank);

    cpu.step();                                // entra na IRQ
    CHECK(cpu.mode() == gba::CpuMode::Irq);
    CHECK(!cpu.thumb());
    CHECK(cpu.pc() == 0x18);

    for (int i = 0; i < 30; ++i) cpu.step();   // BIOS + tratador + retorno + laço
    CHECK(cpu.reg(5) == 0x77);
    CHECK(cpu.reg(1) == 0x1111);               // r0-r3 restaurados pela BIOS
    CHECK(bus.read16(0x04000202) == 0);
    CHECK(cpu.mode() == gba::CpuMode::System);
    CHECK(cpu.thumb());
    CHECK(cpu.pc() == 0x02000100);
    CHECK(!(cpu.cpsr() & gba::flag::I));
}

static void testIrqMaskedByCpsr() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    bus.write16(0x04000200, gba::irq::VBlank);
    bus.write16(0x04000208, 1);
    // Desliga as IRQs no CPSR: MSR CPSR_c, #0x9F (System + I), e só depois a interrupção chega.
    writeWords(bus, 0x02000000, {0xE321F09F, 0xEAFFFFFE});
    g.cpu().jumpTo(0x02000000);
    g.cpu().step();
    bus.requestInterrupt(gba::irq::VBlank);
    CHECK(bus.irqPending());
    for (int i = 0; i < 5; ++i) g.cpu().step();
    CHECK(g.cpu().mode() == gba::CpuMode::System);
}

static void testVideoTiming() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();

    // Habilita IRQ de VBlank e de VCOUNT na linha 100 (IME desligado: só observamos IF).
    bus.write16(0x04000004, static_cast<uint16_t>((100 << 8) | 0x0028));
    g.runFrame();
    const uint16_t flags = bus.read16(0x04000202);
    CHECK(flags & gba::irq::VBlank);
    CHECK(flags & gba::irq::VCount);
    CHECK(!(flags & gba::irq::HBlank));         // não foi habilitada
    CHECK(bus.read16(0x04000006) == 227);       // última linha do quadro
    CHECK(!(bus.read16(0x04000004) & 1));       // na linha 227 a flag de VBlank já caiu

    // Dentro do VBlank a flag fica ligada.
    g.ppu().startLine(200);
    CHECK(bus.read16(0x04000004) & 1);
    CHECK(bus.read16(0x04000006) == 200);
}

// O padrão do Pokémon: o tratador de VBlank marca uma variável e o laço principal espera por ela.
static void testWaitForVBlankPattern() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    auto& cpu = g.cpu();

    constexpr uint32_t kFlag = 0x03000000;
    writeWords(bus, 0x02000000, {           // tratador: [kFlag] = 1, confirma VBlank
        0xE3A00403,   // mov  r0, #0x03000000
        0xE3A01001,   // mov  r1, #1
        0xE5801000,   // str  r1, [r0]
        0xE3A00301,   // mov  r0, #0x04000000
        0xE2800C02,   // add  r0, r0, #0x200
        0xE1C010B2,   // strh r1, [r0, #2]
        0xE12FFF1E}); // bx   lr
    bus.write32(0x03007FFC, 0x02000000);

    writeWords(bus, 0x02000100, {           // laço principal: espera [kFlag] != 0
        0xE3A00403,   // mov  r0, #0x03000000
        0xE5901000,   // ldr  r1, [r0]
        0xE3510000,   // cmp  r1, #0
        0x0AFFFFFC,   // beq  (volta para o ldr)
        0xE3A07042,   // mov  r7, #0x42  ; só chega aqui depois do VBlank
        0xEAFFFFFE}); // b    .
    cpu.jumpTo(0x02000100);

    bus.write16(0x04000004, 0x0008);         // DISPSTAT: IRQ de VBlank
    bus.write16(0x04000200, gba::irq::VBlank);
    bus.write16(0x04000208, 1);
    g.runFrame();
    CHECK(bus.read32(kFlag) == 1);
    CHECK(cpu.reg(7) == 0x42);
}

// ------------------------------------------------------------------ timers

static void testTimerBasics() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    auto& t = g.timers();

    bus.write16(0x04000100, 0xFFF0);                   // recarga
    bus.write16(0x04000102, 0x00C0);                   // liga, IRQ, prescaler 1
    CHECK(bus.read16(0x04000100) == 0xFFF0);           // começa na recarga
    t.advance(2);                                      // atraso de partida: 2 ciclos sem contar
    CHECK(bus.read16(0x04000100) == 0xFFF0);
    t.advance(5);
    CHECK(bus.read16(0x04000100) == 0xFFF5);           // ler devolve o contador
    t.advance(11);                                     // 0xFFF5 + 11 estoura
    CHECK(bus.read16(0x04000100) == 0xFFF0);
    CHECK(bus.read16(0x04000202) & gba::irq::Timer0);

    bus.write16(0x04000100, 0x1234);                   // nova recarga não mexe no contador
    CHECK(bus.read16(0x04000100) == 0xFFF0);
    t.advance(16);
    CHECK(bus.read16(0x04000100) == 0x1234);           // usada no próximo estouro

    bus.write16(0x04000106, 0x0083);                   // timer 1: prescaler 1024
    t.advance(2);                                      // atraso de partida
    t.advance(1023);
    CHECK(bus.read16(0x04000104) == 0);
    t.advance(1);
    CHECK(bus.read16(0x04000104) == 1);

    bus.write16(0x04000102, 0);                        // desligado: para de contar
    const uint16_t frozen = bus.read16(0x04000100);
    t.advance(100);
    CHECK(bus.read16(0x04000100) == frozen);
}

static void testTimerCascade() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    bus.write16(0x04000100, 0xFFFF);                   // timer 0 estoura a cada ciclo
    bus.write16(0x04000102, 0x0080);
    bus.write16(0x04000106, 0x0084);                   // timer 1 em cascata (sem atraso próprio)
    g.timers().advance(2);                             // atraso de partida do timer 0
    g.timers().advance(3);
    CHECK(bus.read16(0x04000104) == 3);
}

// O laço exato em que o Rogue travava (0800DEA6-0800DEC2): liga o Timer 3 com prescaler
// de 1024 e espera o contador passar de 0x11 enquanto escreve no registrador serial.
static void testRogueSerialWait() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    auto& cpu = g.cpu();
    const uint16_t program[] = {
        0x2200,  // mov  r2, #0
        0x805A,  // strh r2, [r3, #2]   ; TM3CNT_H = 0 (desliga)
        0x801A,  // strh r2, [r3]       ; recarga = 0
        0x2283,  // mov  r2, #0x83
        0x805A,  // strh r2, [r3, #2]   ; liga, prescaler 1024
        0x800C,  // strh r4, [r1]       ; RCNT     <- laço do jogo
        0x881A,  // ldrh r2, [r3]       ; contador do Timer 3
        0x2A11,  // cmp  r2, #0x11
        0xD9FB,  // bls  (volta)
        0x2742,  // mov  r7, #0x42      ; só chega aqui quando o timer andou
        0xE7FE,  // b .
    };
    for (size_t i = 0; i < std::size(program); ++i) bus.write16(0x02000000 + static_cast<uint32_t>(i) * 2, program[i]);
    cpu.jumpTo(0x02000000, true);
    cpu.setReg(1, 0x04000134);
    cpu.setReg(3, 0x0400010C);
    cpu.setReg(4, 0xFFFF80A2);
    g.runFrame();
    CHECK(cpu.reg(7) == 0x42);
}

// ------------------------------------------------------------------ espera (HLE)

// Tratador de IRQ típico: marca a interrupção em [03007FF8] (como os jogos fazem) e confirma em IF.
static void installMarkingHandler(gba::Bus& bus) {
    writeWords(bus, 0x02000000, {
        0xE3A00301,   // mov   r0, #0x04000000
        0xE2800C02,   // add   r0, r0, #0x200
        0xE1D010B2,   // ldrh  r1, [r0, #2]      ; IF
        0xE1C010B2,   // strh  r1, [r0, #2]      ; confirma tudo o que chegou
        0xE59F2010,   // ldr   r2, =0x03007FF8
        0xE1D230B0,   // ldrh  r3, [r2]
        0xE1833001,   // orr   r3, r3, r1
        0xE1C230B0,   // strh  r3, [r2]          ; marca para a BIOS
        0xE12FFF1E,   // bx    lr
        0x00000000,
        0x03007FF8}); // literal do ldr acima
    bus.write32(0x03007FFC, 0x02000000);
}

static void testVBlankIntrWait() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    auto& cpu = g.cpu();
    installMarkingHandler(bus);
    const uint16_t program[] = {
        0xDF05,  // swi  5               ; VBlankIntrWait
        0x88CE,  // ldrh r6, [r1, #6]    ; VCOUNT logo depois de acordar
        0x2742,  // mov  r7, #0x42
        0xE7FE,  // b .
    };
    for (size_t i = 0; i < std::size(program); ++i) bus.write16(0x02000100 + static_cast<uint32_t>(i) * 2, program[i]);
    cpu.jumpTo(0x02000100, true);
    cpu.setReg(1, 0x04000000);
    // VBlank e também VCOUNT na linha 50: a de VCOUNT é atendida no meio e a espera continua.
    bus.write16(0x04000004, static_cast<uint16_t>((50 << 8) | 0x0028));
    bus.write16(0x04000200, gba::irq::VBlank | gba::irq::VCount); // IME fica por conta da SWI

    g.runFrame();
    CHECK(cpu.reg(7) == 0x42);
    CHECK(cpu.reg(6) == 160);                            // acordou no início do VBlank
    CHECK((bus.read16(0x03007FF8) & gba::irq::VBlank) == 0); // a SWI consome a marcação
    CHECK(bus.read16(0x03007FF8) & gba::irq::VCount);         // a outra foi atendida e ficou marcada
    CHECK(!cpu.sleeping());
}

static void testIntrWaitAlreadySignaled() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    auto& cpu = g.cpu();
    bus.write16(0x03007FF8, gba::irq::VCount);           // a interrupção já tinha chegado
    bus.write16(0x02000100, 0xDF04);                     // swi 4 (IntrWait) com r0 = 0
    bus.write16(0x02000102, 0x2742);                     // mov r7, #0x42
    cpu.jumpTo(0x02000100, true);
    cpu.setReg(0, 0);
    cpu.setReg(1, gba::irq::VCount);
    cpu.step();
    CHECK(!cpu.sleeping());                              // não dorme: retorna direto
    cpu.step();
    CHECK(cpu.reg(7) == 0x42);
    CHECK(bus.read16(0x03007FF8) == 0);
}

static void testHalt() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    auto& cpu = g.cpu();
    bus.write16(0x02000100, 0xDF02);                     // swi 2 (Halt)
    bus.write16(0x02000102, 0x2742);                     // mov r7, #0x42
    cpu.jumpTo(0x02000100, true);
    bus.write16(0x04000200, gba::irq::Timer0);           // IE; IME desligado
    cpu.step();
    CHECK(cpu.sleeping());
    for (int i = 0; i < 10; ++i) cpu.step();
    CHECK(cpu.reg(7) == 0);                              // ainda dormindo
    bus.requestInterrupt(gba::irq::Timer0);              // acorda mesmo com IME = 0
    cpu.step();
    cpu.step();
    CHECK(cpu.reg(7) == 0x42);

    bus.write8(0x04000301, 0x00);                        // HALTCNT: mesma coisa via registrador
    CHECK(cpu.sleeping());
}

// Regressão: a RegisterRamReset com r0 = 0xFF (a primeira coisa que o Rogue faz) não pode
// escrever em HALTCNT, senão a CPU dorme para sempre logo na 12ª instrução.
static void testRamResetDoesNotHalt() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    auto& cpu = g.cpu();
    bus.write16(0x03007F00, 0x20FF);                     // mov r0, #0xFF
    bus.write16(0x03007F02, 0xDF01);                     // swi 1
    bus.write16(0x03007F04, 0x2742);                     // mov r7, #0x42
    cpu.jumpTo(0x03007F00, true);                        // na área que a SWI preserva
    for (int i = 0; i < 4; ++i) cpu.step();
    CHECK(!cpu.sleeping());
    CHECK(cpu.reg(7) == 0x42);
}

static void testProbe() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    g.setProbing(true);
    auto& bus = g.bus();
    auto& cpu = g.cpu();
    const uint16_t program[] = {
        0x6808,  // ldr  r0, [r1]       ; BIOS (r1 = 0)
        0x8A12,  // ldrh r2, [r2, #16]  ; BG0HOFS, só-escrita (r2 = 0x04000000)
        0x881B,  // ldrh r3, [r3]       ; contador do timer 0 (r3 = 0x04000100)
        0x6808,  // ldr  r0, [r1]       ; repetido: não deve aparecer de novo
        0xE7FE,  // b .
    };
    for (size_t i = 0; i < std::size(program); ++i) bus.write16(0x02000000 + static_cast<uint32_t>(i) * 2, program[i]);
    cpu.jumpTo(0x02000000, true);
    cpu.setReg(1, 0);
    cpu.setReg(2, 0x04000000);
    cpu.setReg(3, 0x04000100);
    g.runFrame();

    const auto& log = g.probeLog();
    CHECK(log.size() == 4);                              // a 2ª leitura da BIOS vem de outra instrução
    if (log.size() >= 3) {
        CHECK(log[0].probe.kind == gba::ProbeKind::BiosRead && log[0].pc == 0x02000000);
        CHECK(log[1].probe.kind == gba::ProbeKind::WriteOnlyIoRead && log[1].pc == 0x02000002);
        CHECK(log[1].probe.addr == 0x04000010);
        CHECK(log[2].probe.kind == gba::ProbeKind::TimerRead && log[2].pc == 0x02000004);
    }
    // Rodar o laço B . por um quadro inteiro não gera nada: buscas de instrução não contam.
    const size_t before = log.size();
    g.runFrame();
    CHECK(g.probeLog().size() == before);
}

// ------------------------------------------------------------------ precisão (o que o Rogue verifica)

static void testBiosProtection() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    auto& cpu = g.cpu();
    const uint16_t program[] = {
        0x6808,  // ldr r0, [r1]   ; lê a BIOS
        0xDF06,  // swi 6          ; Div (HLE)
        0x680A,  // ldr r2, [r1]   ; lê a BIOS de novo
        0xE7FE,  // b .
    };
    for (size_t i = 0; i < std::size(program); ++i) bus.write16(0x02000000 + static_cast<uint32_t>(i) * 2, program[i]);
    cpu.jumpTo(0x02000000, true);
    cpu.setReg(1, 0x17EC);                               // um dos endereços que o Rogue lê
    cpu.setReg(3, 1);
    cpu.step();
    CHECK(cpu.reg(0) == 0xE129F000u);                    // depois da inicialização
    cpu.setReg(0, 10);
    cpu.setReg(1, 2);
    cpu.step();                                          // SWI
    cpu.setReg(1, 0x17EC);
    cpu.step();
    CHECK(cpu.reg(2) == 0xE3A02004u);                    // depois de uma SWI
    CHECK(bus.read8(0x000017EE) == 0xA0);                // byte 2 de E3A02004
}

static void testBiosProtectionAfterIrq() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    auto& cpu = g.cpu();
    installMarkingHandler(bus);
    writeWords(bus, 0x02000100, {0xE5913000, 0xEAFFFFFE}); // ldr r3, [r1] ; b .
    cpu.jumpTo(0x02000100);
    cpu.setReg(1, 0x1000);
    bus.write16(0x04000200, gba::irq::VBlank);
    bus.write16(0x04000208, 1);
    bus.requestInterrupt(gba::irq::VBlank);
    for (int i = 0; i < 60; ++i) cpu.step();             // IRQ inteira (r3 é restaurado) + o ldr
    CHECK(cpu.reg(3) == 0xE55EC002u);                    // depois de uma IRQ (GBATEK)
    CHECK(cpu.mode() == gba::CpuMode::System);
}

static void testOpenBusAndWriteOnlyIo() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    auto& cpu = g.cpu();
    writeWords(bus, 0x02000000, {
        0xE5910000,   // ldr r0, [r1]      ; endereço não mapeado (r1 = 0x01000000)
        0xE5923010,   // ldr r3, [r2, #16] ; BG0HOFS: palavra inteira só-escrita
        0xE1D24CB4,   // ldrh r4, [r2, #0xC4] ; contagem do DMA1: só-escrita, ao lado de CNT_H
        0xEAFFFFFE,   // b .
        0x11223344}); // não executado
    bus.write16(0x04000010, 0x1234);                     // o jogo escreveu na rolagem
    bus.write16(0x040000C4, 4);                          // e na contagem do DMA
    cpu.jumpTo(0x02000000);
    cpu.setReg(1, 0x01000000);
    cpu.setReg(2, 0x04000000);
    for (int i = 0; i < 3; ++i) cpu.step();
    CHECK(cpu.reg(0) == 0xE1D24CB4u);                    // open bus: a instrução em PC+8
    CHECK(cpu.reg(3) == 0xEAFFFFFEu);                    // idem (PC+8 da 2ª instrução)
    CHECK(cpu.reg(4) == 0);                              // parte só-escrita de uma palavra legível
    CHECK(bus.read16(0x04000006) == 0);                  // VCOUNT continua legível
}

// O padrão do laço principal do Pokémon: VBlankIntrWait e depois uma leitura da BIOS.
// No hardware a IRQ é atendida dentro da SWI, então o valor visto é o do fim da SWI.
static void testBiosValueAfterVBlankIntrWait() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    auto& cpu = g.cpu();
    installMarkingHandler(bus);
    const uint16_t program[] = {
        0xDF05,  // swi 5          ; VBlankIntrWait
        0x6808,  // ldr r0, [r1]   ; lê a BIOS
        0xE7FE,  // b .
    };
    for (size_t i = 0; i < std::size(program); ++i) bus.write16(0x02000100 + static_cast<uint32_t>(i) * 2, program[i]);
    cpu.jumpTo(0x02000100, true);
    cpu.setReg(1, 0x14D4);
    bus.write16(0x04000004, 0x0008);
    bus.write16(0x04000200, gba::irq::VBlank);
    g.runFrame();
    CHECK(cpu.reg(0) == 0xE3A02004u);
}

// O teste de "emulador impreciso" do Rogue, com as instruções exatas da ROM (080C09AC):
// liga o som, escreve FFFF em SOUNDCNT_L e confere se a leitura devolve FF77.
static void testRogueAccuracyCheck() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    auto& cpu = g.cpu();
    const uint16_t check[] = {
        0x4B09, 0x4A0A, 0x8811, 0x8818, 0xB510, 0x0004, 0x2080, 0x8010, 0x3881, 0x8018,
        0x8818, 0x801C, 0x4B05, 0x18C0, 0x1E43, 0x4198, 0x8011, 0xBC10, 0xBC02, 0x4708,
    };
    for (size_t i = 0; i < std::size(check); ++i) bus.write16(0x02000100 + static_cast<uint32_t>(i) * 2, check[i]);
    writeWords(bus, 0x02000128, {0x04000080, 0x04000084, 0xFFFF0089}); // literais da função
    bus.write16(0x02000000, 0xF000);   // bl 0x02000100
    bus.write16(0x02000002, 0xF87E);
    bus.write16(0x02000004, 0x1C07);   // mov r7, r0   (resultado: 1 = impreciso)
    bus.write16(0x02000006, 0xE7FE);   // b .
    cpu.jumpTo(0x02000000, true);
    cpu.setReg(7, 0xDEAD);
    for (int i = 0; i < 40; ++i) cpu.step();
    CHECK(cpu.reg(7) == 0);                              // emulador preciso
    CHECK(bus.read16(0x04000084) == 0);                  // o teste restaurou o som desligado
}

static void testSoundRegisterRules() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    auto& bus = g.bus();
    bus.write16(0x04000080, 0xFFFF);                     // som desligado: ignorado
    CHECK(bus.read16(0x04000080) == 0);
    bus.write16(0x04000084, 0x008F);                     // liga (bits 0-3 são só leitura)
    CHECK(bus.read16(0x04000084) == 0x0080);
    bus.write16(0x04000080, 0xFFFF);
    CHECK(bus.read16(0x04000080) == 0xFF77);             // bits 3 e 7 não existem
    bus.write16(0x04000062, 0xFFFF);
    CHECK(bus.read16(0x04000062) == 0xFFC0);             // comprimento é só de escrita
    bus.write16(0x04000084, 0);                          // desliga: canais zerados
    bus.write16(0x04000084, 0x0080);
    CHECK(bus.read16(0x04000080) == 0 && bus.read16(0x04000062) == 0);
    bus.write16(0x04000004, 0xFFF8);
    CHECK((bus.read16(0x04000004) & 0x00C0) == 0);       // DISPSTAT bits 6-7
    bus.write16(0x04000102, 0xFFFF);
    CHECK(bus.read16(0x04000102) == 0x00C7);             // TM0CNT_H
}

// ------------------------------------------------------------------ EEPROM

static std::vector<uint8_t> eepromRom() {
    auto rom = loopRom();
    const char sig[] = "EEPROM_V124";
    for (size_t i = 0; i < sizeof sig - 1; ++i) rom[0x100 + i] = static_cast<uint8_t>(sig[i]);
    return rom;
}

// Como os jogos fazem: os bits vão para a memória (um por halfword) e o DMA 3 os envia ao EEPROM.
static void eepromSend(gba::Gba& g, const std::vector<int>& bits) {
    for (size_t i = 0; i < bits.size(); ++i) g.bus().write16(0x02010000 + static_cast<uint32_t>(i) * 2, static_cast<uint16_t>(bits[i]));
    g.bus().write32(0x040000D4, 0x02010000);
    g.bus().write32(0x040000D8, 0x0D000000);
    g.bus().write32(0x040000DC, static_cast<uint32_t>(bits.size()) | (0x8000u << 16));
}

static std::vector<int> eepromReceive(gba::Gba& g) {
    g.bus().write32(0x040000D4, 0x0D000000);
    g.bus().write32(0x040000D8, 0x02011000);
    g.bus().write32(0x040000DC, 68u | (0x8000u << 16));
    std::vector<int> bits;
    for (uint32_t i = 0; i < 68; ++i) bits.push_back(g.bus().read16(0x02011000 + i * 2) & 1);
    return bits;
}

static void pushBits(std::vector<int>& bits, uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) bits.push_back(static_cast<int>((value >> i) & 1));
}

static void testEeprom(int addrBits) {
    gba::Gba g;
    g.loadRom(eepromRom());
    g.reset();
    CHECK(g.bus().backup().type() == gba::Backup::Type::Eeprom);

    const uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    std::vector<int> write = {1, 0};
    pushBits(write, 3, addrBits);                          // bloco 3
    for (uint8_t b : data) pushBits(write, b, 8);
    write.push_back(0);
    eepromSend(g, write);
    CHECK(g.bus().backup().data().size() == (addrBits == 6 ? 512u : 8192u));
    CHECK(g.bus().backup().dirty());
    CHECK(g.bus().backup().data()[3 * 8] == 0x11);

    std::vector<int> request = {1, 1};
    pushBits(request, 3, addrBits);
    request.push_back(0);
    eepromSend(g, request);
    const auto reply = eepromReceive(g);
    bool same = reply[0] == 0 && reply[1] == 0 && reply[2] == 0 && reply[3] == 0; // 4 bits descartáveis
    for (int i = 0; i < 8; ++i) {
        uint32_t byte = 0;
        for (int b = 0; b < 8; ++b) byte = (byte << 1) | static_cast<uint32_t>(reply[4 + static_cast<size_t>(i * 8 + b)]);
        same = same && byte == data[i];
    }
    CHECK(same);
    CHECK((g.bus().read16(0x0D000000) & 1) == 1);         // sem leitura pendente: "pronto"
}

// ------------------------------------------------------------------ relógio do cartucho (RTC)

// Fala com o RTC como o Pokémon faz: pino por pino, pelo barramento (SCK = bit 0, SIO = bit 1, CS = bit 2).
struct RtcTalker {
    gba::Bus& bus;
    void pins(uint16_t v) { bus.write16(0x080000C4, v); }
    void begin() {
        bus.write16(0x080000C8, 1);   // porta legível
        bus.write16(0x080000C6, 7);   // os 3 pinos saindo do GBA
        pins(1);                      // SCK alto, CS baixo
        pins(5);                      // CS sobe
    }
    void sendByte(uint8_t b) {
        bus.write16(0x080000C6, 7);
        for (int i = 0; i < 8; ++i) {
            const uint16_t bit = static_cast<uint16_t>((b >> i) & 1);
            pins(static_cast<uint16_t>(4 | (bit << 1)));     // SCK baixo, dado em SIO
            pins(static_cast<uint16_t>(5 | (bit << 1)));     // SCK sobe: o relógio lê o bit
        }
    }
    uint8_t readByte() {
        bus.write16(0x080000C6, 5);   // SIO vira entrada para o GBA
        uint8_t b = 0;
        for (int i = 0; i < 8; ++i) {
            pins(4);
            pins(5);
            b = static_cast<uint8_t>(b | (((bus.read16(0x080000C4) >> 1) & 1) << i));
        }
        return b;
    }
    void end() { bus.write16(0x080000C6, 7); pins(1); }
};

static std::time_t fixedTime() {
    std::tm t{};
    t.tm_year = 2026 - 1900; t.tm_mon = 9; t.tm_mday = 19;   // 19/10/2026, uma segunda-feira
    t.tm_hour = 14; t.tm_min = 35; t.tm_sec = 7; t.tm_isdst = -1;
    return std::mktime(&t);
}

static void testRtc() {
    auto rom = loopRom();
    rom[0xC4] = 0xAB;                                       // código normal da ROM nesses endereços
    gba::Gba g;
    g.loadRom(rom);
    g.reset();
    auto& bus = g.bus();
    bus.rtc().setClock(fixedTime);
    CHECK(bus.read8(0x080000C4) == 0xAB);                   // porta não legível: a CPU vê a ROM

    RtcTalker rtc{bus};
    rtc.begin();
    CHECK(bus.read8(0x080000C4) != 0xAB);                   // agora vê os pinos

    rtc.sendByte(0x06 | (4 << 4) | 0x80);                   // ler o controle
    CHECK(rtc.readByte() == 0x40);                          // modo 24 h, sem "falha de energia"
    rtc.end();

    rtc.begin();
    rtc.sendByte(0x06 | (2 << 4) | 0x80);                   // ler data e hora (7 bytes, BCD)
    uint8_t dt[7];
    for (auto& b : dt) b = rtc.readByte();
    rtc.end();
    CHECK(dt[0] == 0x26 && dt[1] == 0x10 && dt[2] == 0x19); // 26/10/19
    CHECK(dt[3] == 0x01);                                   // segunda-feira
    CHECK(dt[4] == 0x14 && dt[5] == 0x35 && dt[6] == 0x07); // 14:35:07

    rtc.begin();
    rtc.sendByte(0x06 | (0 << 4));                          // reset: volta para o modo 12 h
    rtc.end();
    rtc.begin();
    rtc.sendByte(0x06 | (6 << 4) | 0x80);                   // ler só a hora (3 bytes)
    const uint8_t h = rtc.readByte(), m = rtc.readByte(), sec = rtc.readByte();
    rtc.end();
    CHECK(h == 0x02 && m == 0x35 && sec == 0x07);           // 14 h = 2 h no modo 12 h

    rtc.begin();
    rtc.sendByte(0x06 | (4 << 4));                          // gravar o controle...
    rtc.sendByte(0x40);                                     // ...modo 24 h de novo
    rtc.end();
    rtc.begin();
    rtc.sendByte(0x06 | (4 << 4) | 0x80);
    CHECK(rtc.readByte() == 0x40);
    rtc.end();

    bus.write16(0x080000C8, 0);                             // porta volta a não ser legível
    CHECK(bus.read8(0x080000C4) == 0xAB);
}

// Save states da versão 1 (antes do RTC) continuam carregando.
static void testStateVersion1StillLoads() {
    gba::Gba g;
    g.loadRom(loopRom());
    g.reset();
    g.runFrame();
    auto state = g.saveState();
    auto rtcOnly = gba::StateArchive::writer();
    g.bus().rtc().serialize(rtcOnly);
    auto timingOnly = gba::StateArchive::writer();
    g.bus().serializeTiming(timingOnly);
    // Campos das versões 2 (relógio), 3 (registrador do DMA, 4 bytes) e 4 (tempo do barramento) ficam no
    // fim: tirá-los dá a versão 1.
    // ...e 5 (atraso de partida dos timers: 4 ints).
    state.resize(state.size() - rtcOnly.data().size() - 4 - timingOnly.data().size() - 4 * sizeof(int));
    state[8] = 1; state[9] = state[10] = state[11] = 0;    // ...e marca como versão 1
    bool ok = true;
    try { g.loadState(state); } catch (const std::exception&) { ok = false; }
    CHECK(ok);
}

int main() {
    testIoRules();
    testIrqThroughBiosStub();
    testIrqMaskedByCpsr();
    testVideoTiming();
    testWaitForVBlankPattern();
    testTimerBasics();
    testTimerCascade();
    testRogueSerialWait();
    testVBlankIntrWait();
    testIntrWaitAlreadySignaled();
    testHalt();
    testRamResetDoesNotHalt();
    testProbe();
    testBiosProtection();
    testBiosProtectionAfterIrq();
    testOpenBusAndWriteOnlyIo();
    testBiosValueAfterVBlankIntrWait();
    testRogueAccuracyCheck();
    testSoundRegisterRules();
    testEeprom(14);
    testEeprom(6);
    testRtc();
    testStateVersion1StillLoads();
    if (failures == 0) std::printf("Sistema: todos os testes passaram.\n");
    return failures == 0 ? 0 : 1;
}
