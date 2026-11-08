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

Requisitos: Rust, Python 3.12, PowerShell 7 e .NET 10 x64 (extrator de pacotes). O setup é o próprio `Launcher.exe` empacotado com o nome `MK8-online-<versão>-setup.exe`: esse nome abre a janela de instalação (`apps/launcher/src/installer.rs`).

```powershell
$env:MK8_UPDATE_REPO = "owner/nome-do-repositorio"
python scripts/release.py              # target/dist/MK8-online-<versão>-setup.exe + .sha256 + release-notes.md
python scripts/release.py --skip-package   # só copia de novo target/portable/Launcher.exe como setup
```

Para publicar uma build local, crie o Release `v<versão>` e anexe o `setup.exe` **e** o `.sha256`. Sem SHA-256 (o `digest` do GitHub ou o arquivo `.sha256`), o Launcher recusa a atualização.

## O que vai no instalador

- Só o `Launcher.exe` empacotado: fontes verificadas do runtime e do pipeline, extrator e runtime .NET. As fontes `upstream` são baixadas na hora de empacotar e conferidas pelos manifestos em `upstream/manifests`.
- Nunca: o jogo, chaves, saves, builds, `local/` ou dados de teste.
- O instalador é por usuário, sem administrador: copia o launcher, grava `mk8-online.json` (pasta de dados), já desempacota as fontes da versão, cria os atalhos e registra a desinstalação em `HKCU\...\Uninstall\MK8-online`. A pasta de dados fica preservada em atualizações e na desinstalação.
- Linha de comando (o atualizador usa a primeira): `/SILENT` instala por cima com as pastas anteriores e abre o launcher; `/DIR=`, `/DATADIR=`, `/DESKTOP=0|1` e `/NOLAUNCH` mudam isso. `Launcher.exe --uninstall [/SILENT]` desinstala. Uma instalação anterior feita pelo Inno Setup (até 0.1.1) na mesma pasta é substituída, e o desinstalador e o grupo de menu antigos são removidos.

## Checagem antes de publicar

- [ ] `cargo fmt --all -- --check`, Clippy e `cargo test --workspace --locked`
- [ ] testes de navegador: `python -m unittest discover -s tests -p test_launcher_e2e.py` (com `MK8_TEST_RELAY_EXE` para o relay)
- [ ] instalar o setup numa pasta temporária (`setup.exe /SILENT /DIR=... /DATADIR=... /DESKTOP=0 /NOLAUNCH`), abrir o Launcher e desinstalar com `Launcher.exe --uninstall /SILENT`; abrir a janela do setup e percorrer as quatro telas
- [ ] `docs/STATUS.md` e `docs/ROADMAP.md` atualizados; nada de caminhos pessoais ou dados do jogo nos arquivos versionados
