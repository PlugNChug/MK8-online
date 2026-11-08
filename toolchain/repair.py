"""Folders the pipeline made itself are redone instead of stopping the build.

An import cut off halfway, a prepared runtime whose files changed, or a junction left by an older launcher in the
game data used to stop the build with a message asking for a new preparation; the user then had to find and delete
folders by hand. These helpers remove only what the pipeline created inside the folder that owns it, and never
follow a link: a junction or symbolic link is removed as a link and its target is left untouched (an old
`game-data/.../romfs` junction pointed at the user's own extracted game).
"""
import os
from pathlib import Path


def is_link(path):
    path = Path(path)
    return path.is_symlink() or bool(getattr(path, "is_junction", lambda: False)())


def unlink_link(path):
    """Removes `path` if it is a junction or symbolic link (only the link). True if something was removed."""
    path = Path(path)
    if not is_link(path):
        return False
    try:
        os.rmdir(path)  # a junction or a directory symlink
    except OSError:
        path.unlink()
    return True


def _remove_tree(path):
    with os.scandir(path) as entries:
        for entry in entries:
            child = Path(entry.path)
            if is_link(child):
                unlink_link(child)
            elif entry.is_dir(follow_symlinks=False):
                _remove_tree(child)
            else:
                try:
                    child.unlink()
                except PermissionError:
                    os.chmod(child, 0o666)
                    child.unlink()
    os.rmdir(path)


def remove_owned(path, owner):
    """Deletes `path`, a folder the pipeline created inside `owner`, without following links inside it."""
    path, owner = Path(os.path.abspath(path)), Path(os.path.abspath(owner))
    if path == owner or not path.is_relative_to(owner):
        raise ValueError(f"Remoção recusada fora da pasta de dados: {path}")
    if is_link(path):
        unlink_link(path)
    elif path.is_dir():
        _remove_tree(path)
    elif path.exists():
        path.unlink()


def unlink_links_on_the_way(path, owner):
    """Removes junctions/links among `path` and its parents below `owner` (links only): files are then written into
    real folders of the data folder, never through a link into the folder it points at."""
    path, owner = Path(os.path.abspath(path)), Path(os.path.abspath(owner))
    removed = []
    for part in reversed([path, *path.parents]):
        if part != owner and part.is_relative_to(owner) and unlink_link(part):
            removed.append(part)
    return removed


def cleanup_stale(folder, pattern):
    """Leftovers of steps cut off before they finished (their names are unique per attempt)."""
    for stale in list(Path(folder).glob(pattern)) if Path(folder).is_dir() else ():
        try:
            remove_owned(stale, folder)
        except OSError as error:
            print(f"Aviso: sobra de uma etapa anterior não removida ({stale.name}): {error}", flush=True)
