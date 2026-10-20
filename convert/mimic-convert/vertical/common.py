"""Helpers shared by the vertical converter's modules: argument validation and durable JSON
writes."""

import json
import os
import sys
from pathlib import Path
from typing import Optional

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from errors import ConverterError  # noqa: E402


def require_positive_int(name: str, value, maximum: Optional[int] = None) -> int:
    """``value`` as a plain ``int`` of at least 1 (and at most ``maximum`` when given).

    Raises:
        ConverterError: naming ``name`` and the value, for a bool or non-integer, a value below
            1, or one above ``maximum``.
    """
    if isinstance(value, bool) or not isinstance(value, (int, np.integer)) or int(value) < 1:
        raise ConverterError("{} must be a positive integer, got {!r}".format(name, value))
    if maximum is not None and int(value) > maximum:
        raise ConverterError("{} must be at most {}, got {}".format(name, maximum, value))
    return int(value)


def fsync_directory(path: Path) -> None:
    """fsync a directory so a rename inside it is durable (POSIX)."""
    try:
        descriptor = os.open(str(path), os.O_RDONLY)
    except OSError:  # pragma: no cover - platforms without directory fds
        return
    try:
        os.fsync(descriptor)
    except OSError:  # pragma: no cover - some filesystems refuse directory fsync
        pass
    finally:
        os.close(descriptor)


def write_json_atomic(path: Path, document) -> None:
    """Write ``document`` to ``path`` through a fsynced sibling temporary file and a rename,
    so a crash leaves the previous file or this one, never a torn one."""
    tmp = path.with_name(path.name + ".tmp")
    text = json.dumps(document, indent=2, sort_keys=True, allow_nan=False) + "\n"
    with open(tmp, "w", encoding="utf-8") as handle:
        handle.write(text)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(str(tmp), str(path))
    fsync_directory(path.parent)
