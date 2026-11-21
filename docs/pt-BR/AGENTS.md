# Guia para agentes

[English](../../AGENTS.md) · **Português**

Estas regras são obrigatórias para agentes de IA que trabalham neste repositório. Os comandos e a estrutura estão em [CONTRIBUTING.md](CONTRIBUTING.md). A cópia em inglês na raiz traz as mesmas regras; mantenha as duas sincronizadas.

## Leia primeiro

1. [README.pt-BR.md](../../README.pt-BR.md) — o que é o projeto.
2. [STATUS.md](STATUS.md) — estado atual e qualificação.
3. [ROADMAP.md](ROADMAP.md) — pendências.
4. [registro técnico](log/README.md) — histórico técnico datado, dividido por tema. Abra só o arquivo do tema de que precisa.

Esses arquivos são a documentação operacional. Atualize-os quando o estado mudar, seguindo as regras abaixo.

## Documentação

**Dois idiomas.** Todo documento existe em inglês e em português: `docs/en/` e `docs/pt-BR/`. Os arquivos da raiz ficam em inglês e têm pares em português: `README.pt-BR.md`, `docs/pt-BR/AGENTS.md`, `docs/pt-BR/CONTRIBUTING.md`. Atualize as duas versões na mesma mudança e mantenha os mesmos títulos em cada uma. Nunca deixe um idioma para trás.

**Onde fica cada conteúdo.**

| Conteúdo | Arquivo |
| --- | --- |
| O que é o projeto, download, início rápido | `README.md` (só porta de entrada curta; sem changelog nem notas de correção) |
| Como usar um recurso | `docs/*/GUIDE.md` |
| Compilação, testes, estrutura | `CONTRIBUTING.md` |
| Mudanças de uma versão | `docs/releases/<versão>.md` (bilíngue; lido por `scripts/release.py`) |
| Estado atual e qualificação | `docs/*/STATUS.md` |
| Pendências | `docs/*/ROADMAP.md` |
| Investigações datadas, medições, evidências | `docs/*/log/<tema>.md` |
| Fontes externas e atribuições | `docs/*/THIRD_PARTY.md` |

**STATUS e registro.**

- Mantenha o `STATUS.md` curto e atual: substitua as linhas desatualizadas em vez de acrescentar.
- No registro, acrescente ao arquivo do tema existente, com a seção mais recente primeiro, uma seção por assunto e não por tentativa.
- Crie um arquivo de tema novo só se nenhum servir, e inclua-o nos dois índices do registro.
- Não crie arquivos de relatório avulsos.

**Estrutura e links.**

- Não mova nem renomeie documentos sem atualizar todos os links para eles.
- Mantenha os redirecionamentos `docs/STATUS.md` e `docs/THIRD_PARTY.md`; comentários no código citam esses caminhos.
- Links relativos devem resolver.
- Não acrescente documentos novos na raiz.

## Escopo e estrutura

- Faça a menor mudança completa. Preserve a separação entre launcher, pipeline, runtime e dados privados. Não crie pacotes para funcionalidades hipotéticas.
- As adaptações próprias ficam em `runtime/mk8` e `toolchain`. Nunca edite `upstream/` nem código gerado diretamente: a preparação verifica hashes e aplica as mudanças à cópia de build.
- Os perfis ativos são MK8 4.0.1 AArch64. O apoio genérico a subsdk1–4 não significa que esses módulos comerciais foram testados.
- Identificadores serializados antigos e nomes de ambiente/ABI são contratos, não texto para renomeação cosmética.

## Mudanças de guest e ABI

- Mudanças de ABI ou de estrutura exigem recompilar todos os seus consumidores.
- Hooks de guest devem validar versão/hash e rejeitar limites inesperados.
- Nunca retorne sucesso fictício para passar de uma falha.

## Dados privados e sistema de arquivos

- Preserve `local/config.json`, `local/mii-profile.json`, `local/profiles`, `local/user*`, entradas e saves.
- Nunca copie ROM, chaves ou código gerado para fontes ou testes públicos.
- Reutilize as pastas de build existentes. Antes de qualquer limpeza, confira o caminho absoluto, os manifestos e os processos em execução. Não siga junctions.
- Mantenha pelo menos 4 GiB livres em disco.
- Se uma exclusão for bloqueada, não a contorne com outro mecanismo.

## Build

| Mudança | Ação |
| --- | --- |
| Launcher | Compilar o workspace Rust |
| Runtime sem mudança de guest | `toolchain/build.py ... --runtime-only` |
| Guest | Regenerar e recompilar as unidades afetadas |

Iniciar uma build não é concluí-la. Espere a build terminar e confira o resultado.

## Testes

- Rode os testes da área alterada.
- Em mudanças de Rust, rode também `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets --locked -- -D warnings` e `cargo test --workspace --locked`.
- Em mudanças de interface, rode os testes de navegador (`python -m unittest discover -s tests -p test_launcher_e2e.py`).
- Um teste nativo deve exercer o comportamento, não só procurar texto no código.

## Qualificação

- A qualificação exige evidência de menu, corrida inteira, reinício/save, imagem, áudio, controle e velocidade do relógio.
- `gameplay_verified` permanece `false` até que tudo isso seja demonstrado.
- O multijogador exige validação separada em duas máquinas.

## Mii e gráficos

- Perfis e GLBs do editor não são modelos integrados ao motor do MK8.
- Não reative consumidores gráficos Mii com ponteiros ou arquivos vazios.

## Atribuição e licenças

- Referências externas e atribuições ficam em [THIRD_PARTY.md](THIRD_PARTY.md) e no par em inglês, com os textos das licenças em `docs/licenses`.
- Preserve os avisos de licença das dependências.
- Não transfira resultados de outros projetos para esta build.
