# DLSS 5 Neural Rendering (NGX Feature 18) — avaliação local

**Data:** 20/11/2026 · **Máquina:** Windows x64, GeForce RTX 5080, driver 32.0.16.2101 (621.01) · **Python:** 3.12.10

**Resultado:** o runtime `te_dlss5_native.dll` executou Feature 18 em D3D12 nesta GPU e produziu imagens reais a partir de quadros do MK8. Isso **não** é integração ao jogo e não qualifica DLSS 5 como recurso do MK8-online. A ponte é D3D12, o jogo é Vulkan, e o binário de Neural Rendering é uma modificação não redistribuível de um binário da NVIDIA. Decisão de integração: **não integrar** (ver o final).

## 1. Proveniência e status do binário

Conjunto obtido de [Bandukids/DLSS-Runtimes](https://huggingface.co/Bandukids/DLSS-Runtimes/tree/main), o runtime indicado pela extensão [ComfyUI-DLSS5-VideoSuite](https://github.com/Bandukids/ComfyUI-DLSS5-VideoSuite). O README do repositório declara:

- `nvngx_dlssnr.dll` v310.8.0.0 contém os pesos e o pipeline de **DLSS 5 (3D-Guided Neural Rendering / NGX Feature 18)**;
- o build veio de uma build de desenvolvimento/inicial de *NBA 2K27*;
- o driver NVIDIA original bloqueia a feature em Ada Lovelace e anteriores com `0xBAD00001`; esta cópia foi **modificada pela comunidade RenoDX/ReShade** para contornar a checagem de arquitetura;
- a licença do repositório é `other` e o texto pede uso **não comercial, educacional e de avaliação de interoperabilidade**, sem garantia;
- NVIDIA, GeForce, RTX, DLSS e NGX são marcas da NVIDIA; NBA 2K é marca da Take-Two/Visual Concepts; o repositório não é afiliado a elas.

Portanto: **binário NVIDIA modificado, não redistribuível, uso apenas local**. Nada dele foi copiado para o repositório, para o launcher ou para um pacote. Os arquivos ficam em `~/.cache/mk8-dlss5` (fora do Git) e na máquina Windows.

| Arquivo | Bytes | SHA-256 |
| --- | ---: | --- |
| `nvngx_dlss.dll` | 58.956.400 | `c85f971ce023c9f3492fc7455f0b01a24ba18ea39636407a846902c4360b0b7e` |
| `nvngx_dlssg.dll` | 7.453.808 | `5d5cbf14d2727d47f93fd10bf77bd91708ae122482a6f86fd564971641ebd47b` |
| `nvngx_dlssnr.dll` | 165.840.496 | `8270b350cd82de5ce89806872cdd6b6a9249b80836b91bbeb3573470744cc206` |
| `te_dlss5_native.dll` | 296.960 | `99f13bfcf888da15d2ccba1a3175d3b5db758bb9284b525a581e95369031320a` |
| `renodx-dlss5.addon64` | 1.732.608 | `d5adf82eb44b065f4c590ac91fe824bab07afea0eb9f994bde936710c8593952` |

Os cinco arquivos foram copiados para `C:/Users/USER/mk8-rtx/dlss5/` e os SHA-256 foram conferidos **na máquina remota** com `Get-FileHash`; todos coincidiram com a tabela acima. O addon foi colocado no mesmo diretório, mas a ponte não o referencia por nome e não foi provado que ele seja carregado nesse caminho; o que se observou é que `te_dlss5_native.dll` contém código próprio de patch em módulo carregado (`VirtualProtect` + troca de ponteiro).

## 2. Execução no RTX 5080

Harness: [`scripts/dlss5_te_harness.py`](../scripts/dlss5_te_harness.py). É Python 3 somente biblioteca padrão (decodifica/grava PNG, `ctypes`, estatísticas). Entrada real: `merged-4.1.2-00900.png` da qualificação local (2560×1440), reduzida para 1280×720 e 1920×1080 em RGB. O quadro é o menu de criação de Mii, não uma corrida.

Comandos (a partir do host Linux; o shell remoto é PowerShell):

```bash
scp -i ~/.ssh/mkdx_windows_ed25519 -o IdentitiesOnly=yes -o BatchMode=yes \
  nvngx_dlss.dll nvngx_dlssg.dll nvngx_dlssnr.dll te_dlss5_native.dll renodx-dlss5.addon64 \
  USER@WINDOWS-HOST:C:/Users/USER/mk8-rtx/dlss5/

scp -i ~/.ssh/mkdx_windows_ed25519 -o IdentitiesOnly=yes -o BatchMode=yes \
  scripts/dlss5_te_harness.py mk8_frame_1280x720.png mk8_frame_1920x1080.png \
  USER@WINDOWS-HOST:C:/Users/USER/mk8-rtx/dlss5/
```

```powershell
cd C:/Users/USER/mk8-rtx/dlss5
python dlss5_te_harness.py --input in/mk8_frame_1280x720.png --out out/nr1280 `
  --width 1280 --height 720 --frames 3 --runtime-dir C:/Users/USER/mk8-rtx/dlss5 `
  --log-file out/nr1280.log --skip-destroy

python dlss5_te_harness.py --input in/mk8_frame_1920x1080.png --out out/nr1920 `
  --width 1920 --height 1080 --frames 3 --runtime-dir C:/Users/USER/mk8-rtx/dlss5 `
  --log-file out/nr1920.log --skip-destroy
```

### 2.1 Criação e processamento

| Resolução | `te_nr_create` | handle | `last_error` | `te_nr_process_rgba` | tempo/frame |
| --- | ---: | --- | --- | --- | --- |
| 1280×720 | 8339 ms | não nulo | `''` | `rc=0` nos 3 quadros | 70,7 / 24,8 / 43,0 ms |
| 1920×1080 | 8388 ms | não nulo | `''` | `rc=0` nos 3 quadros | 68,1 / 42,3 / 76,0 ms |

O primeiro teste, com `settings={}`, **falhou** em 533 ms: `te_nr_create -> None`, `last_error='LoadNvngxDlssnr'`. O mesmo binário, com `{"runtimeDir": "C:/Users/USER/mk8-rtx/dlss5"}`, criou o handle. Ou seja, `runtimeDir` é obrigatório na prática; `{}` não descobre o `nvngx_dlssnr.dll` ao lado da ponte.

A versão final commitada em `scripts/dlss5_te_harness.py`, reexecutada na mesma máquina em 1280×720 com 1 quadro, criou o handle em 6882 ms, processou em 13,3 ms e reproduziu exatamente o SHA-256 `b3bf5a12…` do frame 0, com `rc=0`. O tempo de criação variou entre 6,9 s e 8,4 s nas execuções.

### 2.2 Estatísticas das saídas

`mean_abs_diff` do harness é sobre RGBA (o alfa é 255 em entrada e saída, então dilui a média em 25%). A coluna RGB foi medida à parte.

| Saída | SHA-256 do RGBA | mean abs diff RGBA | mean abs diff RGB | máx. | bytes alterados | valores únicos |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 720p frame 0 | `b3bf5a12ef24051480bdde6237a72a20e23422ac9781e063af5453f83667233b` | 1,6154 | 2,154 | 35 | 66,18% | 255 |
| 720p frame 1 | `0b9755f84a88cdba79ad53c1346e9e802f7d0d780bb6a2cd949d39eb609a02f5` | 1,5737 | 2,098 | 34 | 65,90% | 255 |
| 720p frame 2 | `c97bca1ce6abca5e1297b4a45330c4feee9a85b059a3b2972c7b8228da4ffd14` | 1,5679 | 2,091 | 34 | 65,77% | 255 |
| 1080p frame 0 | `165c12479b85a035ba3f295728db73104475128406edfcee970df276630add70` | 1,3506 | — | 50 | 62,22% | 255 |
| 1080p frame 1 | `eb922bd6597759bfba86b846da7a5119c60867bfb6bc1c085ba9380042579afd` | 1,3287 | — | 49 | 61,84% | 255 |
| 1080p frame 2 | `54d2e9d010eb3d1f2ca2907dd4362a4a3318db8efc3f9a9b750bf450d1deafd4` | 1,3237 | — | 47 | 61,61% | 255 |

As saídas não são cópias: todos os bytes RGB foram percorridos, o alfa permaneceu 255 e a diferença máxima chegou a 35/50 níveis. Também não são ruído: a variância do Laplaciano caiu de 177,3 (entrada) para 165,0 (frame 0), e a luminância média ficou em 115,97 → 116,33. O efeito visível é suavização/reconstrução de bordas, não nitidez agressiva. Quadros consecutivos com a mesma entrada ainda diferem entre si (média 0,24 e 0,13 níveis), o que mostra acumulação temporal.

Saídas e logs copiados para inspeção em `local/dlss5-2026-11-20/` (fora do Git): `nr1280.frame0..2.png`, `nr1920.frame0..2.png`, `compare1280.png` (entrada/saída/diferença ×8), `nr1280.log`, `nr1920.log`.

### 2.3 Guided e teardown

`te_nr_process_rgba_guided(handle, src, NULL, depth_zeros_float32, dst, size)` retornou `rc=0` em 24,0 ms. A saída teve o mesmo SHA-256 do frame 1 não guiado (`0b9755…`), então o caminho foi exercitado, mas profundidade zero e movimento nulo **não** são evidência de qualidade guiada.

`te_nr_reset`/`te_nr_destroy` **travam**: depois de criar e processar, o processo ficou em 100% de CPU por mais de 10 minutos (428 s de CPU acumulada) e foi encerrado com `taskkill /F`. O harness novo usa `--skip-destroy` e `os._exit`; o sistema operacional recupera o processo. Isso é um defeito observável da ponte, não uma falha de `te_nr_process_rgba`.

## 3. Sonda do `nvngx_dlss.dll` de 58 MB e de Feature 18

O DLL de Super Resolution do conjunto é **v310.8.0.0**, `NGXApiVersion=0x0000013`, driver mínimo 512.15, arquitetura mínima Turing (`0x160`), com exportações `NVSDK_NGX_DirectSR_*`. Não há cadeia “DLSS 5”, lista de features novas nem preset DLSS 5; as cadeias de preset são `Preset_A/B/C` e as hints `DLSS.Hint.Render.Preset.*` clássicas. Ou seja, é o runtime DLSS SR 310.8, **não** um DLSS 5 SR.

Sonda D3D12 via `DXGIFactory1 → adapter 0 → NVSDK_NGX_D3D12_GetFeatureRequirements` (Python `ctypes`, sem criar feature):

| DLL | feature | `rc` | `supported` | `minArch` | `minOS` |
| --- | --- | --- | ---: | ---: | --- |
| `nvngx_dlss.dll` | SuperSampling (1) | `0x00000001` | `0x0` | `0x160` | 10.0.0 |
| `nvngx_dlss.dll` | FrameGeneration (11) | `0xBAD00005` | — | — | — |
| `nvngx_dlss.dll` | NeuralRendering (18) | `0xBAD00005` | — | — | — |
| `nvngx_dlssg.dll` | qualquer 1/11/18 | `0xBAD00002` | — | — | — |
| `nvngx_dlssnr.dll` | qualquer 1/11/18 | `0xBAD00002` | — | — | — |

`supported=0x0` é o valor de “suportado” no enum (`NVSDK_NGX_Feature_Support_Result`); `0xBAD00005` é parâmetro inválido (a feature não pertence àquele plugin) e `0xBAD00002` é erro de plataforma. Como DLSS-G/NR foram consultados antes de `Init`, esse erro é **inconclusivo** para eles. `NVSDK_NGX_GetAPIVersion` retornou `0x13` nos três; `GetGPUArchitecture` retornou `0x160` (Turing), `0x190` (Ada) e `0x1b0` (Blackwell2) para SR, FG e NR respectivamente.

A mesma consulta por Vulkan (`VkInstance` + `VkPhysicalDevice`, sem device) deu resultados idênticos: SR suportado com `minArch=0x160`, e os demais plugins em `0xBAD00002` antes de init. O `nvngx_dlssnr.dll` exporta `NVSDK_NGX_VULKAN_*`, mas não existe enum público de Feature 18 no cabeçalho NGX e não foi executada criação de feature 18 por Vulkan. **Feature 18 só foi executada em D3D12**, pela ponte `te_nr_*`.

O harness Vulkan existente (`tools/dlss_rtx_harness.cpp` do worktree MK8-online-current, compilado como `.exe`) **não pôde ser executado**: o Windows Defender bloqueou o binário não assinado (ThreatID `2147731250`; o arquivo foi zerado no destino e o carregamento falhou com `WinError 193`, tanto para o `.exe` quanto para uma DLL construída do mesmo código). A política do antivírus **não foi alterada**. Por isso o parâmetro `SuperSampling.Available` pós-`Init` não foi lido; `GetFeatureRequirements` foi a alternativa disponível e é o que a tabela acima registra.

## 4. Avaliação de integração para o MK8-online (Vulkan)

1. **Caminho gráfico incompatível.** `te_dlss5_native.dll` cria dispositivo e feature por D3D12 (`D3D12CreateDevice`, `NVSDK_NGX_D3D12_*`). O runtime do MK8-online é Vulkan. Usar Feature 18 no jogo exigiria um backend D3D12 paralelo, interop de texturas/command queues e uma swapchain própria — não é uma opção de configuração e não foi implementado.
2. **Binário modificado e não redistribuível.** O `nvngx_dlssnr.dll` é um binário NVIDIA alterado pela comunidade, com termos de uso próprios e sem garantia. Não pode entrar no repositório, no instalador nem no download do launcher. Um download opcional pelo launcher continuaria dependente dos termos da NVIDIA/comunidade e de um hash fixado; isso não resolve a licença.
3. **Custo por quadro.** 25–76 ms por quadro em 720p/1080p na RTX 5080 para um passe 2D (aprox. 13–40 fps), sem movimento/profundidade reais. Não cabe no orçamento de 60 Hz do jogo, muito menos em cima do render atual.
4. **Qualidade não demonstrada.** A entrada foi um menu, sem motion vectors, profundidade ou jitter. O resultado é uma reconstrução sutil de bordas; não há evidência de corrida, de estabilidade temporal em movimento ou de ganho visual que justifique o custo.
5. **Feature 18 não é DLSS 5 “de jogo” aqui.** O fluxo da extensão ComfyUI é off-line (imagem/vídeo), com guias temporais próprios. O conjunto também não traz um SR DLSS 5: o `nvngx_dlss.dll` é o SR 310.8 clássico.

**Decisão:** manter como experimento local e documentação. Não integrar ao runtime, ao launcher ou ao pipeline. Reavaliar somente se a NVIDIA expuser Feature 18 por Vulkan de forma redistribuível, houver um caminho D3D12/interop sustentável e houver medição em corrida real com movimento e profundidade dentro do orçamento de quadro.

## 5. Referências

- [Bandukids/DLSS-Runtimes](https://huggingface.co/Bandukids/DLSS-Runtimes/tree/main) — binários e README de proveniência (licença `other`, uso não comercial).
- [ComfyUI-DLSS5-VideoSuite](https://github.com/Bandukids/ComfyUI-DLSS5-VideoSuite) — API `te_nr_*` e integração de referência (MIT para o código da extensão; os binários NVIDIA são separados).
- [NVIDIA NGX / DLSS SDK](https://developer.nvidia.com/rtx/dlss) — `NVSDK_NGX_*`, Feature IDs e requisitos de arquitetura.
- O harness Vulkan do projeto e o ABI mínimo estão em `runtime/mk8/graphics/ngx/mk8_ngx_abi.h` (worktree MK8-online-current); nenhum header proprietário da NVIDIA foi copiado.
