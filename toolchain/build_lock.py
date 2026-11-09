"""One preparation per data directory; OS locks survive neither crashes nor cancellation."""
from contextlib import contextmanager
import os
from pathlib import Path


@contextmanager
def exclusive_build(folder):
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    # Never unlink the lock file: another process may already hold its handle.
    with (folder / '.build.lock').open('a+b') as handle:
        if os.name == 'nt':
            import msvcrt
            if handle.seek(0, 2) == 0:
                handle.write(b'\0')
                handle.flush()
            handle.seek(0)
        else:
            import fcntl
        try:
            if os.name == 'nt':
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError('Já existe uma preparação usando esta pasta de dados.') from error
        try:
            yield
        finally:
            if os.name == 'nt':
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle, fcntl.LOCK_UN)
