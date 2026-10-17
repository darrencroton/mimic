"""The L-Halo file writer: every planned file of a conversion written, verified, renamed and
recorded in the conversion manifest, one file per worker at a time.

**A file.** ``<tree_name>.<k>`` holds the forests of the file plan's entry ``k`` in ascending
``ForestIndex``: the header (``int32`` ``ntrees``, ``int32`` ``totNHalos``, then one ``int32``
halo count per forest in forest order, from the plan's forest totals), then every forest's
records as :class:`vertical.assemble.ForestAssembler` yields them, in rank order with
forest-local links. :func:`convert_file` writes it to ``<tree_name>.<k>.partial``, computing
its SHA-256 as it writes, fsyncs it, re-reads it whole and checks it (the header against the
plan, the size against the header plus ``totNHalos`` records, and the SHA-256 of the re-read
stream against the one computed while writing), and renames it only then. A file that fails
a check raises :class:`VerificationError` and stays ``.partial``.

**The run.** :func:`convert` refuses, before any halo row is read, an invalid ``tree_name``,
worker count or window, a dataset the open-time checks of
:class:`horizontal_dataset.HorizontalDataset` refuse, a missing or mismatching
``file_plan.json`` (:func:`vertical.plan.read_file_plan`), a record the dataset's columns cannot
fill, and a manifest written for another conversion. It then derives every forest's halo
total (:func:`vertical.plan.forest_totals`), requires every planned file's total to agree,
prints the memory bound per worker and converts the selected files, the largest first, in
``workers`` processes, at most ``workers`` files submitted at a time. Every forest's assembled
halo count must equal its total, which is refused otherwise naming the forest. A file that
fails, by a refusal, a verification failure or any other error, is reported and the run goes
on, recording every file that completes; when a worker process dies, the files in flight fail,
no further file is started, and a recorded file that was not moved aside keeps its entry.

**The manifest.** ``manifest.json`` records the dataset identity, the record (the package's
``halo_properties.yaml`` path and md5, the layout and its digest), the plan's md5 and caps, the
``tree_name``, every invocation (the converter's commit and command line) and, per completed
file, its number, forest range, forest and halo counts, byte size, SHA-256, wall time, worker
and invocation. Only the parent process writes it, atomically, after every completed file.

**Resume.** A rerun with the same inputs skips every selected file whose on-disk SHA-256
equals its manifest entry's and redoes the rest. A file of a planned name that the manifest
does not record as complete (a run killed between the rename and the manifest write), and any
``.partial``, are moved aside to ``<name>.stale-<timestamp>`` and logged with their sizes, never
deleted, and the file is rebuilt.
"""

import hashlib
import json
import multiprocessing
import os
import subprocess
import sys
import time
from concurrent.futures import FIRST_COMPLETED, ProcessPoolExecutor, wait
from concurrent.futures.process import BrokenProcessPool
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Sequence, Tuple, Union

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from errors import ConverterError  # noqa: E402
from horizontal_dataset import HorizontalDataset  # noqa: E402
from vertical.assemble import DEFAULT_WINDOW_ROWS, ForestAssembler  # noqa: E402
from vertical.plan import (  # noqa: E402
    FILE_PLAN_NAME,
    dataset_identity,
    forest_totals,
    read_file_plan,
)
from vertical.record import record_digest, record_layout  # noqa: E402

#: The manifest's file name inside a conversion's output directory.
MANIFEST_NAME = "manifest.json"

#: The ``format`` and ``version`` a manifest declares.
MANIFEST_FORMAT = "mimic-vertical-conversion-manifest"
MANIFEST_VERSION = 1

#: The suffix of a file being written; it is renamed away only once verified.
PARTIAL_SUFFIX = ".partial"

#: The L-Halo header's integer type: the reader's C ``int``.
HEADER_DTYPE = np.dtype("<i4")

#: Bytes read per block when a written file is re-read and when an existing file is hashed.
VERIFY_BLOCK_BYTES = 64 << 20

#: The write buffer of a file being written.
WRITE_BUFFER_BYTES = 8 << 20

#: Bytes per forest of a file's header while it is written and checked: the plan's int64 total
#: and the header's int32 count.
HEADER_BYTES_PER_FOREST = 12

#: Exit statuses: every selected file complete, a verification failure, a refusal.
EXIT_OK = 0
EXIT_VERIFICATION = 1
EXIT_REFUSED = 2

#: The fields of the dataset identity a manifest is bound to (the directory is not one: a
#: moved dataset is the same dataset).
BINDING_DATASET_FIELDS = (
    "forest_id_sha256",
    "n_forests_total",
    "total_halos",
    "column_mapping_sha256",
    "links_adjacent",
)


class VerificationError(ConverterError):
    """A written file failed a check of its re-read: it is left ``.partial``, never renamed."""


def _log(message: str) -> None:
    print(message, flush=True)


def file_name(tree_name: str, number: int) -> str:
    """The reader's file name of file ``number``: ``<tree_name>.<number>``."""
    return "{}.{}".format(tree_name, number)


def header_bytes(n_forests: int) -> int:
    """The bytes of an L-Halo header for ``n_forests`` forests."""
    return HEADER_DTYPE.itemsize * (2 + int(n_forests))


def check_tree_name(tree_name) -> str:
    """``tree_name`` as a file name prefix.

    Raises:
        ConverterError: for an empty name, one with a path separator or a NUL, or ``.``/``..``.
    """
    separators = {"/", os.sep} | ({os.altsep} if os.altsep else set())
    if (
        not isinstance(tree_name, str)
        or tree_name in ("", ".", "..")
        or "\0" in tree_name
        or any(separator in tree_name for separator in separators)
    ):
        raise ConverterError(
            "tree_name {!r} must be a plain file name prefix, without a path separator".format(
                tree_name
            )
        )
    return tree_name


def _require_positive(name: str, value) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, np.integer)) or int(value) < 1:
        raise ConverterError("{} must be a positive integer, got {!r}".format(name, value))
    return int(value)


def parse_file_selection(text: str) -> List[int]:
    """The file numbers of ``a,b-c`` (single numbers and inclusive ranges), ascending and
    distinct.

    Raises:
        ConverterError: for an empty selection, a term that is not a number or a range, or a
            range whose end is below its start.
    """
    numbers = set()
    for term in str(text).split(","):
        term = term.strip()
        low, dash, high = term.partition("-")
        try:
            first = int(low)
            last = int(high) if dash else first
        except ValueError:
            raise ConverterError(
                "--only-files: {!r} is not a file number or a range a-b".format(term)
            ) from None
        if first < 0 or last < first:
            raise ConverterError("--only-files: {!r} is not an ascending range".format(term))
        numbers.update(range(first, last + 1))
    return sorted(numbers)


def worker_memory_bound(
    assembler: ForestAssembler, largest_forest_halos: int, most_forests_in_a_file: int
) -> Dict[str, int]:
    """The bytes one worker holds at most, by term: the assembler's bound for the largest
    forest, the one re-read buffer, the write buffer and the largest file's header."""
    terms = {
        "assembler": int(assembler.memory_bound(largest_forest_halos)),
        "reread_block": VERIFY_BLOCK_BYTES,
        "write_buffer": WRITE_BUFFER_BYTES,
        "header": HEADER_BYTES_PER_FOREST * int(most_forests_in_a_file),
    }
    terms["total"] = sum(terms.values())
    return terms


def _size(n_bytes: int) -> str:
    if n_bytes >= 1 << 30:
        return "{:.2f} GiB".format(n_bytes / float(1 << 30))
    return "{:.1f} MiB".format(n_bytes / float(1 << 20))


# ---- one file ---------------------------------------------------------------


@dataclass(frozen=True)
class FileJob:
    """Everything a worker needs to write one file; picklable for a worker process."""

    dataset: str
    halo_properties: Union[str, Tuple[dict, ...]]
    window_rows: int
    output: str
    tree_name: str
    number: int
    first_forest: int
    last_forest: int
    n_halos: int
    #: the plan's halo total of every forest of the file, in forest order
    counts: np.ndarray
    #: the SHA-256 the manifest records for the file, or ``None``
    expected_sha256: Optional[str]
    #: the timestamp of moved-aside names
    stamp: str


class _RangeTotals:
    """The plan's totals of one file's forests, indexed by ``ForestIndex`` as the assembler
    indexes its expected totals (by slices within the file's range)."""

    def __init__(self, first: int, counts: np.ndarray):
        self.first = first
        self.counts = counts

    def __getitem__(self, key: slice) -> np.ndarray:
        return self.counts[key.start - self.first : key.stop - self.first]


def _blocks(path, block_bytes: int = VERIFY_BLOCK_BYTES) -> Iterator[memoryview]:
    """A file's bytes in blocks read into one reused buffer of ``block_bytes``: each block is
    valid only until the next is read."""
    buffer = bytearray(block_bytes)
    view = memoryview(buffer)
    with open(path, "rb", buffering=0) as handle:
        while True:
            n_read = handle.readinto(buffer)
            if not n_read:
                return
            yield view[:n_read]


def sha256_file(path, block_bytes: int = VERIFY_BLOCK_BYTES) -> str:
    """The SHA-256 of a file's contents, read in blocks into one buffer."""
    digest = hashlib.sha256()
    for block in _blocks(path, block_bytes):
        digest.update(block)
    return digest.hexdigest()


def _fsync_directory(path: Path) -> None:
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


def move_aside(path: Path, stamp: str) -> Path:
    """Rename ``path`` to ``<path>.stale-<stamp>`` (with ``.<n>`` added if that name is taken),
    logging both names and the size; nothing is deleted."""
    target = path.with_name("{}.stale-{}".format(path.name, stamp))
    suffix = 1
    while target.exists() or target.is_symlink():
        target = path.with_name("{}.stale-{}.{}".format(path.name, stamp, suffix))
        suffix += 1
    size = path.lstat().st_size
    os.rename(str(path), str(target))
    _log("moved aside {} ({} bytes) to {}".format(path, size, target.name))
    return target


def _reread(path: Path, block_bytes: int = VERIFY_BLOCK_BYTES) -> Iterator[memoryview]:
    """The bytes of a written file, re-read from disk in blocks into one reused buffer."""
    return _blocks(path, block_bytes)


def _write_partial(job: FileJob, assembler: ForestAssembler, partial: Path) -> str:
    """Write the file's header and every forest's records to ``partial``; returns the SHA-256
    of the bytes written. Each forest's records are written before the next is assembled."""
    counts = np.asarray(job.counts, dtype=np.int64)
    header = np.concatenate(([counts.size, job.n_halos], counts)).astype(HEADER_DTYPE)
    digest = hashlib.sha256()
    expected = _RangeTotals(job.first_forest, counts)
    with open(partial, "xb", buffering=WRITE_BUFFER_BYTES) as handle:
        handle.write(header.tobytes())
        digest.update(header.tobytes())
        for forest in assembler.forests(job.first_forest, job.last_forest + 1, expected):
            view = forest.records.view(np.uint8)
            handle.write(view)
            digest.update(view)
        handle.flush()
        os.fsync(handle.fileno())
    return digest.hexdigest()


def _verify(job: FileJob, partial: Path, final: Path, record_bytes: int, written: str) -> int:
    """Re-read ``partial`` whole and check it; returns its size.

    Raises:
        VerificationError: naming the file and the check, for a header whose ``ntrees``,
            ``totNHalos`` or counts are not the plan's, a size other than the header plus
            ``totNHalos`` records, or a SHA-256 other than the one computed while writing.
    """
    counts = np.asarray(job.counts, dtype=np.int64)
    head = header_bytes(counts.size)
    expected_size = head + job.n_halos * record_bytes
    digest = hashlib.sha256()
    header = bytearray()
    size = 0
    for block in _reread(partial):
        if len(header) < head:
            header += block[: head - len(header)]
        digest.update(block)
        size += len(block)

    def fail(check: str, detail: str) -> VerificationError:
        return VerificationError(
            "{}: verification failed ({}): {}; {} is not written".format(
                partial, check, detail, final.name
            )
        )

    if len(header) < head:
        raise fail("header", "{} bytes, shorter than the {}-byte header".format(size, head))
    values = np.frombuffer(bytes(header), dtype=HEADER_DTYPE)
    if int(values[0]) != counts.size:
        raise fail("header ntrees", "{}, the plan has {}".format(values[0], counts.size))
    if int(values[1]) != job.n_halos:
        raise fail("header totNHalos", "{}, the plan has {}".format(values[1], job.n_halos))
    differs = np.flatnonzero(values[2:] != counts)
    if differs.size:
        index = int(differs[0])
        raise fail(
            "header counts",
            "forest {} has count {}, the plan has {}".format(
                job.first_forest + index, values[2 + index], counts[index]
            ),
        )
    if size != expected_size:
        raise fail(
            "size",
            "{} bytes, not the header's {} plus {} records of {} bytes ({})".format(
                size, head, job.n_halos, record_bytes, expected_size
            ),
        )
    if digest.hexdigest() != written:
        raise fail(
            "sha256",
            "the re-read file's {} is not the {} computed while writing".format(
                digest.hexdigest(), written
            ),
        )
    return size


def convert_file(job: FileJob) -> Dict[str, object]:
    """Write, verify and rename one file; the worker's whole job.

    A leftover ``.partial`` is moved aside first. An existing final file whose SHA-256 equals
    ``job.expected_sha256`` is kept and the file skipped; any other is moved aside and rebuilt.

    Returns:
        ``{"number", "skipped"}`` and, for a written file, its manifest ``entry``.

    Raises:
        ConverterError: a refusal of the dataset or of a forest in the file's range.
        VerificationError: a failed check of the written file, which stays ``.partial``.
    """
    started = time.monotonic()
    output = Path(job.output)
    final = output / file_name(job.tree_name, job.number)
    partial = final.with_name(final.name + PARTIAL_SUFFIX)
    if partial.exists() or partial.is_symlink():
        move_aside(partial, job.stamp)
    if final.exists() or final.is_symlink():
        if job.expected_sha256 is not None and sha256_file(final) == job.expected_sha256:
            _log("file {}: {} matches its manifest entry; skipped".format(job.number, final.name))
            return {"number": job.number, "skipped": True}
        move_aside(final, job.stamp)
    dataset = HorizontalDataset(job.dataset)
    assembler = ForestAssembler(dataset, job.halo_properties, job.window_rows)
    written = _write_partial(job, assembler, partial)
    size = _verify(job, partial, final, int(assembler.dtype.itemsize), written)
    os.rename(str(partial), str(final))
    _fsync_directory(output)
    entry = {
        "number": job.number,
        "name": final.name,
        "first_forest": job.first_forest,
        "last_forest": job.last_forest,
        "n_forests": job.last_forest - job.first_forest + 1,
        "n_halos": job.n_halos,
        "bytes": size,
        "sha256": written,
        "wall_seconds": round(time.monotonic() - started, 3),
        "worker": os.getpid(),
    }
    return {"number": job.number, "skipped": False, "entry": entry}


# ---- the manifest -----------------------------------------------------------


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
    _fsync_directory(path.parent)


def converter_identity() -> Dict[str, object]:
    """The converter's git commit and whether its directory has uncommitted changes (both
    ``None`` outside a git checkout)."""
    directory = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    def git(*args) -> Optional[str]:
        try:
            result = subprocess.run(
                ["git"] + list(args), cwd=directory, capture_output=True, text=True, check=False
            )
        except OSError:
            return None
        return result.stdout if result.returncode == 0 else None

    commit = git("rev-parse", "HEAD")
    status = git("status", "--porcelain", "--", ".")
    return {
        "commit": None if commit is None else commit.strip(),
        "dirty": None if status is None else bool(status.strip()),
    }


def _manifest_binding(
    dataset: HorizontalDataset,
    halo_properties,
    record_dtype: np.dtype,
    plan_path: Path,
    plan: Dict[str, object],
    tree_name: str,
) -> Dict[str, object]:
    """The manifest's description of the conversion it records, before any file."""
    identity = dataset_identity(dataset)
    identity["links_adjacent"] = dataset.links_adjacent
    identity["directory"] = str(dataset.directory)
    layout = record_layout(record_dtype)
    record = {"layout": layout, "digest": record_digest(layout)}
    if isinstance(halo_properties, (str, Path)):
        record["halo_properties"] = str(halo_properties)
        record["halo_properties_md5"] = hashlib.md5(Path(halo_properties).read_bytes()).hexdigest()
    else:
        record["halo_properties"] = None
        record["halo_properties_md5"] = None
    return {
        "format": MANIFEST_FORMAT,
        "version": MANIFEST_VERSION,
        "dataset": identity,
        "record": record,
        "plan": {
            "md5": hashlib.md5(plan_path.read_bytes()).hexdigest(),
            "caps": plan["caps"],
            "n_files": len(plan["files"]),
        },
        "tree_name": tree_name,
        "runs": [],
        "files": [],
    }


def _load_manifest(path: Path, binding: Dict[str, object]) -> Dict[str, object]:
    """The existing manifest at ``path`` if it records this conversion, else ``binding``.

    Raises:
        ConverterError: for an unreadable or malformed manifest, or one recording another
            dataset, record, plan or ``tree_name``, naming what differs.
    """
    if not path.exists():
        return binding
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
        if manifest.get("format") != MANIFEST_FORMAT or manifest.get("version") != (
            MANIFEST_VERSION
        ):
            raise ValueError("not a {} version {} file".format(MANIFEST_FORMAT, MANIFEST_VERSION))
        recorded = {
            "dataset": {name: manifest["dataset"][name] for name in BINDING_DATASET_FIELDS},
            "record digest": manifest["record"]["digest"],
            "plan md5": manifest["plan"]["md5"],
            "tree_name": manifest["tree_name"],
        }
        files = manifest["files"]
        runs = manifest["runs"]
        if not isinstance(files, list) or not isinstance(runs, list):
            raise ValueError("files and runs must be lists")
        numbers = [entry["number"] for entry in files]
        if not all(isinstance(entry["sha256"], str) for entry in files):
            raise ValueError("a file entry's sha256 is not a string")
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        raise ConverterError(
            "{}: malformed conversion manifest ({})".format(path, error)
        ) from error
    if len(set(numbers)) != len(numbers):
        raise ConverterError("{}: a file is recorded twice".format(path))
    current = {
        "dataset": {name: binding["dataset"][name] for name in BINDING_DATASET_FIELDS},
        "record digest": binding["record"]["digest"],
        "plan md5": binding["plan"]["md5"],
        "tree_name": binding["tree_name"],
    }
    for name, value in current.items():
        if recorded[name] != value:
            raise ConverterError(
                "{}: records a conversion whose {} is {!r}, not this one's {!r}; convert into "
                "another directory, or move this one's files aside".format(
                    path, name, recorded[name], value
                )
            )
    return manifest


def _set_entry(manifest: Dict[str, object], entry: Optional[Dict[str, object]], number: int):
    """Replace (or, with ``entry`` ``None``, drop) file ``number``'s manifest entry."""
    files = [item for item in manifest["files"] if item["number"] != number]
    if entry is not None:
        files.append(entry)
    manifest["files"] = sorted(files, key=lambda item: item["number"])


# ---- the run ----------------------------------------------------------------


@dataclass(frozen=True)
class _Failure:
    """A file that did not complete: whether it failed its verification, and the message.
    Only the text is kept, so no exception (or the arrays its traceback holds) outlives it."""

    verification: bool
    message: str


def _failure(job: FileJob, error: Exception) -> _Failure:
    """The failure of ``job`` by ``error``: a verification failure, a refusal, or any other
    exception, which is a refusal of the file naming its type."""
    if isinstance(error, VerificationError):
        return _Failure(True, str(error))
    if isinstance(error, (ConverterError, OSError)):
        return _Failure(False, str(error))
    return _Failure(
        False, "file {}: unexpected {}: {}".format(job.number, type(error).__name__, error)
    )


def _run_jobs(jobs: Iterator[FileJob], workers: int) -> Iterator[Tuple[FileJob, object]]:
    """Run ``convert_file`` over ``jobs``, yielding ``(job, result or _Failure)`` as each
    completes: in this process for one worker, else in ``workers`` spawned processes with at
    most ``workers`` jobs submitted at a time, in ``jobs`` order. Every ``Exception`` of a job
    is that job's failure; a ``BaseException`` that is not one (``KeyboardInterrupt``)
    propagates. When a worker process dies, the jobs in flight fail, the death is logged once
    and no further job is submitted."""
    if workers == 1:
        for job in jobs:
            try:
                outcome = convert_file(job)
            except Exception as error:
                outcome = _failure(job, error)
            yield job, outcome
        return
    executor = ProcessPoolExecutor(
        max_workers=workers, mp_context=multiprocessing.get_context("spawn")
    )
    pending = {}
    broken = False

    def break_pool() -> None:
        nonlocal broken
        if not broken:
            _log(
                "a worker process died (killed, or out of memory): the files in flight are not "
                "completed and no further file is started"
            )
        broken = True

    def submit_next() -> None:
        job = next(jobs, None)
        if job is None:
            return
        try:
            pending[executor.submit(convert_file, job)] = job
        except BrokenProcessPool:
            break_pool()

    try:
        for _ in range(workers):
            submit_next()
        while pending:
            done, _ = wait(pending, return_when=FIRST_COMPLETED)
            for future in done:
                job = pending.pop(future)
                try:
                    outcome = future.result()
                except BrokenProcessPool:
                    break_pool()
                    outcome = _Failure(
                        False, "file {}: in flight when a worker process died".format(job.number)
                    )
                except Exception as error:
                    outcome = _failure(job, error)
                yield job, outcome
                if not broken:
                    submit_next()
    finally:
        executor.shutdown(wait=True, cancel_futures=True)


def convert(
    dataset_dir,
    halo_properties,
    output,
    tree_name: str,
    workers: int = 1,
    window_rows: int = DEFAULT_WINDOW_ROWS,
    only_files: Optional[Sequence[int]] = None,
    command_line: Sequence[str] = (),
) -> int:
    """Convert the planned files of ``output`` (or the selected ones) and record them.

    Args:
        dataset_dir: the version 3 dataset directory.
        halo_properties: the package's ``halo_properties.yaml`` (the record).
        output: the directory holding ``file_plan.json``; the files and ``manifest.json`` are
            written there.
        tree_name: the files' name prefix.
        workers: files converted concurrently, one per worker process.
        window_rows: rows per window the assembler reads from each slab.
        only_files: the file numbers to convert (default: every planned file).
        command_line: recorded in the manifest.

    Returns:
        :data:`EXIT_OK` when every selected file is complete, :data:`EXIT_VERIFICATION` when a
        file failed its verification and none was refused, :data:`EXIT_REFUSED` when a file
        was refused. Every file that completes is recorded either way.

    Raises:
        ConverterError: a refusal before any file is attempted.
    """
    check_tree_name(tree_name)
    workers = _require_positive("workers", workers)
    window_rows = _require_positive("window_rows", window_rows)
    output = Path(output)
    dataset = HorizontalDataset(dataset_dir)
    plan = read_file_plan(output, dataset, halo_properties)
    planned = plan["files"]
    if only_files is None:
        selected = [entry["number"] for entry in planned]
    else:
        selected = sorted(set(int(number) for number in only_files))
        outside = [number for number in selected if not 0 <= number < len(planned)]
        if not selected or outside:
            raise ConverterError(
                "--only-files {}: the plan's files are 0 to {}".format(
                    outside or "selects nothing", len(planned) - 1
                )
            )
    assembler = ForestAssembler(dataset, halo_properties, window_rows)
    manifest_path = output / MANIFEST_NAME
    binding = _manifest_binding(
        dataset, halo_properties, assembler.dtype, output / FILE_PLAN_NAME, plan, tree_name
    )
    manifest = _load_manifest(manifest_path, binding)

    _log("deriving every forest's halo total from {}".format(dataset.directory))
    totals = forest_totals(dataset)
    for entry in planned:
        found = int(totals[entry["first_forest"] : entry["last_forest"] + 1].sum())
        if found != entry["n_halos"]:
            raise ConverterError(
                "{}: file {} plans {} halos; its forests hold {}".format(
                    output / FILE_PLAN_NAME, entry["number"], entry["n_halos"], found
                )
            )
    largest = int(totals.max()) if totals.size else 0
    bound = worker_memory_bound(
        assembler, largest, max((entry["n_forests"] for entry in planned), default=0)
    )
    _log(
        "memory bound per worker: {} bytes ({}): the assembler's {} for --window-rows {} over {} "
        "slabs, {} B per row and {} B per record, with the plan's largest forest of {} halos; "
        "the {} re-read buffer, the {} write buffer and {} B of header for the plan's most "
        "forests in a file; {} worker(s): {}; the forest totals held here: {} (8 B per "
        "forest)".format(
            bound["total"],
            _size(bound["total"]),
            _size(bound["assembler"]),
            window_rows,
            len(dataset.snapshots),
            assembler.row_bytes,
            assembler.dtype.itemsize,
            largest,
            _size(bound["reread_block"]),
            _size(bound["write_buffer"]),
            bound["header"],
            workers,
            _size(workers * bound["total"]),
            _size(totals.nbytes),
        )
    )

    for number in selected:
        name = file_name(tree_name, number)
        if not (output / name).exists():
            _set_entry(manifest, None, number)
    manifest["runs"].append(
        dict(
            converter_identity(),
            command_line=list(command_line),
            started_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            workers=workers,
            window_rows=window_rows,
        )
    )
    run_index = len(manifest["runs"]) - 1
    write_json_atomic(manifest_path, manifest)

    recorded = {entry["number"]: entry["sha256"] for entry in manifest["files"]}
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    properties = (
        str(halo_properties)
        if isinstance(halo_properties, (str, Path))
        else tuple(dict(entry) for entry in halo_properties)
    )
    order = sorted(selected, key=lambda number: (-planned[number]["n_halos"], number))

    def job_of(number: int) -> FileJob:
        """File ``number``'s job, built when it is submitted; its counts are a view of the
        totals (a worker process receives a copy of that slice alone)."""
        entry = planned[number]
        return FileJob(
            dataset=str(dataset.directory),
            halo_properties=properties,
            window_rows=window_rows,
            output=str(output),
            tree_name=tree_name,
            number=number,
            first_forest=entry["first_forest"],
            last_forest=entry["last_forest"],
            n_halos=entry["n_halos"],
            counts=totals[entry["first_forest"] : entry["last_forest"] + 1],
            expected_sha256=recorded.get(number),
            stamp=stamp,
        )

    _log(
        "converting {} file(s) of {} into {} with {} worker(s), the largest first".format(
            len(order), len(planned), output, workers
        )
    )

    refused = []
    unverified = []
    for job, outcome in _run_jobs((job_of(number) for number in order), workers):
        if isinstance(outcome, _Failure):
            (unverified if outcome.verification else refused).append((job.number, outcome.message))
            _log("FAILED file {}: {}".format(job.number, outcome.message))
            # The entry goes only with its file: a job that failed before it moved its file
            # aside (a worker death, say) leaves the rerun's SHA-256 check to decide.
            final = output / file_name(tree_name, job.number)
            if job.number in recorded and not (final.exists() or final.is_symlink()):
                _set_entry(manifest, None, job.number)
                write_json_atomic(manifest_path, manifest)
            continue
        if outcome["skipped"]:
            continue
        entry = dict(outcome["entry"], run=run_index)
        _set_entry(manifest, entry, job.number)
        write_json_atomic(manifest_path, manifest)
        _log(
            "file {}: {} forests, {} halos, {} bytes in {:.1f} s (worker {})".format(
                job.number,
                entry["n_forests"],
                entry["n_halos"],
                entry["bytes"],
                entry["wall_seconds"],
                entry["worker"],
            )
        )

    wanted = set(selected)
    complete = sum(1 for entry in manifest["files"] if entry["number"] in wanted)
    _log("{} of {} selected file(s) complete".format(complete, len(selected)))
    for number, message in refused + unverified:
        _log("  file {} failed: {}".format(number, message))
    if refused:
        return EXIT_REFUSED
    if unverified:
        return EXIT_VERIFICATION
    return EXIT_OK
