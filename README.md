# NiGBA

[![CI](https://github.com/Nidragnnar/NiGBA/actions/workflows/ci.yml/badge.svg)](https://github.com/Nidragnnar/NiGBA/actions/workflows/ci.yml)

Emulador de Game Boy Advance em C++20, escrito do zero usando o GBATEK como especificação.

## Estado atual

- [x] Barramento de memória com o mapa completo e os espelhos (BIOS, EWRAM, IWRAM, I/O, paleta, VRAM, OAM, ROM, SRAM)
- [x] Regras de escrita de 8 bits (paleta/VRAM duplicam, OAM ignora), SRAM de 8 bits, open bus da ROM
- [x] Proteção da BIOS (leitura devolve a última instrução buscada pela BIOS), open bus e leitura de registradores só-escrita como no hardware
- [x] Bits inexistentes dos registradores leem 0; registradores de som zerados e travados com o som desligado
- [x] Leitura e validação do cabeçalho da ROM
- [x] Janela SDL2
- [x] CPU ARM7TDMI — estado ARM: todas as instruções, bancos de registradores, CPSR/SPSR, exceções, pipeline com busca antecipada
- [x] BIOS em alto nível (HLE) sem BIOS real: todas as funções usadas por jogos (reset, espera, divisão, raiz, ângulos, cópia, matrizes afins, BitUnPack, LZ77, Huffman, RLE, filtros de diferença, MidiKey2Freq, checksum); o driver de som da BIOS é ignorado
- [x] CPU ARM7TDMI — estado Thumb: os 19 formatos; Halt e HALTCNT
- [ ] Mais funções da BIOS em HLE (conforme o jogo pedir)
- [x] Temporização do vídeo (VCOUNT, DISPSTAT, VBlank/HBlank) e interrupções (IE/IF/IME)
- [x] Tratador de IRQ da BIOS (HLE) e teclado
- [x] DMA: 4 canais, imediato, VBlank e HBlank, repetição e recarga
- [x] PPU linha a linha: modos 0-5, backgrounds de tiles (16/256 cores, espelhamento, rolagem, prioridade), afins e bitmap
- [x] Cheats (Emulação → Cheats): Raw (VBA), CodeBreaker e GameShark v1/v2, salvos num `.cht` ao lado da ROM e aplicados a cada quadro
- [x] Filtros de imagem (menu Vídeo): cores da tela do GBA original (fórmula de Near/higan), suavização bilinear e efeito de tela LCD
- [x] Voltar no tempo (segurar R ou o gatilho esquerdo): ~20 s de histórico, uma foto a cada 10 quadros, guardadas como diferenças comprimidas (poucos MB)
- [x] Captura de tela (F12): PNG em 240×160 numa pasta `Capturas` ao lado da ROM
- [x] Patches de tradução e hacks (soft-patching IPS, UPS e BPS): `Jogo.gba` + `Jogo.ups` na mesma pasta e o patch é aplicado na memória, sem alterar a ROM; UPS e BPS conferem o CRC32 e avisam se a ROM for de outra versão
- [x] Temporização realista: custo de cada acesso por região (WAITCNT, sequencial ou não), buffer de prefetch do cartucho, ciclos internos da CPU, DMA gastando tempo da CPU e timers precisos no meio das instruções e DMA com tempo próprio (sequência de origem e destino, 2 ciclos de partida) (suíte do mGBA: Timing 1784/2020)
- [x] Relógio do cartucho (RTC Seiko S-3511, porta GPIO) com a hora do computador: Pokémon Ruby/Sapphire/Emerald e derivados, Boktai
- [x] Save: detecção automática, SRAM, Flash 64/128 KB e EEPROM 512 bytes/8 KB, arquivo .sav com gravação segura e automática
- [x] Sprites: normais e afins (rotação/zoom, tamanho duplo), 12 tamanhos, 16/256 cores, espelhamento, mapeamento 1D/2D, prioridade com backgrounds e entre sprites, limite de ciclos por linha
- [x] Janelas (WIN0, WIN1, janela de sprites e "fora"), blending (mistura, clarear, escurecer, sprites semitransparentes) e mosaico (BG e sprites)
- [x] Timers: 4 canais, prescalers 1/64/256/1024, cascata, recarga e IRQ
- [x] Áudio: 4 canais do Game Boy (quadradas com sweep, onda, ruído; duração e envelope) e filas A/B com DMA de som e timers, 32.768 Hz estéreo
- [x] Ritmo pela placa de som (velocidade correta em qualquer monitor) e aceleração com Tab
- [x] Save states (4 slots), com verificação de jogo e de versão (estados antigos continuam carregando)
- [x] Interface com Dear ImGui: menus, abrir ROM (diálogo, arrastar e soltar, recentes), pausar, reiniciar, escala, tela cheia, volume, tema pastel
- [x] Configuração de teclas e controle (gamepad): remapeamento na janela Controles, analógico como direcional, gatilho direito acelera
- [x] Janelas de depuração: CPU (registradores, flags, rastro, avançar quadro), memória (hexadecimal), VRAM (tiles), paletas, sprites (OAM com miniaturas) e registradores de I/O
- [x] Passa nas ROMs de teste do jsmolka (arm, thumb, memory, nes, unsafe, saves, ppu) e, na suíte do mGBA, 100% em Memory, I/O read, DMA, BIOS math, SIO registers, Shifter e Carry; o CI baixa e roda tudo isso a cada push
- [x] GitHub Actions: compila e testa no Windows, Linux e macOS a cada push; publica a Release ao enviar uma tag de versão
- [x] Pacote para distribuir: um único NiGBA.exe, sem DLLs e sem terminal (`empacotar.bat`)

## Compilar no Windows

Requisitos: Visual Studio 2022 (com C++), CMake e [vcpkg](https://github.com/microsoft/vcpkg).

```bat
vcpkg install sdl2:x64-windows
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/caminho/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release
```

O Dear ImGui (interface) é baixado automaticamente pelo CMake na primeira configuração (precisa de internet e do Git).

## Rodar

Sem argumentos, abre a tela inicial: escolha um jogo pelo menu, arraste o `.gba` para a janela ou clique num dos recentes. Também dá para passar a ROM direto:

```bat
build\Release\NiGBA.exe jogo.gba --test-pattern
```

Sem `--test-pattern`, a CPU executa a ROM. Se ela encontrar algo ainda não implementado (como uma função da BIOS), para e mostra o motivo no terminal. A barra de título mostra o PC atual.

Controles (todos remapeáveis em Ajuda → Controles; controle/gamepad funciona junto com o teclado): setas = direcional, Z = A, X = B, Enter = START, Backspace = SELECT, A = L, S = R, Tab (segurar) = acelerar, Esc = pausar, F5/F8 = salvar/carregar estado, Ctrl+1..4 = slot, Ctrl+O = abrir ROM, Ctrl+R = reiniciar, F11 = tela cheia, F1 = diagnóstico, F2 = rastro da CPU.

Opções: `--mudo` (sem som), `--desvios-para ENDERECO` (desvios para o endereço e as funções chamadas antes deles), `--chamadas-para ENDERECO` (BLs e ponteiros para o endereço exato), `--quem-chama ENDERECO` (função que contém o endereço e quem a chama), `--rastrear-texto "TEXTO"` (acha um texto do jogo na ROM, os ponteiros para ele, a função que o exibe e quem chama essa função), `--dump ENDERECO:TAMANHO` (mostra a memória em hexadecimal, ex.: `--dump 080C2580:100`), `--sonda` (registra no terminal acessos que costumam revelar imprecisões: leituras da BIOS, open bus, registradores só-escrita, timers e WAITCNT), `--bios gba_bios.bin` (16 KB, extraída do seu próprio console), `--test-pattern` (desenha um degradê na VRAM para validar barramento → PPU → tela), `--scale N` (padrão 3). `Esc` fecha.

## Distribuir para outras pessoas

Quem só vai jogar não precisa de Visual Studio, CMake nem vcpkg. Rode:

```bat
empacotar.bat
```

Ele gera `dist\NiGBA-windows.zip` com um `NiGBA.exe` único (o SDL e as bibliotecas do C++ ficam embutidos, então não há DLLs) e um `LEIA-ME.txt` para quem recebe. Essa versão abre sem a janela do terminal. Não inclua ROMs no pacote.

## Publicar uma versão nova

```bat
git tag v1.2
git push origin v1.2
```

O GitHub Actions roda os testes, gera o pacote do Windows e cria a Release com o `NiGBA-windows.zip` anexado. Depois, é só editar o texto da Release no site, se quiser.

## Traduções e outros patches

Coloque o patch na mesma pasta da ROM, com o mesmo nome: `Pokemon FireRed.gba` + `Pokemon FireRed.ups`. Ao abrir o jogo, o NiGBA procura `.bps`, `.ups` e `.ips` (nessa ordem) e aplica o patch na memória; o arquivo da ROM nunca é alterado. Também dá para arrastar um patch para a janela com o jogo aberto. Em **Emulação → Reabrir sem o patch**, o jogo volta ao original. Patches UPS e BPS guardam o CRC32 da ROM esperada: se a ROM for de outra versão (por exemplo, 1.0 em vez de 1.1), o NiGBA avisa e abre o jogo sem o patch.

## Save e save states

As preferências (jogos recentes, escala, volume, teclas) ficam em `NiGBA.ini`, na pasta de dados do usuário (no Windows, `%APPDATA%\NiGBA\NiGBA`).

Save states ficam ao lado da ROM (`.ss1` a `.ss4`). Eles guardam o estado do console, mas não o save do cartucho: carregar um estado antigo não apaga o progresso salvo pelo próprio jogo.


O save fica ao lado da ROM, com o mesmo nome e extensão `.sav` (por exemplo `ROGUE-2.2.1.sav`). Ele é gravado a cada ~5 segundos se algo mudou e ao fechar o emulador. Se já existir um `.sav` de tamanho inesperado, ele é preservado e o emulador grava em `.NiGBA.sav`.

## Estrutura

```
src/core/      núcleo sem dependências (gba = console completo; bus, cartucho, save, cpu ARM/Thumb, BIOS HLE, dma, timers, ppu, apu (áudio), analysis = ferramentas de engenharia reversa)
src/frontend/assets/  ícone do programa (nigba.ico; nigba.png em 512 px para outros usos)
src/frontend/  aplicação (SDL2 + Dear ImGui): app, ui, config, diagnóstico, diálogo de arquivos
tests/         testes do núcleo, da CPU, do sistema, do vídeo, do áudio, dos save states e das ROMs de teste
               (coloque arm.gba, thumb.gba, suite.gba etc. em tests/roms/ para rodá-las localmente)
```
