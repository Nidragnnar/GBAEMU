// Testes da CPU no estado ARM. Cada programa é gravado na EWRAM (0x02000000) e executado passo a passo.
#include <cstdio>
#include <initializer_list>
#include <stdexcept>

#include "core/bus.hpp"
#include "core/cpu.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FALHOU %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

using gba::flag::C; using gba::flag::N; using gba::flag::V; using gba::flag::Z;

constexpr uint32_t kBase = 0x02000000;

struct Machine {
    gba::Bus bus;
    gba::Cpu cpu{bus};

    void load(std::initializer_list<uint32_t> program) {
        uint32_t addr = kBase;
        for (uint32_t op : program) { bus.write32(addr, op); addr += 4; }
        cpu.reset(true);
        cpu.jumpTo(kBase);
    }
    void loadThumb(std::initializer_list<uint16_t> program) {
        uint32_t addr = kBase;
        for (uint16_t op : program) { bus.write16(addr, op); addr += 2; }
        cpu.reset(true);
        cpu.jumpTo(kBase, true);
    }
    void run(int steps) { for (int i = 0; i < steps; ++i) cpu.step(); }
    bool has(uint32_t f) const { return (cpu.cpsr() & f) != 0; }
};

static void testArithmetic() {
    Machine m;
    m.load({0xE3A00005,   // MOV r0, #5
            0xE2801003}); // ADD r1, r0, #3
    m.run(2);
    CHECK(m.cpu.reg(1) == 8);

    m.load({0xE3A00001,   // MOV  r0, #1
            0xE2500001}); // SUBS r0, r0, #1
    m.run(2);
    CHECK(m.cpu.reg(0) == 0);
    CHECK(m.has(Z) && m.has(C) && !m.has(N) && !m.has(V));

    m.load({0xE3E00102,   // MVN  r0, #0x80000000  -> 0x7FFFFFFF
            0xE2900001}); // ADDS r0, r0, #1
    m.run(2);
    CHECK(m.cpu.reg(0) == 0x80000000u);
    CHECK(m.has(N) && m.has(V) && !m.has(C) && !m.has(Z));

    m.load({0xE3A00000,   // MOV  r0, #0
            0xE2500001}); // SUBS r0, r0, #1  -> empréstimo: C = 0
    m.run(2);
    CHECK(m.cpu.reg(0) == 0xFFFFFFFFu);
    CHECK(!m.has(C) && m.has(N));
}

static void testConditions() {
    Machine m;
    m.load({0xE3A00001,   // MOV   r0, #1
            0xE3500002,   // CMP   r0, #2
            0x03A01009,   // MOVEQ r1, #9 (não executa)
            0xB3A02007}); // MOVLT r2, #7 (executa)
    m.run(4);
    CHECK(m.cpu.reg(1) == 0);
    CHECK(m.cpu.reg(2) == 7);
}

static void testShifter() {
    Machine m;
    m.load({0xE1A02201}); // MOV r2, r1, LSL #4
    m.cpu.setReg(1, 0x0F);
    m.run(1);
    CHECK(m.cpu.reg(2) == 0xF0);

    m.load({0xE1B02021}); // MOVS r2, r1, LSR #32 (codificado como #0)
    m.cpu.setReg(1, 0x80000000);
    m.run(1);
    CHECK(m.cpu.reg(2) == 0 && m.has(C) && m.has(Z));

    m.load({0xE1B02061}); // MOVS r2, r1, RRX (ROR #0)
    m.cpu.setReg(1, 0x00000003);
    m.run(1);
    CHECK(m.cpu.reg(2) == 0x00000001 && m.has(C));

    m.load({0xE1A0000F}); // MOV r0, pc  -> endereço da instrução + 8
    m.run(1);
    CHECK(m.cpu.reg(0) == kBase + 8);
}

static void testBranches() {
    Machine m;
    m.load({0xEA000000,   // B   +0 -> pula para kBase+8
            0xE3A00001,   // MOV r0, #1 (pulado)
            0xE3A01002}); // MOV r1, #2
    m.run(2);
    CHECK(m.cpu.reg(0) == 0 && m.cpu.reg(1) == 2);

    m.load({0xEB000000}); // BL +0
    m.run(1);
    CHECK(m.cpu.reg(14) == kBase + 4);
    CHECK(m.cpu.pc() == kBase + 8);

    m.load({0xE12FFF10}); // BX r0 com bit 0 ligado -> Thumb
    m.cpu.setReg(0, kBase + 0x101);
    m.run(1);
    CHECK(m.cpu.thumb());
    CHECK(m.cpu.pc() == kBase + 0x100);
}

static void testLoadStore() {
    Machine m;
    m.load({0xE3A00402,   // MOV r0, #0x02000000
            0xE3A010AB,   // MOV r1, #0xAB
            0xE5801100,   // STR r1, [r0, #0x100]
            0xE5902100,   // LDR r2, [r0, #0x100]
            0xE5903201}); // LDR r3, [r0, #0x201] (desalinhado)
    m.bus.write32(kBase + 0x200, 0x11223344);
    m.run(5);
    CHECK(m.cpu.reg(2) == 0xAB);
    CHECK(m.cpu.reg(3) == 0x44112233u); // palavra girada 8 bits

    m.load({0xE4801004}); // STR r1, [r0], #4 (pós-indexado: sempre atualiza a base)
    m.cpu.setReg(0, kBase + 0x300);
    m.cpu.setReg(1, 0xCAFE);
    m.run(1);
    CHECK(m.bus.read32(kBase + 0x300) == 0xCAFE);
    CHECK(m.cpu.reg(0) == kBase + 0x304);

    m.load({0xE1C010B2,   // STRH  r1, [r0, #2]
            0xE1D020D2,   // LDRSB r2, [r0, #2]
            0xE1D030B2}); // LDRH  r3, [r0, #2]
    m.cpu.setReg(0, kBase + 0x400);
    m.cpu.setReg(1, 0x12340080);
    m.run(3);
    CHECK(m.bus.read16(kBase + 0x402) == 0x0080);
    CHECK(m.cpu.reg(2) == 0xFFFFFF80u);
    CHECK(m.cpu.reg(3) == 0x0080);
}

static void testBlockTransfer() {
    Machine m;
    m.load({0xE8A00006,   // STMIA r0!, {r1, r2}
            0xE9300018}); // LDMDB r0!, {r3, r4}
    m.cpu.setReg(0, kBase + 0x500);
    m.cpu.setReg(1, 0x11111111);
    m.cpu.setReg(2, 0x22222222);
    m.run(2);
    CHECK(m.bus.read32(kBase + 0x500) == 0x11111111u);
    CHECK(m.bus.read32(kBase + 0x504) == 0x22222222u);
    CHECK(m.cpu.reg(3) == 0x11111111u && m.cpu.reg(4) == 0x22222222u);
    CHECK(m.cpu.reg(0) == kBase + 0x500);

    m.load({0xE92D4003}); // STMFD sp!, {r0, r1, lr}  (PUSH)
    const uint32_t sp = m.cpu.reg(13);
    m.cpu.setReg(14, 0xDEAD0000);
    m.run(1);
    CHECK(m.cpu.reg(13) == sp - 12);
    CHECK(m.bus.read32(sp - 4) == 0xDEAD0000u);
}

static void testMultiply() {
    Machine m;
    m.load({0xE0020190}); // MUL r2, r0, r1
    m.cpu.setReg(0, 6);
    m.cpu.setReg(1, 7);
    m.run(1);
    CHECK(m.cpu.reg(2) == 42);

    m.load({0xE0832190}); // UMULL r2, r3, r0, r1
    m.cpu.setReg(0, 0xFFFFFFFF);
    m.cpu.setReg(1, 2);
    m.run(1);
    CHECK(m.cpu.reg(2) == 0xFFFFFFFEu && m.cpu.reg(3) == 1);

    m.load({0xE0C32190}); // SMULL r2, r3, r0, r1  (-1 * 2 = -2)
    m.cpu.setReg(0, 0xFFFFFFFF);
    m.cpu.setReg(1, 2);
    m.run(1);
    CHECK(m.cpu.reg(2) == 0xFFFFFFFEu && m.cpu.reg(3) == 0xFFFFFFFFu);
}

static void testModesAndPsr() {
    Machine m;
    m.load({0xE321F012,   // MSR CPSR_c, #0x12 (IRQ)
            0xE3A0D001,   // MOV sp, #1
            0xE321F01F,   // MSR CPSR_c, #0x1F (System)
            0xE10F0000}); // MRS r0, CPSR
    CHECK(m.cpu.mode() == gba::CpuMode::System);
    CHECK(m.cpu.reg(13) == 0x03007F00u);
    m.run(1);
    CHECK(m.cpu.mode() == gba::CpuMode::Irq);
    CHECK(m.cpu.reg(13) == 0x03007FA0u); // pilha própria do modo IRQ
    m.run(1);
    m.run(1);
    CHECK(m.cpu.reg(13) == 0x03007F00u); // pilha do System preservada
    m.run(1);
    CHECK((m.cpu.reg(0) & 0x1F) == 0x1F);

    m.load({0xEF000005}); // SWI 5
    m.run(1);
    CHECK(m.cpu.mode() == gba::CpuMode::Supervisor);
    CHECK(m.cpu.reg(14) == kBase + 4);
    CHECK(m.cpu.pc() == 0x08);
    CHECK((m.cpu.spsr() & 0x1F) == 0x1F);
    CHECK(m.has(gba::flag::I));
}

static void testExceptionReturn() {
    Machine m;
    // Simula um tratador: entra em IRQ, ajusta SPSR para voltar ao System em Thumb.
    m.load({0xE321F012,   // MSR  CPSR_c, #0x12 (IRQ)
            0xE369F03F,   // MSR  SPSR_fc, #0x3F (System + T)
            0xE1B0F00E}); // MOVS pc, lr
    m.run(2);
    m.cpu.setReg(14, kBase + 0x600);
    m.run(1);
    CHECK(m.cpu.mode() == gba::CpuMode::System);
    CHECK(m.cpu.thumb());
    CHECK(m.cpu.pc() == kBase + 0x600);
}

static void testHleBios() {
    Machine m;
    // r0 = 0xFE: apaga tudo menos a EWRAM, onde está este programa de teste.
    m.load({0xE3A000FE,   // MOV r0, #0xFE
            0xEF010000,   // SWI 0x01       (RegisterRamReset)
            0xE3A01001,   // MOV r1, #1     (continua normalmente depois)
            0xEF7F0000}); // SWI 0x7F       (não implementada)
    m.cpu.setHleBios(true);
    m.bus.write32(0x03000010, 0x12345678);   // IWRAM comum: deve ser apagada
    m.bus.write32(0x03007F00, 0xAAAAAAAA);   // área das pilhas: deve ser preservada
    m.bus.write16(0x05000000, 0x7FFF);
    m.run(3);
    CHECK(m.bus.read32(0x03000010) == 0);
    CHECK(m.bus.read32(0x03007F00) == 0xAAAAAAAAu);
    CHECK(m.bus.read16(0x05000000) == 0);
    CHECK(m.bus.read16(0x04000000) == 0x0080);
    CHECK(m.cpu.mode() == gba::CpuMode::System); // não entrou em Supervisor
    CHECK(m.cpu.reg(1) == 1);

    bool threw = false;
    try { m.run(1); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

// ------------------------------------------------------------------ Thumb

static void testThumbArithmetic() {
    Machine m;
    m.loadThumb({0x2005,   // MOV r0, #5
                 0x3003,   // ADD r0, #3
                 0x3808}); // SUB r0, #8
    m.run(2);
    CHECK(m.cpu.reg(0) == 8);
    m.run(1);
    CHECK(m.cpu.reg(0) == 0 && m.has(Z) && m.has(C));

    m.loadThumb({0x1842,   // ADD r2, r0, r1
                 0x1E43,   // SUB r3, r0, #1
                 0x0104}); // LSL r4, r0, #4
    m.cpu.setReg(0, 5);
    m.cpu.setReg(1, 7);
    m.run(3);
    CHECK(m.cpu.reg(2) == 12);
    CHECK(m.cpu.reg(3) == 4);
    CHECK(m.cpu.reg(4) == 0x50);
}

static void testThumbAlu() {
    Machine m;
    m.loadThumb({0x4348}); // MUL r0, r1
    m.cpu.setReg(0, 6);
    m.cpu.setReg(1, 7);
    m.run(1);
    CHECK(m.cpu.reg(0) == 42);

    m.loadThumb({0x4248}); // NEG r0, r1
    m.cpu.setReg(1, 5);
    m.run(1);
    CHECK(m.cpu.reg(0) == static_cast<uint32_t>(-5) && m.has(N));

    m.loadThumb({0x40C8}); // LSR r0, r1 (por registrador)
    m.cpu.setReg(0, 0x80);
    m.cpu.setReg(1, 4);
    m.run(1);
    CHECK(m.cpu.reg(0) == 8);

    m.loadThumb({0x4388}); // BIC r0, r1
    m.cpu.setReg(0, 0xFF);
    m.cpu.setReg(1, 0x0F);
    m.run(1);
    CHECK(m.cpu.reg(0) == 0xF0);
}

static void testThumbHiRegisters() {
    Machine m;
    m.loadThumb({0x4680,   // MOV r8, r0
                 0x4440}); // ADD r0, r8
    m.cpu.setReg(0, 21);
    m.run(2);
    CHECK(m.cpu.reg(8) == 21);
    CHECK(m.cpu.reg(0) == 42);

    m.loadThumb({0x4700}); // BX r0 (endereço par -> ARM)
    m.cpu.setReg(0, kBase + 0x200);
    m.run(1);
    CHECK(!m.cpu.thumb());
    CHECK(m.cpu.pc() == kBase + 0x200);
}

static void testThumbLoads() {
    Machine m;
    // As duas LDR leem a mesma palavra: o PC é alinhado a 4 antes de somar o deslocamento.
    m.loadThumb({0x4801,   // LDR r0, [pc, #4]  (em kBase+0)
                 0x4901,   // LDR r1, [pc, #4]  (em kBase+2)
                 0x46C0, 0x46C0,
                 0x5678, 0x1234});
    m.run(2);
    CHECK(m.cpu.reg(0) == 0x12345678u);
    CHECK(m.cpu.reg(1) == 0x12345678u);

    m.loadThumb({0x6041,   // STR   r1, [r0, #4]
                 0x6842,   // LDR   r2, [r0, #4]
                 0x7041,   // STRB  r1, [r0, #1]
                 0x7843,   // LDRB  r3, [r0, #1]
                 0x8041,   // STRH  r1, [r0, #2]
                 0x8844,   // LDRH  r4, [r0, #2]
                 0x5785}); // LDSB  r5, [r0, r6]
    m.cpu.setReg(0, kBase + 0x300);
    m.cpu.setReg(1, 0x0000ABCD);
    m.cpu.setReg(6, 1);
    m.run(7);
    CHECK(m.cpu.reg(2) == 0xABCD);
    CHECK(m.cpu.reg(3) == 0xCD);
    CHECK(m.cpu.reg(4) == 0xABCD);
    CHECK(m.cpu.reg(5) == 0xFFFFFFCDu); // 0xCD com sinal
}

static void testThumbStack() {
    Machine m;
    m.loadThumb({0xB503,   // PUSH {r0, r1, lr}
                 0xBD0C}); // POP  {r2, r3, pc}
    const uint32_t sp = m.cpu.reg(13);
    m.cpu.setReg(0, 0x11);
    m.cpu.setReg(1, 0x22);
    m.cpu.setReg(14, kBase + 0x101);
    m.run(1);
    CHECK(m.cpu.reg(13) == sp - 12);
    CHECK(m.bus.read32(sp - 4) == kBase + 0x101);
    m.run(1);
    CHECK(m.cpu.reg(2) == 0x11 && m.cpu.reg(3) == 0x22);
    CHECK(m.cpu.reg(13) == sp);
    CHECK(m.cpu.thumb() && m.cpu.pc() == kBase + 0x100);

    m.loadThumb({0xB082,   // SUB sp, #8
                 0xA802}); // ADD r0, sp, #8
    const uint32_t sp2 = m.cpu.reg(13);
    m.run(2);
    CHECK(m.cpu.reg(13) == sp2 - 8);
    CHECK(m.cpu.reg(0) == sp2);

    m.loadThumb({0xC006,   // STMIA r0!, {r1, r2}
                 0x3808,   // SUB   r0, #8
                 0xC818}); // LDMIA r0!, {r3, r4}
    m.cpu.setReg(0, kBase + 0x400);
    m.cpu.setReg(1, 0xAAAA);
    m.cpu.setReg(2, 0xBBBB);
    m.run(3);
    CHECK(m.cpu.reg(3) == 0xAAAA && m.cpu.reg(4) == 0xBBBB);
    CHECK(m.cpu.reg(0) == kBase + 0x408);
}

static void testThumbBranches() {
    Machine m;
    m.loadThumb({0x2800,   // CMP r0, #0
                 0xD000,   // BEQ +0  -> pula a próxima
                 0x2101,   // MOV r1, #1 (pulada)
                 0x2202}); // MOV r2, #2
    m.run(3);
    CHECK(m.cpu.reg(1) == 0 && m.cpu.reg(2) == 2);

    m.loadThumb({0xE7FE}); // B . (laço infinito, comum enquanto o jogo espera algo)
    m.run(3);
    CHECK(m.cpu.pc() == kBase);

    m.loadThumb({0xF000, 0xF87E}); // BL kBase+0x100
    m.run(2);
    CHECK(m.cpu.pc() == kBase + 0x100);
    CHECK(m.cpu.reg(14) == ((kBase + 4) | 1));

    m.loadThumb({0xDF05}); // SWI 5 sem HLE -> exceção em ARM
    m.run(1);
    CHECK(m.cpu.mode() == gba::CpuMode::Supervisor);
    CHECK(!m.cpu.thumb());
    CHECK(m.cpu.reg(14) == kBase + 2);
    CHECK(m.cpu.pc() == 0x08);
}

static void testArmThumbInterworking() {
    Machine m;
    // ARM chama uma rotina Thumb com BX e ela volta com BX lr.
    m.load({0xE28F0005,   // ADD r0, pc, #5  -> kBase+8+5 = kBase+0xD (Thumb em kBase+0xC)
            0xE1A0E00F,   // MOV lr, pc      (o teste troca lr depois)
            0xE12FFF10,   // BX  r0
            0x00000000}); // espaço para o código Thumb
    m.bus.write16(kBase + 0xC, 0x2063);  // MOV r0, #0x63
    m.bus.write16(kBase + 0xE, 0x4770);  // BX lr
    m.run(3);
    CHECK(m.cpu.thumb());
    m.cpu.setReg(14, kBase + 0x40);      // retorno para ARM (endereço par)
    m.run(2);
    CHECK(m.cpu.reg(0) == 0x63);
    CHECK(!m.cpu.thumb());
    CHECK(m.cpu.pc() == kBase + 0x40);
}

// ------------------------------------------------------------------ BIOS em HLE

// Executa uma SWI Thumb com os registradores dados, como o jogo faria.
static void callSwi(Machine& m, uint8_t number, uint32_t r0, uint32_t r1, uint32_t r2) {
    m.loadThumb({static_cast<uint16_t>(0xDF00 | number)});
    m.cpu.setHleBios(true);
    m.cpu.setReg(0, r0);
    m.cpu.setReg(1, r1);
    m.cpu.setReg(2, r2);
    m.run(1);
}

static void writeBytes(gba::Bus& bus, uint32_t addr, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) bus.write8(addr++, b);
}

static void testHleCopyAndFill() {
    Machine m;
    const uint32_t src = 0x03000000, dst = 0x03000100;
    for (uint32_t i = 0; i < 16; ++i) m.bus.write16(src + i * 2, static_cast<uint16_t>(0x100 + i));

    callSwi(m, 0x0B, src, dst, 4);                       // CpuSet: copia 4 halfwords
    CHECK(m.bus.read16(dst) == 0x100 && m.bus.read16(dst + 6) == 0x103);
    CHECK(m.bus.read16(dst + 8) == 0);                   // não passou do limite

    m.bus.write32(0x03000200, 0xCAFEBABE);
    callSwi(m, 0x0B, 0x03000200, dst, (1u << 24) | (1u << 26) | 3); // preenche 3 palavras
    CHECK(m.bus.read32(dst) == 0xCAFEBABEu && m.bus.read32(dst + 8) == 0xCAFEBABEu);
    CHECK(m.bus.read32(dst + 12) == 0);

    m.bus.write32(0x03000200, 0x11111111);
    callSwi(m, 0x0C, 0x03000200, 0x03000300, (1u << 24) | 3); // CpuFastSet: 3 vira 8
    CHECK(m.bus.read32(0x03000300 + 7 * 4) == 0x11111111u);
    CHECK(m.bus.read32(0x03000300 + 8 * 4) == 0);
}

static void testHleMath() {
    Machine m;
    callSwi(m, 0x06, static_cast<uint32_t>(-7), 2, 0);   // Div: -7 / 2
    CHECK(static_cast<int32_t>(m.cpu.reg(0)) == -3);
    CHECK(static_cast<int32_t>(m.cpu.reg(1)) == -1);
    CHECK(m.cpu.reg(3) == 3);

    callSwi(m, 0x07, 2, 9, 0);                           // DivArm: 9 / 2
    CHECK(m.cpu.reg(0) == 4 && m.cpu.reg(1) == 1);

    callSwi(m, 0x08, 1000000, 0, 0);                     // Sqrt
    CHECK(m.cpu.reg(0) == 1000);
    callSwi(m, 0x08, 99, 0, 0);
    CHECK(m.cpu.reg(0) == 9);
    callSwi(m, 0x08, 0xFFFFFFFF, 0, 0);
    CHECK(m.cpu.reg(0) == 65535);

    bool threw = false;
    try { callSwi(m, 0x06, 1, 0, 0); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

static void testHleDecompress() {
    Machine m;
    // LZ77 de "ABCABCABC": 3 literais + referência (distância 3, tamanho 6).
    writeBytes(m.bus, 0x03000000, {0x10, 0x09, 0x00, 0x00, 0x10, 'A', 'B', 'C', 0x30, 0x02});
    callSwi(m, 0x11, 0x03000000, 0x02010000, 0);
    const char* expected = "ABCABCABC";
    bool same = true;
    for (int i = 0; i < 9; ++i) same = same && m.bus.read8(0x02010000 + i) == static_cast<uint8_t>(expected[i]);
    CHECK(same);
    CHECK(m.bus.read8(0x02010009) == 0);

    callSwi(m, 0x12, 0x03000000, 0x06000000, 0);         // mesma coisa direto na VRAM
    CHECK(m.bus.read16(0x06000000) == 0x4241);            // "AB"
    CHECK(m.bus.read16(0x06000008) == 0x0043);            // "C" + byte final zerado

    // RLE de "AAAAAXY": bloco repetido (5× 'A') + bloco literal ("XY").
    writeBytes(m.bus, 0x03000040, {0x30, 0x07, 0x00, 0x00, 0x82, 'A', 0x01, 'X', 'Y'});
    callSwi(m, 0x14, 0x03000040, 0x02020000, 0);
    CHECK(m.bus.read32(0x02020000) == 0x41414141u);
    CHECK(m.bus.read8(0x02020004) == 'A' && m.bus.read8(0x02020005) == 'X' && m.bus.read8(0x02020006) == 'Y');

    // Como na BIOS real: tamanho 0 não faz nada (era o que travava o Rogue ao escolher o Pokémon)...
    m.bus.write32(0x03000080, 0x00000000);
    m.bus.write32(0x02030000, 0x12345678);
    bool threw = false;
    try { callSwi(m, 0x11, 0x03000080, 0x02030000, 0); } catch (const std::exception&) { threw = true; }
    CHECK(!threw);
    CHECK(m.bus.read32(0x02030000) == 0x12345678u);
    // ...origem dentro da BIOS é ignorada...
    callSwi(m, 0x11, 0x00000100, 0x02030000, 0);
    CHECK(m.bus.read32(0x02030000) == 0x12345678u);
    // ...e o tipo no cabeçalho não é conferido: dados LZ77 com o tipo zerado descomprimem igual.
    writeBytes(m.bus, 0x03000000, {0x00, 0x09, 0x00, 0x00});
    callSwi(m, 0x11, 0x03000000, 0x02010100, 0);
    CHECK(m.bus.read8(0x02010108) == 'C');

    // Referência para antes do início lê o que já estava no destino.
    m.bus.write8(0x02040000 - 1, 'Z');
    writeBytes(m.bus, 0x03000060, {0x10, 0x03, 0x00, 0x00, 0x80, 0x00, 0x00}); // 3 bytes: copia de 1 atrás
    callSwi(m, 0x11, 0x03000060, 0x02040000, 0);
    CHECK(m.bus.read8(0x02040000) == 'Z' && m.bus.read8(0x02040002) == 'Z');

    // CpuSet com origem na BIOS também é ignorado.
    m.bus.write16(0x03000300, 0xAAAA);
    callSwi(m, 0x0B, 0x00000000, 0x03000300, 1);
    CHECK(m.bus.read16(0x03000300) == 0xAAAA);
}

static void testHleAffineAndAngles() {
    Machine m;
    // ObjAffineSet: escala 1.0, ângulo 0 -> identidade
    m.bus.write16(0x03000000, 0x0100);
    m.bus.write16(0x03000002, 0x0100);
    m.bus.write16(0x03000004, 0x0000);
    m.bus.write16(0x03000008, 0x0200);                   // segunda entrada: escala 2.0
    m.bus.write16(0x0300000A, 0x0200);
    m.bus.write16(0x0300000C, 0x4000);                   // 90 graus
    m.loadThumb({0xDF0F});
    m.cpu.setHleBios(true);
    m.cpu.setReg(0, 0x03000000);
    m.cpu.setReg(1, 0x03000100);
    m.cpu.setReg(2, 2);
    m.cpu.setReg(3, 2);                                  // PA, PB, PC, PD lado a lado
    m.run(1);
    CHECK(m.bus.read16(0x03000100) == 0x0100 && m.bus.read16(0x03000102) == 0);
    CHECK(m.bus.read16(0x03000104) == 0 && m.bus.read16(0x03000106) == 0x0100);
    CHECK(m.bus.read16(0x03000108) == 0);                // cos 90° = 0
    CHECK(m.bus.read16(0x0300010A) == static_cast<uint16_t>(-0x200));
    CHECK(m.bus.read16(0x0300010C) == 0x0200);
    CHECK(m.bus.read16(0x0300010E) == 0);

    // BgAffineSet: identidade, ponto (64, 64) da imagem no ponto (120, 80) da tela
    m.bus.write32(0x03000200, 64 << 8);
    m.bus.write32(0x03000204, 64 << 8);
    m.bus.write16(0x03000208, 120);
    m.bus.write16(0x0300020A, 80);
    m.bus.write16(0x0300020C, 0x0100);
    m.bus.write16(0x0300020E, 0x0100);
    m.bus.write16(0x03000210, 0);
    callSwi(m, 0x0E, 0x03000200, 0x03000300, 1);
    CHECK(m.bus.read16(0x03000300) == 0x0100 && m.bus.read16(0x03000306) == 0x0100);
    CHECK(static_cast<int32_t>(m.bus.read32(0x03000308)) == (64 - 120) * 256);
    CHECK(static_cast<int32_t>(m.bus.read32(0x0300030C)) == (64 - 80) * 256);

    callSwi(m, 0x0A, 1, 0, 0);                           // ArcTan2(x=1, y=0) = 0
    CHECK(m.cpu.reg(0) == 0);
    callSwi(m, 0x0A, 0, 1, 0);                           // ArcTan2(x=0, y=1) = 90°
    CHECK(m.cpu.reg(0) == 0x4000);
}

static void testHleMoreBiosFunctions() {
    Machine m;
    callSwi(m, 0x0D, 0, 0, 0);                            // GetBiosChecksum
    CHECK(m.cpu.reg(0) == 0xBAAE187Fu);

    // BitUnPack: 1 byte de unidades de 2 bits -> unidades de 8 bits, somando 0x10.
    m.bus.write8(0x03000000, 0x1B);                       // unidades (do bit baixo): 3, 2, 1, 0
    m.bus.write16(0x03000010, 1);                         // tamanho da origem: 1 byte
    m.bus.write8(0x03000012, 2);                          // 2 bits por unidade na origem
    m.bus.write8(0x03000013, 8);                          // 8 bits por unidade no destino
    m.bus.write32(0x03000014, 0x10);                      // soma 0x10 (mas não aos zeros)
    callSwi(m, 0x10, 0x03000000, 0x03000100, 0x03000010);
    CHECK(m.bus.read32(0x03000100) == 0x00111213u);
    m.bus.write32(0x03000014, 0x80000010);                // agora soma também aos zeros
    callSwi(m, 0x10, 0x03000000, 0x03000100, 0x03000010);
    CHECK(m.bus.read32(0x03000100) == 0x10111213u);

    // Huffman: árvore com A = 0, B = 10, C = 11; texto "ABCA" -> bits 0 10 11 0.
    writeBytes(m.bus, 0x03000200, {
        0x28, 0x04, 0x00, 0x00,   // cabeçalho: 8 bits por valor, tipo 2, tamanho 4
        0x03,                     // tamanho da árvore: (3 + 1) * 2 = 8 bytes
        0x80,                     // raiz: filho 0 é valor; filhos logo adiante
        'A',                      // filho 0 da raiz
        0xC0,                     // filho 1 da raiz: nó cujos dois filhos são valores
        'B', 'C',                 // filhos desse nó
        0x00, 0x00});             // preenchimento até alinhar a sequência de bits
    m.bus.write32(0x0300020C, 0x58000000);                // bits 31..26: 0 1 0 1 1 0
    callSwi(m, 0x13, 0x03000200, 0x03000300, 0);
    CHECK(m.bus.read32(0x03000300) == 0x41434241u);       // "ABCA"

    // Filtros de diferença: cada valor é a diferença para o anterior.
    writeBytes(m.bus, 0x03000400, {0x81, 0x04, 0x00, 0x00, 10, 1, 2, 3});
    callSwi(m, 0x16, 0x03000400, 0x03000500, 0);
    CHECK(m.bus.read32(0x03000500) == (10u | (11u << 8) | (13u << 16) | (16u << 24)));
    callSwi(m, 0x17, 0x03000400, 0x06000000, 0);          // versão VRAM: grava em halfwords
    CHECK(m.bus.read16(0x06000000) == (10u | (11u << 8)) && m.bus.read16(0x06000002) == (13u | (16u << 8)));
    m.bus.write32(0x03000600, (6u << 8) | 0x82);
    m.bus.write16(0x03000604, 100);
    m.bus.write16(0x03000606, 5);
    m.bus.write16(0x03000608, static_cast<uint16_t>(-10));
    callSwi(m, 0x18, 0x03000600, 0x03000700, 0);
    CHECK(m.bus.read16(0x03000700) == 100 && m.bus.read16(0x03000702) == 105 && m.bus.read16(0x03000704) == 95);

    // MidiKey2Freq: nota 168 está uma oitava abaixo da 180, então divide a frequência por 2.
    m.bus.write32(0x03000804, 0x02000000);
    callSwi(m, 0x1F, 0x03000800, 168, 0);
    CHECK(m.cpu.reg(0) == 0x01000000u);
    callSwi(m, 0x1F, 0x03000800, 180, 0);
    CHECK(m.cpu.reg(0) == 0x02000000u);

    callSwi(m, 0x25, 0, 0, 0);                            // MultiBoot: falha (sem cabo de link)
    CHECK(m.cpu.reg(0) == 1);
    bool threw = false;                                   // driver de som da BIOS: segue sem erro
    try { callSwi(m, 0x1A, 0, 0, 0); callSwi(m, 0x28, 0, 0, 0); } catch (const std::exception&) { threw = true; }
    CHECK(!threw);
}

int main() {
    testArithmetic();
    testConditions();
    testShifter();
    testBranches();
    testLoadStore();
    testBlockTransfer();
    testMultiply();
    testModesAndPsr();
    testExceptionReturn();
    testHleBios();
    testThumbArithmetic();
    testThumbAlu();
    testThumbHiRegisters();
    testThumbLoads();
    testThumbStack();
    testThumbBranches();
    testArmThumbInterworking();
    testHleCopyAndFill();
    testHleMath();
    testHleDecompress();
    testHleAffineAndAngles();
    testHleMoreBiosFunctions();
    if (failures == 0) std::printf("CPU: todos os testes passaram.\n");
    return failures == 0 ? 0 : 1;
}
