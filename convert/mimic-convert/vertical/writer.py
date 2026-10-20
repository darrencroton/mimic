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
fill, a manifest written for another conversion, and a second ``convert`` in the same output
directory. It then derives every forest's halo total (:func:`vertical.plan.forest_totals`),
requires every planned file's total to agree, prints the memory bound per worker and converts
the selected files, the largest first, in ``workers`` processes, at most ``workers`` files
submitted at a time. Every forest's assembled halo count must equal its total, which is refused
otherwise naming the forest. A file that fails, by a refusal, a verification failure or any
other error, is reported and the run goes on, recording every file that completes; when a
worker process dies, the files in flight fail, no further file is started and the files never
started are named.

**Locks.** The parent holds an exclusive, non-blocking ``flock`` on ``<output>/.convert.lock``
for the run, so a second ``convert`` in the directory is refused naming it. Each file is written
under an exclusive ``flock`` on ``<name>.<k>.lock``, held by the worker for the whole of
:func:`convert_file`, so a worker orphaned by a parent that was killed (SIGKILL, out of memory)
cannot be raced by a rerun: a rerun that cannot take the file's lock refuses that file, naming
it, and goes on. The worker unlinks ``<name>.<k>.lock``, while still holding it, when it
completes or skips the file; the lock of a file that failed or was killed stays, and is removed by
the next run that completes that file. ``.convert.lock`` stays and blocks nothing
once the run ends; the lock is the kernel's, so a stale file means nothing. On a filesystem that
cannot lock (``ENOLCK``, ``ENOSYS``, ``EOPNOTSUPP``) the run warns once, naming
``.convert.lock``, that neither lock is enforced there, and goes on without either.

**Stopping.** While files are being converted, the first SIGINT or SIGTERM sent to the parent
is a request to stop: no further file is started, the file or files in flight are completed and
recorded, and the run ends with :data:`EXIT_REFUSED` unless every selected file is complete. The
second signal terminates the workers at once and waits until none is alive (the files they were
writing stay ``.partial`` and the next run moves them aside). The workers ignore SIGINT, so a
terminal's Ctrl-C reaches only the parent. With one worker the file is written in the parent and
a first signal lets it finish.

**The manifest.** ``manifest.json`` records the dataset identity, the record (the package's
``halo_properties.yaml`` path and md5, the layout and its digest), the plan's md5 and caps, the
``tree_name``, every invocation (the converter's commit and command line) and, per completed
file, its number, forest range, forest and halo counts, byte size, SHA-256, wall time, worker
and invocation. Only the parent process writes it, atomically, after every completed file, and
others read it only after a run. An entry stays while its file does: an intact recorded file
never loses its entry, whatever happens to the parent, and a file moved aside for a rebuild has
no file on disk, so its entry is dropped when the job fails and, if the parent dies first, by
the next run's sweep.

**Resume.** A rerun with the same inputs skips every selected file whose on-disk SHA-256
equals its manifest entry's and redoes the rest. A file of a planned name that the manifest
does not record as complete (a run killed between the rename and the manifest write), and any
``.partial``, are moved aside to ``<name>.stale-<timestamp>`` and logged with their sizes, never
deleted, and the file is rebuilt. The entries of planned files that are absent from the output
are dropped at the start.
"""

import contextlib
import errno
import fcntl
import hashlib
import json
import multiprocessing
import os
import signal
import subprocess
import sys
import threading
import time
from concurrent.futures import FIRST_COMPLETED, ProcessPoolExecutor, wait
from concurrent.futures.process import BrokenProcessPool
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, Iterator, List, Optional, Sequence, Tuple, Union

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from errors import ConverterError  # noqa: E402
from horizontal_dataset import HorizontalDataset  # noqa: E402
from vertical.assemble import DEFAULT_WINDOW_ROWS, ForestAssembler  # noqa: E402
from vertical.common import (  # noqa: E402
    fsync_directory,
    require_positive_int,
    write_json_atomic,
)
from vertical.plan import (  # noqa: E402
    FILE_PLAN_NAME,
    dataset_identity,
    forest_totals,
    read_file_plan,
    record_binding,
)
from vertical.record import record_layout  # noqa: E402

#: The manifest's file name inside a conversion's output directory.
MANIFEST_NAME = "manifest.json"

#: The ``format`` and ``version`` a manifest declares.
MANIFEST_FORMAT = "mimic-vertical-conversion-manifest"
MANIFEST_VERSION = 1

#: The lock file a running ``convert`` holds inside its output directory.
LOCK_NAME = ".convert.lock"

#: The suffix of the lock file beside each file (``<name>.<k>.lock``).
FILE_LOCK_SUFFIX = ".lock"

#: Tries to lock a file whose lock file keeps being replaced under it.
FILE_LOCK_ATTEMPTS = 5

#: The ``errno`` values with which a filesystem says it cannot lock at all.
_UNSUPPORTED_LOCK_ERRNOS = (errno.ENOLCK, errno.ENOSYS, errno.EOPNOTSUPP, errno.ENOTSUP)

#: The suffix of a file being written; it is renamed away only once verified.
PARTIAL_SUFFIX = ".partial"

#: The L-Halo header's integer type: the reader's C ``int``.
HEADER_DTYPE = np.dtype("<i4")

#: Bytes read per block when a written file is re-read and when an existing file is hashed.
VERIFY_BLOCK_BYTES = 64 << 20

#: The write buffer of a file being written.
WRITE_BUFFER_BYTES = 8 << 20

#: Bytes per forest of a file's header while it is written and checked, the worker's copy of
#: the plan's int64 totals included. Measured with ``tracemalloc`` at 20.0 B per forest for a
#: 4-million-forest header and 20.4 B for 1 million (the totals, their int64 concatenation with
#: ``ntrees`` and ``totNHalos``, and the int32 header); the re-read's parse of the header peaks
#: at 17 B. Rounded up to 21 B.
HEADER_BYTES_PER_FOREST = 21

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
    #: whether the file is written under its own lock (the directory could be locked)
    lock_files: bool = True


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


def _write_partial(job: FileJob, assembler: ForestAssembler, partial: Path) -> str:
    """Write the file's header and every forest's records to ``partial``; returns the SHA-256
    of the bytes written. Each forest's records are written before the next is assembled."""
    counts = np.asarray(job.counts, dtype=np.int64)
    values = np.concatenate(([counts.size, job.n_halos], counts))
    largest = int(values.max())
    if largest > np.iinfo(HEADER_DTYPE).max:
        raise ConverterError(
            "{}: the header holds {}, which does not fit {}; the file is not written".format(
                partial, largest, HEADER_DTYPE
            )
        )
    header = values.astype(HEADER_DTYPE).tobytes()
    digest = hashlib.sha256()
    with open(partial, "xb", buffering=WRITE_BUFFER_BYTES) as handle:
        handle.write(header)
        digest.update(header)
        for forest in assembler.forests(job.first_forest, job.last_forest + 1, counts):
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
    for block in _blocks(partial):
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


class _LockHeld(Exception):
    """Another open file description holds the lock."""


def _lock(handle) -> bool:
    """Take an exclusive, non-blocking ``flock`` on an open file; ``True`` if taken, ``False``
    if the filesystem cannot lock.

    Raises:
        _LockHeld: if another holder has it.
        OSError: for any other failure of the call.
    """
    try:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as error:
        if error.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
            raise _LockHeld() from error
        if error.errno in _UNSUPPORTED_LOCK_ERRNOS:
            return False
        raise
    return True


@contextlib.contextmanager
def _file_lock(final: Path, job: FileJob) -> Iterator[None]:
    """Hold file ``job.number``'s lock (``<name>.<k>.lock``) for the block, when the job asks
    for it.

    When the block returns normally (the file was completed or skipped) the lock file is
    unlinked while the lock is still held, so a process that opened the path earlier and locks
    it afterwards holds a detached inode, which the inode recheck sends to a fresh attempt. A
    lock file that exists is therefore one some process is still writing the file under or did
    not finish: if the block raises, the file stays, and the next run that completes the file
    removes it.

    Raises:
        ConverterError: if another process holds the lock, naming the file.
    """
    if not job.lock_files:
        yield
        return
    path = final.with_name(final.name + FILE_LOCK_SUFFIX)
    for _ in range(FILE_LOCK_ATTEMPTS):
        with open(path, "a") as handle:
            try:
                locked = _lock(handle)
            except _LockHeld as error:
                raise ConverterError(
                    "file {}: {} is being written by another process ({} is locked); wait for "
                    "it or kill it".format(job.number, final.name, path.name)
                ) from error
            # A lock on a file that was unlinked (and perhaps recreated) after it was opened
            # excludes nobody: the lock counts only while the path still names this file.
            if not locked or _same_file(handle, path):
                yield
                path.unlink()
                return
    raise ConverterError(
        "file {}: {} keeps being replaced, so it cannot be locked".format(job.number, path.name)
    )


def _same_file(handle, path: Path) -> bool:
    """Whether ``path`` still names the file ``handle`` has open."""
    try:
        named = os.stat(path)
    except FileNotFoundError:
        return False
    opened = os.fstat(handle.fileno())
    return (named.st_dev, named.st_ino) == (opened.st_dev, opened.st_ino)


def convert_file(job: FileJob) -> Dict[str, object]:
    """Write, verify and rename one file; the worker's whole job.

    The file's lock is held throughout (see :func:`_file_lock`). A leftover ``.partial`` is
    moved aside first. An existing final file whose SHA-256 equals ``job.expected_sha256`` is
    kept and the file skipped; any other is moved aside and rebuilt.

    Returns:
        ``{"number", "skipped"}`` and, for a written file, its manifest ``entry``.

    Raises:
        ConverterError: a refusal of the dataset or of a forest in the file's range, or of the
            file because another process holds its lock.
        VerificationError: a failed check of the written file, which stays ``.partial``.
    """
    final = Path(job.output) / file_name(job.tree_name, job.number)
    with _file_lock(final, job):
        return _convert_locked_file(job, final)


def _convert_locked_file(job: FileJob, final: Path) -> Dict[str, object]:
    """The body of :func:`convert_file`, run under the file's lock."""
    started = time.monotonic()
    output = final.parent
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
    fsync_directory(output)
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
    identity: Dict[str, object],
    halo_properties,
    record_dtype: np.dtype,
    plan_path: Path,
    plan: Dict[str, object],
    tree_name: str,
) -> Dict[str, object]:
    """The manifest's description of the conversion it records, before any file: the dataset's
    ``identity`` (:func:`vertical.plan.dataset_identity`, computed once by the caller), the
    record and the plan."""
    record = {"halo_properties": None, "halo_properties_md5": None}
    record.update(record_binding(halo_properties))
    record["layout"] = record_layout(record_dtype)
    return {
        "format": MANIFEST_FORMAT,
        "version": MANIFEST_VERSION,
        "dataset": dict(
            identity, links_adjacent=dataset.links_adjacent, directory=str(dataset.directory)
        ),
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


def _ignore_interrupts() -> None:
    """Worker process initializer: only the parent decides what an interrupt means (see
    :func:`_run_jobs`), so SIGINT, which a terminal sends to the whole process group, is
    ignored here; the parent stops workers with SIGTERM."""
    signal.signal(signal.SIGINT, signal.SIG_IGN)


#: Seconds a terminated worker is given to exit before it is killed.
TERMINATE_JOIN_SECONDS = 30


def _terminate_workers(executor) -> None:
    """Terminate the pool's worker processes and wait until none is alive: ``terminate`` each,
    ``join`` each for :data:`TERMINATE_JOIN_SECONDS`, and ``kill`` and join any still running.
    The processes are listed before anything is shut down, because the executor forgets them
    once it is shut down."""
    processes = list((getattr(executor, "_processes", None) or {}).values())
    for process in processes:
        process.terminate()
    for process in processes:
        process.join(TERMINATE_JOIN_SECONDS)
        if process.is_alive():
            process.kill()
            process.join()


#: Seconds between looks at the stop request while the parent waits for its workers.
STOP_POLL_SECONDS = 0.5


def _run_jobs(
    jobs: Iterator[FileJob], workers: int, stop: Callable[[], bool]
) -> Iterator[Tuple[FileJob, object]]:
    """Run ``convert_file`` over ``jobs``, yielding ``(job, result or _Failure)`` as each
    completes: in this process for one worker, else in ``workers`` spawned processes with at
    most ``workers`` jobs submitted at a time, in ``jobs`` order. Every ``Exception`` of a job
    is that job's failure. When a worker process dies, the jobs in flight fail, the death is
    logged once and no further job is submitted.

    Once ``stop()`` is true (the first signal; the parent polls it every
    :data:`STOP_POLL_SECONDS` while it waits) no further job is submitted, the jobs in flight
    are completed and yielded, and then ``KeyboardInterrupt`` is raised. A ``KeyboardInterrupt``
    from outside (the second signal, which may land anywhere) ends the run at once: the
    generator's cleanup terminates the workers and reaps them.
    """
    if workers == 1:
        while not stop():
            job = next(jobs, None)
            if job is None:
                return
            try:
                outcome = convert_file(job)
            except Exception as error:
                outcome = _failure(job, error)
            yield job, outcome
        _log("stop requested: no further file is started")
        raise KeyboardInterrupt
    executor = ProcessPoolExecutor(
        max_workers=workers,
        mp_context=multiprocessing.get_context("spawn"),
        initializer=_ignore_interrupts,
    )
    pending = {}
    broken = False
    exhausted = False
    stopping = False

    def break_pool() -> None:
        nonlocal broken
        if not broken:
            _log(
                "a worker process died (killed, or out of memory): the files in flight are not "
                "completed and no further file is started"
            )
        broken = True

    def submit_next() -> Optional[FileJob]:
        """Submit the next job; returns it if the pool refused it (a worker died)."""
        nonlocal exhausted
        job = next(jobs, None)
        if job is None:
            exhausted = True
            return None
        try:
            pending[executor.submit(convert_file, job)] = job
        except BrokenProcessPool:
            break_pool()
            return job
        return None

    def not_started(job: FileJob) -> _Failure:
        return _Failure(False, "file {}: not started: a worker process died".format(job.number))

    drained = False
    try:
        while True:
            if not stopping and stop():
                stopping = True
                _log(
                    "stop requested: no further file is started; waiting for the {} file(s) in "
                    "flight (signal again to terminate them)".format(len(pending))
                )
            while not (exhausted or broken or stop()) and len(pending) < workers:
                dropped = submit_next()
                if dropped is not None:
                    yield dropped, not_started(dropped)
            if not pending:
                break
            done, _ = wait(pending, timeout=STOP_POLL_SECONDS, return_when=FIRST_COMPLETED)
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
        drained = True
    finally:
        if not drained:
            _terminate_workers(executor)
        executor.shutdown(wait=True, cancel_futures=True)
    if stopping or stop():
        raise KeyboardInterrupt


@contextlib.contextmanager
def _one_run_lock(output: Path) -> Iterator[bool]:
    """Hold an exclusive, non-blocking ``flock`` on ``<output>/.convert.lock`` for the block,
    so one ``convert`` runs per output directory; yields whether locking is enforced.

    On a filesystem that cannot lock, a warning naming the lock file is logged once and the
    block runs without it, yielding ``False``.

    Raises:
        ConverterError: naming the lock file, if another process holds it or the lock fails for
            any other reason.
    """
    path = output / LOCK_NAME
    with open(path, "a") as handle:
        try:
            enforced = _lock(handle)
        except _LockHeld as error:
            raise ConverterError(
                "{}: another convert holds this output directory's lock; run one convert "
                "per output directory at a time".format(path)
            ) from error
        except OSError as error:
            raise ConverterError(
                "{}: cannot lock the output directory: {}".format(path, error)
            ) from error
        if not enforced:
            _log(
                "WARNING: {}: the filesystem cannot lock; one convert per output directory and "
                "one writer per file are not enforced here".format(path)
            )
        yield enforced


class _StopRequests:
    """SIGINT and SIGTERM, counted, for the block it guards (a context manager).

    The first signal only sets :meth:`requested`, for the job loop to see; the second raises
    ``KeyboardInterrupt`` wherever it lands. The previous handlers are restored on exit. Outside
    the main thread, where Python cannot install a handler, no signal is intercepted.
    """

    SIGNALS = (signal.SIGINT, signal.SIGTERM)

    def __init__(self) -> None:
        self.count = 0
        self._previous: Dict[int, object] = {}

    def requested(self) -> bool:
        return self.count > 0

    def _handle(self, signum, frame) -> None:
        self.count += 1
        if self.count > 1:
            raise KeyboardInterrupt

    def __enter__(self) -> "_StopRequests":
        if threading.current_thread() is threading.main_thread():
            for number in self.SIGNALS:
                self._previous[number] = signal.signal(number, self._handle)
        return self

    def __exit__(self, *exc_info) -> None:
        for number, previous in self._previous.items():
            signal.signal(number, previous if previous is not None else signal.SIG_DFL)


class _Ledger:
    """The manifest as the parent keeps it during a run. Every change is written to disk
    before ``manifest`` is replaced, so memory never runs ahead of the file, whatever
    interrupts the write."""

    def __init__(self, manifest: Dict[str, object], output: Path, tree_name: str):
        self.manifest = manifest
        self.output = output
        self.tree_name = tree_name
        self.path = output / MANIFEST_NAME

    def _commit(self, **changes) -> None:
        manifest = dict(self.manifest, **changes)
        write_json_atomic(self.path, manifest)
        self.manifest = manifest

    def reload(self) -> None:
        """Read the manifest back from disk, which is never behind memory (a signal can land
        between a write and the assignment that follows it)."""
        self.manifest = json.loads(self.path.read_text(encoding="utf-8"))

    def _files_without(self, number: int) -> List[Dict[str, object]]:
        return [entry for entry in self.manifest["files"] if entry["number"] != number]

    def stands(self, number: int) -> bool:
        """Whether file ``number`` is in the output."""
        final = self.output / file_name(self.tree_name, number)
        return final.exists() or final.is_symlink()

    def recorded_sha256(self) -> Dict[int, str]:
        """The SHA-256 of every recorded file, by number."""
        return {entry["number"]: entry["sha256"] for entry in self.manifest["files"]}

    def drop_absent(self, numbers: Sequence[int]) -> None:
        """Drop the entries of the files among ``numbers`` that are not in the output, saving
        only if there are any."""
        absent = {number for number in numbers if not self.stands(number)}
        files = [entry for entry in self.manifest["files"] if entry["number"] not in absent]
        if len(files) != len(self.manifest["files"]):
            self._commit(files=files)

    def start_run(self, run: Dict[str, object]) -> int:
        """Record an invocation and save; returns its index."""
        runs = list(self.manifest["runs"]) + [run]
        self._commit(runs=runs)
        return len(runs) - 1

    def record(self, number: int, entry: Dict[str, object]) -> None:
        files = self._files_without(number) + [entry]
        self._commit(files=sorted(files, key=lambda item: item["number"]))

    def forget(self, number: int) -> None:
        """Drop file ``number``'s entry, if it has one."""
        if number in self.recorded_sha256():
            self._commit(files=self._files_without(number))


def _select_files(planned: Sequence[Dict[str, object]], only_files) -> List[int]:
    """The file numbers to convert: ``only_files`` (distinct, ascending) or every planned file.

    Raises:
        ConverterError: for a selection that is empty or names a file the plan lacks.
    """
    if only_files is None:
        return [entry["number"] for entry in planned]
    selected = sorted(set(int(number) for number in only_files))
    outside = [number for number in selected if not 0 <= number < len(planned)]
    if not selected or outside:
        raise ConverterError(
            "--only-files {}: the plan's files are 0 to {}".format(
                outside or "selects nothing", len(planned) - 1
            )
        )
    return selected


def _check_plan_totals(plan_path: Path, planned: Sequence[Dict[str, object]], totals) -> None:
    """Every planned file's halo count is the sum of the dataset's totals over its forests.

    Raises:
        ConverterError: naming the first file whose count differs.
    """
    for entry in planned:
        found = int(totals[entry["first_forest"] : entry["last_forest"] + 1].sum())
        if found != entry["n_halos"]:
            raise ConverterError(
                "{}: file {} plans {} halos; its forests hold {}".format(
                    plan_path, entry["number"], entry["n_halos"], found
                )
            )


def _log_memory_bound(
    assembler: ForestAssembler,
    bound: Dict[str, int],
    largest: int,
    workers: int,
    totals_bytes: int,
) -> None:
    """Print the per-worker memory bound and what it is made of."""
    _log(
        "memory bound per worker: {} bytes ({}): the assembler's {} for --window-rows {} over {} "
        "slabs, {} B per row and {} B per record, with the plan's largest forest of {} halos; "
        "the {} re-read buffer, the {} write buffer and {} B of header for the plan's most "
        "forests in a file; {} worker(s): {}; the forest totals held here: {} (8 B per "
        "forest)".format(
            bound["total"],
            _size(bound["total"]),
            _size(bound["assembler"]),
            assembler.window_rows,
            len(assembler.dataset.snapshots),
            assembler.row_bytes,
            assembler.dtype.itemsize,
            largest,
            _size(bound["reread_block"]),
            _size(bound["write_buffer"]),
            bound["header"],
            workers,
            _size(workers * bound["total"]),
            _size(totals_bytes),
        )
    )


def _jobs(
    order: Sequence[int],
    planned: Sequence[Dict[str, object]],
    totals: np.ndarray,
    recorded: Dict[int, str],
    shared: Dict[str, object],
    submitted: List[int],
) -> Iterator[FileJob]:
    """The jobs of the files ``order``, each built and its number appended to ``submitted``
    only when it is taken for submission. A job carries the manifest's SHA-256 of its file
    (``recorded``, by number) and counts that are a view of the totals (a worker process
    receives a copy of that slice alone)."""
    for number in order:
        entry = planned[number]
        submitted.append(number)
        yield FileJob(
            number=number,
            first_forest=entry["first_forest"],
            last_forest=entry["last_forest"],
            n_halos=entry["n_halos"],
            counts=totals[entry["first_forest"] : entry["last_forest"] + 1],
            expected_sha256=recorded.get(number),
            **shared,
        )


def _record_outcomes(
    jobs: Iterator[FileJob],
    workers: int,
    ledger: _Ledger,
    run_index: int,
    stop: Callable[[], bool],
) -> Tuple[List[Tuple[int, str]], List[Tuple[int, str]], bool]:
    """Run ``jobs`` and record each file as it completes; returns the refusals, the
    verification failures (each ``(file number, message)``) and whether a stop was requested
    or forced. A file that fails and is no longer in the output (it was moved aside for a
    rebuild) loses its entry; every other entry stays."""
    refused = []
    unverified = []
    interrupted = False
    try:
        for job, outcome in _run_jobs(jobs, workers, stop):
            if isinstance(outcome, _Failure):
                (unverified if outcome.verification else refused).append(
                    (job.number, outcome.message)
                )
                _log("FAILED file {}: {}".format(job.number, outcome.message))
                if not ledger.stands(job.number):
                    ledger.forget(job.number)
            elif not outcome["skipped"]:
                entry = dict(outcome["entry"], run=run_index)
                ledger.record(job.number, entry)
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
    except KeyboardInterrupt:
        interrupted = True
    return refused, unverified, interrupted


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

    The parent holds ``<output>/.convert.lock`` for the run, and while files are converted the
    first SIGINT or SIGTERM is a request to stop and the second terminates the workers (see the
    module docstring).

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
        was refused, a worker died or an interrupted run left a file incomplete. Every file that
        completes is recorded either way.

    Raises:
        ConverterError: a refusal before any file is attempted, including another ``convert``
            holding the output directory's lock.
    """
    check_tree_name(tree_name)
    workers = require_positive_int("workers", workers)
    window_rows = require_positive_int("window_rows", window_rows)
    output = Path(output)
    dataset = HorizontalDataset(dataset_dir)
    identity = dataset_identity(dataset)
    plan = read_file_plan(output, dataset, halo_properties, identity)
    planned = plan["files"]
    selected = _select_files(planned, only_files)
    assembler = ForestAssembler(dataset, halo_properties, window_rows)
    plan_path = output / FILE_PLAN_NAME
    with _one_run_lock(output) as lock_files:
        binding = _manifest_binding(
            dataset, identity, halo_properties, assembler.dtype, plan_path, plan, tree_name
        )
        ledger = _Ledger(_load_manifest(output / MANIFEST_NAME, binding), output, tree_name)

        _log("deriving every forest's halo total from {}".format(dataset.directory))
        totals = forest_totals(dataset)
        _check_plan_totals(plan_path, planned, totals)
        largest = int(totals.max()) if totals.size else 0
        bound = worker_memory_bound(
            assembler, largest, max((entry["n_forests"] for entry in planned), default=0)
        )
        _log_memory_bound(assembler, bound, largest, workers, totals.nbytes)

        ledger.drop_absent([entry["number"] for entry in planned])
        run_index = ledger.start_run(
            dict(
                converter_identity(),
                command_line=list(command_line),
                started_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                workers=workers,
                window_rows=window_rows,
            )
        )
        shared = dict(
            dataset=str(dataset.directory),
            halo_properties=(
                str(halo_properties)
                if isinstance(halo_properties, (str, Path))
                else tuple(dict(entry) for entry in halo_properties)
            ),
            window_rows=window_rows,
            output=str(output),
            tree_name=tree_name,
            stamp=time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()),
            lock_files=lock_files,
        )
        order = sorted(selected, key=lambda number: (-planned[number]["n_halos"], number))
        _log(
            "converting {} file(s) of {} into {} with {} worker(s), the largest first".format(
                len(order), len(planned), output, workers
            )
        )
        submitted: List[int] = []
        with _StopRequests() as stops:
            refused, unverified, interrupted = _record_outcomes(
                _jobs(order, planned, totals, ledger.recorded_sha256(), shared, submitted),
                workers,
                ledger,
                run_index,
                stops.requested,
            )
        # A forced stop can leave the entry of a file that was moved aside for a rebuild, and
        # memory one write behind the disk.
        ledger.reload()
        ledger.drop_absent([entry["number"] for entry in planned])

    wanted = set(selected)
    complete = sum(1 for entry in ledger.manifest["files"] if entry["number"] in wanted)
    _log("{} of {} selected file(s) complete".format(complete, len(selected)))
    for number, message in refused + unverified:
        _log("  file {} failed: {}".format(number, message))
    never = sorted(wanted.difference(submitted))
    if never:
        _log("{} file(s) never started: {}".format(len(never), ", ".join(map(str, never))))
    if interrupted and complete < len(selected):
        _log("interrupted: the run is incomplete")
    # Exit 0 means every selected file is complete, whatever stopped the others.
    if refused or (complete < len(selected) and (interrupted or not unverified)):
        return EXIT_REFUSED
    if unverified:
        return EXIT_VERIFICATION
    return EXIT_OK
