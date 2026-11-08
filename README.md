# MK8-online

Recompilação AArch64 antecipada para Windows x64, com launcher local, runtime C/C++ e gráficos Vulkan. Objetivo: preparar uma vez, jogar e hospedar uma sala entre amigos no próprio PC.

**Experimental:** o MK8 4.0.1 concluiu corridas solo de três voltas com Mario. Duas instâncias multiplayer, conectadas pelo código de um relay local, percorreram três voltas e retornaram à seleção de pista. Persistem defeitos visuais e engasgos no primeiro uso de shaders. Multiplayer entre dois computadores e pela internet ainda precisa de validação. A versão atual do launcher simplifica a preparação e não oferece editor de perfis.

**Ponto de retomada:** o teste de tela dividida reconhece dois controles, mas permanece na seleção de personagens. O transporte passou com 12 participantes simulados; a corrida observada usou dois jogos. A repetição pela LAN direta foi interrompida por espaço em disco. Evidências, limitações e próximos passos estão em [docs/STATUS.md](docs/STATUS.md#ponto-de-retomada-do-multiplayer).

## Instalar (testadores)

Baixe `MK8-online-<versão>-setup.exe` na página de **Releases** do repositório e execute. A instalação é por usuário, sem administrador. O launcher abre em **Instalar**. Escolha a pasta extraída do jogo ou os arquivos NSZ/NSP do seu console e clique em **Preparar**: as ferramentas são baixadas e conferidas por SHA-256, e o jogo é compilado na própria máquina. Nada do jogo é distribuído. O instalador pergunta onde guardar os dados (importação de ~28 GiB, ferramentas, builds e saves). O padrão é `%LOCALAPPDATA%\MK8-online`, mas qualquer disco serve. Essa pasta sobrevive a atualizações e à desinstalação. As ferramentas baixadas (`native-tools`, ~0,9 GiB) ficam nela uma única vez e são compartilhadas por todas as versões do launcher.

- **Atualizar:** em **Suporte → Verificar atualizações**, o launcher consulta os Releases (pré-releases incluídos) e oferece **Atualizar agora**. O botão baixa o novo setup, confere o SHA-256 publicado, instala em silêncio e reabre o launcher.
- **Relatar um problema:** em **Suporte → Gerar relatório**, o launcher junta os logs do jogo (atual e três anteriores), da preparação e das ferramentas, além do manifesto da build, num `.zip` em `<pasta de dados>\data\reports` (**Suporte** mostra a pasta e abre o local). Caminhos pessoais e o nome da conta são ocultados; saves e chaves não entram. Quando o jogo fecha com erro, um relatório é criado automaticamente. Anexe o `.zip` à issue.
- **Duas máquinas:** cada PC instala e prepara com os próprios arquivos da mesma versão. Depois, siga **Multijogador → Como testar em duas máquinas**.

Pendências e roteiro: [docs/ROADMAP.md](docs/ROADMAP.md). Publicar uma versão: [docs/RELEASING.md](docs/RELEASING.md).

## Usar a partir do código

Na raiz do projeto, em PowerShell:

```powershell
python scripts/fetch-upstream.py   # dependências fixadas (smm2recomp, LASRecomp), conferidas pelos manifestos
./scripts/start-launcher.ps1
```

O repositório publica só os manifestos de `upstream/`; o script baixa os commits fixados e confere cada arquivo por SHA-256. O script do launcher abre a janela nativa do Windows. Em **Arquivos NSZ ou NSP**, selecione o jogo base e a atualização 4.0.1 e clique em **Preparar**. O launcher procura `prod.keys` ao lado do jogo, na subpasta `keys` ou em `%USERPROFILE%/.switch`. Se precisar, selecione o arquivo do seu console uma vez. O launcher guarda apenas o caminho. A extração exige cerca de 28 GiB livres no disco do launcher, incluindo 4 GiB de reserva. Também é possível selecionar uma pasta com `exefs`, `romfs` e `control.nacp` (ou `control/control.nacp`) já extraídos.

Se o disco do sistema estiver cheio, abra o launcher no PowerShell com uma pasta de dados em outro volume. Por exemplo, `& .\Launcher.exe --data-dir E:\MK8-Local`. Use a mesma pasta nas próximas aberturas para reaproveitar a importação e a build.

`python scripts/package-launcher.py` gera `Launcher.exe` em `target/portable/`, com o extrator e seu runtime .NET embutidos. A máquina de empacotamento precisa de PowerShell 7 e .NET 10 x64 para compilar o auxiliar. O jogador usa Windows x64 com WebView2, Python 3.12 x64 no PATH e internet para obter as ferramentas da recompilação na primeira preparação. O pacote exclui ROM, chaves, saves e builds locais. Cada versão do pacote extrai suas fontes numa pasta própria em `%LOCALAPPDATA%\MK8-online`, mas todas usam a mesma pasta de dados (`data`). Pastas de dados por versão criadas antes desta mudança não são migradas nem apagadas. Use **Jogar solo** depois da preparação para abrir o candidato existente.

Em **Multijogador**, salve seu nome, crie ou busque uma sala na rede local (UDP 47820) e clique em **Novo convite**. Envie um convite individual a cada amigo, que cola e entra. O launcher escolhe o endereço de rede automaticamente. **Endereço alternativo** fica disponível para VPN ou redirecionamento configurado. O convite inclui o endereço e deve ser enviado em privado. Dentro do jogo, **Esc → Salas com amigos** abre a aba de salas do launcher em runtimes compilados com esse menu. O nome também é enviado ao serviço de conta local do jogo. **Jogar na sala** aceita a build experimental atual, confere os hashes e mantém `gameplay_verified=false`. A descoberta de salas depende da rede local e do firewall.

Depois de conectar no launcher, cada pessoa abre **Jogar na sala**. No menu principal do jogo, segure **L + R** e pressione o analógico esquerdo para mostrar **LAN Play**. O anfitrião escolhe **Create Room**; os convidados selecionam o nome dele em **Lobby**. Depois, cada pessoa escolhe personagem e kart, e o anfitrião define as regras. Esse é também o modo usado pelo relay do launcher. O modo **Online Play** oficial e a comunicação local sem fio (`ldn`) não fazem parte dessa integração. O modo LAN admite até 12 jogadores; o teste automatizado de transporte exercita 12 conexões, sem equivaler a uma corrida qualificada com 12 jogos.

## Relay pela internet

Quando a edição do launcher tem um relay configurado, **Criar sala pela internet** gera um código `MK8-…`. O anfitrião compartilha o código e os amigos colam no mesmo campo dos convites. O código permite pedir uma credencial individual ao relay. Compartilhe apenas com quem pode entrar na sala.

Uma pessoa opera o relay em um servidor público. Os jogadores só fazem conexões de saída e não precisam abrir portas em casa. No servidor, compile `cargo build --release --locked -p mk8-session --bin mk8-relay` e inicie:

```text
mk8-relay 127.0.0.1:47819 0.0.0.0 IP_PUBLICO_DO_RELAY 47820
```

No servidor, libere UDP 47820–47835 e publique o controle `127.0.0.1:47819` atrás de um proxy HTTPS com certificado válido. Mantenha o HTTP interno em loopback, limite o corpo e a taxa de requisições de controle no proxy. Os convites e as credenciais passam pelo controle HTTPS. O tráfego do jogo usa o protocolo UDP autenticado existente. Hospedagem que oferece apenas um túnel HTTP não encaminha o UDP do jogo.

Na máquina que empacota o launcher, defina `MK8_RELAY_URL` com o endereço HTTPS do relay antes de executar `python scripts/package-launcher.py`. Distribua o mesmo executável aos jogadores. Em desenvolvimento, a variável também funciona ao iniciar o launcher. O controle HTTP em `127.0.0.1` é aceito para o teste local.

O daemon limita o total a 16 salas. Cada sala admite até 11 convidados além do anfitrião, expira após quatro horas e é removida após inatividade do anfitrião. Foram testados 12 clientes de transporte e uma corrida com dois jogos nesta máquina; isso não qualifica 12 jogos nem uma conexão pela internet.

Uma sala nova recebe um código novo. Na LAN, o PC do anfitrião encaminha os pacotes diretamente. No caminho por código de internet, o relay público encaminha os pacotes, enquanto o anfitrião controla a sala do jogo. A distância até esse relay e a conexão dos jogadores afetam a latência. Sem relay público configurado, é possível testar esse caminho com um relay em loopback, mas não oferecer acesso externo apenas pelo código.

## Onde alterar

| Pasta | Responsabilidade |
| --- | --- |
| `apps/launcher/src` | Janela Tauri (`main.rs`), API local e supervisão de jogo, build e sala (`server.rs`), etapa de ferramentas (`setup.rs`), relatórios de logs (`report.rs`) e atualizações (`update.rs`) |
| `apps/launcher/web` | Interface (HTML, CSS e JS, tema escuro) |
| `installer`, `scripts/release.py`, `.github/workflows` | Setup Inno Setup, build de release e CI |
| `apps/launcher/tauri.conf.json`, `icons` | Identidade da janela; sem permissões de IPC (`capabilities` ausente) |
| `crates/installer` | Validação de instalação, processo do jogo, perfis e geometria |
| `crates/manifest`, `crates/build-manager` | Configuração, hashes e processo de build |
| `crates/session`, `crates/transport` | Sala, convites, ponte LAN e datagramas autenticados |
| `toolchain` | Geração, compilação incremental, ligação e adaptações restritas |
| `runtime/prepare.py` | Preparação da cópia privada do runtime e ABI dos módulos |
| `runtime/mk8/graphics` | Texturas, tradução de shader e captura diagnóstica |
| `runtime/mk8/services` | Vídeo, fontes, toque e exibição |
| `runtime/mk8/mii` | Perfil, editor de nome e modo sem modelos Mii |
| `runtime/mk8/support` | Ponte do launcher e diagnóstico nativo |
| `games/mk8d` | Perfis de versão, símbolos e configuração do jogo |
| `tests` | Regressões nativas, pipeline e navegador |
| `upstream` | Manifestos das dependências fixadas; as fontes são baixadas por `scripts/fetch-upstream.py` e ficam fora do Git |
| `local`, `target` | Dados privados e artefatos; fora do Git |

Fluxo atual: launcher → `toolchain/build.py` → preparar runtime → gerar C → compilar objetos → ligar runtime/DLLs → publicar `local/candidate.json` → teste solo. Um candidato não é uma instalação qualificada. O launcher verifica seus hashes antes de executar.

## Alterar sem recompilar o jogo inteiro

```powershell
# Só launcher/interface (gera target/release/Launcher.exe)
cargo build --release --locked -p mk8-online

# Runtime: reutiliza a biblioteca do jogo da preparação existente
./local/python/Scripts/python.exe -u toolchain/build.py --config local/config.json --local local --runtime-only

# Retomar ligação após compilar os objetos
./local/python/Scripts/python.exe -u toolchain/build.py --config local/config.json --local local --finish
```

Mudanças no tradutor, nos módulos ou nos patches de código guest exigem regenerar ou recompilar as unidades afetadas. `--runtime-only` reutiliza a biblioteca do jogo. O pipeline gera os módulos em paralelo e adianta a compilação do runtime e a preparação do linker enquanto compila o jogo. No Xeon E5-2666 v3 com 16 GB, uma preparação com caches vazios levou **14 min 16 s** com 16 compiladores, após instalar as ferramentas. Reserve **20–25 min** para a primeira execução se houver outras cargas no PC. O padrão usa até 16 compiladores conforme CPU e RAM disponíveis. `MK8_NATIVE_JOBS=<n>` permite ajustar o limite de 1 a 16. Nas preparações seguintes, o cache de objetos verificados em `local/native-tools/objects` reaproveita unidades cujas fontes, headers e flags continuam iguais.

Diagnóstico de desempenho: `MK8_SAMPLE=<frame>:<segundos>` amostra as threads do processo (~400 Hz) e registra no log, por thread, CPU usada e onde as amostras caíram (função do jogo `f_<endereço>`, função do runtime com arquivo:linha, ou módulo). O nome atribuído a uma espera do sistema é aproximado. `MK8_TRACE_TEXPOOL=<primeiro>:<último>` registra, nesse intervalo de quadros, os registros no pool de texturas e os binds de IDs que já foram cubemaps, com os chamadores guest.

Com `LARECOMP_VK_PROF=1`, `MK8_FRAME_TIMES=1` registra cada quadro como `[frame] número total_ms execução_ms prefetch_ms apresentação_ms restante_ms`. O relatório de engasgos também separa o prefetch de shaders; anteriormente esse trabalho aparecia em `other`. Use percentis dos tempos por quadro, excluindo menus/carregamento e capturas, para avaliar estabilidade além do FPS médio: `python scripts/frame-stats.py local/race-grand-prix-xxxx --from 3600 --to 7990` faz isso. `MK8_SHADER_WAIT_MS=4` (opcional) limita por quadro a espera de draws por shaders/pipelines ainda compilando; o draw é pulado até o build ficar pronto.

A apresentação agora usa o timer de alta resolução do runtime, mantendo os prazos de 60 Hz. Isso reduziu a oscilação entre quadros no teste de Canyon; pausas de carregamento e de primeiro uso de shaders ainda podem ocorrer. O tradutor também evita consultas repetidas de configuração e formatação duplicada de texto.

Compute sem espera está ativado nesta build experimental: resultados permanecem pendentes até uma leitura pela CPU exigir espera, e cada dispatch que escreve memória guest é enviado antes dos uploads seguintes. `MK8_DEFER_COMPUTE_FLUSH=0` restaura as esperas antigas; `MK8_COMPUTE_SUBMIT=0` testa o adiamento sem separar os envios. O ganho médio não elimina todos os picos de frametime. Comparações e limitações em `docs/STATUS.md`.

`scripts/test-race.py time-trial --course water-park` e `--course sweet-sweet-canyon` testam outras pistas com aceleração contínua e saves isolados. `--cache-from <pasta/cache>` copia apenas o cache de uma execução anterior para comparar com cache aquecido; sem a opção, o teste começa com cache vazio. O encerramento por captura agora salva também o cache Vulkan, como o encerramento normal. `--env LARECOMP_LOG_WAITS=10` registra esperas de shaders acima de 10 ms. Capturas acrescentam custo e devem ficar fora dos intervalos comparados.

## Verificar

```powershell
cargo fmt --all -- --check
cargo test --workspace --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
./local/python/Scripts/python.exe -m unittest discover -s tests -p test_pipeline.py
./local/python/Scripts/python.exe -m unittest discover -s tests -p test_texture_memo.py
python -m unittest discover -s tests -p test_launcher_e2e.py
```

O último comando requer Playwright/Chromium no Python escolhido e o `Launcher.exe` release atualizado (`MK8_LAUNCHER_EXE` aponta outro executável; `MK8_TEST_RELAY_EXE` ativa o teste do relay); ele usa `--headless` para a interface e abre também a janela nativa (WebView2 com depuração numa porta local e pasta de perfil temporária) para conferir a tela de instalação e o bloqueio de navegação. Testes nativos usam o compilador local; a validação privada do SDK exige o código gerado do próprio jogo. Testes automatizados não comprovam uma corrida correta.

Estado e próximos passos: [docs/STATUS.md](docs/STATUS.md). Orientação para agentes: [AGENTS.md](AGENTS.md). Origens, atribuições e licenças: [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md). Código próprio sob [GPL-3.0-or-later](LICENSE); conteúdo do jogo e chaves não são distribuídos.
