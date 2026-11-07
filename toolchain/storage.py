"""Keep a disk reserve on the volume that receives generated build artifacts."""
import shutil
from pathlib import Path

GIB = 1024 ** 3


def require_space(path, reserve=4 * GIB):
    path = Path(path).resolve()
    while not path.exists():
        path = path.parent
    free = shutil.disk_usage(path).free
    if free < reserve:
        raise ValueError(f'Compilação interrompida para preservar espaço: {free/GIB:.1f} GiB livres; '
                         f'reserva mínima {reserve/GIB:.0f} GiB. Limpe intermediários e retome; os objetos concluídos serão reutilizados.')
