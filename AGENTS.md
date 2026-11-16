# Trabalhar neste projeto

Leia `README.md` e `docs/STATUS.md`. Esses arquivos são a documentação operacional; atualize-os quando o estado mudar. Não crie um relatório novo para cada tentativa.

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
- Referências externas e atribuições ficam em `docs/THIRD_PARTY.md` e licenças. Preserve avisos de licença de dependências; não transfira resultados de outros projetos para esta build.
