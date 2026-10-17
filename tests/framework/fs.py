"""Filesystem helpers for the gates and manual tests that remove their own scratch trees."""

import shutil
from pathlib import Path


def remove_tree(path) -> None:
    """Remove the directory tree at ``path``, retrying once if an entry appears meanwhile.

    On macOS, Finder can recreate ``.DS_Store`` in a directory between
    ``shutil.rmtree``'s listing of it and its final ``rmdir``, which then fails
    with OSError (directory not empty). One retry removes the late entry. A
    directory that still exists after the retry raises the first OSError, so a
    tree that cannot be removed is never left behind silently.
    """
    path = Path(path)
    try:
        shutil.rmtree(path)
    except OSError:
        shutil.rmtree(path, ignore_errors=True)
        if path.exists():
            raise
