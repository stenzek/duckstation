# DuckStation - Mais conhecido como emulador de PlayStation 1
[Últimas noticias](#Últimas-Notícias) | [Caracteristicas](#Características) | [Capturas de tela](#screenshots) | [Como baixar e configurar](#Como-baixar-e-configurar) | [Núcleo libretro](#Núcleo-libretro) | [Construindo](#Baixando) | [Avisos](#avisos)

**Servidor no Discord:** https://discord.gg/Buktv3t

**Versão mais recente para Windows e Linux (AppImage)** https://github.com/stenzek/duckstation/releases/tag/latest

**Disponível na Google Play:** https://play.google.com/store/apps/details?id=com.github.stenzek.duckstation

**Lista de compatibilidade de jogos:** https://docs.google.com/spreadsheets/d/1H66MxViRjjE5f8hOl5RQmF5woS1murio2dsLn14kEqo/edit

**Wiki do projeto:** https://www.duckstation.org/wiki/

DuckStation é um emulador do console da Sony Playstation 1, com foco na jogabilidade, velocidade e manutenção de longo prazo. O objetivo é ser o mais preciso possível, mantendo o desempenho adequado para dispositivos de baixo custo. As funções de 'hacks' (modificações) não são muito recomendadas por serem instáveis na maioria dos casos. as configurações padrão já oferecem suporte aos jogos já categorizados como jogáveis até o momento.

Um arquivo de "BIOS" é obrigátorio para inicar o emulador para que os jogos iniciem. Você pode usar qualquer arquivo de diversas regiões de consoles, embora regiões de jogo diferentes e regiões de BIOS possam ter problemas de compatibilidade entre si. O arquivo de BIOS não pode ser compartilhado junto com o emulador de forma nenhuma por questões legais, você deve fazer o processo chamado de 'dump' ou despejo do arquivo do seu próprio console; usando o Caetla ou outros meios disponíveis.

## Últimas Notícias
Notícias mais antigas estão disponíveis em: https://github.com/stenzek/duckstation/blob/master/NEWS.md

- 2021/07/25: Agora é possível inicializar jogos diretamente do CD-ROM.
- 2021/07/11: Adicionardo porte do emulador para UWP/Xbox. Siga as instruções para a plataforma universal do Windows ou para Xbox One S|X.
- 2021/07/10: Renderizador de hardware Direct3D12 adicionado. Suavização de  imagem ou pós-processamento não incluído (feito tendo em vista mais o uso com Xbox).
- 2021/06/25: Agora é possível recuperar arquivos do editor de cartão de memória adicionado.
- 2021/06/22: Adicionado número de conquistas para RetroAchievements (Ex:.Conquistas 10/25).
- 2021/06/19: Adicionados placares para RetroAchievements.
- 2021/06/01: Auto carregamento / aplicação de modificações do tipo PPF para traduções.
- 2021/05/23: Arquivo de salvamento do tipo SRM adicionado ao núcleo libretro.
- 2021/05/23: Adicionado aceleração de busca do CD-ROM aprimorada.
- 2021/05/16: Adicionado botões de Auto disparo.
- 2021/05/02: Adicionado novo menu de pausa ao aplicativo Android.
- 2021/04/29: Adicionado proporção de aspecto customizado.
- 2021/03/20: Adicionado editor de cartão de memória ao aplicativo Android.
- 2021/03/17: Adicionado para o carregamento de **homebrew** em formato PBP. Jogos em formato PBP oficiais da Sony NÃO SÃO e NÃO SERÃO suportados por riscos legais.
- 2021/03/14: Múltiplos controles adicionados, função multitap, e vibração de controles adicionados ao aplicativo Android. Será necessário nova reatribuição de controles.
- 2021/03/14: RetroAchievements adicionado ao Android.
- 2021/03/03: RetroAchievements agora são suportados. Já é possível se conectar via site retroacheivements.org com sua conta criada lá via Duckstation e assim ganhar pontos com os jogos que tenham essas conquistas. No momento somente para Windows/Linux/Mac, Android terá em breve suporte.
- 2021/03/03: Adicionado Multitap para até 8 controles. Você pode escolher qual das duas portas do controler principal quer usar para a função multitap.
- 2021/03/03: Agora é possível adicionar/remover os botões de controle em tela adicionado ao Android.
- 2021/01/31: Interface de usuário adicionado, também conhecido no Discord como "Big Duck Mode/Modo TV". Esta interface é facilmente navegável com o controle. No momento, só é funcional com o excecutável NOGUI.exe mas em breve será adicionado a opção de atalho para abri-la.
- 2021/01/24: Pulo de quadros - solução de contorno para atrasos em alguns jogos perdendo quadros.
- 2021/01/24: Função rebobinar - agora você pode "retroceder suavemente" (mas não por muito tempo) ou "pular o retrocesso" por algum tempo enquanto joga.

## Características

O DuckStation apresenta um front-end completo construído usando Qt, bem como uma interface de usuário de tela cheia / TV baseada em ImGui. Uma versão do Android está sendo feita, mas ainda não está completa.

<p align="center">
  <img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/main-qt.png" alt="Main Window Screenshot" />
  <img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/bigduck.png" alt="Fullscreen UI Screenshot" />
</p>

Outras características incluem:

 - CPU Recompilador/JIT (x86-64, armv7/AArch32 e também para AArch64)
 - Hardware (D3D11, OpenGL, Vulkan) e renderizador por software
 - Aumento de resolução, filtragem de textura, "função true color" (24-bit) nos renderizadores baseados por hardware (placa de vídeo)
 - PGXP para precisão de geometria, correção de textura, e profundidade de campo
 - Filtro adaptativo de resolução
 - Opções de pós-processamento
 - "Inicialização rápida" para pular a tela de BIOS ou introdução da Sony
 - Suporte para salvamento rápido
 - Windows, Linux, **altamente experimental** suporte MacOS
 - Supporte aos arquivos bin/cue, bin/img, e formato CHD do Mame.
 - Inicialização direta de arquivos homebrew
 - Carregamento de arquivos (psf)
 - Controles analógicos e digitais
 - Controles Namco GunCon lightgun são suportados (simulados com o mouse)
 - Controles NeGcon são suportados
 - Qt e NoGUI para desktop
 - Atualizador automático adicionado
 - Verificação automática de conteúdo - título dos jogos/números validadores fornecidos por redump.org
 - Troca automática opcional de cartões de memória para cada jogo
 - Suporta o carregamento de trapaças no formato libretro ou PCSXR
 - Editor de cartão de memória e de saves
 - Aceleração do CPU emulado
 - Depuração integrada
 - Controle multitap (até 8 no total)
 - RetroAchievements
 - Carregamento de modificações PPF - feito para tradução de jogos do Playstation 1.

## Requisitos do sistema
 - Um processador rápido ou não tão antigo. Mas precisa ter instrução x86_64, AArch32/armv7, ou AArch64/ARMv8, caso contrário você não conseguirá usar de maneira devida nem mesmo com opção recompilador ativada.
 - Para renderizadores por hardware, uma GPU (placa de vídeo) que seja capaz de rodar OpenGL 3.1/ou OpenGL ES 3.0/Direct3D 11 Na versão 10.0 (ou versão Vulkan 1.0) ou mais recente. Sendo assim qualquer coisa feita nos últimos 10 anos ou mais.
 - SDL, XInput ou DInput compatíveis com controle (e.g. XB360/XBOne). DualShock 3 usuários do Windows precisarão instalar os drivers oficiais para o DualShock 3 incluídos com os serviços do Playstation Now.

## Como baixar e configurar
Os arquivos necessários do emulador x64/ARM64, Linux x86_64 (no formato AppImage), e Android ARMv7/ARMv8 estão disponíveis via GitHub lançados e automaticamente adicionados. Pacotes são distribuidos via outras fontes que não são atualizadas ou não gerenciadas pelo desenvolvedor, se este for seu caso qualquer problema fale com quem os distribui mas não conosco.

### Windows

**Windows 10 é o único sistema operacional suportado pelo desenvolvedor.** Windows 7/8 pode funcionar, mas não suportado. Estou ciente de que usuários ainda utilizam (Windows 7), mas não é sequer suportado pela própria Microsoft e requer muito esforço para manter atualizado corretamente.É bem improvável que problemas nos jogos sejam afetados pelo sistema operacional, no entanto, os problemas de desempenho devem ser verificados no Windows 10 também antes de relatar.

Baixando:
 - Vá até https://github.com/stenzek/duckstation/releases/tag/latest, e baixe a versão Windows x64. Este é o arquivo compactado contendo os arquivos necessários.
 - Alternativamente, use o link para download direto: https://github.com/stenzek/duckstation/releases/download/latest/duckstation-windows-x64-release.zip
 - Extraia o arquivo **dentro de uma pasta**. O arquvivo baixado não contém uma pasta criada por isso é necessária a criação e/ou descompactação dos arquivos para que não sejam extraídos diretamente onde foi baixado deixando vários arquivos misturados com os seus.

Uma vez baixado e extraído, você será capaz de executar o emulador clicando no seguinte arquivo `duckstation-qt-x64-ReleaseLTCG.exe`.
Configurando:
1. Configure o caminho para uma imagem BIOS nas configurações, ou copie uma ou mais imagens do BIOS do PlayStation para o subdiretório / bios. No Windows, por padrão, ele estará localizado em `C:\Users\SEU_USUÁRIO\Documentos\DuckStation\bios`. Se você não quiser usar o diretório Documentos para salvar o BIOS / cartões de memória / etc, você pode usar o modo portátil. Veja em [Diretório do usuário](#Diretório-do-usuário).
2. Caso esteja usando a versão QT, adicione os diretórios contendo seus jogos clicando em `Configurações-> Configurar lista de jogos -> Adicionar diretório`.
2. Selecione um jogo da lista, ou abra a imagem do jogo diretamente.

**Caso veja o seguinte erro `vcruntime140_1.dll` não foi encontrado, será necessário atualizar o seu Visual C++.** você pode fazê-lo nesta página: https://support.microsoft.com/en-au/help/2977003/the-latest-supported-visual-c-downloads. Mais precisamente, o pacote x64, ao qual pode ser baixado em: https://aka.ms/vs/16/release/vc_redist.x64.exe.

**Usuários do Windows 7, TLS 1.2 não é supprtado nativamente no sistema sendo assim, não será possível usar o atualizador automático ou a função para conexão ao RetroAchievements.** Este artigo de base de conhecimento contém instruções para ativar a função TLS 1.1/1.2: 
https://support.microsoft.com/en-us/topic/update-to-enable-tls-1-1-and-tls-1-2-as-default-secure-protocols-in-winhttp-in-windows-c4bd73d2-31d7-761e-0178-11268bb10392

A versão QT inclui um analisador de atualizações automáticas. Versões baixadas depois 07/08/2020 seraão automaticamente verificadas por atualizações cada vez que o emulador é iniciado, esta função pode ser desligada nas configurações. Como alternativa, é possível também checar manualmente no menu `Ajuda -> Checar por atualizações`.

### Plataforma Universal Windows (UWP) / Xbox One

A versão do Duckstation em modo tela cheia está disponível para Xbox one e UWP.

Usando no Xbox One:

1. Certifique-se de que seu console esteja no modo de desenvolvedor. Você precisará adquirir uma licença de desenvolvedor da Microsoft.
2. Baixe a seguinte versão do emulador: duckstation-uwp.appx.
3. Navegue até o portal do dispositivo no seu console (conforme exibido na tela inicial).
4. Instale o arquivo appx clicando em adicionar na página principal.
5. Defina o aplicativo para o modo Jogo ao invés do modo aplicativo: role a lista para baixo e procure por Duckstation, vá até a opção 'modo de visualização', selecione exibir detalhes e altere para Jogo. 
6. Carregue uma imagem do BIOS para o diretório local do Duckstation ou coloque uma imagem do BIOS em uma unidade USB removível. Se estiver usando uma unidade USB, você precisará definir o caminho do BIOS nas configurações do DuckStation apontando para este diretório.
7. Adicione jogos ao diretório de jogos local ou use uma unidade USB removível. Novamente, você terá que registrar este caminho nas configurações da lista de jogos para que ele possa ser lido.
8. Abra o aplicativo e divirta-se. Por padrão, o botão 'Exibir` irá abrir o menu rápido.
9. Não se esqueça de habilitar os aprimoramentos, um Xbox One S pode aumentar a resolução de 8x com saída de vídeo em até 4K, os consoles da série S podem até ir além disto.

**NOTA:** Recomendaria usar uma unidade USB para salvar os cartões de memória, pois o diretório de local será removido caso você desinstale o aplicativo.

### Linux

#### Binários

Versões contruídas para Linux estão disponíveis em formato AppImage. No entanto, esses arquivos podem não ser compatíveis com distribuições mais antigas do Linux (ex:. Ubuntu anteriores do que 18.04.4 LTS) devido a distros mais antigas não fornecerem bibliotecas padrão C / C ++ exigidas pelos binários AppImage.

**Também é possível construir da fonte para usuários mais avançados de Linux.**

Baixando em:
 - Vá até: https://github.com/stenzek/duckstation/releases/tag/latest, e baixe também `duckstation-qt-x64.AppImage` ou `duckstation-nogui-x64.AppImage` para a versão desejada.
 - Rode o comando `chmod a+x` para o arquivo baixado AppImage -- seguindo este passo, o arquivo AppImage se tornará um arquvio executável pronto para uso.
 - Outra opção seria usar [appimaged](https://github.com/AppImage/appimaged) ou [AppImageLauncher](https://github.com/TheAssassin/AppImageLauncher) para integração com sistema. [AppImageUpdate](https://github.com/AppImage/AppImageUpdate) pode ser usado junto com o appimage para atualizar facilmente o seu DuckStation na versão appimage.

### macOS

Versões para MacOS não são mais disponibilizadas, não tenho o hardware citado, e não vou de forma alguma gastar $1000 do meu bolso em um harware somente para desenvolver o emulador.

Ainda é possível construir da fonte [source](#Construindo), mas você mesmo terá de fazer depuração de probelmas encontrados por si mesmo.

Se alguém estiver disposto a se voluntariar para oferecer suporte à plataforma para garantir que os usuários tenham uma boa experiência, estou mais do que feliz em reativar os lançamentos.

### Android

APK construído disponível para Android. Será necessário um dispositivo com armv7 (32-bit ARM), AArch64 (64-bit ARM), ou x86_64 (64-bit x86). preferivelmente 64-bits, os requisitos são maiores para 32 bits, necessário no minímo um disposiivo com 1.5GHz.

Link para baixar: https://www.duckstation.org/android/duckstation-android.apk

Versão preview/beta: https://www.duckstation.org/android/duckstation-android-beta.apk

Registro de mudanças: https://www.duckstation.org/android/changelog.txt

Usando:
1. Instale e rode o aplicativo pelo menos uma vez.
2. Adicione o diretório de jogos clicando no botão adicionar "+" e escolha um diretório. Você pode adicionar diretórios a mais escolhendo a opção "Editar diretório de jogos"a partir do menu.
3. Clique em um jogo para iniciar. Quando você iniciar um jogo pela primeira vez lhe será solicitado que um arquivo de BIOS seja importado.

Se você tiver um controle externo, será necessário mapear os botões e eixos nas configurações.

### Núcleo libretro

DuckStation está disponível como núcleo libretro, ao qual pode ser carregado em um front-end tal qual o Retroarch. Ele suporta a maioria dos recursos do front-end QT, dentro das restrições e limitações de ser um núcleo libretro.

O núcleo libretro não está coberto pela licença de código aberto GPL, ainda assim é totalmente grátuito.
A única restrição é o USO E DISTRIBUIÇÃO COMERCIAL QUE É PROIBIDA. Baixanxo o núcleo libretro, você concorda que não irá distribuir ou vender junto com nenhuma aplicação paga, como applicações, serviços, ou produtos como os video-games embarcardos, raspberry pies e afins.

O núcleo é mantido por terceiros e não é fornecido como parte da versão do GitHub. Você pode baixar o núcleo por meio do atualizador ou dentro do próprio Retroarch ou ainda nos links abaixo. O registro de mudanças pode ser lido em: https://www.duckstation.org/libretro/changelog.txt

- Windows x64 (64-bit): https://www.duckstation.org/libretro/duckstation_libretro_windows_x64.zip
- Android AArch64 (64-bit): https://www.duckstation.org/libretro/duckstation_libretro_android_aarch64.zip
- Android armv7 (32-bit): https://www.duckstation.org/libretro/duckstation_libretro_android_armv7.zip
- Linux x64 (64-bit): https://www.duckstation.org/libretro/duckstation_libretro_linux_x64.zip
- Linux AArch64 (64-bit): https://www.duckstation.org/libretro/duckstation_libretro_linux_aarch64.zip
- Linux armv7 (32-bit): https://www.duckstation.org/libretro/duckstation_libretro_linux_armv7.zip

### Detecção de região e imagens BIOS
Por padrão, o DuckStation irá emular a verificação de região presente no controlador de CD-ROM do console. Isto significa que quando a versão do console não bater com a do disco, o mesmo não vai iniciar, apresentando a seguinte mensagem "Please insert PlayStation CD-ROM". DuckStation suporta detecção automática das regiões dos discos, e se você definir a região do console para detecção automática também, isso nunca será um problema.

Se você deseja usar a detecção automática, você não precisa mudar o caminho do BIOS cada vez que mudar de região. Basta somente colocar as outras imagens do BIOS de outras regiões **no mesmo diretório** que das imagens configuradas anteriormente. Terá que sempre ser na pasta `bios/`. Em seguida, defina a região do console para "Detectar região automaticamente" e tudo deve funcionar corretamente. O registro lhe informará se está faltando a imagem da região dos discos.

Alguns usuários ficaram confusos com a opção "Caminho do BIOS", a razão de ser um caminho ao invés de um diretório é para que uma revisão desconhecida do BIOS possa ser usada / testada.

Como alternativa, a verificação da região pode ser desligada na guia de opções do console. Esta é a única maneira de jogar jogos não licenciados ou homebrews que não fornecem uma linha de região correta no disco, além de usar a inicialização rápida que ignora a verificação completamente.

A verificação de discos incompativeis é suportada, mas pode interromper os jogos se eles estiverem sendo lidos pelo BIOS e esperando algum conteúdo específico.

### Proteção LibCrypt e arquivos SBI

Vários jogos de região Européia usam proteção LibCrypt, exigindo informações adicionais do subcanal do CD para funcionar corretamente. Quando o libcrypt não funciona corretamente, é notável por travamentos, carregaemntos infinitos, mas às vezes também afetando a jogabilidade, dependendo de como o jogo o implementou.

Para esses jogos, certifique-se de que a imagem do CD e seu respectivo arquivo SBI (.sbi) correspondente tenham o mesmo nome e sejam colocados no mesmo diretório. DuckStation irá carregar automaticamente o arquivo SBI quando ele for encontrado próximo à imagem do CD.

Por exemplo, se a imagem do seu disco foi nomeada `Spyro3.cue`, você deve colocar o arquivo SBI no mesmo diretório, nomeando o meso assim: `Spyro3.sbi`.

## Construindo

### Windows
Requisitos:
 - Visual Studio 2019
 
1. Clone o repositório com submódulos (`git clone --recursive https://github.com/stenzek/duckstation.git -b dev`).
2. Abra o Visual Studio pelo arquivo `duckstation.sln` na raiz da pasta, ou "Abrir pasta" para compilar via cmake.
3. Construia a solução.
4. Os arquivos compilados estão localizados em `bin/x64`.
5. Execute `duckstation-qt-x64-Release.exe` ou qualquer configuração que tenha usado.

### Linux
Requisitos (Debian/Ubuntu):
 - CMake (`cmake`)
 - SDL2 (`libsdl2-dev`, `libxrandr-dev`)
 - pkgconfig (`pkg-config`)
 - Qt 5 (`qtbase5-dev`, `qtbase5-private-dev`, `qtbase5-dev-tools`, `qttools5-dev`)
 - libevdev (`libevdev-dev`)
 - git (`git`) (Nota: necessário para clonar o repositório e em tempo de construção)
 - Quando o wayland está ativado (padrão): `libwayland-dev` `libwayland-egl-backend-dev` `extra-cmake-modules`
 - Opcional para RetroAchievements (ativado por padrão): libcurl (`libcurl4-gnutls-dev`)
 - Opcional para saída do framebuffer: DRM/GBM (`libgbm-dev`, `libdrm-dev`)
 - Opcional para construção rápida: Ninja (`ninja-build`)

1. Clone o repositório. Submódulos não são necessários, há apenas um e é usado apenas para Windows     (`git clone https://github.com/stenzek/duckstation.git -b dev`).
2. Crie um diretório de construção, em árvore ou em outro lugar.
3. Execute cmake para configurar o sistema de compilação. Tendo em vista o subdiretório `build-release`, `cd build-release && cmake -DCMAKE_BUILD_TYPE=Release -GNinja ..`.
4. Compile o código-fonte. Conforme informado acima, execute `ninja`.
5. Execute o executável, localizado no diretório de compilação em `bin/duckstation-qt`.

### macOS
**NOTE:** macOS é altamente experimental e não foi testado pelo desenvolvedor. Use por sua própria conta e risco, pode ser que quase nada funcione corretamente.

Requisitos:
 - CMake (instalado por padrão? caso contrário, `brew install cmake`)
 - SDL2 (`brew install sdl2`)
 - Qt 5 (`brew install qt5`)

1. Clone o repositório. Submódulos não são necessários, há apenas um e é usado apenas para Windows    (`git clone https://github.com/stenzek/duckstation.git -b dev`).
2. Clone o repositório externo do mac (autor: MoltenVK): `git clone https://github.com/stenzek/duckstation-ext-mac.git dep/mac`.
2. Crie um diretório de construção, diretório em árvore ou em outro lugar, ex:. `mkdir build-release`, `cd build-release`.
3. Execute cmake para configurar o sistema de compilação: `cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_NOGUI_FRONTEND=OFF -DBUILD_QT_FRONTEND=ON -DUSE_SDL2=ON -DQt5_DIR=/usr/local/opt/qt@5/lib/cmake/Qt5 ..`. Será necessário ajustar `Qt5_DIR` dependendo do sistema.
4. Compile o código fonte: `make`. Use `make -jN` onde `N` é o número de núcleos de CPU em seu sistema para uma construção mais rápida.
5. Execute o arquivo, localizado no diretório criado `bin/DuckStation.app`.

## Diretório do usuário
O "diretório do usuário" é onde você deve colocar as imagens do BIOS, onde as configurações são salvas e os cartões de memória / estados salvos são salvos por padrão.
Uma opção [Banco de dados de controle de jogo SDL](#Banco-de-dados-de-controle-de-jogo-SDL) pode ser acrescentada ai.

Ele está localizado nos seguintes locais, dependendo da plataforma que você está usando:

- Windows: Meus documentos\DuckStation
- Linux: `$XDG_DATA_HOME/duckstation`, ou `~/.local/share/duckstation`.
- macOS: `~/Library/Application Support/DuckStation`.

Então, se você estivesse usando Linux, você colocaria suas imagens de BIOS em `~/.local/share/duckstation/bios`. Este diretório será criado ao executar o DuckStation pela primeira vez.

Se você deseja usar uma versão "portátil", onde o diretório do usuário é o mesmo de onde o executável está localizado, crie um arquivo vazio chamado `portable.txt` no mesmo diretório que o executável do DuckStation.

## Atribuições via interface QT
Seu teclado ou controle pode ser usado para simular uma variedade de controles de PlayStation. A entrada do controle é suportada por meio da opção XInput, DInput e SDL e pode ser alterada da seguinte forma: `Configurações -> Configurações Gerais`.

Para atribuir o seu dispositivo, vá até `Configurações -> Configurações de controles`. Cada um dos botões / eixos do controle simulado será listado, correspondente no dispositivo ao qual está vinculado no momento. Para religar, clique na caixa ao lado do botão / nome do eixo e pressione a tecla ou botão no dispositivo de entrada que deseja vincular. Para vincular o 'rumble' vibração, basta pressionar qualquer botão no controle para o qual deseja atribuir a função rumble ou vibração.

## Banco de dados de controle de jogo SDL
As versões do DuckStation vêm com um banco de dados de mapeamentos de controles com a opção de controle SDL, cortesia de: https://github.com/gabomdq/SDL_GameControllerDB. O arquivo `gamecontrollerdb.txt` pode ser encontrado na pasta `database` dentro do diretório do emulador.

Se você estiver tendo problemas para vincular seu controle com a opção SDL, pode ser necessário adicionar um vínculo personalizado ao arquivo de banco de dados. Faça uma cópia do `gamecontrollerdb.txt` e coloque em seu [diretório de usuário](#Diretório-do-usuário) (ou diretamente no diretório do programa, se estiver executando em modo portátil) e siga as instruções em [SDL_GameControllerDB repositório](https://github.com/gabomdq/SDL_GameControllerDB) para criar um novo mapeamento. Adicione a entrada para este mapeamento em `gamecontrollerdb.txt` e seu controle deverá ser reconhecido corretamente.

## Atribuições padrão
Controle 1:
 - **D-Pad:** W/A/S/D
 - **Triangulo/Quadrado/Círculo/Cruz:** Numpad8/Numpad4/Numpad6/Numpad2
 - **L1/R1:** Q/E
 - **L2/R2:** 1/3
 - **Start:** Enter
 - **Select:** Backspace

Atalhos:
 - **Esc:** Desliga o console
 - **ALT+ENTER:** Alterna modo tela-cheia
 - **Tab:** Desativar temporariamente o limitador de velocidade
 - **Barra de espaço:** Pausa/resume a emulação
 
## Tests
 - Passes amidog's CPU and GTE tests in both interpreter and recompiler modes, partial passing of CPX tests

## Capturas de tela
<p align="center">
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/monkey.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/monkey.jpg" alt="Monkey Hero" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/rrt4.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/rrt4.jpg" alt="Ridge Racer Type 4" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/tr2.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/tr2.jpg" alt="Tomb Raider 2" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/quake2.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/quake2.jpg" alt="Quake 2" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/croc.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/croc.jpg" alt="Croc" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/croc2.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/croc2.jpg" alt="Croc 2" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/ff7.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/ff7.jpg" alt="Final Fantasy 7" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/mm8.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/mm8.jpg" alt="Mega Man 8" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/ff8.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/ff8.jpg" alt="Final Fantasy 8 in Fullscreen UI" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/spyro.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/spyro.jpg" alt="Spyro in Fullscreen UI" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/tof.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/tof.jpg" alt="Threads of Fate in Fullscreen UI" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/gamegrid.png"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/gamegrid.png" alt="Game Grid" width="400" /></a>
</p>

## Avisos

Ícone feito icons8: https://icons8.com/icon/74847/platforms.undefined.short-title

"PlayStation" e "PSX" são marcas registradas da Sony Interactive Entertainment Europe Limited. 
Este projeto não é afiliado de forma alguma com a Sony Interactive Entertainment.
