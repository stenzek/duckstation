# DuckStation - Emulador de PlayStation 1, também conhecido como PSX

[Recursos](#features) | [Baixando e executando](#downloading-and-running) | [Compilando](#building) | [Avisos](#disclaimers)

**Versões mais recentes para Windows 10/11 (x64/ARM64), Linux (AppImage x64/ARM32/ARM64) e macOS (13.3+ Universal):** https://github.com/stenzek/duckstation/releases/tag/latest

**Servidor do Discord:** https://www.duckstation.org/discord.html

DuckStation é um simulador/emulador do console Sony PlayStation(TM), com foco em jogabilidade, velocidade e manutenção a longo prazo. O objetivo é ser o mais preciso possível, mantendo um desempenho adequado para dispositivos mais modestos. As opções de “hack” não são recomendadas; a configuração padrão deve oferecer suporte a todos os jogos categorizados como jogáveis, com apenas alguns aprimoramentos apresentando problemas de compatibilidade.

É preciso de uma imagem do “BIOS” do PS1 ou PS2 para iniciar o emulador e jogar. Você pode usar uma imagem de qualquer versão de hardware ou região, embora regiões diferentes entre o jogo e o BIOS possam causar problemas de compatibilidade. Por motivos legais, nenhuma imagem de ROM é fornecida com o emulador; você deve extraí-la do seu próprio console usando o Caetla ou outros meios.

## Recursos

DuckStation conta com uma interface completa (frontend) criada com Qt, além de uma interface em tela cheia/TV baseada em Dear ImGui.

<p align="center">
  <img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/main-qt.png" alt="Main Window Screenshot" />
  <img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/bigduck.png" alt="Fullscreen UI Screenshot" />
</p>

Outros recursos incluem:

 - Recompilador/JIT de CPU (x86-64, armv7/AArch32, AArch64, RISC-V/RV64).
 - Renderizador por hardware com suporte às APIs D3D11, D3D12, OpenGL, Vulkan e Metal.
 - Upscaling, filtragem de textura e cores reais (24-bit) nos renderizadores por hardware.
 - PGXP para precisão de geometria, correção de textura e emulação de buffer de profundidade.
 - Mesclagem precisa por meio de Rasterizer Order Views (ROVs)/Fragment Shader Interlock.
 - Sistema de substituição de texturas nos renderizadores por hardware.
 - Renderizador por software vetorizado e multithread.
 - Desentrelaçamento adaptativo.
 - Filtro de redução de amostragem adaptativo.
 - Rotação de tela para jogos verticais ou shmups “TATE”.
 - Cadeias de shaders de pós-processamento (GLSL, Reshade FX e predefinições Slang).
 - Sobreposições/molduras (bezels) exibidas ao redor do conteúdo do jogo.
 - “Fast boot” para pular a tela/intro da BIOS.
 - Suporte a save states, com runahead e retrocesso.
 - Suporte para Windows, Linux e macOS.
 - Suporte à leitura direta de CD, imagens bin/cue, MAME CHD, ECM de faixa única, MDS/MDF, CCD e formatos PBP não criptografados.
 - Pré-carregamento de imagens de disco na RAM para evitar engasgos quando o disco “hiberna”.
 - Mesclagem de jogos multidisco na lista/grade de jogos, com cartões de memória compartilhados entre os discos.
 - Carregamento/aplicação automática de modificações PPF.
 - Inicialização direta de executáveis homebrew.
 - Carregamento direto de arquivos Portable Sound Format (psf).
 - Áudio com time stretching ao executar fora de 100% da velocidade.
 - Controles digitais e analógicos para entrada (a vibração é repassada ao host).
 - Suporte a pistolas de luz GunCon e Justifier (simuladas com mouse).
 - Suporte ao NeGcon.
 - Predefinições de controle e configuração por jogo.
 - Interface Qt e “Big Picture”.
 - Atualizações automáticas com canais preview e latest.
 - Varredura automática de conteúdo — títulos/hashes dos jogos fornecidos por redump.org.
 - Troca automática opcional de cartões de memória para cada jogo separadamente.
 - Suporte ao carregamento de trapaças a partir de listas.
 - Editor de cartões de memória e importação de arquivos de dados de jogos salvos.
 - Overclock emulado da CPU.
 - Depuração integrada e remota.
 - Controles Multitap (até 8 dispositivos).
 - RetroAchievements.
 - Presença rica no Discord.
 - Captura de vídeo com backends Media Foundation (Windows) e [FFmpeg](https://www.ffmpeg.org/) (todas as plataformas).
 - Função de câmera livre.
 - Emulação de cartucho da porta paralela.

## Requisitos do sistema
 - Um CPU mais rápido do que uma 'batata'. Mas precisa ser x86_64, AArch32/armv7, AArch64/ARMv8 ou RISC-V/RV64.
 - Uma GPU capaz de OpenGL 3.1/OpenGL ES 3.1/Direct3D 11 Feature Level 10.0/Vulkan 1.0. Ou seja, basicamente qualquer coisa feita nos últimos 10 anos, mais ou menos.
 - Controle de jogo compatível com SDL, XInput ou DInput (ex.: XB360/XBOne/XBSeries). Usuários de DualShock 3 no Windows precisarão instalar os drivers oficiais do DualShock 3 incluídos como parte do PlayStation Now.

## Baixando e executando
Os binários do DuckStation para Windows x64/ARM64, Linux x86_64/ARM32/ARM64 (no formato AppImage) e macOS (binários universais) estão disponíveis via GitHub na guia Releases e são compilados automaticamente a cada commit/push.

Conforme os termos da CC-BY-NC-ND, a redistribuição de **releases e código não modificados** é permitida. No entanto, preferimos que você aponte para https://www.duckstation.org/ em vez disso. Configurações e pacotes pré-configurados são considerados modificações.

Para máquinas x86 (a maioria dos sistemas), você precisará de uma CPU que suporte o conjunto de instruções SSE4.1 para a build “normal”. Isso inclui todas as CPUs Intel fabricadas após 2007 e CPUs AMD fabricadas após 2011. Se você tiver uma CPU mais antiga, será necessário baixar a build “SSE2” na página de releases, que tem desempenho inferior, mas ainda oferece suporte a essas CPUs.

A página principal de releases é limitada às últimas 30 versões devido a limitações do atualizador automático. Versões mais antigas podem ser baixadas em https://github.com/duckstation/old-releases/releases.

### Canais de atualização

O atualizador automático do DuckStation tem dois canais: “Stable” e “Preview”.
 - **“Stable Releases”**: Atualizações menos frequentes e acompanha a release “latest” no GitHub. As releases neste canal foram mais testadas.
 - **“Preview Releases”**: Compiladas sempre que um commit é enviado ao repositório e acompanha o pre-release no GitHub. Este canal contém builds com testes mínimos e pode conter bugs ou problemas.

Por padrão, o atualizador acompanhará o canal do qual você fez o download. Você pode mudar o canal em `Configurações -> Interface -> Atualizações`.

### Windows

O DuckStation **exige** Windows 10/11, especificamente a versão 1809 ou mais recente. Se você ainda usa Windows 7/8/8.1, o DuckStation **não vai rodar** no seu sistema operacional. Usar esses sistemas operacionais em 2026 deve ser considerado um risco de segurança, e recomendo atualizar para algo que ainda receba suporte do fornecedor.
Se você precisar usar um sistema operacional mais antigo, a [v0.1-5624](https://github.com/duckstation/old-releases/releases/tag/v0.1-5624) é a última versão que roda. Mas não espere receber suporte; essas builds não são mais suportadas.

As builds para Windows são disponibilizadas em dois formatos:
 - **Instalador (.exe, recomendado):** Um instalador que extrai o DuckStation para o diretório de programas do usuário e, opcionalmente, cria atalhos no Menu Iniciar/Área de Trabalho.
 - **Arquivo (.zip):** Um arquivo zip contendo o binário pré-compilado. Escolha esta opção se você quiser uma instalação “portátil” ou não quiser executar um instalador.

Para usar o instalador, basta baixá-lo na página de releases, executá-lo e seguir as instruções.
 - Link direto de download: https://github.com/stenzek/duckstation/releases/download/latest/duckstation-windows-x64-installer.exe
 - Link de download ARM64 (notebooks Snapdragon): https://github.com/stenzek/duckstation/releases/download/latest/duckstation-windows-arm64-installer.exe
 - Instalador SSE2 legado (para CPUs pré-2008): https://github.com/stenzek/duckstation/releases/download/latest/duckstation-windows-x64-sse2-installer.exe

O instalador ainda é uma adição recente; portanto, se você encontrar problemas, por favor nos avise pelo Discord.

Para usar o arquivo (.zip) ou a instalação portátil, siga estes passos:
1. Baixe https://github.com/stenzek/duckstation/releases/download/latest/duckstation-windows-x64-release.zip. Se você tiver uma máquina Windows ARM64, como Snapdragon, baixe `duckstation-windows-arm64-release.zip` em vez disso.
2. Extraia o arquivo **para um subdiretório**. O arquivo não tem um subdiretório raiz; portanto, extrair para o diretório atual vai espalhar um monte de arquivos na sua pasta de downloads se você não extrair para um subdiretório.
3. Se você quiser uma instalação portátil (veja [Diretórios de usuário](#user-directories)), crie um arquivo vazio chamado `portable.txt` no mesmo diretório do executável.
4. Depois de baixar e extrair, você pode iniciar o emulador com `duckstation-qt-x64-ReleaseLTCG.exe`. Siga o Assistente de configuração para começar.

**Se você receber um erro informando que `vcruntime140_1.dll` está faltando, será necessário atualizar o runtime do Visual C++.** Você pode fazer isso nesta página: https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist. Especificamente, você precisa do runtime x64, que pode ser baixado em https://aka.ms/vs/17/release/vc_redist.x64.exe.

### Linux

O DuckStation é disponibilizado para Linux x86_64/ARM32/ARM64 no formato AppImage.

#### AppImage

Os AppImages exigem uma distribuição equivalente ao Ubuntu 22.04 ou mais recente para funcionar.

1. Baixe https://github.com/stenzek/duckstation/releases/download/latest/DuckStation-x64.AppImage. Se você tiver uma máquina Linux ARM64, baixe `DuckStation-arm64.AppImage`.
2. Execute `chmod a+x` no AppImage baixado — após este passo, o AppImage pode ser executado como um executável comum. Como alternativa, no gerenciador de arquivos de sua preferência, habilite a permissão de execução nas propriedades do arquivo.
3. Ao executar o AppImage pela primeira vez, será exibida uma solicitação para criar um atalho do lançador de programas.

Se você estava usando anteriormente o pacote Flatpak, para migrar seus dados do Flatpak para o AppImage, execute o seguinte comando:
```bash
mv ~/.var/app/org.duckstation.DuckStation/config/duckstation ~/.local/share
```

Você precisará adicionar novamente seus diretórios de jogos após mudar para o AppImage.

### macOS

Builds universais do macOS são fornecidas tanto para x86_64 (Intel) quanto para ARM64 (Apple Silicon).

O macOS Ventura (13.3) é obrigatório, pois este também é o requisito mínimo do Qt.

Para baixar:

1. Baixe https://github.com/stenzek/duckstation/releases/download/latest/duckstation-mac-release.zip.
2. Extraia o zip com um clique duplo.
3. Abra `DuckStation.app`, opcionalmente movendo-o primeiro para o local desejado.

Se você receber uma mensagem informando que o app é de um desenvolvedor não identificado:

1. Abra Ajustes do Sistema -> Privacidade e Segurança, ou digite “Gatekeeper” na barra de busca.
2. Na seção “Segurança”, deve haver uma mensagem informando que o DuckStation foi bloqueado. Clique em “Abrir mesmo assim”.

Infelizmente isso é necessário porque a Apple exige assinatura de código para que apps possam ser executados sem avisos, e eu não tenho um certificado de assinatura de código, já que um custo anual está fora de cogitação para um projeto que gera zero receita.

### Android

Você precisará de um dispositivo com armv7 (ARM 32-bit), AArch64 (ARM 64-bit) ou x86_64 (x86 64-bit). 64-bit é preferível; os requisitos são maiores para 32-bit — provavelmente você vai querer pelo menos uma CPU de 1,5 GHz.

Baixe pela Google Play: https://play.google.com/store/apps/details?id=com.github.stenzek.duckstation

**Não é fornecido suporte para o app de Android**; ele é gratuito e suas expectativas devem ser compatíveis com isso. Por favor, **não** me envie e-mail sobre problemas nele nem peça ajuda; você será ignorado.

Para usar:
1. Instale e execute o app pela primeira vez.
2. Siga o assistente de configuração.

Se você tiver um controle externo, será necessário mapear os botões e os analógicos nas configurações.

### Proteção LibCrypt e arquivos SBI

Vários jogos da região PAL usam a proteção LibCrypt, exigindo informações adicionais de subcanais do CD para funcionar corretamente. Quando o libcrypt não funciona, normalmente isso se manifesta como travamentos ou quedas, mas às vezes também pode afetar a jogabilidade, dependendo de como o jogo a implementou.

Para esses jogos, certifique-se de que a imagem do CD e o arquivo SBI (.sbi) correspondente tenham o mesmo nome e estejam no mesmo diretório. O DuckStation carregará automaticamente o arquivo SBI quando ele for encontrado junto da imagem do CD.

Por exemplo, se a imagem do seu disco se chama `Spyro3.cue`, você colocaria o arquivo SBI no mesmo diretório e o nomearia como `Spyro3.sbi`.

Imagens CHD com informações de subcanal embutidas também são suportadas.

Se você estiver jogando diretamente do disco e seu drive de CD/DVD não suportar leitura de subcanais, ou tiver discrepância (skew) no SubQ retornado, você pode colocar o arquivo SBI no diretório `subchannels` dentro do diretório do usuário, com o serial ou o título do jogo.

### Banco de dados de trapaças e modificações

O DuckStation vem com um banco de dados interno de trapaças e modificações, ambos fornecidos pela comunidade. Contribuições são bem-vindas em https://github.com/duckstation/chtdb.

Cada release inclui a versão mais recente do banco de dados; no entanto, você também pode atualizá-lo manualmente para a versão mais recente.

## Compilando

### Windows
Requisitos:
 - Visual Studio 2026 ou mais recente, com a carga de trabalho “Desenvolvimento para desktop com C++” instalada.

1. Clone o repositório: `git clone https://github.com/stenzek/duckstation.git`.
2. Baixe o pacote de dependências em https://github.com/duckstation/dependencies. Você precisará do arquivo `deps-windows-x64.7z` e, se quiser compilar para ARM64, baixe também `deps-windows-arm64.7z`. Extraia esses arquivos para `dep\prebuilt`.
3. Abra o projeto do Visual Studio `duckstation.sln` na raiz (recomendado) ou use “Abrir pasta” para build via CMake (não recomendado/nem suportado).
4. Compile o projeto.
5. Os binários ficam em `bin/x64`.
6. Execute `duckstation-qt-x64-Release.exe` (ou a configuração que você tiver usado).

### Linux

#### Dependências necessárias

Nomes de pacotes no Ubuntu/Debian:
```
autoconf automake build-essential clang cmake curl extra-cmake-modules git libasound2-dev libcurl4-openssl-dev libdbus-1-dev libdecor-0-dev libegl-dev libevdev-dev libfontconfig-dev libfreetype-dev libgtk-3-dev libgudev-1.0-dev libharfbuzz-dev libinput-dev libopengl-dev libpipewire-0.3-dev libpulse-dev libssl-dev libudev-dev libwayland-dev libx11-dev libx11-xcb-dev libxcb1-dev libxcb-composite0-dev libxcb-cursor-dev libxcb-damage0-dev libxcb-glx0-dev libxcb-icccm4-dev libxcb-image0-dev libxcb-keysyms1-dev libxcb-present-dev libxcb-randr0-dev libxcb-render0-dev libxcb-render-util0-dev libxcb-shape0-dev libxcb-shm0-dev libxcb-sync-dev libxcb-util-dev libxcb-xfixes0-dev libxcb-xinput-dev libxcb-xkb-dev libxext-dev libxkbcommon-x11-dev libxrandr-dev libxss-dev libtool lld llvm nasm ninja-build pkg-config zlib1g-dev
```

Nomes de pacotes no Fedora:
```
alsa-lib-devel autoconf automake brotli-devel clang cmake dbus-devel egl-wayland-devel extra-cmake-modules fontconfig-devel gcc-c++ gtk3-devel libavcodec-free-devel libavformat-free-devel libavutil-free-devel libcurl-devel libdecor-devel libevdev-devel libICE-devel libinput-devel libSM-devel libswresample-free-devel libswscale-free-devel libX11-devel libXau-devel libxcb-devel libXcomposite-devel libXcursor-devel libXext-devel libXfixes-devel libXft-devel libXi-devel libxkbcommon-devel libxkbcommon-x11-devel libXpresent-devel libXrandr-devel libXrender-devel libXScrnSaver-devel libtool lld llvm make mesa-libEGL-devel mesa-libGL-devel nasm ninja-build openssl-devel patch pcre2-devel perl-Digest-SHA pipewire-devel pulseaudio-libs-devel systemd-devel wayland-devel xcb-util-cursor-devel xcb-util-devel xcb-util-errors-devel xcb-util-image-devel xcb-util-keysyms-devel xcb-util-renderutil-devel xcb-util-wm-devel xcb-util-xrm-devel zlib-devel
```

#### Compilando

1. Clone o repositório: `git clone https://github.com/stenzek/duckstation.git`, `cd duckstation`.
2. Baixe o pacote de dependências em https://github.com/duckstation/dependencies. Você precisará do arquivo `deps-linux-x64.tar.xz` e das variantes de cross-compile se quiser compilar para ARM. Extraia esses arquivos para `dep\prebuilt`.
3. Execute o CMake para configurar o sistema de build. Considerando um subdiretório de build `build-release`, execute `cmake -B build-release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXE_LINKER_FLAGS_INIT="-fuse-ld=lld" -DCMAKE_MODULE_LINKER_FLAGS_INIT="-fuse-ld=lld" -DCMAKE_SHARED_LINKER_FLAGS_INIT="-fuse-ld=lld" -G Ninja`. Se você quiser uma build de release (otimizada), inclua `-DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`.
4. Compile o código-fonte. No exemplo acima, execute `ninja -C build-release`
5. Execute o binário, localizado no diretório de build em `./build-release/bin/duckstation-qt`.

### macOS

Requisitos:
 - CMake
 - Xcode

1. Clone o repositório: `git clone https://github.com/stenzek/duckstation.git`.
2. Baixe o pacote de dependências em https://github.com/duckstation/dependencies. Você precisará do arquivo `deps-macos-universal.tar.xz`. Extraia os arquivos para `dep\prebuilt`.
3. Execute o CMake para configurar o sistema de build: `cmake -Bbuild-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`.
4. Compile o código-fonte: `cmake --build build-release --parallel`.
5. Execute o binário, localizado no diretório de build em `bin/DuckStation.app`.

## Diretórios de usuário
O “Diretório do usuário” é onde você deve colocar suas imagens de BIOS, onde as configurações são salvas e onde os cartões de memória/save states são salvos por padrão.
Um arquivo opcional de banco de dados de controles de jogo do SDL ([SDL game controller database file](#sdl-game-controller-database)) também pode ser colocado aqui.

Ele fica nos seguintes locais, dependendo da plataforma que você estiver usando:

- Windows: `AppData\Local\DuckStation` (instalações antigas usarão `Documents\DuckStation`).
- Linux: `$XDG_DATA_HOME/duckstation` ou `~/.local/share/duckstation`.
- macOS: `~/Library/Application Support/DuckStation`.

Então, se você estiver usando Linux, deve colocar suas imagens do BIOS em `~/.local/share/duckstation/bios`. Esse diretório será criado quando você executar o DuckStation pela primeira vez.

Se você quiser usar uma build “portátil”, em que o diretório do usuário seja o mesmo local do executável, crie um arquivo vazio chamado `portable.txt` no mesmo diretório do executável do DuckStation.

Há um atalho para abrir o diretório do usuário selecionando `Open Data Directory` no menu `Tools`.

O DuckStation permite substituir certos recursos ao colocar arquivos no subdiretório `resources` do diretório do usuário. Isso inclui imagens e efeitos sonoros (ex.: navegação de menu/desbloqueio de conquista). Consulte https://github.com/stenzek/duckstation/wiki/Resource-Overrides para mais informações.

## Mapeamentos para o frontend Qt
Seu teclado ou controle pode ser usado para simular vários controles de PlayStation. A entrada do controle é suportada pelos backends DInput, XInput e SDL, e pode ser alterada em `Configurações -> Controles`.

Para mapear seu dispositivo de entrada, vá em `Configurações -> Controles` e selecione o controle virtual que você deseja mapear. O mapeamento automático lida com a maioria dos controles. Porém, se você precisar mapear um controle manualmente, clique na caixa abaixo do nome do botão/eixo e pressione a tecla ou botão no seu dispositivo de entrada que você deseja atribuir.

## Banco de dados de controles de jogo SDL
As releases do DuckStation incluem um banco de dados de mapeamentos de controles para o backend de controle SDL, cortesia de https://github.com/mdqinc/SDL_GameControllerDB. O arquivo incluído `gamecontrollerdb.txt` pode ser encontrado no subdiretório `resources` do diretório do programa do DuckStation.

Se você estiver tendo problemas para mapear seu controle com o backend de controle SDL, talvez seja necessário adicionar um mapeamento personalizado ao arquivo do banco de dados. Faça uma cópia de `gamecontrollerdb.txt` e coloque-a no seu [diretório de usuário](#user-directories) (ou diretamente no diretório do programa, se estiver executando em modo portátil) e, em seguida, siga as instruções no [repositório SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) para criar um novo mapeamento. Adicione esse mapeamento à nova cópia de `gamecontrollerdb.txt` e seu controle deverá ser reconhecido corretamente.

## Mapeamentos padrão

Os mapeamentos para controles e teclas de atalho podem ser alterados em `Configurações -> Controles`.

Controle 1:
 - **Analógico esquerdo:** W/A/S/D
 - **Analógico direito:** T/F/G/H
 - **Direcional:** Cima/Esquerda/Baixo/Direita
 - **Triângulo/Quadrado/Círculo/Cruz:** I/J/L/K
 - **L1/R1:** Q/E
 - **L2/R2:** 1/3
 - **L3/R3:** 2/4
 - **Start:** Enter
 - **Select:** Backspace

Teclas de atalho:
 - **Escape:** Abrir menu de pausa
 - **F1:** Carregar estado
 - **F2:** Salvar estado
 - **F3:** Selecionar o estado anterior
 - **F4:** Selecionar o próximo estado
 - **F10:** Salvar captura de tela
 - **F11:** Alternar tela cheia
 - **Tab:** Desativar temporariamente o limitador de velocidade
 - **Espaço:** Pausar/Retomar emulação

## Avisos

Ícone por icons8: https://icons8.com/icon/74847/platforms.undefined.short-title

“PlayStation” e “PSX” são marcas registradas da Sony Interactive Entertainment Europe Limited. Este projeto não é afiliado de forma alguma à Sony Interactive Entertainment.
