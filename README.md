# 🚨 NÃO AMIGO ISSO É SÓ UM TESTE POC NÃO PERDA SEU TEMPO ANUNCIANDO QUE NEM PUTA QUE FIZERAM ALGO EM SERVIDORZINHO!

# BASE COMMIT: `392d9631c4b2a748b8dd460caa1643f79ffaca7f` -- DATA: 29/09/2021

> Este repositório é um experimento/POC de port do NooDS para PlayStation 2. Não existe promessa de compatibilidade, desempenho ou lançamento.

# NooDS-PS2 — objetivos

## Regras do projeto

- [x] Usar o NooDS oficial como base.
- [x] Congelar a base no commit `392d9631c4b2a748b8dd460caa1643f79ffaca7f` de 29/09/2021.
- [ ] Manter o core do NDS separado do backend específico de PS2.
- [ ] Não adicionar hacks específicos por jogo.
- [ ] Não atualizar o upstream inteiro automaticamente; trazer correções novas somente quando forem realmente úteis ao port.
- [ ] Preservar os avisos de copyright/licença e cumprir a GPLv3.
- [ ] Priorizar desempenho no hardware real do PS2 sem quebrar comportamento essencial do NDS.

## Bring-up / PS2SDK

- [x] Criar `Makefile.ps2` usando PS2SDK.
- [x] Criar a camada de plataforma PS2 sem poluir o core.
- [x] Remover do build PS2 dependências de desktop que não sejam necessárias.
- [ ] Gerar um `.elf` mínimo executável no PCSX2 e no hardware real.
- [ ] Inicializar serviços básicos do PS2 necessários ao port.
- [x] Adicionar logging serial/console útil para depuração.
- [x] Garantir que falhas de inicialização produzam mensagens claras em vez de simplesmente travar.
- [x] Criar build Debug e Release separados.

## Arquivos, BIOS, firmware e ROMs

- [x] Implementar suporte inicial a `host:` / hostfs para desenvolvimento rápido no PCSX2.
- [x] Permitir abrir e validar ROMs `.nds` diretamente por `host:` no bootstrap de testes.
- [ ] Permitir carregar BIOS/firmware por `host:`.
- [ ] Permitir carregar/salvar saves por `host:` durante desenvolvimento.
- [ ] Adicionar suporte a `mass:` para USB.
- [ ] Adicionar suporte a Memory Card quando fizer sentido.
- [ ] Avaliar HDD/APA e outros dispositivos posteriormente sem acoplar o core a um filesystem específico.
- [ ] Implementar leitura de ROM por streaming/cache para não exigir a ROM inteira na RAM.
- [ ] Tratar ROMs maiores que a memória disponível sem cópia integral.
- [ ] Validar erros de arquivo ausente, BIOS inválida, firmware inválido e ROM inválida.

## Core NDS

- [ ] Fazer o core inicializar no EE sem frontend desktop.
- [ ] Fazer ARM9 iniciar e executar instruções.
- [ ] Fazer ARM7 iniciar e executar instruções.
- [ ] Validar mapa de memória.
- [ ] Validar IRQ/exceptions.
- [ ] Validar timers.
- [ ] Validar DMA.
- [ ] Validar IPC ARM9 ↔ ARM7.
- [ ] Validar acesso ao cartucho.
- [ ] Validar saves.
- [ ] Validar sincronização do scheduler.
- [ ] Criar testes determinísticos para CPU, memória, DMA, timers e IPC.
- [ ] Criar mecanismo de comparação de estado para detectar regressões.

## Vídeo / GS

- [ ] Exibir um framebuffer simples no GS.
- [ ] Exibir as duas telas do NDS.
- [ ] Implementar layout vertical.
- [ ] Implementar layout lado a lado.
- [ ] Implementar tela principal grande + secundária pequena.
- [ ] Implementar troca rápida de layout.
- [ ] Medir custo do renderer 2D por software original.
- [ ] Identificar operações 2D que podem ser transferidas ao GS.
- [ ] Implementar caminho acelerado no GS sem hacks por jogo.
- [ ] Medir custo do renderer 3D por software original.
- [ ] Mapear operações 3D compatíveis diretamente com o GS.
- [ ] Avaliar uso de VU0/VU1 para geometria/conversões que realmente tragam ganho.
- [ ] Manter fallback correto para efeitos que o GS não reproduza diretamente.
- [ ] Adicionar frameskip configurável.
- [ ] Evitar conversões e cópias de framebuffer desnecessárias.

## Controles / touchscreen

- [ ] Mapear D-pad e botões do DualShock 2 para os controles do NDS.
- [ ] Usar analógico direito como cursor de touchscreen.
- [ ] Adicionar botão configurável para toque.
- [ ] Adicionar modo de manter stylus pressionado.
- [ ] Permitir trocar as telas por botão.
- [ ] Avaliar suporte a mouse USB posteriormente.
- [ ] Emular ação simples de “soprar no microfone” por botão para jogos que só exigem ruído.

## Áudio / SPU2

- [ ] Fazer o mixer do NDS produzir PCM corretamente.
- [ ] Criar saída de áudio pelo SPU2.
- [ ] Implementar buffering estável.
- [ ] Sincronizar áudio sem usar hacks específicos por jogo.
- [ ] Medir o custo real do mixer.
- [ ] Avaliar otimizações específicas do PS2 somente depois da saída básica funcionar.

## Memória

- [ ] Criar orçamento explícito dos 32 MB de RAM do PS2.
- [ ] Medir memória usada pelo core.
- [ ] Medir memória usada por framebuffers/renderers.
- [ ] Criar cache de ROM com tamanho configurável.
- [ ] Evitar alocações por frame.
- [ ] Remover buffers duplicados sempre que possível.
- [ ] Detectar e registrar falta de memória de forma controlada.

## Desempenho / CPU

- [ ] Criar contador de FPS e frametime.
- [ ] Criar métricas separadas de ARM9, ARM7, GPU2D, GPU3D, áudio e scheduler.
- [ ] Medir o interpreter original antes de otimizar.
- [ ] Identificar os gargalos reais no R5900.
- [ ] Otimizar caminhos quentes de memória.
- [ ] Reduzir dispatch/overhead do interpreter onde possível.
- [ ] Planejar um JIT ARM/Thumb → R5900.
- [ ] Implementar cache de blocos do JIT.
- [ ] Implementar invalidação correta de código recompilado.
- [ ] Implementar ARM9 → R5900.
- [ ] Implementar ARM7 → R5900.
- [ ] Validar JIT contra o interpreter usando testes determinísticos.
- [ ] Só ativar JIT por padrão depois de estabilidade suficiente.
- [ ] Avaliar uso do IOP apenas onde existir ganho real; não mover ARM7 para o IOP só por terem clocks parecidos.

## Compatibilidade e testes

- [ ] Criar ROM/homebrew de teste mínima para boot.
- [ ] Criar conjunto de testes de ARM e Thumb.
- [ ] Criar testes de DMA.
- [ ] Criar testes de timers.
- [ ] Criar testes de IPC.
- [ ] Criar testes de GPU2D.
- [ ] Criar testes de GPU3D.
- [ ] Criar testes de áudio.
- [ ] Testar primeiro no PCSX2 usando `host:`.
- [ ] Repetir marcos importantes em PS2 real.
- [ ] Registrar FPS, RAM e falhas de cada teste.
- [ ] Não considerar “abre o jogo” como compatibilidade; validar progressão e estabilidade.
- [ ] Corrigir causas no core, nunca adicionar `if (game == X)`.

## Marcos

- [ ] M0 — PS2 ELF inicia e escreve log.
- [ ] M1 — arquivos podem ser lidos por `host:`.
- [ ] M2 — BIOS/firmware/ROM são carregados pelo backend PS2.
- [ ] M3 — ARM9 e ARM7 executam sem crash imediato.
- [ ] M4 — primeiro frame é produzido.
- [ ] M5 — duas telas aparecem corretamente no GS.
- [ ] M6 — controles e touchscreen funcionam.
- [ ] M7 — áudio funciona pelo SPU2.
- [ ] M8 — homebrew simples executa de forma estável.
- [ ] M9 — primeiro jogo comercial chega ingame sem hack específico.
- [ ] M10 — profiling identifica gargalos principais.
- [ ] M11 — JIT R5900 executa blocos ARM/Thumb e passa testes contra interpreter.
- [ ] M12 — primeiro título comercial atinge velocidade jogável.
- [ ] M13 — backend GS reduz de forma mensurável o custo de GPU.
- [ ] M14 — build funciona no PCSX2 e em PS2 real com resultados reproduzíveis.

## Critério para deixar de ser apenas um POC

Este projeto continua sendo considerado POC até que exista, no mínimo:

- [ ] boot reproduzível em PS2 real;
- [ ] carregamento de ROM sem depender de hacks por título;
- [ ] vídeo das duas telas;
- [ ] input/touch funcional;
- [ ] áudio funcional;
- [ ] pelo menos um conjunto de testes determinísticos passando;
- [ ] pelo menos um jogo comercial executando de forma estável e jogável;
- [ ] documentação suficiente para outra pessoa reproduzir o build e os testes.


## Estado atual do bootstrap PS2

O primeiro estágio ainda **não inicia o core do NooDS**. Ele existe para validar PS2SDK, geração do ELF e acesso ao dispositivo `host:` antes de ligar ARM9/ARM7, GPU e áudio.

Build release:

```sh
make -f Makefile.ps2
```

Build debug:

```sh
make -f Makefile.ps2 debug
```

O ELF resultante é `noods-ps2.elf`.

### Teste rápido no PCSX2 com hostfs

Ative **Settings → Emulation → Enable Host Filesystem** no PCSX2 e deixe estes arquivos no diretório usado como raiz de `host:` (normalmente o diretório do ELF ao iniciar um ELF diretamente):

```text
noods-ps2.elf
game.nds
bios9.bin
bios7.bin
firmware.bin
```

O bootstrap tenta abrir por padrão:

```text
host:game.nds
host:bios9.bin
host:bios7.bin
host:firmware.bin
```

A ROM também pode ser passada explicitamente como primeiro argumento, por exemplo `host:roms/test.nds`. Neste estágio BIOS e firmware são apenas verificados; o carregamento pelo core entra no M2.

**Não marque M0/M1 como concluídos só porque o CI gerou um ELF.** M0 exige executar o ELF e ver o log no PS2/PCSX2; M1 exige confirmar uma leitura real via `host:`.


---

# README original do NooDS na base escolhida

# NooDS
A (hopefully!) speedy NDS emulator.

### Overview
The goal of NooDS is to be a fast and portable Nintendo DS emulator. It's not quite there speed-wise, but it does offer most other features that you might expect from a DS emulator. It even supports GBA backwards compatability! I'm doing this for fun and as a learning experience, and also because I'm a huge fan of the DS. It may not be a worthy competitor for the other DS emulators just yet, but I believe that I can get it there someday. If not, that's fine too; like I said, I'm just having fun!

### Downloads
NooDS is available for Linux, macOS, Windows, Switch, Vita, and Android. Automatic builds are provided via GitHub Actions; you can download them from the [releases page](https://github.com/Hydr8gon/NooDS/releases).

### Usage
NooDS doesn't provide high-level emulation of the BIOS yet, so you'll need to provide BIOS and firmware files dumped from your physical DS. The file paths can be configured in the settings. It has basic save type detection, but can easily be wrong. If you load a new game and saving doesn't work, you'll have to manually change the save type. This information can be difficult to find, so it's easier if you have working save files already present.

### Compiling for Linux or macOS
To compile on Linux or macOS, you'll need to install [wxWidgets](https://www.wxwidgets.org) and [PortAudio](http://www.portaudio.com) using your favourite package manager. You can use [Homebrew](https://brew.sh) on macOS, since there is no package manager provided by default. The command will look something like `apt install libwxgtk3.0-dev portaudio19-dev` (Ubuntu) or `brew install wxmac portaudio` (macOS). After that, you can simply run `make` in the project root directory to compile.

### Compiling for Windows
To compile on Windows, you'll need to install [MSYS2](https://www.msys2.org). Once you have that set up and running, you can install all of the packages you'll need by running `pacman -S mingw-w64-x86_64-{gcc,pkg-config,wxWidgets,portaudio,jbigkit} make`. It might also be a good idea to run `pacman -Syu` to ensure everything is up to date. After that, you can simply run `make` in the project root directory to compile.

### Compiling for Switch
To compile for the Switch, you'll need to install [devkitPro](https://devkitpro.org/wiki/Getting_Started) and the `switch-dev` package. You can then run `make -f Makefile.switch` in the project root directory to compile.

### Compiling for Vita
To compile for the Vita, you'll need to install [Vita SDK](https://vitasdk.org/). You can then run `make -f Makefile.vita` in the project root directory to compile.

### Compiling for Android
To compile for Android, the easiest way would be to use [Android Studio](https://developer.android.com/studio). You'll also need to install the [Android NDK](https://developer.android.com/studio/projects/install-ndk) for compiling native code. Alternatively, you can use the [command line tools](https://developer.android.com/studio#command-tools); use `sdkmanager` to install `build-tools`, `cmake`, `ndk-bundle`, `platform-tools`, and `platforms;android-29`, and set an `ANDROID_SDK_ROOT` environment variable to the folder containing `cmdline-tools`. You should then be able to compile by running `./gradlew assembleRelease` in the project root directory.

### References
* [GBATEK](https://problemkaputt.de/gbatek.htm) by Martin Korth - This is where most of my information came from
* [GBATEK Addendum](http://melonds.kuribo64.net/board/thread.php?id=13) by Arisotura - GBATEK isn't perfect, so some information came from here as well
* Blog Posts [1](http://melonds.kuribo64.net/comments.php?id=85), [2](http://melonds.kuribo64.net/comments.php?id=56), [3](http://melonds.kuribo64.net/comments.php?id=32), and [4](http://melonds.kuribo64.net/comments.php?id=27) by Arisotura - Great resources that detail the 3D GPU's lesser-known quirks
* [ARM Opcode Map](http://imrannazar.com/ARM-Opcode-Map) by Imran Nazar - Used to create the interpreter lookup table
* Hardware tests by me - When there's something that I can't find or want to verify, I write tests for it myself!

### Other Links
* [Hydra's Lair](https://hydr8gon.github.io/) - you can read about my progress here!
* [NooDS Discord](https://discord.gg/JbNz7y4) - here we can chat and do other fun stuff!
