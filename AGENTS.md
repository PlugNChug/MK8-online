# Trabalhar neste projeto

Leia `README.md`, `docs/pt-BR/STATUS.md` e o registro técnico em `docs/pt-BR/log/`. Esses arquivos são a documentação operacional; atualize-os quando o estado mudar. Não crie um relatório novo para cada tentativa.

- Faça a menor mudança completa. Preserve a separação entre launcher, pipeline, runtime e dados privados; não crie pacotes para funcionalidades hipotéticas.
- Adaptações próprias ficam em `runtime/mk8` e `toolchain`. Não edite `upstream` nem código gerado diretamente: a preparação verifica hashes e aplica mudanças à cópia de build.
- Mudanças de ABI/estrutura exigem recompilar seus consumidores. Hooks de guest devem validar versão/hash e rejeitar limites inesperados. Nunca retornar sucesso fictício para passar de uma falha.
- Perfis ativos são MK8 4.0.1 AArch64; apoio genérico a subsdk1–4 não significa teste com esses módulos comerciais. Identificadores serializados antigos e nomes de ambiente/ABI são contratos, não texto para renomeação cosmética.
- Preserve `local/config.json`, `local/mii-profile.json`, `local/profiles`, `local/user*`, entradas e saves. Não copie ROM, chaves ou código gerado para fontes/testes públicos.
- Reutilize as pastas de build existentes. Antes de qualquer limpeza, confira caminho absoluto, manifestos e processos; não siga junctions. A reserva mínima de disco é 4 GiB. Não contorne uma exclusão bloqueada com outro mecanismo.
- Para launcher, compile Rust. Para runtime sem mudança de guest, use `--runtime-only`. Para guest, regenere/recompile as unidades afetadas. Não confunda iniciar uma build com sua conclusão.
- Execute testes da área alterada, formatação e Clippy quando mudar Rust; teste navegador quando mudar interface. Um teste nativo deve exercer o comportamento, não só procurar texto no código.
- Qualificação requer evidência: menu, corrida inteira, reinício/save, imagem, áudio, controle e velocidade do relógio. `gameplay_verified` permanece falso até isso acontecer. Multiplayer exige validação separada em duas máquinas.
- Perfis/GLB do editor não são modelos integrados ao motor MK8. Não reative consumidores gráficos Mii com ponteiros ou arquivos vazios.
- Referências externas e atribuições ficam em `docs/pt-BR/THIRD_PARTY.md` e licenças. Preserve avisos de licença de dependências; não transfira resultados de outros projetos para esta build.

---

## Agent Handoff (always active)
<!-- agent-handoff:v3 -->

You are part of a multi-agent workflow. Multiple AI agents work on this project
at different times. Each agent has isolated conversation history. A shared `.ai/`
directory bridges that gap.

**ON EVERY CONVERSATION START — before doing any work**, read these files:

1. `.ai/PROJECT.md` — Tech stack, architecture, conventions
2. `.ai/PATHS.md` — Key files and reference document index
3. `.ai/PLAN.md` — Current plan and task status
4. `.ai/conversations/HANDOFF.md` — What other agents did, what's active, blockers

If any file is missing, empty, or still contains installer placeholders, repair the
handoff system before continuing: create the `.ai/` directory structure, scan the
project, and populate the context files. See the templates in the agent-handoff
skill reference files for exact formats.

Before writing timestamps or session filenames, get the real local time from the
environment (`date '+%Y-%m-%d %H:%M %Z'` and `date '+%Y-%m-%d/%H%M%S'` on Unix-like
systems). Do not infer the date from model memory, old docs, or previous handoff
entries.

**BEFORE YOU FINISH EVERY RESPONSE**, complete the handoff write-back. Do not stop
after only reading handoff context.

- **Always:** Append to `.ai/conversations/LOG.md`.
- **Always for file changes, tests, investigations, plans, or decisions:** update
  `.ai/conversations/HANDOFF.md` and create a session file in
  `.ai/conversations/sessions/YYYY-MM-DD/`.
- **If architecture/design decision:** Write to `.ai/conversations/decisions/`.
- **If project structure changed:** Update `.ai/PATHS.md`.
- **If plan status changed:** Update `.ai/PLAN.md`.
- **In your final response:** mention that handoff was updated, or explain why no
  handoff write was needed.

Session filenames must be unique and sortable:
`.ai/conversations/sessions/YYYY-MM-DD/HHMMSS-agent-task-slug.md`.

Identify yourself by agent name in all writes. For simple Q&A with no file changes,
a one-liner in LOG.md is sufficient.
