"""A capture directory's manifests, and the reader of the CSV files they bind.

A capture has two manifests, each in the format ``shasum -a 256 -c`` reads from inside its own
directory (``<sha256>  <relative path>`` per line, sorted by path):

* ``<capture>/runs/manifest.sha256`` lists every file under ``runs/``, with paths relative to
  ``runs/``;
* ``<capture>/manifest.sha256`` lists the small files directly in the capture (``context.json``,
  ``design.csv``, ``runs.csv``, ``outputs.csv``, ``bins.csv``) and ``runs/manifest.sha256``.

The stages that read a capture check only the top level (:func:`check_manifest`), so a copy of
the small files and ``runs/manifest.sha256``, without the per-run files, is a capture they accept.

Usage:
    write_manifests(capture)
    files = check_manifest(capture, ("context.json", "runs.csv"))
    rows = read_csv_rows(files["runs.csv"], capture / "runs.csv", ("id", "set"))
"""

from __future__ import annotations

import csv
import io
import json
import re
from pathlib import Path
from typing import Iterable, Sequence

from mimic_bench.provenance import sha256_bytes

MANIFEST = "manifest.sha256"
RUNS_DIRECTORY = "runs"

_DIGEST = re.compile(r"[0-9a-f]{64}")


class ManifestError(ValueError):
    """A capture whose manifest, or a file it binds, cannot be used; the message names the file."""


def write_manifest(directory: Path, relatives: Iterable[str]) -> Path:
    """Write ``<directory>/manifest.sha256`` over the given files, sorted by path.

    Args:
        directory: The directory the paths are relative to.
        relatives: POSIX paths relative to ``directory``.

    Returns:
        The manifest's path.
    """
    directory = Path(directory)
    lines = [
        f"{sha256_bytes((directory / relative).read_bytes())}  {relative}\n"
        for relative in sorted(relatives)
    ]
    manifest = directory / MANIFEST
    manifest.write_text("".join(lines))
    return manifest


def write_manifests(capture: Path) -> Path:
    """Write a capture's two manifests: ``runs/`` first, then the top level that binds it.

    The top level lists every file directly in ``capture`` (its manifest excepted) and
    ``runs/manifest.sha256``; ``runs/manifest.sha256`` lists every file under ``runs/``.

    Returns:
        The top-level manifest's path.
    """
    capture = Path(capture)
    runs = capture / RUNS_DIRECTORY
    write_manifest(
        runs,
        (
            path.relative_to(runs).as_posix()
            for path in runs.rglob("*")
            if path.is_file() and path != runs / MANIFEST
        ),
    )
    small = [p.name for p in capture.iterdir() if p.is_file() and p.name != MANIFEST]
    return write_manifest(capture, [*small, f"{RUNS_DIRECTORY}/{MANIFEST}"])


def parse_manifest(data: bytes, path: Path) -> dict[str, str]:
    """Parse a manifest into ``{relative path: sha256}``.

    Raises:
        ManifestError: If a line is not ``<64 hex digits>  <path>``, or a path repeats.
    """
    entries: dict[str, str] = {}
    for number, line in enumerate(data.decode("utf-8", errors="replace").splitlines(), start=1):
        digest, separator, relative = line.partition("  ")
        if not separator or not _DIGEST.fullmatch(digest) or not relative:
            raise ManifestError(f"{path}:{number}: malformed manifest line {line!r}")
        if relative in entries:
            raise ManifestError(f"{path}:{number}: {relative} is listed twice")
        entries[relative] = digest
    return entries


def check_manifest(directory: Path, required: Sequence[str]) -> dict[str, bytes]:
    """Check a capture's top-level manifest; return its bytes and those of every file it lists.

    Each listed file is read once and its SHA-256 taken from the bytes returned, so a caller
    parses exactly what was checked. The per-run files are not read; ``runs/manifest.sha256``,
    which binds them, is checked like any other listed file.

    Args:
        directory: The capture directory.
        required: Files the caller reads, each of which must be listed.

    Returns:
        ``{relative path: bytes}`` for the manifest (:data:`MANIFEST`) and every listed file.

    Raises:
        ManifestError: If the manifest is missing or malformed, a required file is not listed,
            or a listed file is missing or differs from its entry.
    """
    directory = Path(directory)
    path = directory / MANIFEST
    try:
        manifest = path.read_bytes()
    except OSError as exc:
        raise ManifestError(f"{path}: cannot read the capture's manifest ({exc.strerror})") from exc
    entries = parse_manifest(manifest, path)
    missing = [name for name in required if name not in entries]
    if missing:
        raise ManifestError(f"{path}: has no entry for {', '.join(missing)}")
    files = {MANIFEST: manifest}
    for relative, digest in entries.items():
        target = directory / relative
        try:
            files[relative] = target.read_bytes()
        except OSError as exc:
            raise ManifestError(
                f"{target}: listed in {MANIFEST} and unreadable ({exc.strerror})"
            ) from exc
        if sha256_bytes(files[relative]) != digest:
            raise ManifestError(f"{target}: SHA-256 differs from its {MANIFEST} entry")
    return files


def read_json_object(data: bytes, path: Path) -> dict:
    """Parse a capture's JSON object (``context.json``).

    Raises:
        ManifestError: If the bytes are not a UTF-8 JSON object.
    """
    try:
        document = json.loads(data.decode("utf-8"))
    except ValueError as exc:
        raise ManifestError(f"{path}: not valid JSON ({exc})") from exc
    if not isinstance(document, dict):
        raise ManifestError(f"{path}: not a JSON object")
    return document


def read_csv_rows(data: bytes, path: Path, header: Sequence[str] | None = None) -> list[list[str]]:
    """Parse a capture CSV whose first column is a unique identifier; return its rows.

    Args:
        data: The file's bytes (UTF-8).
        path: The file, named in messages.
        header: The exact header expected, or ``None`` to accept any header beginning ``id``.

    Returns:
        The header, then every row, as lists of cells.

    Raises:
        ManifestError: If the file is empty, the header differs, a line is blank, a row's width
            differs from the header's, or an identifier is empty or repeats.
    """
    rows = list(csv.reader(io.StringIO(data.decode("utf-8", errors="replace"))))
    if not rows:
        raise ManifestError(f"{path}: empty")
    if (header is not None and rows[0] != list(header)) or rows[0][:1] != ["id"]:
        raise ManifestError(f"{path}: header must be {','.join(header or ['id', '...'])}")
    seen: dict[str, int] = {}
    for number, row in enumerate(rows[1:], start=2):
        if not row:
            raise ManifestError(f"{path}:{number}: blank line")
        if len(row) != len(rows[0]):
            raise ManifestError(f"{path}:{number}: {len(row)} cells, the header has {len(rows[0])}")
        if not row[0]:
            raise ManifestError(f"{path}:{number}: empty id")
        if row[0] in seen:
            raise ManifestError(
                f"{path}:{number}: id {row[0]} is on more than one row (first line {seen[row[0]]})"
            )
        seen[row[0]] = number
    return rows
