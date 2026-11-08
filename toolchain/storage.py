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


def hard_disk(path):
    """True on a disk that reports a seek penalty (hard disk), False on an SSD, None when Windows does not say.

    Same query as the installer's notice (IOCTL_STORAGE_QUERY_PROPERTY, StorageDeviceSeekPenaltyProperty); it needs no
    access rights, so no elevation.
    """
    import ctypes
    import os
    from ctypes import wintypes
    if os.name != 'nt':
        return None
    path = Path(path).resolve()
    while not path.exists():
        if path.parent == path:
            return None  # (a drive that does not exist)
        path = path.parent
    drive = path.drive
    if len(drive) != 2 or drive[1] != ':':
        return None
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.restype = wintypes.HANDLE
    handle = kernel.CreateFileW('\\\\.\\' + drive, 0, 3, None, 3, 0, None)
    if handle in (None, wintypes.HANDLE(-1).value):
        return None
    try:
        query = (ctypes.c_uint32 * 3)(7, 0, 0)
        answer = (ctypes.c_uint8 * 12)()
        returned = wintypes.DWORD()
        ok = kernel.DeviceIoControl(wintypes.HANDLE(handle), 0x002D1400, query, ctypes.sizeof(query), answer,
                                    ctypes.sizeof(answer), ctypes.byref(returned), None)
        return bool(answer[8]) if ok and returned.value >= 9 else None
    finally:
        kernel.CloseHandle(wintypes.HANDLE(handle))


def io_workers(path, cpu_bound=8):
    """Threads for a job that writes many files: fewer on a hard disk, where parallel writes mostly add seeks."""
    import os
    if os.environ.get('MK8_IMPORT_JOBS'):
        jobs = int(os.environ['MK8_IMPORT_JOBS'])
        if not 1 <= jobs <= 16:
            raise ValueError('MK8_IMPORT_JOBS deve estar entre 1 e 16')
        return jobs
    if hard_disk(path):
        return 2
    return max(1, min(cpu_bound, (os.cpu_count() or 2) // 2))
