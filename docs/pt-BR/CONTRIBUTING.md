# Como contribuir

[English](../../CONTRIBUTING.md) · **Português**

Obrigado por ajudar. Esta página explica como compilar a partir do código-fonte e como o repositório está organizado. Também cobre testes e documentação. Agentes de IA também devem ler o [AGENTS.md](AGENTS.md), que reúne as regras obrigatórias do projeto.

## Regras básicas

- Use seus próprios arquivos do jogo. Nunca versione nem anexe ROMs, chaves do console, saves, código gerado ou recursos do jogo, inclusive em testes e fixtures.
- Mantenha as adaptações do MK8 em `runtime/mk8` e `toolchain`. A preparação as aplica a uma cópia verificada da build. Não edite diretamente os snapshots de `upstream/` nem o código gerado.
- Faça mudanças pequenas e completas e rode os testes da área alterada.
- Credite fontes externas em [THIRD_PARTY.md](THIRD_PARTY.md) e no par em inglês, e preserve os avisos de licença das dependências.

## Requisitos

- Windows x64
- Rust (stable, com `rustfmt` e `clippy`)
- Python 3.12 x64
- PowerShell 7
- .NET 10 x64 (para o importador)

Para o porte Linux, veja o [guia Linux](LINUX.md) e o submódulo `platforms/linux`.

## Primeiros passos

O desenvolvimento acontece na branch `fix-chore`. Na raiz do repositório:

```powershell
.\scripts\start-launcher.ps1
```

O script baixa as fontes upstream fixadas, confere os hashes, compila o launcher e o abre.

## Estrutura do repositório

| Local | Responsabilidade |
| --- | --- |
| `apps/launcher` | Launcher nativo, interface web, instalador, atualizações e relatórios |
| `crates/installer`, `crates/manifest`, `crates/build-manager` | Instalação, validação, Miis e supervisão de builds |
| `crates/session`, `crates/transport` | Salas, Steam, relay e transporte autenticado |
| `toolchain`, `runtime/mk8` | Pipeline de recompilação e adaptações do runtime do MK8 |
| `games/mk8d` | Perfis de versão, símbolos e configuração do jogo |
| `scripts`, `tests` | Preparação, empacotamento e testes de regressão |
| `upstream/manifests` | Manifestos das fontes fixadas; os snapshots baixados ficam fora do Git |
| `platforms/linux` | Porte Linux (submódulo) |
| `docs/en`, `docs/pt-BR` | Documentação, uma cópia por idioma |
| `docs/releases` | Notas de versão bilíngues, lidas por `scripts/release.py` |
| `local`, `target`, `prebuilt` | Dados privados e artefatos gerados, ignorados pelo Git |

## Compilação

| O que mudou | O que recompilar |
| --- | --- |
| Launcher (Rust, interface web) | `cargo build` / `.\scripts\start-launcher.ps1` |
| Só o runtime, guest inalterado | `toolchain/build.py ... --runtime-only` |
| Hooks de guest ou unidades geradas | Regenerar e recompilar as unidades afetadas com uma build completa do jogo |
| ABI ou estruturas compartilhadas | Todos os consumidores dessa estrutura |

Exemplo de recompilação só do runtime numa pasta de dados isolada e já preparada:

```powershell
.\local\python\Scripts\python.exe toolchain/build.py --config 'E:\MK8-TESTE\config.json' --local 'E:\MK8-TESTE' --runtime-only
```

Reutilize as pastas de build existentes. As builds demoram, e iniciar uma não significa que ela terminou.

**Empacotamento:**

- Launcher portátil: `python scripts/package-launcher.py` (saída: `target/portable/Launcher.exe`).
- Instalador local de teste: `python scripts/release.py` (saída: `<target do Cargo>/dist/MK8-online-<versão>-setup.exe`). Instale manualmente builds de teste com a mesma versão; o atualizador só oferece versões mais novas.
- Publicação: veja [RELEASING.md](RELEASING.md).

## Testes

```powershell
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --locked
python -m unittest discover -s tests
python -m unittest discover -s tests -p test_launcher_e2e.py        # testes de navegador (MK8_TEST_RELAY_EXE para o relay)
python -m unittest discover -s tests -p test_launcher_languages.py  # traduções do launcher e do instalador
```

- Mudanças em Rust exigem formatação, Clippy e testes.
- Mudanças de interface exigem os testes de navegador.
- Um teste nativo deve exercer o comportamento, não só procurar texto no código.

**Importação de DLC pela linha de comando**, numa pasta de dados isolada e preparada:

```powershell
.\local\python\Scripts\python.exe toolchain/import_dlc.py --package 'D:\Switch\BoosterCoursePass.nsp' --keys 'D:\Switch\prod.keys' --local 'E:\MK8-TESTE'
```

## Desenvolvimento da interface do launcher

`python scripts/preview-launcher.py` compila em debug e abre a interface real com dados separados em `local/design-preview`.

- `--target-dir D:/caminho/do/target` reutiliza uma pasta Cargo; `--no-build` pula a compilação.
- Definir `MK8_WEB_ROOT` serve arquivos de interface ao vivo, só em builds debug. Edite HTML/CSS/JS e recarregue.
- `/?storyboard=1#install` mostra uma simulação da instalação, identificada como tal, com dados de exemplo. Ela não chama nenhuma API de instalação ou build.

As builds de release usam os arquivos embutidos e não têm esses controles de debug. A prévia não reutiliza perfis, recursos ou saves instalados e não qualifica o gameplay.

## Caminhos só de desenvolvimento

Estes caminhos só são acessíveis pela API e não são recursos públicos: os transportes LAN, relay e P2P e o conversor estático de cabeça Mii. Veja o [registro técnico](log/README.md).

## Documentação

- Todo documento existe em inglês (`docs/en/`) e português (`docs/pt-BR/`). Atualize os dois na mesma mudança. Os arquivos da raiz (`README.md`, `AGENTS.md`, `CONTRIBUTING.md`) ficam em inglês e têm pares em português (`README.pt-BR.md`, `docs/pt-BR/AGENTS.md`, este arquivo).
- `docs/*/STATUS.md` é um resumo curto do estado atual. Substitua as linhas desatualizadas em vez de acrescentar.
- Os detalhes datados das investigações vão para o [registro técnico](log/README.md). Use uma seção por tema, a mais recente primeiro, e não uma por tentativa.
- As mudanças de cada versão vão para `docs/releases/<versão>.md`, não para o README.
- `docs/STATUS.md` e `docs/THIRD_PARTY.md` são redirecionamentos mantidos por causa de comentários no código que os citam. Não os apague.

## Relatar bugs

Veja [Relatar um problema](GUIDE.md#relatar-um-problema) no guia do usuário.
