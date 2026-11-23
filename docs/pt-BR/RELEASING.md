# Publicar uma versão

[English](../en/RELEASING.md) · **Português**

Este é o fluxo para o mantenedor e para agentes de IA que preparam uma release. Os testadores recebem o `setup.exe` pelos Releases do GitHub e se atualizam pelo próprio Launcher (**Suporte → Atualizar agora**).

O atualizador consulta a API pública do GitHub sem autenticação. O repositório e os anexos precisam estar públicos; enquanto estiverem privados, a consulta falha. A versão 0.1.6 foi empacotada com `MK8_UPDATE_REPO=Vnnc1/MK8-online`: mudar a visibilidade desse mesmo repositório para público não exige recompilar nem configurar o PC do jogador. Não embuta tokens no launcher.

Para as versões seguintes, aumente a versão compilada e publique a tag correspondente com `MK8-online-<versão>-setup.exe` e os pacotes portátil Windows, AppImage e tar.gz Linux, cada um acompanhado do seu `.sha256`. O SHA-256 deve ser calculado **depois** de qualquer mudança ou assinatura do executável. Anexe também os fontes/scripts FFmpeg exigidos abaixo. Drafts não são oferecidos; pré-releases publicadas são. A atualização é acionada pelo jogador, sem instalação automática ao abrir o launcher.

## Versões

O projeto usa `MAJOR.MINOR.PATCH`, com sufixo quando a publicação for uma pré-release (por exemplo `0.3.0-alpha.1`). A 0.5.0 usa a tag sem sufixo `v0.5.0` e é uma release pública experimental. O workflow mantém a exceção histórica para a tag `v0.3.0`. Seu estado experimental e as limitações CTGP/Steam permanecem explícitos no título e nas notas. A versão fica em `Cargo.toml` (`[workspace.package]`) e em `apps/launcher/tauri.conf.json`; `scripts/release.py` mantém as duas iguais. O Launcher só oferece atualização quando a tag do Release é **maior** que a versão compilada.

## Pelo GitHub Actions (recomendado)

```powershell
python scripts/release.py --bump 0.5.0   # altera Cargo.toml, tauri.conf.json e Cargo.lock
git commit -am "Release 0.5.0"
git tag v0.5.0
git push origin master v0.5.0
```

A tag dispara `.github/workflows/release.yml`. O Windows compila o FFmpeg em um runner Linux, monta o instalador e o ZIP no Windows e abre a edição portátil para verificar a chave do runtime extraído. O job Linux compila o AppImage e o tar.gz, inicia o launcher e exercita o importador e a recuperação do atualizador. O job de publicação depende das duas plataformas e verifica os arquivos SHA-256. Uma tag com sufixo ou a tag histórica `v0.3.0` é publicada como pré-release.

O código Linux desta versão está fixado por SHA no submódulo `platforms/linux`. Use `git submodule update --init platforms/linux` para compilar o porte localmente. Ao preparar uma nova versão, atualize o workspace na linha Linux, publique a revisão no mesmo repositório e registre o novo commit do submódulo na branch principal. O empacotador confere a versão com a tag. [Fonte Linux 0.5.0](https://github.com/Vnnc1/MK8-online/tree/edfc423697977594b49db64b67c433c73086664f)

Revise `docs/releases/<versão>.md`: a primeira linha define o título e o restante compõe as notas, junto do SHA-256 e dos avisos gerados. Ao pedir para uma IA preparar a release, peça que ela atualize `docs/{en,pt-BR}/STATUS.md` e `ROADMAP.md` com a evidência nova (detalhes datados em `docs/{en,pt-BR}/log/`), rode a verificação do README e só então crie a tag.

## Localmente

Requisitos: Rust, Python 3.12, PowerShell 7 e .NET 10 x64 (extrator de pacotes). Também é preciso ter as ferramentas nativas instaladas (`scripts/setup-native.py` e `scripts/setup-runtime-deps.py`): o empacotamento compila o runtime do jogo (`scripts/prebuild-host.py`, ~1 min) e o leva pronto em `prebuilt/host`, com a chave das entradas; a build do jogador o usa só se a chave dele for idêntica. O FFmpeg mínimo (`scripts/build-ffmpeg.py`) é compilado no WSL (Ubuntu com `make`, `gcc` e `curl`; ~6 min na primeira vez, depois reaproveitado enquanto o script não mudar) e a release anexa `ffmpeg-9.0.2.tar.xz` e `build-ffmpeg.sh` (LGPL). Para recompilar o extrator, defina `MK8_IMPORT_PWSH` com o caminho do `pwsh.exe` do PowerShell 7 quando ele não estiver no PATH. O setup é o próprio `Launcher.exe` empacotado com o nome `MK8-online-<versão>-setup.exe`: esse nome abre a janela de instalação (`apps/launcher/src/installer.rs`).

```powershell
$env:MK8_UPDATE_REPO = "owner/nome-do-repositorio"
python scripts/release.py              # target/dist/MK8-online-<versão>-setup.exe + .sha256 + release-notes.md
python scripts/release.py --skip-package   # só copia de novo target/portable/Launcher.exe como setup
```

Os scripts resolvem a pasta de saída por `cargo metadata`: `target/` nos comandos acima significa o `target_directory` configurado, inclusive quando fica em outro disco. Não mova nem limpe builds para seguir um caminho literal de exemplo.

Para publicar os binários já testados, prepare um rascunho com a revisão de `master`, o título e as notas bilíngues. Anexe o setup, o ZIP portátil, o AppImage amd64 e o tar.gz, cada um com seu SHA-256, além do código-fonte e do script de compilação do FFmpeg. Confira os hashes do rascunho antes de torná-lo público. Quando a tag já tem uma release publicada, o workflow confere os anexos existentes e preserva os binários testados.

## O que vai no instalador

- Só o `Launcher.exe` empacotado: fontes verificadas do runtime e do pipeline, extrator e runtime .NET. As fontes `upstream` são baixadas na hora de empacotar e conferidas pelos manifestos em `upstream/manifests`.
- Nunca: o jogo, chaves, saves, builds, `local/` ou dados de teste.
- O instalador por usuário copia o launcher e o `Uninstall.exe`, grava `mk8-online.json` com a pasta de dados, desempacota as fontes da versão, cria os atalhos e registra a desinstalação em `HKCU\...\Uninstall\MK8-online`. A atualização preserva os dados. Na desinstalação, manter os dados é a opção padrão. A remoção dos dados gerenciados exige confirmação.
- Linha de comando (o atualizador usa a primeira): `/SILENT` instala por cima com as pastas anteriores e abre o launcher; `/DIR=`, `/DATADIR=`, `/DESKTOP=0|1` e `/NOLAUNCH` mudam isso. `Launcher.exe --uninstall [/SILENT]` desinstala. Uma instalação anterior feita pelo Inno Setup (até 0.1.1) na mesma pasta é substituída, e o desinstalador e o grupo de menu antigos são removidos.

## Checagem antes de publicar

- [ ] `cargo fmt --all -- --check`, Clippy e `cargo test --workspace --locked`
- [ ] testes de navegador: `python -m unittest discover -s tests -p test_launcher_e2e.py` (com `MK8_TEST_RELAY_EXE` para o relay)
- [ ] instalar o setup numa pasta temporária (`setup.exe /SILENT /DIR=... /DATADIR=... /DESKTOP=0 /NOLAUNCH`), abrir o Launcher e desinstalar com `Launcher.exe --uninstall /SILENT`; abrir a janela do setup e percorrer as quatro telas
- [ ] `docs/{en,pt-BR}/STATUS.md` e `ROADMAP.md` atualizados; nada de caminhos pessoais ou dados do jogo nos arquivos versionados
- [ ] `README.md` (inglês) e `README.pt-BR.md` sincronizados, assim como cada par `docs/en` / `docs/pt-BR`; links e imagens verificados
- [ ] `python -m unittest discover -s tests -p test_launcher_languages.py`; conferir launcher e instalador nos dois idiomas, incluindo persistência ao reiniciar
- [ ] Revisar arquivos novos, histórico Git e metadados de autoria antes de abrir o repositório; `.gitignore` não remove informações já registradas em commits. Configurar identidade pública e e-mail `noreply` para commits futuros não altera os antigos
- [ ] Revisar separadamente anexos de releases, logs públicos e configurações/segredos do GitHub; a varredura local não cobre esses serviços
- [ ] Usar `scripts/package-launcher.py` para o executável público: o empacotamento remapeia caminhos Rust e habilita `tauri/custom-protocol` para não embutir o diretório de configuração de desenvolvimento. Conferir também os bytes do binário e o payload; o executável de desenvolvimento em `target/release` contém o caminho do checkout e não é um pacote de distribuição
