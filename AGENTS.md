# Trabalhar no MK8-online

Responda no idioma da conversa em curso. Ao editar a documentação, preserve o idioma de cada arquivo. Quando houver versões em inglês e português brasileiro, atualize as duas na mesma mudança.

## Antes de mudar código

- Leia `README.md` e o estado técnico da branch, em `docs/STATUS.md` ou `docs/pt-BR/STATUS.md`. Onde houver `docs/pt-BR/log/`, consulte o registro técnico da área. Consulte o roteiro da branch (`docs/ROADMAP.md` ou `docs/pt-BR/ROADMAP.md`) para prioridades e `.github/workflows/ci.yml` para as verificações automatizadas. Confira o estado do Git e os arquivos já modificados. Baseie o diagnóstico no código e na build realmente usados no teste.
- Siga o fluxo real da operação antes de editar. `apps/launcher` e `crates/` cuidam da interface, instalação, supervisão e sessões. `games/mk8d` descreve perfis e símbolos. `toolchain` prepara e recompila o jogo. `runtime/mk8` adapta os serviços e o comportamento em execução. `scripts` e `tests` contêm os roteiros e regressões existentes.
- Faça a menor mudança completa que corrige a causa. Procure os chamadores, contratos e testes afetados. Reutilize os mecanismos do projeto e mantenha a separação entre launcher, pipeline, runtime e dados privados. Crie pacotes apenas quando houver uma necessidade concreta. Se houver subagentes, delimite arquivos e responsabilidades, serialize builds que compartilham saídas e revise as alterações antes de integrá-las.

## Fidelidade ao jogo de referência

- Para o comportamento padrão, use como referência o jogo original da mesma versão, no mesmo cenário. Registre a versão, pista ou menu, trecho, configurações e condições relevantes. Compare com observação direta ou com capturas, traces e outros registros verificáveis. Indique quando a referência não estiver disponível ou a comparação for parcial.
- Preserve imagem, áudio, controles, fluxo de menus, saves e tempo de simulação. Meça FPS de apresentação e velocidade do relógio separadamente. Ao comparar desempenho, controle resolução, cache de shaders, geração de quadros e captura. Ao comparar imagem, use o mesmo trecho e enquadramento sempre que possível.
- Em mods ou melhorias opcionais, registre a diferença intencional em relação ao original e exercite o caminho padrão. Use os mecanismos de ativação e carregamento existentes. Perfis e GLBs do editor não são modelos integrados ao motor MK8. Mantenha consumidores gráficos Mii desativados quando faltarem recursos válidos.

## Recompilação estática e mods

- Identifique a camada antes de escolher a correção. O guest AArch64 é traduzido para C e compilado para o host x64 da build, em Windows ou Linux conforme a branch. Uma instrução traduzida incorretamente pertence ao caminho de geração. Um serviço com semântica incorreta ou um recurso GPU liberado cedo demais pertence ao runtime. Um mod de recurso deve passar pelo importador ou overlay apropriado.
- Mantenha adaptações em `toolchain` e `runtime/mk8`. Para patches estáticos, aplique a transformação na preparação da cópia de build e regenere as unidades afetadas. Preserve os snapshots fixados em `upstream` e suas verificações de hash. Faça mudanças no gerador ou na preparação para alterar os arquivos gerados. Para mods em execução, use os pontos de extensão existentes e respeite o ciclo de vida dos recursos entre carregamento, troca de pista e reinício.
- Antes de alterar um hook, confirme versão, hash, módulo e endereço ou símbolo. Confirme assinatura, convenção de chamada, tamanho, alinhamento e offsets das estruturas envolvidas. Rejeite versões, assinaturas e limites inesperados. Preserve os resultados reais do jogo e dos serviços e propague suas falhas. Recompile todos os consumidores de uma mudança de ABI ou layout compartilhado.
- Os perfis ativos são MK8 4.0.1 AArch64. Suporte genérico a `subsdk1`–`subsdk4` não comprova execução dos módulos comerciais. Preserve identificadores serializados, formatos, nomes de ambiente e ABI para dados e builds existentes.
- Ao diagnosticar a tradução, associe o endereço guest ao módulo e compare os operandos e efeitos da instrução com o C gerado. Em falhas gráficas intermitentes, siga o recurso desde a criação e gravação dos comandos até a submissão e a conclusão. Verifique quem mantém a referência, em qual thread ela é liberada e quando o conteúdo passa a estar visível para leitura.

## Investigar e verificar

1. Em uma issue, confira a versão relatada, os passos, os logs e correções já existentes. Tente reproduzir na base pertinente. Registre commit ou identidade do executável, configuração e resultado observado. Trate relatos, hipóteses e resultados confirmados de acordo com a evidência disponível.
2. Isole a primeira divergência. Use os diagnósticos existentes e varie uma condição por vez quando possível. Use `scripts/test-race.py` para cenários de corrida compatíveis, conforme o README. Prefira um caso mínimo que falhe antes e passe depois, com um teste que execute o comportamento. Repita o cenário original e um caso afetado próximo.
3. Use a build adequada. Compile o launcher em Rust. Para uma mudança só no host com guest já válido, use `--runtime-only`. Para mudanças no guest ou no gerador, regenere e recompile as unidades afetadas. Acompanhe cada build até o término e verifique o artefato realmente usado no reteste.
4. Rode os testes da área. Para Rust, inclua `cargo fmt --all -- --check`, Clippy conforme a CI e os testes dos componentes afetados. Para interface, teste no navegador. Para código nativo, exercite o comportamento compilado. Informe falhas de ambiente, testes pulados e verificações que dependam de hardware ou dados indisponíveis.
5. Mantenha `gameplay_verified=false` até haver evidência de menu, corrida inteira, reinício e persistência, imagem, áudio, controles e velocidade do relógio. Qualifique multiplayer separadamente em duas máquinas, com entrada na mesma sala e corrida completa.

## Ferramentas, dados e armazenamento

- Prefira ferramentas e scripts existentes. Confira o escopo, os caminhos, as entradas e os efeitos antes de executar comandos que façam downloads, gravem dados ou iniciem processos. Use as fontes e verificações de integridade do pipeline. Identifique os processos que iniciou e espere sua conclusão ou registre o que ficou em execução.
- Preserve `local/config.json`, `local/mii-profile.json`, `local/profiles`, `local/user*`, entradas e saves. Use cópias isoladas para testes que gravem estado. Mantenha ROMs, chaves, código gerado do jogo e dados privados fora de fontes, fixtures e commits. Revise logs e anexos antes de compartilhar.
- Reutilize as pastas de build existentes e respeite a reserva mínima de 4 GiB. Antes de qualquer limpeza, confira caminhos absolutos, manifestos e processos que usam os arquivos. Não siga junctions nem contorne uma exclusão bloqueada. Ao investigar uso de disco, confira a identidade dos arquivos com hard links antes de concluir que há cópias físicas.

## Git e entrega

- Confira `git status`, o diff e a URL do remoto. Novas branches devem partir do `fix-chore` atualizado de `Vnnc1/MK8-online`, também a base para integração e PR, salvo instrução explícita diferente. Confirme o remoto pela URL, pois o nome varia entre clones. Isole o trabalho em uma worktree quando a cópia atual tiver alterações alheias. Preserve o trabalho existente e evite reescrever histórico compartilhado.
- Revise o diff e a área de stage. Faça commits pequenos e coesos, com a correção e sua regressão juntas quando aplicável. Inclua apenas arquivos da tarefa e rode as verificações pertinentes antes de cada commit. Reserve pushes, PRs, merges remotos e comentários públicos para solicitações explícitas. Escreva em inglês os títulos e as descrições das PRs que abrirmos.
- Atualize `README.md`, o estado técnico e o registro técnico da área quando o estado documentado mudar. Reúna as tentativas de um mesmo diagnóstico no registro existente e acrescente evidências à seção correspondente quando couber. Guarde referências e atribuições em `docs/THIRD_PARTY.md` ou `docs/pt-BR/THIRD_PARTY.md`, conforme a branch, e preserve os avisos de licença das dependências. Resultados de outra build ou de outro projeto precisam de verificação própria antes de serem atribuídos a esta build.
- Na entrega, informe o que foi corrigido ou isolado, a branch e os commits quando houver, as verificações executadas e qualquer lacuna que ainda afete a conclusão.

## Continuidade entre agentes (sempre ativa)

<!-- agent-handoff:v3 -->

Agentes diferentes trabalham neste projeto em momentos distintos e têm históricos de conversa separados. Use o diretório compartilhado `.ai/` para manter a continuidade do trabalho.

**No início de cada conversa, antes de começar o trabalho**, leia os quatro arquivos a seguir.

1. `.ai/PROJECT.md` reúne as tecnologias, a arquitetura e as convenções do projeto.
2. `.ai/PATHS.md` indica os arquivos principais e a documentação de referência.
3. `.ai/PLAN.md` registra o plano atual e a situação das tarefas.
4. `.ai/conversations/HANDOFF.md` registra o trabalho dos outros agentes, o que está em andamento e os impedimentos.

Se algum arquivo estiver ausente, vazio ou ainda contiver texto provisório do instalador, restaure o registro antes de prosseguir. Crie a estrutura de diretórios de `.ai/`, examine o projeto e preencha os arquivos de contexto. Quando disponíveis, consulte os modelos de referência da habilidade `agent-handoff` para seguir os formatos esperados.

Antes de gravar datas, horários ou nomes de arquivos de sessão, consulte o relógio local do ambiente. No PowerShell, use `Get-Date -Format 'yyyy-MM-dd HH:mm zzz'` e `Get-Date -Format 'yyyy-MM-dd/HHmmss'`. Em sistemas Unix, use `date '+%Y-%m-%d %H:%M %Z'` e `date '+%Y-%m-%d/%H%M%S'`. Use o horário consultado em cada registro.

**Antes de concluir cada resposta**, registre o trabalho conforme as condições abaixo.

- Sempre acrescente uma entrada a `.ai/conversations/LOG.md`. Uma linha basta para uma pergunta simples sem alteração de arquivos.
- Para alterações de arquivos, testes, investigações, planos ou decisões, atualize `.ai/conversations/HANDOFF.md` e crie um arquivo de sessão em `.ai/conversations/sessions/YYYY-MM-DD/`.
- Para decisões de arquitetura ou de projeto, registre a decisão em `.ai/conversations/decisions/`.
- Se a estrutura do projeto mudar, atualize `.ai/PATHS.md`.
- Se a situação do plano mudar, atualize `.ai/PLAN.md`.
- Na resposta final, informe se atualizou o registro de continuidade. Se nenhuma atualização tiver sido necessária, explique o motivo.

Identifique-se pelo nome do agente em cada registro. Use nomes de sessão únicos e ordenáveis no formato `.ai/conversations/sessions/YYYY-MM-DD/HHMMSS-agente-tarefa.md`, com uma descrição curta da tarefa separada por hífens.
