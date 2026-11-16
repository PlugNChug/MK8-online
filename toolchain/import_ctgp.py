"""Importa o CTGP Deluxe do próprio jogador como pacote de pistas opcional (toolchain/ctgp_pack.py).

A pasta extraída do ZIP oficial (ctgpdx.com) vira uma sobreposição em <dados>/ctgp/pack: hardlinks quando a pasta
está no mesmo disco, cópia quando não está. O jogo base, o DLC e os saves não são alterados; o launcher liga ou
desliga o pacote (ctgp.json) e o jogo só o vê com ele ligado.
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

import ctgp_pack
from build import atomic_json
from build_lock import exclusive_build
from repair import cleanup_stale, is_link, remove_owned
from storage import GIB, require_space


def import_pack(source, romfs, local, reinstall=False, layout='base'):
    local = Path(local).resolve()
    folder = local / 'ctgp'
    target = folder / 'pack'
    if any(is_link(p) for p in (folder, target) if p.exists()):
        raise ValueError('CTGP: pasta de dados com link recusada.')
    folder.mkdir(parents=True, exist_ok=True)
    cleanup_stale(folder, 'pack-*')
    if target.exists() and not reinstall:
        try:
            manifest = ctgp_pack.verify(target)
            if manifest.get('layout', 'base') == layout:
                print(f"CTGP já importado ({manifest['courses']} pistas); reaproveitado.", flush=True)
                return manifest
            print('Outra disposição das copas pedida; montando de novo.', flush=True)
        except (ValueError, OSError, KeyError) as error:
            print(f'Importação anterior incompleta ({error}); montando de novo.', flush=True)
    title = ctgp_pack.locate(source)
    print(f'CTGP encontrado em {title}.', flush=True)
    if layout not in ctgp_pack.LAYOUTS:
        raise ValueError(f'Disposição desconhecida: {layout}')
    notes = []
    files = ctgp_pack.plan(title, romfs, layout, notes)
    for note in notes:
        print(f'Aviso: ícone de copa mantido original ({note}).', flush=True)
    same_disk = os.stat(title).st_dev == os.stat(folder).st_dev
    copy = 0 if same_disk else sum(p.stat().st_size for p in files.values() if isinstance(p, Path))
    copy += sum(len(b) for b in files.values() if isinstance(b, bytes))
    require_space(folder, 4 * GIB + copy)
    print(f"Montando {len(files)} arquivos ({'hardlinks' if same_disk else f'cópia de {copy / GIB:.1f} GiB'}).",
          flush=True)
    stage = folder / f'pack-{time.time_ns()}'
    manifest = ctgp_pack.build(title, romfs, stage, link=same_disk, layout=layout)
    ctgp_pack.verify(stage)
    if target.exists():
        remove_owned(target, folder)
    ctgp_pack.rename_folder(stage, target)
    atomic_json(local / 'ctgp.json', {'enabled': True})
    if layout == 'added':
        print(f"CTGP Deluxe importado: {manifest['courses']} pistas em 16 copas novas (terceira e quarta páginas da "
              "tela de copas); as pistas originais continuam nas copas delas.", flush=True)
    elif layout == 'dlc':
        print(f"CTGP Deluxe importado: {manifest['courses']} pistas nas copas do Booster Course Pass (com o DLC "
              "ativo); o jogo base fica original.", flush=True)
    else:
        print(f"CTGP Deluxe importado: {manifest['base_courses']} pistas no jogo base, "
              f"{manifest['courses'] - manifest['base_courses']} nas copas do Booster Course Pass (com o DLC ativo).",
              flush=True)
    if not manifest['names_verified']:
        print('Aviso: versão do CTGP diferente da 1.1.1; os nomes das pistas podem não corresponder.', flush=True)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path, help='Pasta extraída do ZIP do CTGP Deluxe')
    parser.add_argument('--romfs', required=True, type=Path, help='RomFS do MK8 4.0.1 preparado')
    parser.add_argument('--local', required=True, type=Path)
    parser.add_argument('--reinstall', action='store_true')
    parser.add_argument('--layout', choices=sorted(ctgp_pack.LAYOUTS), default='base',
                        help='base: no lugar das copas do jogo base; dlc: no lugar das copas do DLC; '
                             'added: em copas novas, sem tirar nenhuma pista')
    args = parser.parse_args()
    with exclusive_build(args.local):
        try:
            import_pack(args.source, args.romfs, args.local, args.reinstall, args.layout)
        except (ValueError, OSError) as error:
            print(f'Erro: {error}', flush=True)
            sys.exit(1)


if __name__ == '__main__':
    main()
