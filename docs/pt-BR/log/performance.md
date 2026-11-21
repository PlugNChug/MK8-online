# Registro técnico — Desempenho e memória

[Índice](README.md) · [English](../../en/log/performance.md)

## Persistência do cache de pipelines

Validação manual posterior com Nextendo: mantenedor confirmou que os testes rodaram perfeitamente em `local/nextendo-playtest-iub0kdkc`, runtime `268ccc00`. Login confirmado pelo log; sessão de aproximadamente 11 minutos encerrada com código 0, sem `FATAL`. Sete gravações periódicas de cache registradas, agora também exercidas no jogo real. Últimas cinco janelas registram 60 FPS e máximo de quadro de 18 ms; são o fim da sessão, não uma medição de todas as corridas. Saves originais preservados. Pistas, modos e quantidade de corridas não foram detalhados; esta confirmação não encerra automaticamente as issues de CTGP, split-screen, Miis ou APU, nem qualifica multiplayer Steam entre duas máquinas. `gameplay_verified` permanece falso.

Corrigida perda potencial do cache: a gravação anterior removia `vk_pipelines.bin` antes do rename e ignorava erros de fechamento/substituição. A preparação validada agora verifica snapshot completo, alocação, escrita e fechamento, e usa substituição com `MoveFileExA` no Windows sem apagar previamente o destino. O wrapper público mantém sua ABI; o gravador periódico recebe o resultado real, mantém builds pendentes em caso de falha e tenta novamente após o intervalo normal de 60 s. Não emite sucesso fictício nem entra em repetição rápida. Isso protege trabalho já compilado; não demonstra a causa dos engasgos relatados nem elimina compilação na primeira utilização.

Seis testes aprovados: cinco de runtime stalls e um novo teste nativo da função preparada, com arquivos reais e falhas de snapshot incompleto, escrita parcial, fechamento e destino bloqueado. O cache anterior permanece intacto; após desbloqueio, a substituição funciona. Agendador também testado com falha inicial e retry sem novas compilações. Host recompilado (220 unidades, guest reutilizado), SHA-256 `268ccc0083c572863b2a5006520f88cd23e3216673fc97a3ab9046ed73f24966`. Teste de integração em andamento em `local/race-grand-prix-e9lev31t`, saves isolados e cache copiado. Sem setup/publicação. Faróis #49, Miis brancos, CTGP/split-screen e validação da APU #52 continuam pendentes.

## Recuperação de memória e pré-teste Vulkan — continuação

Revisão de `memory-check-0h2eizvj` após relato de engasgos: encerramento 0 em aproximadamente 265 s de runtime, sem `FATAL` nem acionamento registrado do fallback de snapshot. Captura 6300 confirma corrida, volta 1/3; captura 14400 mostra carregamento. As 16 capturas periódicas foram correlacionadas ao próximo resumo de frames: 15 coincidem com máximos de 368–617 ms (a última, em loading, 131 ms). O diagnóstico síncrono introduziu pausas; esses números não devem ser usados como desempenho normal. Durante frames 4200–14000 há ainda um pico de 570 ms sem captura no mesmo intervalo, próximo de estatística detalhada de VRAM, causa não isolada. Pausas de 1831 ms/3782 ms coincidem com transições, uploads e 630/428 compilações, não prova de tempo exclusivamente gasto em shaders. No fim, heap 0 registra 1776 MB reservados em blocos e 876 MB em alocações; isso não é medição de pico nem garantia de ausência de pressão de memória.

Helper privado `open-memory-check.py` ajustado para remover capturas periódicas e estatísticas detalhadas de VRAM, mantendo logs usuais de frames. Correção do método de medição, não patch de gameplay.

Teste automático posterior concluído em `local/race-grand-prix-jd01947u`: runtime `a92e1d9b`, saída 0 em 216,4 s, saves isolados e cache copiado da sessão manual. Capturas pontuais nos frames 3500 e 12000 confirmam corrida em Mario Kart Stadium, voltas 1/3 e 3/3; não houve captura periódica no trecho medido nem estatísticas detalhadas de VRAM. Análise de 67 janelas de aproximadamente 2 s, entre 73,8 e 205,9 s (frames 3800–11899, excluindo as capturas): mediana 60,0 FPS, mínimo por janela 57,6 FPS, máximo de quadro 41 ms; duas janelas com máximo acima de 33 ms e nenhuma acima de 50 ms. Os picos de 38/41 ms coincidem com janelas com 3/15 compilações, sem isolamento da causa. Sem FATAL nem acionamento da recuperação de snapshot. Resumo e CSV privados em `performance-summary.json` e `performance.csv`. Não mede 1% low, pico de VRAM, corrida completa até resultados ou copa completa. Pista diferente da sessão manual e cache reaproveitado impedem comparação controlada antes/depois; não qualifica CTGP, split-screen ou a APU da #52. `gameplay_verified=false`.

Ensaio manual monitorado solicitado pelo mantenedor: `local/memory-check-0h2eizvj`, mesmo runtime `a92e1d9b`, saves/perfil/cache copiados, `MK8_TRACE_VRAM=1` e capturas a cada 900 frames. Processo abriu e logs de heaps/alocações estão ativos. Solicitada copa 150cc completa seguida de fechamento normal. Resultado ainda pendente; RX 580 local não reproduz automaticamente os limites da APU AMD da #52.

Após `26a6dce`, adicionado `snapshot_memory.py`: caminho `snapshot_image` anteriormente abortava na primeira falha de `vmaCreateImage`, ao contrário das texturas principais. Recuperação limitada a `VK_ERROR_OUT_OF_DEVICE_MEMORY`: coleta recursos diferidos já concluídos, repete a alocação e tenta preferência por memória de host compatível. Formato, dimensões, samples e flags mantidos; outros erros retornam imediatamente e esgotamento continua sendo falha. Teste C do helper preparado cobre sucesso inicial, recuperação após coleta, fallback, esgotamento e device-lost/host-OOM sem repetição indevida. 14 testes de textura também passaram. Isso trata uma lacuna no caminho de snapshots observado próximo ao ponto da #52, sem provar que a GPU AMD integrada do relator agora completa corridas.

Pré-teste Vulkan do launcher agora executa `SDL_Init(VIDEO)` e `SDL_Vulkan_LoadLibrary` com a DLL local no subprocesso, libera SDL/Vulkan ao terminar e informa erro real do SDL. Erro Windows 193 distingue formato/arquitetura inválida de módulo ausente (126); não identifica ficticiamente a dependência culpada. Teste com DLL sintética que exporta `vkGetInstanceProcAddr` mas rejeita consultas confirma recusa do SDL; teste direcionado real passou com casos ausente/corrompido/dependência ausente/válido. Suíte completa: dois casos passaram, terceiro falhou apenas na limpeza de `C:/Users/dzvini/AppData/Local/Temp/mk8-vulkan-bundle-h6pptnfo/runtime.exe` com WinError 5; arquivo preservado, sem contornar a exclusão bloqueada. Não declarar toda a suíte aprovada. Launcher compilado, 36 testes Rust aprovados, Clippy e formatação aprovados. #47 exige novo diagnóstico do relator para determinar a DLL/arquitetura causadora.

Runtime host recompilado, SHA-256 `a92e1d9be2087e9404e5f4de420a329728c9772b04381163d162991f060ef9e4`; ensaio curto iniciado em `local/race-time-trial-uq7i4xq7` com saves isolados e `MK8_TRACE_VRAM=1`. Sem setup, commit ou publicação nesta rodada. Pendentes controles #50, iluminação #49, Miis brancos, variantes DLC #36 e recursos de importação/portabilidade.

Resultado do ensaio: saída 0 em 70,2 s; captura `frame-03400.png` inspecionada mostra seleção de cilindrada, não corrida. Sequência automática não alcançou a pista; os 60 FPS observados no fim são do menu. Sem qualificação da recuperação em GPU com pouca memória. Teste Vulkan direcionado repetido e aprovado em 6,34 s; falha de limpeza da suíte completa permanece registrada acima. Nenhum arquivo bloqueado foi removido por outro mecanismo.

## Engasgos e FPS oscilando nas corridas — 14/11/2026

**Medição nesta máquina** (RX 580 8 GB, driver AMD 26.1.1; GP Mushroom Cup, corrida 1, quadros 3000–14000; `scripts/frame-stats.py`):

| Execução | Cache | FPS | p99 | Pior | >25 ms |
| --- | --- | --- | --- | --- | --- |
| `race-grand-prix-odwx3qkw` | jogo vazio (driver aquecido) | 59,95 | 17,74 | 47,4 ms | 17 |
| `xchoj6wg` / `oy047b4c` | aquecido, FIFO | 59,99 / 59,98 | 17,66 / 17,61 | 30,8 / 32,5 ms | 5 / 7 |
| `a8nu0oou` / `59bxbx65` | aquecido, FIFO_RELAXED | 59,99 / 60,00 | 17,66 / 17,62 | 29,4 / 30,1 ms | 4 / 3 |

Aqui a oscilação em corrida não se reproduz. Engasgos grandes ficam nas transições: fim da corrida, com 475–519 ms em `other` enquanto uma thread do jogo espera ~265 ms por um núcleo emulado ocupado por outra (`kernel.c` não tem preempção dentro do código guest), e carregamento de pista com centenas de compilações.

**Modo de apresentação (`toolchain/present_mode.py`, padrão).** O driver AMD no Windows não oferece MAILBOX, então o runtime usava FIFO sem registrar isso. Agora a ordem é MAILBOX → FIFO_RELAXED → FIFO, o modo aparece no log (`[vk] present mode …`) e `LARECOMP_VK_PRESENT=fifo|relaxed|immediate|mailbox` continua escolhendo. Na tabela, FIFO e FIFO_RELAXED medem igual, porque aqui quase nenhum quadro perde o prazo. **O ganho em máquinas que perdem o prazo com frequência não está comprovado.** `tests/test_present_mode.py` compila a escolha preparada (AMD sem MAILBOX, MAILBOX disponível, só FIFO, escolhas explícitas e recusas). Runtime recompilado com `--runtime-only`, candidato `6dde4ee3…`.

**Picos restantes em corrida (≤ 30 ms, cache aquecido).** Vêm principalmente do prefetch de shaders. Na primeira vez que um programa aparece *na sessão*, `scan_draw` espera a tradução Maxwell→GLSL dele no caminho do quadro (`job_run_or_wait(g_tq, …)`, um único tradutor), porque a chave do pipeline depende do texto traduzido. São 13–34 programas novos por janela, 3–7 ms cada. Isso se repete a cada sessão porque só SPIR-V e pipelines vão para o disco, não a tradução. Além disso, a varredura custa ~0,6–0,9 ms por quadro.

**Cache de traduções (`toolchain/translation_cache.py`, `runtime/mk8/graphics/translation_cache.c`, padrão; `MK8_TRANSLATION_CACHE=0` desliga).**
- **Onde atua:** as quatro traduções do backend Vulkan em `exec.c` (três estágios de programa e compute) passam por `mk8_translate_cached`. `translate.c` (que os testes compilam sozinho) e `ahead.c` ficam intactos.
- **Chave:** cobre tudo que `maxwell_translate` lê: cabeçalho e as palavras de código até onde a própria varredura de fim para (reproduzida), `ShaderKey`, globais do tradutor e o executável do runtime (tamanho e data). Variáveis de depuração que mudam a saída desviam o cache.
- **Formato:** um único `cache/xlate.pack`, aberto uma vez e indexado em memória. Cada registro tem um segundo hash da chave e um hash do conteúdo; um registro que não confere é traduzido de novo. Outro runtime começa um pack novo. Um final cortado (falha durante a escrita) encerra o índice ali e é sobrescrito.
- **Por que pack:** a primeira versão, com um arquivo por shader, custava 0,2–0,6 ms só para abrir no Windows (medido), quase o mesmo que traduzir. No pack, uma leitura custa ~0,04 ms, contra ~0,9 ms de tradução.
- **Ganho já na primeira sessão:** com o cache vazio, mais da metade das buscas já acerta, porque o mesmo shader era traduzido de novo para cada programa e chave.
- **Tamanho:** a corrida 1 gravou ~11 MB (615 traduções).

A/B no mesmo build (`cb567004…`), com o mesmo SPIR-V e os mesmos pipelines de `race-grand-prix-a_k0sjcc`, quadros 3000–14000:

| Execução | Prefetch > 5 ms | > 10 ms | Prefetch antes da corrida | Pior quadro | FPS |
| --- | --- | --- | --- | --- | --- |
| `x_caqan8` / `rj4yge6a`, desligado | 22 / 27 | 10 / 14 | 1.629 / 1.506 ms | 34,8 / 41,2 ms | 59,97 / 59,97 |
| `tnd1lp2e` / `7pfxb1qx`, ligado | 5 / 7 | 2 / 2 | 1.240 / 1.125 ms | 37,4 / 29,9 ms | 59,94 / 60,00 |

Os picos de prefetch caíram ~75–85% e o carregamento ficou ~0,3–0,4 s mais curto. A média já era 60 FPS. O pior quadro e a contagem acima de 17,5 ms variam entre execuções idênticas (127–295) e não são atribuídos à mudança. Captura do quadro 14200 com o cache ligado inspecionada: chegada, 3/3, imagem correta.

`tests/test_translation_cache.py` usa o tradutor real em vários processos sobre a mesma pasta: leitura igual campo a campo, bytes além do trecho lido, chave/código/global diferentes, registro danificado (segundo hash, texto GLSL e campo da `ShaderInfo`) nunca servido, final de lixo, runtime novo, `=0` e desvio de depuração. Sem a conferência do hash de conteúdo, o teste falha. Runtime recompilado com `--runtime-only`; 49 testes relacionados passaram. **Ainda não validado:** máquina mais fraca, em que a tradução pesa mais, e sessão com as 4 pistas.

Ao repetir as corridas, o disco C: caiu abaixo da reserva de 4 GiB: cada sessão copia ~650 MB de cache, e o próprio roteiro interrompeu uma execução. Foram removidas só as cópias `data/cache` das 15 sessões criadas nesta rodada (sem junções, sem jogo aberto); logs e capturas ficaram.

**Relatório de jogador (#8, 0.1.8, RTX 3060 notebook, 830 s):** média de 59,3 FPS em corrida, com 18 janelas abaixo de 57 FPS. Em 10 delas houve ≥ 10 compilações (até 759 na largada, cache frio). Num trecho de 8 s ficou travado em 30,0 FPS sem compilação, com espera pela GPU de 99–495 ms por janela, o que indica GPU no limite. O modo de apresentação não aparecia no log; numa NVIDIA ele provavelmente era MAILBOX. Faltam relatórios 0.2.x com a linha `[vk] present mode` e a GPU de quem vê a oscilação.

## VRAM em sessões longas — 14/11/2026

**Medição.** `race-grand-prix-ov14d8nd`: 180.000 quadros, 50 min, 3 GPs da Copa Cogumelo (12 corridas) no mesmo processo, `MK8_TRACE_VRAM=1`. Saída 0, sem `FATAL`. **Não há crescimento exponencial.**
- **1º GP:** o reservado da VMA sobe de 752 para 1.520 MB, o dobro, enquanto cada pista é vista pela primeira vez. No Gerenciador de Tarefas isso parece "exponencial".
- **Depois:** o uso fica entre ~900 e 1.100 MB. O reservado sobe devagar, 1.520 → 1.904 MB nos GPs 2 e 3, por fragmentação.
- **Cache de vértices:** subia sem parar, 31 → 130 MB, até o teto de 768 MB (`VC_BUDGET`). A cada carregamento os buffers do jogo mudam de endereço, e as faixas antigas só saíam ao atingir esse teto.
- **Alocações:** 4.056 → 10.160.
- **Texturas sem uso há mais de 1 minuto:** oscilam entre ~340 e 510 MB.

**Texturas retidas.** As maiores são alvos de renderização em tamanhos variáveis (38 tamanhos distintos na sessão, de 1454×818 a 1905×1071) criados sobre os **mesmos endereços**, `refs 2`, `finalized 0`. O jogo reaproveita a memória para alvos de outro tamanho e abandona os objetos antigos sem finalizá-los. No console isso não custa nada; aqui cada objeto tem imagem Vulkan própria.

**Liberação de imagens ociosas (`toolchain/texture_retire.py`, padrão; `MK8_TEXTURE_IDLE_S=<s>`, 60 por padrão, 0 desliga).**
- **Quando:** a cada ~10 s, na thread de tradução, logo depois da submissão do quadro.
- **O que sai:** imagens que nenhum uso tocou nos últimos 60 s e que se encaixam em um destes casos:
  - conteúdo vindo da memória guest (não escrito pela GPU): o próximo uso recria e recarrega a imagem, como o console lê essa memória;
  - alvo escrito pela GPU cujo endereço recebeu, depois do último uso, uma textura de outro formato: no console o conteúdo antigo também deixou de existir ali.
- **O que nunca sai:** imagem apresentada, com vídeo pendente ou envolvida numa cópia de profundidade adiada.
- **O que fica:** o registro do backend.

**Por que é seguro (auditoria dos 47 acessos diretos a `->backend`):**
- O passe de render, `copy_texture`, a amostragem e o memo por quadro passam antes por `vk_view`/`vk_tex` → `tex_backend`, cujo ramo "sem imagem" (`pool_release.py`) recria a imagem.
- `pass_layout`, as flags e os logs leem só metadados.
- Os aliases de cópia validam o handle.
- A sombra de profundidade recopia a cada submissão.
- Os descriptor sets são por submissão.
- A liberação esquece as views (incrementa `vk_view_gen`), invalida os snapshots daquela imagem e adia a destruição até a GPU terminar as submissões que podiam lê-la.
- O carimbo de uso passa a ser sempre registrado, não só com `MK8_TRACE_VRAM`, e o backend novo nasce carimbado.

**A/B no mesmo build** (`bccd7dce…`), GP completo com o cache `xh0dhyxh`:

| Execução | Reservado no fim | Pico em uso | Em uso nas corridas 2–4 | Ociosas > 1 min | FPS |
| --- | --- | --- | --- | --- | --- |
| `qa8ysx5_`, desligado | 1.776 MB | 1.249 MB | 731–869 MB | até 719 MB | 60 |
| `0so9vrpg`, padrão 60 s | 1.392 MB | 834 MB | 561–690 MB | até 237 MB | 60 |
| `xlgl6t9w`, estresse 10 s | 1.264 MB | 658 MB | 501–549 MB | até 180 MB | 60 |

Nas três execuções: saída 0, sem `FATAL`/exceção/perda de dispositivo e p99 de 17,4–17,6 ms em todas as corridas.
- **Estresse:** liberou 10.300 imagens, 2,3 GB somados com recriações; os alvos substituídos também foram liberados.
- **Capturas** (Sweet Sweet Canyon, Thwomp Ruins e a 5ª corrida no Estádio): imagem correta, sem textura preta ou faltando.
- **Custo:** todos os quadros acima de 100 ms continuam nas transições entre corridas. No modo padrão, algumas transições ganharam um quadro a mais de ~100–250 ms (16 quadros/3,4 s contra 13/2,9 s desligado), compatível com recriar texturas liberadas durante a corrida quando a tela volta. Para trocar VRAM por menos recriação, `MK8_TEXTURE_IDLE_S` pode ser maior.

**Teste:** `tests/test_texture_retire.py` extrai `mk8_retire_image` do `textures.c` preparado e cobre: textura da CPU ociosa liberada, recente mantida, alvo não substituído mantido, substituído depois do último uso liberado e antes mantido, apresentada, com vídeo ou com alias mantidas, um único fim de passe, snapshots esquecidos. Também confere o carimbo sempre ativo e a recriação vindo antes em `tex_backend`. `tests/test_pool_release.py` ganhou o campo novo e extrai só até o código novo.

**Não validado:** GPU de 4 GB ou integrada, onde o ganho importa mais; telas fora da corrida (menus, MKTV, multijogador); sessões de várias horas.

**Hipótese descartada.** A resolução variável não vem da medição de GPU. Com `MK8_TIMESTAMP_TOP_GPU=1` (início e fim medidos na GPU, em vez de misturar relógio do host e GPU), `race-grand-prix-a_yjfgwl` usou os mesmos ~6 tamanhos de alvo que o padrão (`x0sdgcp3`), com a mesma VRAM e 60 FPS. O padrão não muda.

**Cache de vértices por idade (`toolchain/vertex_cache_age.py`, padrão; `MK8_VCACHE_IDLE_S=<s>`, 60 por padrão, 0 desliga).**
- **Como funciona:** a cada ~10 s, `vc_flush` marca a submissão pendente e descarta as faixas sem uso desde a marca de ~60 s antes. O descarte passa por `vc_drop`, que só libera o buffer depois que a GPU termina as submissões que podiam lê-lo. A varredura para antes de encher a lista de descarte, para não esperar a GPU.
- **Medição** (GP completo `race-grand-prix-vunwgxoi`, contra `race-grand-prix-2avu_1go`): o cache ficou em ~21–26 MB em vez de subir até 98 MB, e as alocações caíram de ~8.350 para ~4.100. FPS de 59,99–60,00 nas quatro corridas, p99 de 17,5 ms, sem `FATAL`. A pista seguinte copia de novo só o que usa.
- **Teste:** `tests/test_vertex_cache_age.py` usa a tabela real, `vc_drop` e `vc_free_dead` do `exec.c` preparado. A entrada ociosa sai entre 60 e 70 s após o último uso, a usada fica, a da submissão pendente nunca sai, os buffers só são destruídos depois da GPU e `=0` desliga.

Runtime `--runtime-only`, candidato `ee1f252a…`.

**Folga de CPU na corrida.** `MK8_SAMPLE=6000:8`, `race-grand-prix-ofuel2rl`, a 60 FPS nesta máquina (Xeon E5-2666 v3 Haswell, 2,9 GHz, por núcleo parecido com uma i7-3770). A thread de tradução, com desenhos e driver AMD, usa 43% de um núcleo, ~7 ms por quadro; a principal do jogo, 31%; as demais, 26% e 18%. Uma CPU com metade do desempenho por núcleo ainda caberia em 60 FPS na corrida. O relato de 1 FPS (#7: GTX 1070 + i7-3770) não se explica por CPU e precisa do log dele. `LARECOMP_LOG_PACE` não serve para isso: o "ocupado" sai igual ao período, porque a espera do ritmo nessa thread não é contada como bloqueio.

## Congelamento ao cruzar a chegada: Yaz0 nativo e preempção de núcleo — 14/11/2026

**Sintoma.** No fim de toda corrida (e no primeiro carregamento), quadros de 475–564 ms só em `other`, com o log mostrando uma thread do jogo esperando ~260 ms por um núcleo emulado. O `kernel.c` fixado entrega o núcleo apenas em chamadas de kernel (`core_check` em `rt_svc`), sem preempção dentro do código guest.

**Duas inversões de prioridade:**
- **Núcleo 2:** um worker de prioridade 52 descompactava os arquivos da próxima tela em `f_7000a88a44`, que é o decodificador **Yaz0** do jogo (constante `Yaz0`, retornos -1/-2). São dois arquivos de ~92 MB por fim de corrida. Como código recompilado, com registradores em memória e acessos pelas macros guest, o laço levava ~0,5 s com o núcleo preso.
- **Núcleo 0:** um worker de prioridade 45 monta a próxima tela por ~0,5 s sem chamar o kernel, enquanto a thread principal (44) espera. `MK8_SAMPLE` na janela da transição não achou função dominante: é lógica de jogo espalhada.

**Yaz0 nativo (`toolchain/yaz0_native.py`, `runtime/mk8/support/yaz0.c`; `MK8_YAZ0_GUEST=1` volta ao laço guest).**
- **Patch:** uma linha antes do laço, na cópia de build de `main_230.c`, conferida por SHA-256 e com CRLF normalizado. O cabeçalho (magic e tamanho pelos helpers de ordem de bytes do próprio jogo, retornos -1/-2 e `size < 1`) continua no guest.
- **Laço nativo:** faz as mesmas leituras e escritas, na mesma ordem, inclusive o excesso da última cópia, e deixa x8–x13 e as flags como o guest deixava. `mk8_yaz0_loop` foi exportado pelo executável.
- **Medido:** 94.817 KB em 192 ms e 91.736 KB em 205 ms.
- **Efeito:** a espera no núcleo 2 sumiu dos logs. O congelamento visível continuou, porque vinha do núcleo 0.
- **Teste:** `tests/test_yaz0_native.py` extrai, no momento do teste, o laço do `main_230.c` gerado localmente (nada privado entra no repositório) e compara o laço do jogo com o nativo, junto de um decodificador independente em Python. Cobre literais, cópias curtas, longas e sobrepostas, e um tamanho menor que o fluxo. Com um erro de comprimento ou sem as flags, o teste falha.

**Preempção nas chamadas indiretas (`toolchain/core_preempt.py`, padrão; `MK8_NO_PREEMPT=1` desliga).**
- **Ponto de checagem:** `rt_dispatch`, por onde passa toda chamada indireta do guest (virtuais, ponteiros de função). Lê uma flag global, ligada só enquanto algum núcleo tem uma thread esperando com prioridade melhor que a do dono.
- **Caminho lento:** faz o mesmo que `rt_svc` (`core_check` sob `g_klock`), com `trylock` para nunca esperar pelo lock ali.
- **Por que é seguro:** continua uma thread por núcleo de cada vez. Uma thread de prioridade melhor tomar o núcleo numa chamada indireta é uma intercalação que o Horizon permite em qualquer instrução, e uma chamada guest já podia ceder o núcleo na próxima chamada de kernel.
- **Teste:** `tests/test_core_preempt.py` roda as funções reais de núcleo do `kernel.c` preparado com threads reais. A espera da thread de prioridade melhor cai para 0,0 ms (era ~220 ms), sem sobreposição no núcleo. Com prioridade igual ou desligado, espera o trabalho inteiro.

A/B no mesmo build (`b7180f73…`), cache `race-grand-prix-xh0dhyxh`, fim da corrida 1:

| Execução | Pior quadro na transição | Quadros > 50 ms (soma) | Espera de núcleo | Corrida 3000–14000: > 20 ms |
| --- | --- | --- | --- | --- |
| `yscjaxvn` / `par8c_om`, desligada | 533 / 564 ms | 1.458 / 1.464 ms | 262 / 264 ms | 30 / 38 |
| `5p3rtv2d` / `3c3pphsa`, ligada | 337 / 300 ms | 760 / 685 ms | nenhuma | 7 / 7 |

O congelamento caiu pela metade e os pequenos tropeços de 20 ms+ durante a corrida também diminuíram. A média continua em 60 FPS. A captura do quadro 15600 mostra a corrida 2 iniciada normalmente.

**Ainda resta:** ~190 ms de trabalho do jogo e o primeiro quadro da tela nova, ~300 ms, dos quais ~250 ms são tradução das listas e prefetch.

**GP completo com tudo ligado** (preempção, Yaz0 nativo e cache de traduções), `race-grand-prix-2avu_1go`, contra o GP anterior `race-grand-prix-xh0dhyxh`:
- **Estabilidade:** saída 0 em 1.086 s, sem `FATAL`/exceção e sem espera de núcleo registrada.
- **Yaz0:** 142 arquivos descompactados nativamente, 579 MB em 1,3 s.
- **Corridas:** 59,95–60,00 FPS e p99 17,56–17,59 ms nas quatro.
- **Transições:** antes, cada uma tinha três quadros de 294–614 ms, com 455–613 ms parados em `other`. Agora são um ou dois quadros de 202–375 ms, com no máximo 219 ms em `other`. Os quadros 15999/31999/47999 são capturas do próprio teste e ficam fora da comparação.

Runtime final `--runtime-only`, candidato `c9b9aaa4…`. Passaram 80 testes relacionados mais `test_native`, sem mudança em Rust. **Não validado:** máquina mais fraca, multijogador, áudio e controle manual durante as transições. `gameplay_verified` continua falso.

## Investigação ampliada: shaders, concorrência e medição — 10/11/2026

Aplicado `toolchain/shader_storage.py` pela preparação normal, com limites de substituição conferidos. As tabelas de programas (4.096 slots) e pipelines (8.192 slots) tinham sondagem sem limite: uma tabela cheia não terminava a busca de uma chave nova. Agora crescem antes de 75% de ocupação, preservam os ponteiros dos objetos e verificam overflow/alocação. Isso evita a saturação da busca; **não limita o consumo dos próprios shaders/pipelines nem implementa sua expulsão da GPU**.

Pedidos repetidos do mesmo pipeline enquanto o programa compila agora compartilham uma solicitação. Quando a tabela separada de pendências fica cheia, retorna NULL para o chamador liberar o candidato e deixar a construção para o draw, em vez de compilar um pipeline que não poderia ser recuperado. A disputa entre worker e execução inline agora decide quem executa sob o mutex da fila: não sobra um ponteiro retirado da fila, mas ainda passível de ser tomado e concluído por outra thread. Os testes de fila e duplicação falharam nas assertions esperadas ao desativar apenas esta adaptação em cópias temporárias; a saturação de pendências também reproduziu falha no código anterior.

O salvamento periódico inclui pipelines construídos antes de a thread de gravação iniciar. O teste de relógio simulado confirma gravações em 60, 120 e 310 segundos, respeitando o intervalo mínimo e o fim das rajadas. O analisador `scripts/frame-stats.py` aceita resíduos negativos de contabilidade assíncrona sem perder o quadro completo e informa linhas inválidas rejeitadas; totais nulos/não finitos não entram no FPS.

**Correção da evidência anterior:** o compilador com `-O1` define `NDEBUG`. Alguns testes nativos, inclusive os de descritores recém-adicionados, não restauravam `assert`; a execução anterior dos 39 testes não comprovava todas as verificações descritas abaixo. Adicionado `-UNDEBUG` aos comandos afetados e repetida a suíte. A fixture de descarte foi corrigida para aceitar invalidação de views por ponteiro mesmo após a imagem ser aposentada, como faz o código real, e passou a verificar explicitamente sete descartes únicos, sem handle nulo nem duplicação.

Validação atual: suíte ampliada com 46 testes, sem testes ignorados; inclui os 11 de descritores com assertions ativas, 20.000 entradas em cada tabela de shaders/pipelines com colisões, 20.000 pedidos repetidos, tabela de pendências cheia e disputa determinística entre threads. São executáveis nativos com fixtures sem GPU, mais verificações da preparação e do analisador. Compilação completa Windows: 212 unidades e link concluídos. Novo SHA-256 de `local/performance-validation/host/mk8drecomp.exe`: `4377658d6bcd5919b98e6267af04314759d0ad074e9fa72f5476a3d66a696172`. O executável é de desenvolvimento, sem biblioteca do jogo; não é um pacote jogável independente.

Nenhuma partida ou medição de VRAM/FPS foi executada nesta rodada. Permanecem pendentes a qualificação em GPU real, caches de pipelines ao reiniciar e retenção de listas durante sessões extensas. `MK8_RELEASE_POOL_TEXTURES` continua desligado por padrão e `gameplay_verified` continua falso. Usar o roteiro abaixo na outra máquina, incluindo uma execução com cache frio e outra com cache aquecido, mesma resolução e pistas; repetir por pelo menos uma hora, registrando transições e verificando se a memória estabiliza ou cresce a cada copa.

## Liberação de descritores e validação sem jogo — 10/11/2026

Implementadas nas fontes locais as correções de ownership encontradas na auditoria abaixo. `toolchain/descriptor_lifetime.py` é aplicado pela preparação normal do host; não exige ativar um experimento. Finalizar pools de texturas ou samplers zera o handle guest e libera sua referência, mantendo o objeto enquanto comandos gravados/copiados ou o executor ainda o utilizam. O prefetch empresta as referências, sem duplicá-las. A destruição das entradas ocorre na thread de tradução; os backends de textura continuam usando o descarte GPU adiado existente. O modo síncrono drena pools aposentados antes de executar novas submissões.

O histórico de cubemaps passou de 64 slots estáticos para registros por pool, removidos quando sua última referência acaba. Continua preservando o cubemap quando o jogo reutiliza o slot com uma textura 2D: a correção de iluminação não foi desligada. O reset agora aguarda a tradução antes de tocar no renderer, elimina gravações inacabadas, listas ativas/aposentadas e referências do executor, e libera pools antes de invalidar os backends. Os buffers de comandos são rastreados para que suas listas sem faixa de memória também participem desse descarte. Não se antecipa o prazo de 20.000 listas durante gameplay normal.

A revisão da integração encontrou duas referências ausentes adicionais: a apresentação enfileirada e `g_last_presented`, usado para reapresentar o último quadro. Ambas agora mantêm a textura até seu último uso; reapresentar a mesma textura retém antes de liberar. Corrigida também a leitura de `it->handles` antes de verificar se a alocação de `it` falhou. Tamanhos inválidos e falhas de alocação de pools/entradas interrompem com diagnóstico, sem retornar sucesso fictício. `MK8_TRACE_VRAM` agora informa pools vivos, finalizados ainda retidos, referências, descarte pendente e bytes de entradas no host; esse último contador não representa VRAM.

**Validação sem ROM/save:** suíte da área com 39 testes, incluindo 11 novos testes nativos de descritores. Cobrem pool finalizado atrás de apresentação bloqueada, destruição na thread correta, comandos copiados e estado persistente, prefetch sem referência extra, 130 pools simultâneos, 200 ciclos de criação/finalização de texturas e samplers, reutilização do endereço guest enquanto o pool anterior continua gravado, oito threads/80.000 pares de retain/release, reset das quatro categorias de listas, repetição de finalização/reset, apresentação enfileirada/repetida, modo síncrono e falhas de alocação injetadas. O teste de falha distingue o diagnóstico explícito de uma exceção por acesso nulo. A suíte também cobre aliases de imagens, orçamento de shaders, fences, pacing e salvamento do cache já existentes.

Compilação completa do host Windows concluída: 212 unidades e link, incluindo os consumidores da estrutura `NvnDescPool` modificada. Artefato local de validação: `local/performance-validation/host/mk8drecomp.exe`, SHA-256 `d1203d7b632579098ddd819ebdc8ab95aaa267aaa6eca6c40a38580d3f70db4d`. Usa o perfil de desenvolvimento e não contém a biblioteca recompilada do jogo; não é instalador nem substituição independente de uma instalação preparada. Upstream conferido pelo manifesto, sem alteração das fontes originais. Foram provisionados Python 3.12, compilador e dependências locais para esses testes; a limitação de ferramentas descrita na auditoria anterior foi superada. Nenhuma partida executada e nenhuma release publicada.

### Reproduzir e validar em outra máquina

1. Executar `scripts/setup-tools.ps1` para provisionar Python 3.12 e pacotes. Com `local/python/Scripts/python.exe`, executar `scripts/fetch-upstream.py smm2recomp`, `scripts/setup-native.py` e `scripts/setup-runtime-deps.py`, nesta ordem, para obter fontes fixadas, compilador e dependências gráficas. O setup de Python sozinho não instala os três.
2. Executar `local/python/Scripts/python.exe scripts/verify-graphics.py`. O comando verifica hashes do upstream e recusa sucesso com pré-requisitos ausentes ou testes ignorados. Não requer dados comerciais.
3. Na instalação de desenvolvimento com jogo já preparado, recompilar pelo caminho existente: `local/python/Scripts/python.exe toolchain/build.py --config <config.json> --local <pasta-de-dados> --runtime-only`. Usar os caminhos reais dessa instalação, sem substituir saves nem copiar DLLs/EXE isoladamente. A alteração de estruturas exige todos os consumidores do host recompilados, o que o cache de headers do pipeline invalida automaticamente.
4. Para a sessão de diagnóstico, iniciar pelo launcher com `LARECOMP_VK_PROF=1`, `MK8_FRAME_TIMES=1` e `MK8_TRACE_VRAM=1` no ambiente. Manter `MK8_RELEASE_POOL_TEXTURES` desligado e os demais padrões. Fazer três copas consecutivas no mesmo processo, visitar menus entre elas e testar retorno ao título/reinicialização. Conferir imagem continuamente, iluminação/reflexos, áudio, controle, cronômetro e save. Testar também Contra o Relógio e, separadamente, tela dividida/multiplayer se forem usados.
5. Guardar o `game.log` de cada comparação antes da rotação. Analisar janelas equivalentes com `python scripts/frame-stats.py <game.log> --from <primeiro-quadro> --to <último-quadro>`. Comparar mesma máquina, pistas, resolução e cópias do mesmo cache inicial. Separar primeira visita/repetição e excluir capturas. Registrar p95/p99, máximo, quadros acima de 33,4/50 ms e memória ativa/reservada/dedicada/compartilhada em cada transição. Encerrar o launcher e remover as variáveis de diagnóstico antes da medição final sem instrumentação.

**Limites:** estes testes comprovam os caminhos de liberação exercitados e a compilação, não ausência universal de vazamentos ou estabilidade visual. Pools que o jogo não finaliza e listas ainda referenciadas continuam vivos. A reserva/fragmentação da VMA, o custo de primeira compilação e os draws omitidos pelo orçamento de shaders permanecem investigações separadas; seus padrões não foram alterados sem evidência de corrida. A falha experimental de `MK8_RELEASE_POOL_TEXTURES` continua sem solução demonstrada. Ainda falta validar resets com GPU real, sessões prolongadas, múltiplos drivers, relógio, áudio e saves; `gameplay_verified=false`. Não há medição nova de FPS ou redução de VRAM nesta rodada.

## Auditoria de VRAM e engasgos — 10/11/2026

Revisão estática do checkout, sem executar o jogo. Esta cópia não contém `local/`, build, logs privados das sessões anteriores nem o compilador nativo configurado. As medições abaixo são evidências históricas deste documento, não novas medições. O fetch completo do upstream falhou ao renomear a pasta temporária (WinError 5); não se contornaram permissões. Os arquivos `objects.c`, `cmdbuf.c` e `vulkan/textures.c` do commit fixado `9dd1858fe8f4688ae5daa390c5e18d027300ada6` foram lidos remotamente, conferindo SHA-256 com o manifesto após normalização LF/CRLF. Nenhum runtime ou padrão foi alterado.

Achados, em ordem de investigação:

1. **Alta — finalização de pools de descritores sem liberação.** No upstream fixado, `runtime/nvn/objects.c:554–571`, `desc_pool_init` aloca o pool e suas entradas, mas `nvnTexturePoolFinalize` é vazio. Nenhuma adaptação em `toolchain`/`runtime/mk8` substitui essa finalização. `toolchain/texture_lifetime.py` acrescenta uma referência forte por entrada registrada; essa referência só cai quando o slot é sobrescrito. Finalizar um pool não libera suas entradas, referências ou registro. Isso deixa texturas e potencialmente imagens Vulkan vivas após a finalização do dono. É uma falha concreta de ciclo de vida; sua contribuição na corrida relatada ainda precisa de rastreio, pois a sessão histórica `ddpqpo1_` não registrou finalização de pool de descritores. Não confundir esse pool com `nvnMemoryPoolFinalize`, já tratado pelas correções anteriores.
2. **Alta — histórico de cubemaps sem descarte.** `toolchain/texture_pool.py:30–65` mantém tabelas para até 64 pools e uma referência por cubemap histórico. Não há remoção de pool, limpeza das tabelas ou hook de reset desse histórico. Trocar o slot por uma textura 2D preserva deliberadamente o cubemap para evitar a cintilação já diagnosticada; isso também prolonga sua vida. Depois de 64 pools distintos com histórico, criar outro histórico falha silenciosamente. Os testes atuais cobrem substituição do slot, mas não finalização/reset nem saturação. A correção precisa preservar as referências consumidas pela tradução assíncrona; desativar o histórico indiscriminadamente reintroduz a falha de iluminação.
3. **Média — descarte de comandos por contagem, não por conclusão de uso.** `cmdbuf.c:63–88` conserva listas aposentadas por mais de 20.000 novas listas, e `texture_lifetime.py` preserva esse mecanismo. `runtime/mk8/graphics/command_texture_refs.inc` só libera suas texturas quando a lista é descartada. Isso pode manter recursos após voltar ao menu se a produção de listas diminui; o limite é de listas, não de bytes. É retenção potencial, não prova de vazamento ilimitado. Antes de substituir o prazo, rastrear o último uso por comandos reutilizáveis, tickets da tradução e conclusão GPU; simplesmente reduzir 20.000 não prova segurança.
4. **Alta para fidelidade visual — o orçamento de shaders omite draws.** `toolchain/shader_budget.py:43–100` usa 4 ms por quadro por padrão e retorna sem o draw quando a compilação não ficou pronta. Pode explicar objetos/efeitos que somem momentaneamente mesmo com 60 FPS. A elegibilidade usa `TexBackend.scalable` como aproximação de alvo redesenhado a cada quadro; essa propriedade não prova que o conteúdo seja descartável. Outros alvos e compute continuam esperando completamente. Portanto, 4 ms não é limite de duração do quadro nem elimina pausas de compilação. Evidência histórica separada: Water Park teve 343 ms esperando shader; repetições com cache reduziram esses picos.
5. **Média — reserva da VMA não é limite de VRAM.** `toolchain/host.py:243` prefere blocos de 128 MiB, sem estabelecer orçamento total. No GP histórico completo `dka1cgd3`, o heap 0 terminou no menu com 1.648 MiB reservados e 923 MiB ativos. Há reserva excedente e recursos ainda ativos; os números não demonstram, isoladamente, vazamento nem esgotamento da GPU. `toolchain/vram_trace.py` esclarece que a estimativa VMA não usa `VK_EXT_memory_budget`; memória compartilhada também inclui o heap guest importado. Faltam dados para atribuir os engasgos tardios especificamente a pressão/residência de VRAM.
6. **Risco já demonstrado — descarte agressivo não resolveu a sessão longa.** `toolchain/pool_release.py` mantém `MK8_RELEASE_POOL_TEXTURES` desligado por padrão. O comparativo histórico elevou o pico reservado de 2.272 para 2.528 MiB e, na quarta pista, apresentou 50,91 contra 59,52 FPS, com aumento de `SyncWait` mesmo sem novas compilações. Não promover essa opção como solução para VRAM. A causa dessa regressão continua aberta.

Próxima validação: instrumentar criação/finalização e referências por pool (descritores, histórico, comandos e dono guest), bytes de imagens e descarte pendente, junto dos frametimes e tempos de shaders/prefetch/SyncWait. Repetir a mesma copa pelo menos três vezes no mesmo processo, registrando entrada/saída de pista e menu; usar o mesmo cache inicial nas comparações. Separar primeira visita de repetição, capturas de janelas cronometradas e memória ativa de reservada/dedicada/compartilhada. Priorizar a correção de ownership dos pools antes de qualquer expulsão por idade. Validar finalização com trabalho ainda na fila, múltiplos aliases, reset e mais de 64 ciclos de pool. Não há correção de performance validada nem nova qualificação de gameplay nesta auditoria.

