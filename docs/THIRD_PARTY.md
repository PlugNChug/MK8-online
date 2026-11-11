# Dependências e atribuições

Referência de diagnóstico de hardware (08/11/2026): [Intel — driver gráfico para processadores de 7ª a 10ª geração](https://www.intel.com/content/www/us/en/download/776137/intel-7th-10th-gen-processor-graphics-windows.html), pacote que inclui o i3-10110U e Windows 10. Consultado para o bloqueio Vulkan 1.2.131 descrito em `STATUS.md`; o driver não é distribuído pelo projeto, e sua compatibilidade com o runtime nessa máquina ainda precisa ser testada.

O código próprio usa GPL-3.0-or-later (`../LICENSE`). Os snapshots e bibliotecas mantêm suas licenças originais. Nomes em caminhos, símbolos e identificadores serializados são contratos de integração, não alegações de equivalência entre projetos.

Licenciamento conferido em 08/11/2026: o `LICENSE` do snapshot SMM2Recomp contém GPLv3; o de LASRecomp contém MIT. A origem MIT de partes do runtime não relicencia as alterações GPL incorporadas posteriormente. Uma eventual oferta MIT de código próprio independente exige separar sua proveniência; mudar apenas o `LICENSE` principal não transforma o conjunto em MIT. A licença atual foi preservada. Referência: [GNU — titularidade e licenciamento alternativo](https://www.gnu.org/licenses/gpl-faq.en.html#HeardOtherLicense).

| Origem efetivamente utilizada | Revisão / uso | Licença preservada |
| --- | --- | --- |
| [SMM2Recomp](https://github.com/smm2recomp/smm2recomp) | `9dd1858fe8f4688ae5daa390c5e18d027300ada6`; snapshot do tradutor/runtime, verificado por `upstream/manifests/smm2recomp.json` | `upstream/smm2recomp/LICENSE`, `upstream/smm2recomp/recomp/UPSTREAM.md` |
| [LASRecomp](https://github.com/peiikai/LASRecomp) | `a6b4f03844ecfb4012f18deff96da8b410aeafcd`; origem da infraestrutura derivada | `upstream/LASRecomp/LICENSE` e avisos dos snapshots |
| [Power Moon Recomp](https://gitlab.com/zzzarathustra-dev/power-moon-recomp) | `cdbe05c9df0e2ff4bfbecece89ad3f0fd9806193`; adaptação de heap sempre protegido em `toolchain/experimental.py` | [MIT / créditos](licenses/power-moon-recomp.txt) |
| [SkyRecomp](https://github.com/Apolinario-coder/SkyRecomp) | `ad649b4fd319b7f7c309857b2d14353d2e5dcd79`; TLD4S F32 adaptado em `runtime/mk8/graphics/maxwell_gather.inc` | [MIT, Apolinario-coder, 2026](licenses/SkyRecomp-MIT.txt), [aviso LAS](licenses/SkyRecomp-LASRecomp-MIT.txt) |
| [LibHac](https://www.nuget.org/packages/LibHac/0.18.0) | 0.18.0, pacote conferido por SHA-256; leitura e aplicação da atualização NCA | [MIT](licenses/LibHac-MIT.txt) |

O setup leva o executável do runtime já compilado (`prebuilt/host`, gerado por `scripts/prebuild-host.py` a partir das mesmas fontes que vão no pacote, sem nenhum arquivo do jogo). Nele entram estaticamente mbedTLS, Dear ImGui e SMAA (licenças nos snapshots do upstream), além da libc++/libunwind do LLVM (Apache-2.0 com LLVM Exception) e da CRT do mingw-w64 fornecidas pelo Zig 0.17.0. O código-fonte correspondente acompanha o pacote.

O setup leva também um FFmpeg 9.0.2 mínimo (`prebuilt/ffmpeg`: avcodec, avformat, avutil e swscale, só com mov/matroska, h264/vp8/aac e o protocolo de arquivo), compilado por `scripts/build-ffmpeg.sh` a partir do pacote oficial ([ffmpeg.org](https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz), SHA-256 conferido) com o assembly do nasm 2.16.03. Licença LGPL 2.1 ou posterior (nenhum componente GPL ou não livre habilitado); `LICENSE.md` e `COPYING.LGPLv2.1` vão junto das DLLs, e cada release anexa o código-fonte e o script de build. Os headers e bibliotecas de importação continuam os do pacote MSYS2 fixado da mesma versão.

Os avisos MIT completos nesta pasta acompanham os trechos adaptados. Não retirar licenças dos snapshots ao organizar o código. Dependências Rust estão fixadas por `Cargo.lock`; compilador e dependências nativas pelos scripts/manifestos locais.

As demais avaliações históricas não são dependências de execução nem evidência de FPS ou compatibilidade deste projeto. Não copiar DLLs, endereços ou resultados de jogos diferentes. ROM, chaves, código gerado e modelos do dump permanecem privados.

Referência operacional do multiplayer: [Nintendo — How to Use the LAN Play Feature of Mario Kart 8 Deluxe](https://en-americas-support.nintendo.com/app/answers/detail/a_id/25961/p/897), consultada em 07/11/2026. Documenta o atalho L + R + clique no analógico esquerdo e o limite de 12 jogadores. Esse limite do jogo não constitui qualificação da nossa implementação.

Direção futura solicitada pelo mantenedor: [Nextendo Network](https://nextendo.network/), consultada em 08/11/2026. O site apresenta serviços independentes de contas, amigos e matchmaking para suas integrações com Ryujinx, Citron e Switch com CFW. Não há código ou integração Nextendo nesta build MK8-online, nem validação de compatibilidade com seu runtime nativo ou afiliação entre os projetos. A avaliação está no roteiro; os resultados divulgados pelo serviço não qualificam este projeto.

Referência de metodologia solicitada pelo usuário: [Co-op Anything](https://github.com/cranebeaker/coop-anything), consultada em 07/11/2026, especialmente [falhas de rede](https://github.com/cranebeaker/coop-anything/blob/main/playbook/network-chaos-testing.md), [diagnóstico de dessincronização](https://github.com/cranebeaker/coop-anything/blob/main/playbook/desync-hunting.md) e [instâncias isoladas](https://github.com/cranebeaker/coop-anything/blob/main/playbook/multi-instance-testing.md). Útil para ampliar os testes da ponte LAN existente. Nenhum código, MCP ou relay Steam desse projeto foi incorporado; seus resultados em outros jogos não são evidência para MK8.

## Captura durante redimensionamento

A correção local de captura durante redimensionamento (11/11/2026) adapta `runtime/nvn/vulkan/device.c::frame_end` do snapshot SMM2Recomp já listado. `toolchain/capture_resize.py` preserva os metadados anteriores ao present; o teste nativo usa dados sintéticos e páginas protegidas Windows, sem imagem ou código comercial. Nenhuma implementação de captura do emulador foi incorporada ao runtime.

## Vulkan Loader distribuído com o runtime

O pacote MSYS2 `vulkan-loader-1~1.4.363.0-1` já é fixado no manifesto de dependências upstream com SHA-256 `065076fe13d39ab97eea4740596426d5ae741b5044e48e1c9495d04835c92dbe`. A DLL original passa a acompanhar a pasta do jogo; [licença original](licenses/Vulkan-Loader.txt) preservada (Apache 2.0 e avisos do projeto). Referências: [SDL_Vulkan_LoadLibrary](https://wiki.libsdl.org/SDL3/SDL_Vulkan_LoadLibrary) e [LunarG — Loader Application Interface](https://vulkan.lunarg.com/doc/view/1.4.309.0/windows/LoaderApplicationInterface.html). O loader é separado do driver Vulkan da GPU; nenhuma implementação de driver foi adicionada.

## Supervisão de processos Windows

Diagnóstico de acesso a arquivos (09/11/2026): [Microsoft — System Error Codes 0–499](https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--0-499-) e [1000–1299](https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--1000-1299-). Referências para diferenciar violações de compartilhamento/bloqueio, acesso negado e dispositivo indisponível. O código retornado não identifica qual produto ou processo causou o bloqueio.

Supervisão de processos Windows (09/11/2026): [Microsoft — Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects) e [AssignProcessToJobObject](https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-assignprocesstojobobject). Fundamentam a herança do job pelos descendentes e o encerramento com `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. Chamadas por `windows-sys 0.61.2`, já presente no lockfile; não se aplica terminação automática ao jogo quando apenas a janela do launcher fecha.

## Python privado para ferramentas

Diagnóstico do issue #4 (09/11/2026): [metadados oficiais de lz4 4.4.5](https://pypi.org/pypi/lz4/4.4.5/json) distinguem o SHA-256 do arquivo-fonte e do wheel CPython 3.12 Windows x64. A instalação segue a documentação do pip sobre [configuração e PIP_CONFIG_FILE](https://pip.pypa.io/en/stable/topics/configuration/) e [only-binary](https://pip.pypa.io/en/stable/cli/pip_install/#cmdoption-only-binary). Os hashes e avisos das dependências permanecem preservados.

[CPython 3.12.10 x64 no NuGet](https://www.nuget.org/packages/python/3.12.10), distribuído pelos mantenedores CPython e documentado em [Using Python on Windows](https://docs.python.org/3.12/using/windows.html#the-nuget-org-packages). O bootstrap baixa o pacote original de `api.nuget.org`, SHA-256 `0eb85c2dfccccf1b17352de4c397f69194035b7d37149eacc16f1147d93de3b8`. A extração integral preserva `tools/LICENSE.txt` e os avisos incluídos (Python Software Foundation License e licenças de componentes). Sem redistribuição do interpretador no repositório.

## Investigação de DLC

Referências consultadas em 10/11/2026: [Switchbrew — serviço `aoc:u`](https://switchbrew.org/wiki/NS_services#aoc:u), para descoberta, preparação e notificações de conteúdo adicional, e [Nintendo — acesso ao Booster Course Pass](https://www.nintendo.com/en-gb/Support/Purchases-Subscriptions/Games/How-to-Access-Mario-Kart-8-Deluxe-Booster-Course-Pass-DLC-Content-2178331.html), para o fluxo oficial de acesso ao conteúdo. São referências de protocolo e uso, não evidência de compatibilidade deste runtime. A inspeção local reutilizou o LibHac 0.18.0 já atribuído acima; nenhum código dessas páginas foi incorporado. NSP, tickets, chaves e arquivos do DLC permanecem privados.

O protótipo consulta também as assinaturas IPC de [`nn::aocsrv::detail::IAddOnContentManager` no SwIPC](https://reswitched.github.io/SwIPC/ifaces.html#nn::aocsrv::detail::IAddOnContentManager): comandos, argumentos e descritores de saída. A implementação própria fica em `runtime/mk8/services/dlc.c` e é aplicada às cópias de build por `toolchain/dlc.py`; não houve cópia de implementação de outros emuladores.

## Investigação de cores RGB565

Consultadas em 10/11/2026: [Khronos — `VkFormat`](https://docs.vulkan.org/refpages/latest/refpages/source/VkFormat.html), para a posição dos componentes nos formatos Vulkan PACK16, e [NX Native — Rendering](https://nx-native.com/docs/reference/rendering), como referência do problema de canais trocados em formatos compactados de 16 bits. A hipótese local foi confrontada com `soft16_decode` no snapshot SMM2Recomp já atribuído, com metadados dos ícones do dump privado e com teste nativo autoral. Nenhuma implementação dessas páginas foi copiada; os resultados de outros jogos não qualificam esta build. Evidências locais em `STATUS.md`.

## Alimentação durante os testes

A leitura de AC/bateria usa a API Win32 existente, via `ctypes` da biblioteca padrão Python: [Microsoft — GetSystemPowerStatus](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getsystempowerstatus) e [SYSTEM_POWER_STATUS](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-system_power_status), consultadas em 10/11/2026. O monitor registra os valores informados pelo Windows; não altera planos de energia nem seleciona uma GPU.

Na medição prolongada de 11/11/2026, helpers privados usam também contadores Windows de memória por PID e consultas CSV do [NVIDIA System Management Interface](https://docs.nvidia.com/deploy/nvidia-smi/index.html) já instalado no notebook. Só foram feitas leituras de memória, utilização, temperatura, clocks e estado de desempenho; nenhum clock/limite de energia foi alterado. O CSV mantém os cabeçalhos e unidades fornecidos pelo driver, sem transformar valores indisponíveis em zero. A documentação [Microsoft — PROCESS_MEMORY_COUNTERS_EX](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex) distingue memória privada comprometida de working set; esses valores não equivalem à alocação VRAM da VMA. Nenhuma biblioteca NVIDIA foi adicionada às dependências do projeto.

Consulta para o diagnóstico privado de handles: [Microsoft — NtQueryObject](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntqueryobject) e declarações de [`ntexapi.h` no phnt/System Informer](https://github.com/winsiderss/phnt/blob/master/ntexapi.h), em 11/11/2026. A tentativa limitou-se ao PID do runtime de teste e a tipos/contagens, sem registrar nomes de objetos ou fechar handles do jogo. O helper lento foi interrompido sem classificação conclusiva. Nenhum código desses projetos ou dependência de inspeção foi incorporado ao runtime/launcher.

## Comparação de fidelidade visual

Referências do RenderDoc consultadas em 10/11/2026: [inspeção e histórico de pixels](https://github.com/baldurk/renderdoc/blob/v1.x/docs/how/how_inspect_pixel.rst) e [depuração de shaders](https://github.com/baldurk/renderdoc/blob/v1.x/docs/how/how_debug_shader.rst). Fundamentam a proposta de comparar alvos intermediários e identificar o draw que altera um pixel, além de comparar a imagem final. Nenhuma implementação do RenderDoc foi incorporada; as capturas locais de emulador estão descritas abaixo.

## Referência visual com Ryubing

Consultados em 10/11/2026: [guia oficial do Ryubing — modo portátil](https://docs.ryujinx.app/guides/setup-guide/#portable-mode), para separar dados por pasta/argumento `-r`, e o projeto independente [ryubing-mcp](https://github.com/Leuconoe/ryubing-mcp), incluindo `src/capabilities.ts` e `src/patchless-client.ts`. O projeto declara controle por teclado e captura nativa com o executável sem alterações; controle analógico/touch requer seu build de bridge. O código foi consultado para avaliar viabilidade, sem instalação, execução ou incorporação. A licença MIT do servidor e as licenças do emulador deverão ser preservadas se forem distribuídos futuramente. Suas alegações não substituem teste local do executável do usuário.

Referência de hardware: [SysDVR](https://github.com/exelix11/SysDVR), para captura via USB/rede. O README documenta vídeo H.264 fixado em 720p/30, restrições de jogos e ausência de captura da interface do sistema. O streaming, sozinho, não estabelece uma automação completa de entrada e checkpoints. Nenhum sysmodule, cliente ou código GPL-2.0 desse projeto foi instalado ou incorporado.

A validação local usa APIs Win32 existentes, sem incorporar código do servidor MCP: [Microsoft — PrintWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-printwindow), [SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput) e [SetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow). Captura/entrada são limitadas ao PID cujo executável corresponde à cópia privada; falha de foco impede o envio global de teclas.

Experimento privado em 11/11/2026: [Leuconoe/Ryubing — MCP Bridge v0.2.0](https://github.com/Leuconoe/Ryubing/releases/tag/mcp-bridge-v0.2.0), commit [`7231e00ea0302dd61b617b4edc29672b3dbcda30`](https://github.com/Leuconoe/Ryubing/commit/7231e00ea0302dd61b617b4edc29672b3dbcda30). O ZIP Windows x64 tem SHA-256 `da92c746b1029077f90a57c7b7695d747fc3a90c16cebd58532f63b115dde945`, conferido com o arquivo publicado. Extração integral em `local/ryubing-bridge/app`, mantendo `LICENSE.txt` e `THIRDPARTY.md`; nenhuma redistribuição no repositório ou integração ao runtime. O cliente autoral usa biblioteca padrão Python e o protocolo JSON-RPC direto, sem executar o servidor ryubing-mcp. A inspeção do patch contra sua base não encontrou mudanças GPU/shader, mas não estabelece equivalência com a Canary 1.3.351 do usuário. O controle e as capturas foram exercidos localmente; resultados declarados pelo projeto externo não são tratados como testes desta recompilação. Limites e falha posterior do backend de áudio em `STATUS.md`.

Diagnóstico de interpolação em 11/11/2026: [`ShaderHeader.cs`](https://github.com/Leuconoe/Ryubing/blob/7231e00ea0302dd61b617b4edc29672b3dbcda30/src/Ryujinx.Graphics.Shader/Translation/ShaderHeader.cs) e [`InstEmitAttribute.cs`](https://github.com/Leuconoe/Ryubing/blob/7231e00ea0302dd61b617b4edc29672b3dbcda30/src/Ryujinx.Graphics.Shader/Instructions/InstEmitAttribute.cs), consultados para o formato SPH (unused/constant/perspective/screen-linear) e a semântica de IPA. A adaptação própria em `toolchain/interpolation.py` foi confrontada com metadados do dump privado e testes sintéticos do tradutor existente; não incorpora esses arquivos C# nem shaders comerciais. Os binários de referência retêm seus avisos originais. As evidências locais não qualificam outras builds do emulador nem todos os shaders do jogo.
