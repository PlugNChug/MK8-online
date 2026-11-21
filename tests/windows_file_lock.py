"""A real Windows sharing violation, scoped to a test-owned file."""
from contextlib import contextmanager
import ctypes
from ctypes import wintypes


@contextmanager
def locked_file(path, deny_read=False):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    flags = 0x02000000 if path.is_dir() else 0  # FILE_FLAG_BACKUP_SEMANTICS for a directory handle.
    handle = kernel.CreateFileW(str(path), 0x80000000, 0 if deny_read else 1, None, 3, flags, None)
    if handle == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        yield
    finally:
        kernel.CloseHandle(handle)
