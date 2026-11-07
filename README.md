# Bleach: Soul Resurrección — PC Recomp

**[Português](#português) · [English](#english)**

---

## Português

Uma recompilação estática de **Bleach: Soul Resurrección** (PS3, BLUS30769) para Windows. O código do jogo é
traduzido de PowerPC/SPU para C++ e compilado como um programa nativo do Windows, com renderização em DirectX 12
— não é um emulador.

> **Este projeto não contém o jogo.** Nenhum código, arquivo, imagem, som, texto ou save do jogo é distribuído
> aqui ou no instalador. Você precisa da sua própria cópia original do jogo (BLUS30769, a versão americana sem
> atualização), obtida legalmente, por exemplo fazendo o dump do seu disco no seu PS3.

### O que o recomp oferece

- Resolução interna de 720p até 4K.
- 30, 60 ou 120 FPS (a simulação roda na taxa escolhida; a velocidade do jogo continua a original).
- Tela cheia sem borda ou janela.
- Texto do jogo em inglês (original), português do Brasil, francês ou espanhol.
- Launcher em inglês, português, francês e espanhol, com atalhos prontos para Steam/Playnite.

### Requisitos

- Windows 10 ou 11, 64 bits.
- Placa de vídeo com DirectX 12.
- 8 GB de RAM recomendados.
- 4 GB livres durante a instalação (depois o jogo ocupa cerca de 0,4 GB).
- Internet durante a instalação.
- Seu dump do **Bleach: Soul Resurrección BLUS30769**: o arquivo `.iso` ou a pasta com `PS3_GAME`.

### Como instalar

1. Baixe **`Bleach Soul Resurrección - Setup.exe`** na página de [Releases](../../releases).
2. Abra o setup. A primeira tela verifica o seu PC e mostra o que vai ser baixado.
3. Escolha a pasta de instalação e o seu jogo (`.iso` ou a pasta do dump). O setup confere se é o BLUS30769.
4. Firmware do PS3: deixe **Baixar da Sony** (o setup baixa o `PS3UPDAT.PUP` oficial do servidor de
   atualizações da Sony) ou escolha um `PS3UPDAT.PUP` que você já tenha. Só as fontes do sistema são usadas.
5. Clique em **Instalar**. O setup então:
   - decifra o `EBOOT.BIN` do **seu** jogo com o RPCS3;
   - extrai as fontes e os módulos de fonte do firmware;
   - baixa as ferramentas de compilação dos sites oficiais (Python, Capstone, CMake, Ninja e LLVM-MinGW,
     cerca de 245 MB, cada arquivo conferido por SHA-256);
   - **recompila o jogo no seu PC** (de 3 a 20 minutos, conforme o processador);
   - apaga as ferramentas e cria os atalhos.

Os downloads ficam guardados em `%LOCALAPPDATA%\BleachSoulResurreccion\downloads`; uma reinstalação ou
atualização não baixa tudo de novo.

#### Por que o RPCS3 vem junto?

O [RPCS3](https://rpcs3.net) é o emulador de PS3 de código aberto (GPL v2). O setup usa o programa dele só para
duas coisas: **decifrar o EBOOT do seu disco** e **ler o arquivo de firmware da Sony**. O jogo não roda no
RPCS3; ele roda como um programa nativo do Windows.

### Como jogar

Abra o atalho **Bleach Soul Resurrección**. No launcher escolha resolução, FPS, modo de tela e o idioma do texto
do jogo, e clique em **Jogar**. O launcher lembra o seu dump; se ele for um `.iso`, o Windows o monta sozinho
enquanto o jogo roda.

- **Saves e configurações:** `%LOCALAPPDATA%\BleachSoulResurreccion` (o desinstalador pergunta se deve apagá-los).
- **Shaders:** na primeira vez que cada cena aparece, o jogo prepara os shaders dela e pode dar uma pausa curta;
  depois eles ficam guardados no disco.
- **Esc não fecha o jogo.** Para sair, use o menu do jogo, o botão de fechar da janela ou Alt+F4.
- **Atalhos sem launcher:** veja a pasta *Opções de execução* na pasta de instalação (`--jogar`, `--fps`,
  `--tela`, `--resolucao`, `--texto`).

### Problemas conhecidos

- Esta versão roda sempre em 16:9. O suporte a ultrawide (21:9 / 32:9) está em desenvolvimento e virá numa
  atualização.
- Se o jogo fechar com erro, os logs ficam em `%LOCALAPPDATA%\BleachSoulResurreccion\logs` (e o da instalação em
  `logs\instalacao.log`). Abra uma *issue* com eles.

### Para desenvolvedores

| Pasta | Conteúdo |
|---|---|
| `ps3recomp/` | O SDK [ps3recomp](https://github.com/sp00nznet/ps3recomp) (MIT) com as alterações deste projeto (runtime, HLE, renderizador D3D12). |
| `bleach-recomp/` | O código próprio do recomp (`main.cpp`, `native_*.h`, `CMakeLists.txt`), os manifestos e `construir.py`, que gera e compila tudo a partir do EBOOT decifrado. |
| `bleach-recomp/edicoes-recompilado.json` | As correções feitas à mão no código gerado, gravadas como *hash* das linhas originais + linhas novas (o código do jogo não está nele). |
| `launcher/` | Launcher e setup (C#, .NET Framework 4, `csc`). `montar-instalador.py` monta o setup. |
| `traducao/` | Traduções do texto do jogo: só os textos traduzidos e o *hash* do texto original. |

Compilar à mão (com Python 3.11 + `capstone`, LLVM-MinGW, CMake e Ninja):

```
python bleach-recomp/construir.py --elf EBOOT.elf --firmware <pasta com libfont.prx, libfontFT.prx, libfreetype.prx, libresc.prx> --kit . --toolchain <llvm-mingw>\bin --cmake cmake.exe --ninja ninja.exe --trabalho build --saida bin
```

### Créditos

- **ps3recomp** — sp00nznet e colaboradores (MIT).
- **RPCS3** — equipe RPCS3 (GPL v2): decifração do EBOOT e do firmware, e referência de comportamento do PS3.
- **FFmpeg** (GPL v3) — vídeos do jogo. **LLVM-MinGW** (mstorsjo), **CMake**, **Ninja**, **Python**, **Capstone**.
- Recompilação, correções, launcher e traduções: este projeto.
- *Bleach* © Tite Kubo/Shueisha, TV Tokyo, dentsu, Pierrot. *Bleach: Soul Resurrección* (desenvolvido pela Racjin)
  pertence aos seus respectivos donos. Este é um projeto de fãs, sem fins lucrativos e sem ligação com esses
  titulares. Todas as marcas pertencem aos seus donos.

---

## English

A static recompilation of **Bleach: Soul Resurrección** (PS3, BLUS30769) for Windows. The game's code is
translated from PowerPC/SPU to C++ and built as a native Windows program, rendering with DirectX 12 — it is
not an emulator.

> **This project does not contain the game.** No game code, files, images, sound, text or saves are distributed
> here or in the setup. You need your own, legally obtained copy of the game (BLUS30769, the US release without
> updates), for example a dump of your own disc made on your own PS3.

### What the recomp offers

- Internal resolution from 720p up to 4K.
- 30, 60 or 120 FPS (the simulation runs at the chosen rate; game speed stays the original).
- Borderless fullscreen or windowed.
- Game text in English (original), Brazilian Portuguese, French or Spanish.
- Launcher in English, Portuguese, French and Spanish, with ready-made shortcuts for Steam/Playnite.

### Requirements

- Windows 10 or 11, 64-bit.
- A DirectX 12 graphics card.
- 8 GB of RAM recommended.
- 4 GB free while installing (afterwards the game takes about 0.4 GB).
- An internet connection while installing.
- Your dump of **Bleach: Soul Resurrección BLUS30769**: the `.iso` file or the folder with `PS3_GAME`.

### Installing

1. Download **`Bleach Soul Resurrección - Setup.exe`** from [Releases](../../releases).
2. Run it. The first page checks your PC and lists what will be downloaded.
3. Choose the install folder and your game (`.iso` or dump folder). The setup checks that it is BLUS30769.
4. PS3 firmware: keep **Download from Sony** (the setup downloads the official `PS3UPDAT.PUP` from Sony's PS3
   update server) or choose a `PS3UPDAT.PUP` you already have. Only the system fonts are used.
5. Click **Install**. The setup then:
   - decrypts **your** game's `EBOOT.BIN` with RPCS3;
   - extracts the firmware's fonts and font modules;
   - downloads the build tools from their official sites (Python, Capstone, CMake, Ninja and LLVM-MinGW, about
     245 MB, every file checked against its SHA-256);
   - **recompiles the game on your PC** (3 to 20 minutes depending on the CPU);
   - removes the tools and creates the shortcuts.

Downloads are kept in `%LOCALAPPDATA%\BleachSoulResurreccion\downloads`, so a reinstall or an update does not
download everything again.

#### Why does RPCS3 come with it?

[RPCS3](https://rpcs3.net) is the open-source PS3 emulator (GPL v2). The setup uses its program for only two
things: **decrypting your disc's EBOOT** and **reading Sony's firmware file**. The game does not run on RPCS3;
it runs as a native Windows program.

### Playing

Open the **Bleach Soul Resurrección** shortcut. In the launcher pick the resolution, FPS, display mode and the
game text language, then click **Play**. The launcher remembers your dump; an `.iso` is mounted by Windows
automatically while the game runs.

- **Saves and settings:** `%LOCALAPPDATA%\BleachSoulResurreccion` (the uninstaller asks whether to remove them).
- **Shaders:** the first time each scene appears, the game prepares its shaders and may pause briefly; after
  that they are kept on disk.
- **Esc does not quit the game.** To quit, use the game's menu, the window's close button or Alt+F4.
- **Shortcuts without the launcher:** see the *Launch options* folder in the install folder (`--play`, `--fps`,
  `--display`, `--resolution`, `--text`).

### Known issues

- This version always runs at 16:9. Ultrawide support (21:9 / 32:9) is in development and will come in an
  update.
- If the game closes with an error, the logs are in `%LOCALAPPDATA%\BleachSoulResurreccion\logs` (the install
  log is `logs\instalacao.log`). Please open an issue with them.

### For developers

| Folder | Contents |
|---|---|
| `ps3recomp/` | The [ps3recomp](https://github.com/sp00nznet/ps3recomp) SDK (MIT) with this project's changes (runtime, HLE, D3D12 renderer). |
| `bleach-recomp/` | The recomp's own code (`main.cpp`, `native_*.h`, `CMakeLists.txt`), the manifests and `construir.py`, which generates and builds everything from the decrypted EBOOT. |
| `bleach-recomp/edicoes-recompilado.json` | The hand fixes to the generated code, stored as hashes of the original lines + the new lines (the game's code is not in it). |
| `launcher/` | Launcher and setup (C#, .NET Framework 4, `csc`). `montar-instalador.py` builds the setup. |
| `traducao/` | Game text translations: only the translated strings and a hash of each original string. |

Building by hand (with Python 3.11 + `capstone`, LLVM-MinGW, CMake and Ninja):

```
python bleach-recomp/construir.py --elf EBOOT.elf --firmware <folder with libfont.prx, libfontFT.prx, libfreetype.prx, libresc.prx> --kit . --toolchain <llvm-mingw>\bin --cmake cmake.exe --ninja ninja.exe --trabalho build --saida bin
```

### Credits

- **ps3recomp** — sp00nznet and contributors (MIT).
- **RPCS3** — the RPCS3 team (GPL v2): EBOOT and firmware decryption, and a reference for PS3 behaviour.
- **FFmpeg** (GPL v3) — the game's videos. **LLVM-MinGW** (mstorsjo), **CMake**, **Ninja**, **Python**, **Capstone**.
- Recompilation, fixes, launcher and translations: this project.
- *Bleach* © Tite Kubo/Shueisha, TV Tokyo, dentsu, Pierrot. *Bleach: Soul Resurrección* (developed by Racjin)
  belongs to its respective owners. This is a non-commercial fan project, not affiliated with those rights
  holders. All trademarks belong to their owners.
