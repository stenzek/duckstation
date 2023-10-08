Tradução:

# DuckStation - Emulador de PlayStation 1, também conhecido como PSX
[Últimas Notícias](#latest-news) | [Recursos](#features) | [Download e Execução](#downloading-and-running) | [Compilação](#building) | [Avisos Legais](#disclaimers)

**Últimas Versões para Windows 10/11, Linux (AppImage/Flatpak) e macOS:** https://github.com/stenzek/duckstation/releases/tag/latest

**Lista de Compatibilidade de Jogos:** https://docs.google.com/spreadsheets/d/1H66MxViRjjE5f8hOl5RQmF5woS1murio2dsLn14kEqo/edit

**Wiki:** https://www.duckstation.org/wiki/

DuckStation é um simulador/emulador do console Sony PlayStation(TM), focando na jogabilidade, velocidade e manutenção a longo prazo. O objetivo é ser o mais preciso possível, mantendo um desempenho adequado para dispositivos de baixo desempenho. Opções de "hack" não são recomendadas, a configuração padrão deve suportar todos os jogos jogáveis, com apenas algumas das melhorias tendo problemas de compatibilidade.

Uma imagem ROM do "BIOS" é necessária para iniciar o emulador e jogar os jogos. Você pode usar uma imagem de qualquer versão de hardware ou região, embora regiões de jogos e regiões de BIOS que não idênticas podem resultar em problemas de compatibilidade. A imagem ROM ou Jogo não é fornecida com o emulador por motivos legais; você deve obter do seu próprio console usando Caetla ou outros meios.

## Recursos

O DuckStation possui uma interface totalmente funcional construída usando Qt, bem como uma interface de tela cheia/TV baseada no Dear ImGui.

<p align="center">
  <img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/main-qt.png" alt="Captura de Tela da Janela Principal" />
  <img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/bigduck.png" alt="Captura de Tela da Interface de Tela Cheia" />
</p>

Outros recursos incluem:

 - Recompilador de CPU/JIT (x86-64, armv7/AArch32 e AArch64).
 - Renderização de hardware (D3D11, D3D12, OpenGL, Vulkan, Metal) e renderização de software.
 - Ampliação, filtragem de textura e cor verdadeira (24 bits) nos renderizadores de hardware.
 - PGXP para precisão de geometria, correção de textura e emulação de buffer de profundidade.
 - Filtro de downsampling adaptativo.
 - Cadeias de shaders de pós-processamento (GLSL e Reshade FX experimental).
 - "Inicialização rápida" para pular a tela de abertura/intro do BIOS.
 - Suporte a salvar estados.
 - Suporte para Windows, Linux e macOS.
 - Suporta imagens bin/cue, arquivos bin/img crus, MAME CHD, ECM de única faixa, MDS/MDF e formatos PBP não criptografados.
 - Inicialização direta de executáveis homebrew.
 - Carregamento direto de arquivos Portable Sound Format (psf).
 - Controles digitais e analógicos.
 - Suporte ao lightgun Namco GunCon (simulado com o mouse).
 - Suporte ao NeGcon.
 - Interface Qt e "Big Picture".
 - Atualizações automáticas a partir dos canais oficiais.
 - Verificação automática de conteúdo - os títulos/jogos são fornecidos por redump.org.
 - Troca automática opcional de cartões de memória para cada jogo.
 - Suporta carregar trapaças de listas existentes.
 - Editor de cartões de memória e importador de salvamento.
 - Overclock emulado de CPU.
 - Depuração integrada e remota.
 - Controles multitap (até 8 dispositivos).
 - RetroAchievements.
 - Carregamento/aplicação automática de patches PPF.

## Requisitos do Sistema
 - Um CPU rápido. Mas precisa ser x86_64, AArch32/armv7 ou AArch64/ARMv8, caso contrário, o recompilação será lenta.
 - Para os renderizadores de hardware, é necessário uma GPU compatível com OpenGL 3.1/OpenGL ES 3.1/Direct3D 11 Feature Level 10.0 (ou Vulkan 1.0) e superior. basicamente, qualquer computador produzido nos últimos 10 anos mais ou menos deve dar conta.
 - Controlador de jogo compatível com SDL, XInput ou DInput (por exemplo, XB360/XBOne/XBSeries). Usuários de DualShock 3 no Windows precisarão instalar os drivers oficiais do DualShock 3 incluídos como parte do PlayStation Now.

## Download e Execução
Executáveis do DuckStation para Windows x64/ARM64, Linux x86_64 (nos formatos AppImage/Flatpak) e para macOS estão disponíveis via GitHub na aba Releases e são automaticamente compilados a cada commit/envio. Executáveis ou pacotes distribuídos por outras fontes podem estar desatualizados e não são suportados pelo desenvolvedor, por favor, entre em contato com eles para obter suporte, não conosco.

### Windows

DuckStation **requer** Windows 10/11, especificamente a versão 1809 ou mais recente. Se você ainda estiver usando Windows 7/8/8.1, o DuckStation **não funcionará** no seu sistema operacional. Usar esses sistemas operacionais em 2023 deve ser considerado um risco de segurança, recomendaria atualizar para algo que receba suporte do fornecedor.
Se você precisa usar um sistema operacional mais antigo, [v0.1-5624](https://github.com/stenzek/duckstation/releases/tag/v0.1-5624) é a última versão que funcionará. Mas não espere receber nenhuma assistência, essas compilações não são mais suportadas.

Para baixar:
 - Acesse https://github.com/stenzek/duckstation/releases/tag/latest e baixe a compilação do Windows x64. Este é um arquivo ZIP contendo o executável pré-compilado.
 - Alternativamente, link de download direto: https://github.com/stenzek/duckstation/releases/download/latest/duckstation-windows-x64-release.zip
 - Extraia o arquivo ZIP **para uma pasta**. O arquivo ZIP não tem um subdiretório raiz, então, se você não extrair para um subdiretório, ele irá despejar vários arquivos no seu diretório de download.

Depois de baixado e extraído, pode iniciar o emulador com `duckstation-qt-x64-ReleaseLTCG.exe`. Siga o Assistente de configuração para começar.

**Se você receber um erro sobre a falta de `vcruntime140_1.dll`, precisará atualizar sua runtime do Visual C++.**faça da seguinte forma, nesta página: https://support.microsoft.com/en-au/help/2977003/the-latest-supported-visual-c-downloads. Especificamente, você deseja a runtime x64, que pode ser baixada em https://aka.ms/vs/17/release/vc_redist.x64.exe.

### Linux

As únicas versões suportadas do DuckStation para Linux são o AppImage e o Flatpak na página de lançamentos. Se você instalou o DuckStation de outra fonte ou distribuição (por exemplo, EmuDeck), você deve entrar em contato com o responsável para suporte, nós não temos controle sobre isso.

#### AppImage

Os AppImages requerem uma distribuição equivalente ao Ubuntu 22.04 ou mais recente para serem executados.

 - Acesse https://github.com/stenzek/duckstation/releases/tag/latest e baixe `duckstation-x64.AppImage`.
 - Execute `chmod a+x` no AppImage baixado -- após este passo, o AppImage pode ser executado como um executável típico.

#### Flatpak

 - Acesse https://github.com/stenzek/duckstation/releases/tag/latest e baixe `duckstation-x64.flatpak`.
 - Execute `flatpak install ./duckstation-x64.flatpak`.

ou, se você tiver o FlatHub configurado:
 - Execute `flatpak install org.duckstation.DuckStation`.

Use `flatpak run org.duckstation.DuckStation` para iniciar, ou selecione `DuckStation` no lançador do seu ambiente de desktop. Siga o Assistente de Configuração para começar.
 
### macOS

São fornecidas compilações universais do MacOS para x64 e ARM64 (Apple Silicon).

MacOS Big Sir (11.0) é necessário, pois também é o requisito mínimo para o Qt.

Para baixar:
 - Acesse https://github.com/stenzek/duckstation/releases/tag/latest e baixe `duckstation-mac-release.zip`.
 - Extraia o arquivo ZIP dando um duplo clique nele.
 - Abra o DuckStation.app, opcionalmente movendo-o para a localização desejada antes.
 - Dependendo da configuração do GateKeeper, você pode precisar clicar com o botão direito -> Abrir na primeira vez que executá-lo, já que certificados de assinatura de código estão fora de questão para um projeto que não gera receita alguma.
 
### Android

Você precisará de um dispositivo com armv7 (32 bits ARM), AArch64 (64 bits ARM) ou x86_64 (64 bits x86). 64 bits são preferíveis, os requisitos são mais altos para 32 bits, você provavelmente vai querer pelo menos um CPU de 1,5 GHz.

A distribuição pelo Google Play é o mecanismo de distribuição recomendado e resultará em tamanhos de download menores: https://play.google.com/store/apps/details?id=com.github.stenzek.duckstation

**Não é fornecido suporte para o aplicativo Android**, ele é gratuito e suas expectativas devem estar alinhadas com isso. Por favor, **não** me envie e-mails sobre problemas relacionados a ele, eles serão ignorados.

Se você precisar usar um APK, os links para download estão listados em https://www.duckstation.org/android/

Para usar:
1. Instale e execute o aplicativo pela primeira vez.
2. Adicione diretórios de jogos tocando no botão de adição e selecionando um diretório. Você pode adicionar diretórios adicionais depois selecionando "Editar Diretórios de Jogos" no menu.
3. Toque em um jogo para começar. Quando você inicia um jogo pela primeira vez, ele pedirá para importar uma imagem de BIOS.

Se você tiver um controle externo, precisará mapear os botões e analogicos nas configurações.

### Proteção LibCrypt e arquivos SBI

Alguns jogos da região PAL usam a proteção LibCrypt, que requer informações adicionais de subcanal de CD para funcionar corretamente. O não funcionamento do libcrypt geralmente se manifesta como travamentos, mas às vezes pode afetar a jogabilidade, dependendo de como o jogo o implementou.

Para esses jogos, certifique-se de que a imagem do CD e seu arquivo correspondente SBI (.sbi) tenham o mesmo nome e estejam na mesma pasta. O DuckStation carregará automaticamente o arquivo SBI quando ele for encontrado ao lado da imagem do CD.

Por exemplo, se sua imagem de disco se chamasse `Spyro3.cue`, você colocaria o arquivo SBI na mesma pasta e o nomearia como `Spyro3.sbi`.

## Compilação

### Windows
Requisitos:
 - Visual Studio 2022

1. Clone o repositório: `git clone https://github.com/stenzek/duckstation.git`.
2. Baixe o pacote de dependências em https://github.com/stenzek/duckstation-ext-qt-minimal/releases/download/latest/deps-x64.7z e extraia-o para `dep\msvc`.
3. Abra a solução do Visual Studio `duckstation.sln` na raiz ou "Abrir Pasta" para a compilação com CMake.
4. Compile a solução.
5. Os binários estão localizados em `bin/x64`.
6. Execute `duckstation-qt-x64-Release.exe` ou a configuração que você usou.

### Linux
Requisitos (nomes de pacotes Debian/Ubuntu):
 - CMake (`cmake`)
 - SDL2 (pelo menos a versão 2.28.2) (`libsdl2-dev` `libxrandr-dev`)
 - pkgconfig (`pkg-config`)
 - Qt 6 (pelo menos a versão 6.5.1) (`qt6-base-dev` `qt6-base-private-dev` `qt6-base-dev-tools` `qt6-tools-dev` `libqt6svg6`)
 - git (`git`) (Nota: necessário para clonar o repositório e na hora da compilação)
 - Quando o Wayland estiver habilitado (padrão): (`libwayland-dev` `libwayland-egl-backend-dev` `extra-cmake-modules` `qt6-wayland`)
 - libcurl (`libcurl4-openssl-dev`)
 - Opcional para compilação mais rápida: Ninja (`ninja-build`)

1. Clone o repositório: `git clone https://github.com/stenzek/duckstation.git -b dev`.
2. Crie um diretório de compilação, seja dentro ou fora do diretório de origem.
3. Execute o CMake para configurar o sistema de compilação. Supondo que o diretório de compilação seja `build-release`, execute `cmake -Bbuild-release -DCMAKE_BUILD_TYPE=Release`. Se você tiver o Ninja instalado, adicione `-GNinja` ao final da linha de comando do CMake para compilações mais rápidas.
4. Compile o código-fonte. Para o exemplo acima, execute `cmake --build build-release --parallel`.
5. Execute o binário, que está localizado no diretório de compilação em `bin/duckstation-qt`.

### macOS

Requisitos:
 - CMake
 - SDL2 (pelo menos a versão 2.28.2)
 - Qt 6 (pelo menos a versão 6.5.1)

Opcional (recomendado para compilações mais rápidas):
 - Ninja

1. Clone o repositório: `git clone https://github.com/stenzek/duckstation.git`.
2. Execute o CMake para configurar o sistema de compilação: `cmake -Bbuild-release -DCMAKE_BUILD_TYPE=Release`. Você pode precisar especificar `-DQt6_DIR` dependendo do seu sistema. Se você tiver o Ninja instalado, adicione `-GNinja` ao final da linha de comando do CMake para compilações mais rápidas.
4. Compile o código-fonte: `cmake --build build-release --parallel`.
5. Execute o binário, que está localizado no diretório de compilação em `bin/DuckStation.app`.

## Diretórios de Usuários
O "Diretório de Usuário" é onde você deve colocar suas imagens da BIOS, onde as configurações são salvas e onde os cartões de memória e estados de salvamento são salvos por padrão. Um [arquivo opcional de banco de dados de controle de jogo SDL](#sdl-game-controller-database) também pode ser colocado aqui.

Ele está localizado nos seguintes lugares, dependendo da plataforma que você está usando:

- Windows: Meus Documentos\DuckStation
- Linux: `$XDG_DATA_HOME/duckstation`, ou `~/.local/share/duckstation`.
- macOS: `~/Library/Application Support/DuckStation`.

Portanto, se você estiver usando o Linux, sugiro colocar suas imagens do BIOS em `~/.local/share/duckstation/bios`. Este diretório será criado na primeira vez que você executar o DuckStation.

Se você deseja usar uma compilação "portátil", onde o diretório do usuário é o mesmo onde o executável está localizado, crie um arquivo vazio chamado `portable.txt` no mesmo diretório onde o executável do DuckStation está.

## Associações para a interface Qt
Seu teclado ou controle podem ser usados para simular uma variedade de controles de PlayStation. A entrada do controle é suportada através dos back-ends DInput, XInput e SDL e pode ser alterada em `Configurações -> Configurações Gerais`.

Para atribuir seu dispositivo de entrada, vá para `Configurações -> Configurações do Controle`. Cada um dos botões/eixos para o controle emulador será listado, juntamente com a tecla/botão correspondente do seu dispositivo a que ele atualmente em uso. Para atribuir novamente, clique na caixa ao lado do nome do botão/eixo e pressione a tecla ou botão do seu dispositivo de entrada que deseja atribuir. Ao atribuir a vibração, basta pressionar qualquer botão no controle para o qual você deseja que seja configurado.

## Banco de Dados de Controle de Jogo SDL
Os lançamentos do DuckStation incluem um banco de dados de mapeamentos de controle de jogo para o back-end do controle SDL, cortesia de https://github.com/gabomdq/SDL_GameControllerDB. O arquivo `gamecontrollerdb.txt` incluído pode ser encontrado no subdiretório `database` do diretório do programa DuckStation.

Se você estiver tendo problemas para associar seu controle com o back-end do controlador SDL, pode ser necessário adicionar um mapeamento personalizado ao arquivo de banco de dados. Faça uma cópia de `gamecontrollerdb.txt` e coloque-o no seu [diretório de usuário](#user-directories) (ou diretamente no diretório do programa, se estiver executando em modo portátil) e siga as instruções no [repositório SDL_GameControllerDB](https://github.com/gabomdq/SDL_GameControllerDB) para criar um novo mapeamento. Adicione este mapeamento à nova cópia de `gamecontrollerdb.txt` e seu controle deve ser reconhecido corretamente.

## Atribuições padrão
Controle 1:
 - **D-Pad:** W/A/S/D
 - **Triângulo/Quadrado/Círculo/Cruz:** Numpad8/Numpad4/Numpad6/Numpad2
 - **L1/R1:** Q/E
 - **L2/R2:** 1/3
 - **Start:** Enter
 - **Select:** Backspace

Atalhos:
 - **Esc:** Abrir Menu de Pausa
 - **F11:** Alternar Tela Cheia
 - **Tab:** Desativar Temporariamente o Limitador de Velocidade
 - **Espaço:** Pausar/Continuar Emulação

## Avisos Legais

Ícone por icons8: https://icons8.com/icon/74847/platforms.undefined.short-title

"PlayStation" e "PSX" são marcas registradas da Sony Interactive Entertainment Europe Limited. Este projeto não está afiliado de forma alguma com a Sony Interactive Entertainment.
