# DS3 Coop Mod

Mod para **Dark Souls III 1.15.2** feito para jogar junto com o Seamless Co-op do LukeYui (Nexus Mods).

- **Insert abre um menu protegido por senha.** Depois da senha aparece uma tabela de itens separada por categoria (armas, escudos, arcos e bestas, flechas, catalisadores, magias, sets de armadura, anéis, consumíveis, melhorias, almas, itens-chave e mais). Clicar num item coloca ele **só no seu inventário**.
- Armas podem vir com **nível de melhoria (+0 a +10)** e **infusão**. O mod confere nas tabelas do jogo se a combinação existe e corrige sozinho (armas únicas param no +5, armas que não aceitam infusão vêm normais).
- **Sets de armadura** inteiros com um clique, ou peça por peça.
- Busca em todas as categorias de uma vez.
- Enquanto o menu está aberto, o personagem não recebe teclado nem mouse.

- **Launcher para o grupo:** ele baixa sozinho a versão mais nova do mod, então todo mundo joga sempre na mesma versão.

O contador de mortes em cima da cabeça e o bloqueio de invasões vêm do próprio Seamless Co-op, e o launcher configura isso em todo mundo. Veja [Configuração do Seamless Co-op](#configuração-do-seamless-co-op).

## Para jogar (manda isso para os amigos)

Precisa ter o Dark Souls III 1.15.2 com o Seamless Co-op instalado, e a Steam.

1. Baixe o launcher: **https://github.com/sear0M/ds3-coop-mod/releases/latest/download/DS3CoopLauncher.exe**
2. Coloque o `DS3CoopLauncher.exe` dentro da pasta do jogo, onde está o `DarkSoulsIII.exe`. Ele também funciona em outro lugar, mas aí pergunta a pasta na primeira vez.
3. Abra o jogo sempre por ele. A janela **DS3 Moraes e amigos** mostra a versão do launcher e do mod instalado. Ao abrir, ela só **confere** se tem versão nova e avisa ("Versão nova disponível... clique em Atualizar"), sem baixar nada. Os botões:
   - **Atualizar:** o único que baixa e instala. Puxa a versão mais nova do GitHub na hora;
   - **Iniciar jogo:** abre a Steam (se estiver fechada) e o jogo pelo `ds3sc_launcher.exe`, com a versão que estiver instalada;
   - **Senha do coop:** abre o `SeamlessCoop\ds3sc_settings.ini` no Bloco de Notas, onde fica a `cooppassword`;
   - **Opções de vídeo:** abre o `ds3coopmod.ini` (FPS e tela cheia sem borda);
   - **Trocar pasta do jogo:** para quando o launcher não está dentro da pasta do jogo.

Ao clicar em Atualizar, o launcher baixa o mod novo (`dinput8.dll`), se atualiza sozinho quando ele mesmo mudou e aplica as configurações do grupo no `ds3sc_settings.ini` (contador de mortes, sem invasões). Todo arquivo baixado é conferido com SHA-256 antes de ser instalado. Para todo mundo jogar na mesma versão, cliquem em Atualizar quando aparecer o aviso de versão nova.

Na primeira vez o Windows pode mostrar "O Windows protegeu o computador", porque o launcher não tem assinatura digital. Clique em **Mais informações** e depois em **Executar assim mesmo**.

A `cooppassword` do Seamless Co-op o launcher não mexe: combinem a mesma senha entre vocês.

## Instalação manual (sem launcher)

1. Baixe o `dinput8.dll` da [última versão](https://github.com/sear0M/ds3-coop-mod/releases/latest).
2. Copie para a pasta onde está o `DarkSoulsIII.exe`.
3. Abra o jogo como sempre (pelo `ds3sc_launcher.exe` do Seamless Co-op).

Para desinstalar, apague o `dinput8.dll` da pasta do jogo.

O mod escreve um log em `ds3coopmod.log`, na pasta do jogo. Se algo não funcionar, ele é o primeiro lugar para olhar.

## Uso

| Tecla | O que faz |
|---|---|
| `Insert` | Abre a senha / abre e fecha o menu de itens |
| `Esc` | Fecha o menu |

A senha só é pedida uma vez por vez que o jogo é aberto. Depois de 5 senhas erradas o menu trava por 30 segundos.

Com o menu aberto o mouse fica livre: o jogo para de prender o cursor no meio da tela. Se o cursor do Windows mesmo assim não se mexer, o menu desenha um cursor próprio, guiado pelo movimento do mouse que o jogo recebe.

Os itens só chegam quando um personagem está carregado (não funciona na tela de título).

## Vídeo: FPS sem trava e tela cheia sem borda

O mod tira a trava de 60 FPS e tem um modo de tela cheia sem borda. As opções ficam em `ds3coopmod.ini`, na pasta do jogo. Cada jogador tem o seu, e o launcher não mexe nele. Para abrir, use o link **Opções de vídeo** do launcher.

```ini
[Video]
fps_limit = 0     ; 0 = taxa do monitor (ex.: 144 num monitor de 144 Hz), 60 = original, ou um valor (até 360)
borderless = 1    ; tela cheia sem borda
fix_sprint = 1    ; corrige a corrida mais lenta acima de ~82 FPS
```

- **Tela cheia sem borda:** coloque o jogo em **Janela** nas opções de vídeo do DS3, de preferência com a resolução do seu monitor. O mod tira a borda e estica a janela para o monitor inteiro, e o Alt+Tab fica instantâneo.
- **FPS:** o DS3 tem um limitador de FPS de depuração escondido. O mod só liga ele com o valor escolhido, sem alterar o código do jogo. Acima de ~82 FPS o personagem corria mais devagar, e o `fix_sprint` corrige isso.
- Mudou alguma coisa? Feche e abra o jogo de novo.
- O jogo foi feito para 60 FPS. Se notar alguma física estranha num FPS muito alto, teste 120 ou volte para 60.

## Configuração do Seamless Co-op

Arquivo `SeamlessCoop\ds3sc_settings.ini`, na pasta do jogo:

```ini
; contador de mortes em cima da cabeça de cada jogador (sem o ping)
overhead_player_display = 4

; ninguém invade o seu mundo
allow_invaders = 0
```

O launcher já aplica isso no jogo de cada um. Os valores vêm de [`launcher/seamless_overrides.ini`](launcher/seamless_overrides.ini): para mudar para o grupo inteiro, edite esse arquivo e faça push. O launcher só troca as chaves que estão nele, o resto do arquivo de cada um fica igual, e na primeira mudança ele guarda uma cópia em `ds3sc_settings.ini.bak`.

Quem não usa o launcher precisa mudar no próprio jogo: o `overhead_player_display` muda o que *você* vê, e o `allow_invaders` vale para o mundo de quem está hospedando. Além disso, só entra na sessão quem tiver a mesma `cooppassword`.

## Publicar uma versão nova

Todo push na branch `main` faz o GitHub Actions compilar o mod e o launcher e publicar uma release nova (`build-1`, `build-2`...) com `dinput8.dll`, `DS3CoopLauncher.exe`, `seamless_overrides.ini` e `manifest.txt` (os hashes SHA-256 de cada arquivo). Na próxima vez que alguém abrir o launcher, já pega essa versão.

O repositório precisa ser **público** para os amigos conseguirem baixar sem login.

## Compilar

Precisa de um compilador C++ para Windows 64-bit, mais CMake e Ninja. O jeito mais simples é o WinLibs, que já vem com os três:

```bash
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

Depois, na pasta do projeto:

```bash
powershell -ExecutionPolicy Bypass -File build.ps1
```

Para compilar e já copiar para o jogo:

```bash
powershell -ExecutionPolicy Bypass -File build.ps1 -GameDir "C:\Jogos\DARK SOULS III-SteamGG.NET\DARK SOULS III-SteamGG.NET\DARK SOULS III"
```

Isso gera `build\dinput8.dll` (o mod) e `build\DS3CoopLauncher.exe` (o launcher).

### Testar o launcher

`tests/test_launcher.ps1` sobe um servidor local fazendo papel do GitHub e uma pasta de jogo falsa. Ele confere que o launcher se atualiza sozinho, instala o mod, aplica as configurações sem mexer na `cooppassword` e recusa arquivo corrompido:

```bash
powershell -ExecutionPolicy Bypass -File tests\test_launcher.ps1
```

### Testar sem abrir o jogo

`tests/harness.cpp` é um programa D3D11 que se chama `DarkSoulsIII.exe` só para o mod carregar nele. Ele aperta Insert, digita a senha, clica no menu e salva prints em `build\harness\`:

```bash
powershell -ExecutionPolicy Bypass -File tests\run_harness.ps1 -Password "<senha>"
```

Dar itens não funciona no teste: o mod recusa porque o programa não é o jogo 1.15.2. Esse é o comportamento esperado.

### Trocar a senha

A senha não fica no código, só o hash SHA-256 dela, em `src/password.cpp` (`kPasswordSha256`). Para gerar o hash de uma senha nova:

```bash
powershell -Command "$b=[Text.Encoding]::UTF8.GetBytes('NOVA_SENHA'); -join ([Security.Cryptography.SHA256]::Create().ComputeHash($b) | % { $_.ToString('x2') })"
```

Isso só impede alguém de abrir o menu sem saber a senha. Se o repositório for público, o hash fica visível, então não use uma senha que você usa em outro lugar.

## Como funciona

| Arquivo | O que faz |
|---|---|
| `src/dllmain.cpp` | O jogo carrega o `dinput8.dll` da própria pasta. Repassamos o `DirectInput8Create` para o `dinput8.dll` original do Windows e iniciamos o mod numa thread separada (só dentro do `DarkSoulsIII.exe`). |
| `src/hooks.cpp` | Hook em `IDXGISwapChain::Present` para desenhar o menu com Dear ImGui, hook na janela para o Insert e o mouse, hooks em `SetCursorPos`/`ClipCursor` para o jogo não prender o cursor, e hook no DirectInput para o jogo não receber teclado e mouse com o menu aberto (o movimento do mouse vai para o cursor do menu). |
| `src/game.cpp` | Endereços do jogo 1.15.2, leitura das tabelas de itens (params) e a chamada da função do jogo que entrega itens. Toda leitura de memória é conferida antes, e o mod se desliga se o exe não for a versão 1.15.2. |
| `src/menu.cpp` | Telas de senha e de itens. |
| `src/video.cpp` | FPS sem trava (limitador de depuração do `SprjFlipper`), correção da corrida e tela cheia sem borda (hooks em `SetWindowLongW`/`SetWindowLongPtrW`/`SetWindowPos`). Lê o `ds3coopmod.ini`. |
| `src/items_data.inc` | Lista de itens gerada por `tools/gen_items.ps1` a partir de `data/item_ids.json`. |
| `launcher/main.cpp` | O launcher: lê o `manifest.txt` da última release, compara os hashes, baixa o que mudou (inclusive ele mesmo), aplica o `seamless_overrides.ini` e abre o jogo. A janela é desenhada com GDI+ nas cores do Dark Souls III. |
| `tools/make_icon.ps1` | Gera o ícone do launcher (`launcher/icon.ico`). |

## Avisos

- Feito e testado só para a versão **1.15.2** do jogo. Em outra versão o menu abre, mas não entrega itens.
- Não use nos servidores oficiais da FromSoftware.
- Os nomes dos itens estão em inglês, como no jogo em inglês. Use a busca.

## Créditos e licença

- Endereços do jogo e lista de itens: [veeenu/darksoulsiii-practice-tool](https://github.com/veeenu/darksoulsiii-practice-tool) (AGPL-3.0).
- Técnica do FPS sem trava, da correção da corrida e do modo sem borda: [0dm/DS3DebugFPS](https://github.com/0dm/ds3debugfps) (Apache-2.0), reimplementada em `src/video.cpp`.
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT) e [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause), em `third_party/` com as licenças originais.
- Seamless Co-op é do LukeYui e não faz parte deste repositório.

Como usa dados de um projeto AGPL, este projeto também é distribuído sob a **GNU AGPL-3.0**. Veja [LICENSE](LICENSE).
