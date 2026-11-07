# Publicar uma versão

Este é o fluxo para o mantenedor e para agentes de IA que preparam uma release. Os testadores recebem o `setup.exe` pelos Releases do GitHub e se atualizam pelo próprio Launcher (**Suporte → Atualizar agora**).

## Versões

O projeto usa `MAJOR.MINOR.PATCH`, com sufixo de pré-release enquanto for experimental (por exemplo `0.2.0-alpha.1`). A versão fica em `Cargo.toml` (`[workspace.package]`) e em `apps/launcher/tauri.conf.json`; `scripts/release.py` mantém as duas iguais. O Launcher só oferece atualização quando a tag do Release é **maior** que a versão compilada.

## Pelo GitHub Actions (recomendado)

```powershell
python scripts/release.py --bump 0.2.0-alpha.1   # altera Cargo.toml, tauri.conf.json e Cargo.lock
git commit -am "Release 0.2.0-alpha.1"
git tag v0.2.0-alpha.1
git push origin main --tags
```

A tag dispara `.github/workflows/release.yml`, que:

1. roda `cargo fmt`, Clippy e os testes Rust;
2. empacota o Launcher com `MK8_UPDATE_REPO` igual ao repositório, para que a build saiba onde procurar atualizações. Se a variável de repositório `MK8_RELAY_URL` existir, ela também entra na build;
3. gera `MK8-online-<versão>-setup.exe`, o `.sha256` e as notas;
4. publica o Release. Uma tag com `-` (por exemplo `-alpha.1`) vira pré-release.

Revise as notas geradas e acrescente o que mudou. Ao pedir para uma IA preparar a release, peça que ela atualize `docs/STATUS.md`/`docs/ROADMAP.md` com a evidência nova, rode a verificação do README e só então crie a tag.

## Localmente

Requisitos: Rust, Python 3.12, PowerShell 7, .NET 10 x64 (extrator de pacotes) e Inno Setup 6 (`winget install JRSoftware.InnoSetup`).

```powershell
$env:MK8_UPDATE_REPO = "owner/nome-do-repositorio"
python scripts/release.py              # target/dist/MK8-online-<versão>-setup.exe + .sha256 + release-notes.md
python scripts/release.py --skip-package   # só refaz o setup a partir de target/portable/Launcher.exe
```

Para publicar uma build local, crie o Release `v<versão>` e anexe o `setup.exe` **e** o `.sha256`. Sem SHA-256 (o `digest` do GitHub ou o arquivo `.sha256`), o Launcher recusa a atualização.

## O que vai no instalador

- Só o `Launcher.exe` empacotado: fontes verificadas do runtime e do pipeline, extrator e runtime .NET. As fontes `upstream` são baixadas na hora de empacotar e conferidas pelos manifestos em `upstream/manifests`.
- Nunca: o jogo, chaves, saves, builds, `local/` ou dados de teste.
- O instalador é por usuário, sem administrador. A pasta de dados é escolhida no assistente (`/DATADIR=` em instalação silenciosa) e fica preservada em atualizações e na desinstalação.

## Checagem antes de publicar

- [ ] `cargo fmt --all -- --check`, Clippy e `cargo test --workspace --locked`
- [ ] testes de navegador: `python -m unittest discover -s tests -p test_launcher_e2e.py` (com `MK8_TEST_RELAY_EXE` para o relay)
- [ ] instalar o setup numa máquina limpa ou numa pasta temporária e abrir o Launcher
- [ ] `docs/STATUS.md` e `docs/ROADMAP.md` atualizados; nada de caminhos pessoais ou dados do jogo nos arquivos versionados
