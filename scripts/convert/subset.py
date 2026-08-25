"""Whole-forest subset selection and extraction for Consistent-Trees ASCII data.

Builds a tractable, representative subset of a very large ctrees dataset without
ever reading the bulk tree data: forests are ranked from the index files alone
(``forests.list`` + ``locations.dat`` + a file-size inventory), a small pool of
candidate trees is sampled one root row at a time, and only the selected byte
ranges are ever copied.

The design is ``docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`` -> "Subset Selection and
Extraction"; read it before changing anything here. Five subcommands, split along
the host boundary because root-row sampling needs remote bytes that must not be
transferred in bulk:

===================  ======  ========================================================
Subcommand           Host    Produces
===================  ======  ========================================================
``plan-candidates``  local   ``tree_table.npy``, ``forest_table.npy``,
                             ``candidates.npy``, ``filemap.json``
``sample-roots``     remote  ``root_values.npy``
``calibrate-proxy``  local   proxy-quality report (Spearman + recovery fraction)
``finalize``         local   ``selection.npy``, ``selection.json``, ``filemap.json``
``extract``          remote  subset ``tree_*.dat`` + ``forests.list`` +
                             ``locations.dat`` + ``extract_report.json``
===================  ======  ========================================================

``calibrate-proxy`` is **not** a step of the production pipeline. It is a separate
exhaustive pass over a *calibration* dataset small enough to sample in full
(micro-Uchuu, 561,266 trees), because the recovery fraction it measures needs the
true top-``K`` forests over every tree rather than over the byte-prefiltered pool
whose quality is the thing under test. Run ``plan-candidates --m 0`` then
``sample-roots`` there, and carry the calibrated relative depth to the production
dataset as ``M = ceil(depth x n_trees)``; the production run then samples only
that top-``M`` pool. Sampling every production root is neither required nor
affordable.

Exit codes: **0** success; **1** the run completed but a ``finalize`` acceptance
assertion or an ``extract`` verification failed; **2** fatal -- a violated
invariant, bad input, or an unreadable artifact. Note that a ``finalize``
*precondition* failure (no tractable forest, an unclosable file, root values that
do not cover the candidates) is a 2, not a 1: nothing was selected to assert over.

Constraints this implementation is built around, each verified against the
reference sources rather than assumed:

- **Whole forests only.** ``fix_flybys``/``fix_upid`` use per-forest max-snapshot
  scope, so a partial forest converts differently from the same forest in a full
  run. Coverage holes are closed with complete forests, never with lone trees.
- **One-to-one root coverage.** ``scatter.validate_root_coverage()`` aborts on a
  surplus listed root just as loudly as on a missing one, so the subset needs its
  own ``forests.list``.
- **Offsets point at the first data row, not at the ``#tree`` line.** A tree's
  body therefore ends at the *next* tree's offset minus that next tree's
  ``"#tree <root>\\n"`` length; the last tree in a file ends at the file size.
- **The tree-count line is checked** against the ``#tree`` marker count
  (``scatter.scatter_one_file``), and is rewritten here at its original field
  width -- conservative preservation, not a source-enforced rule.
- **File ids must be contiguous from 0 and the file count a perfect cube.**
  ``read_locations()`` asserts ``max_fileid + 1 == numfiles`` and then
  ``round(cbrt(numfiles))**3 == numfiles``, so every source file must contribute
  at least one selected tree and all of them must be emitted.
- **Reader memory has a per-tree floor.** ``load_unit_ctrees_ascii()``
  preallocates 152,000 B per tree before reading a halo, independent of halo
  count -- hence Gate A on tree count as well as Gate B on halo count.

Numeric contract for ``root_values.npy``: the production path parses ctrees
columns to float64 and casts to float32 at record assembly
(``ctrees_parser.CtreesFileParser._assemble``, mirroring the reference
strtod-then-cast), and only then applies the ``J``/``Mvir`` normalisation in
float64 with a float32 store, deliberately leaving zero-mass halos carrying raw
``J`` (``fixups.normalise_spin``; ``read_ctrees_ascii.c``
``apply_ctrees_value_conventions``). This sampler reproduces that ordering
exactly: its float64 fields hold *widened float32 reader-visible values*, not
raw parsed text, so sampled values are comparable with converted output at ties.

numpy-only by requirement: the data-node checkout that runs ``sample-roots`` and
``extract`` has numpy and h5py but no pandas.
"""

import argparse
import hashlib
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np

# ---------------------------------------------------------------------------
# Constants and frozen artifact schemas
# ---------------------------------------------------------------------------

#: ``"#tree "`` prefix plus the trailing newline; the root id's digits are added.
TREE_MARKER_FIXED_LEN = len(b"#tree ") + len(b"\n")

#: Reader preallocation per tree, measured: 1000 x (sizeof(struct halo_data)=104
#: + sizeof(struct additional_info)=48). See read_ctrees_ascii.c:647-655.
DEFAULT_TREE_ALLOC_BYTES = 152_000

#: Gate A: per-forest tree-count cap (~76 GB of reader preallocation).
DEFAULT_MAX_TREES_PER_FOREST = 500_000

#: Gate B: per-forest halo-count cap.
DEFAULT_MAX_HALOS_PER_FOREST = 100_000_000

#: Measured mean ASCII data-row width for Shin-Uchuu (240 MB / 474,031 rows).
DEFAULT_BYTES_PER_HALO = 506.3

#: Balance rule: warn when the high-mass supplement exceeds this halo share.
DEFAULT_SUPPLEMENT_HALO_FRACTION = 0.15

#: Representativeness: population bins below this count are pooled rightward.
BIN_POOLING_MIN_POPULATION = 1_000

#: Representativeness: max allowed ratio between sampled and population share.
BIN_SHARE_MAX_RATIO = 2.0

#: Per-tree index table: every tree, with its forest and byte extent.
TREE_TABLE_DTYPE = np.dtype(
    [
        ("tree_root_id", "<i8"),
        ("forest_id", "<i8"),
        ("file_id", "<i4"),
        ("offset", "<i8"),
        ("extent", "<i8"),
    ]
)

#: Forest-aggregated table.
FOREST_TABLE_DTYPE = np.dtype([("forest_id", "<i8"), ("n_trees", "<i8"), ("total_bytes", "<i8")])

#: Candidate trees shipped to the sampler. ``forest_id`` is carried so a measured
#: root maps back to its forest without re-reading the per-tree table.
CANDIDATES_DTYPE = np.dtype(
    [
        ("tree_root_id", "<i8"),
        ("forest_id", "<i8"),
        ("file_id", "<i4"),
        ("offset", "<i8"),
        ("extent", "<i8"),
    ]
)

#: Sampled root-row values. float64 fields hold widened float32 reader-visible
#: values (see the module docstring's numeric contract), keyed by root id so the
#: join in ``finalize`` is checkable rather than positional.
ROOT_VALUES_DTYPE = np.dtype(
    [
        ("tree_root_id", "<i8"),
        ("mvir", "<f8"),
        ("jx", "<f8"),
        ("jy", "<f8"),
        ("jz", "<f8"),
    ]
)

#: Selected trees, sorted by (file_id, offset). ``forest_id`` is carried because
#: ``extract`` must write a matching subset ``forests.list`` and nothing else in
#: the artifact chain reaches it on the remote host.
SELECTION_DTYPE = np.dtype(
    [
        ("file_id", "<i4"),
        ("tree_root_id", "<i8"),
        ("forest_id", "<i8"),
        ("offset", "<i8"),
        ("extent", "<i8"),
    ]
)

#: Rows accumulated in Python lists before flushing into numpy arrays.
_PARSE_BLOCK_ROWS = 1_000_000

#: Bytes copied per read/write during extraction.
_COPY_CHUNK = 8 << 20

#: Bytes read when locating a file's first '#tree' marker. Generous against the
#: measured layout -- Shin-Uchuu's count line sits at byte ~3,659 -- and a header
#: longer than this aborts loudly rather than being silently truncated.
_HEADER_PROBE = 1 << 20

#: Bytes read from the end of a source file to check its final line. Comfortably
#: above the measured 506.3 B mean row width.
_TAIL_PROBE = 1 << 16


class SubsetError(RuntimeError):
    """Fatal subset-selection failure: abort, never repair silently."""


# ---------------------------------------------------------------------------
# Small shared helpers
# ---------------------------------------------------------------------------

_POW10 = np.array([10**k for k in range(1, 19)], dtype=np.int64)


def decimal_digits(values: np.ndarray) -> np.ndarray:
    """Number of decimal digits of each non-negative int64 value.

    Exact by construction (``searchsorted`` over the powers of ten) rather than
    via ``log10``, which is wrong at powers-of-ten boundaries in float.
    """
    arr = np.asarray(values, dtype=np.int64)
    if arr.size and int(arr.min()) < 0:
        raise SubsetError("negative id encountered where a decimal width is needed")
    return (np.searchsorted(_POW10, arr, side="right") + 1).astype(np.int64)


def tree_marker_lengths(root_ids: np.ndarray) -> np.ndarray:
    """Byte length of each tree's ``"#tree <root>\\n"`` marker line."""
    return decimal_digits(root_ids) + TREE_MARKER_FIXED_LEN


def tree_marker_bytes(root_id: int) -> bytes:
    return b"#tree " + str(int(root_id)).encode("ascii") + b"\n"


def normalize_column_name(token: str) -> str:
    """Truncate at the first '(' -- the reference parse_ctrees.h suffix rule."""
    return token.split("(", 1)[0]


def parse_header_columns(header_line: str) -> List[str]:
    """Normalized column names from a ctrees header line.

    Accepts the indexed primary dialect (``#scale(0) id(1) ...``) and the
    ``#fields:`` secondary dialect, splitting on the reference delimiter set.
    Deliberately duplicated from ``ctrees_parser.parse_header_line`` rather than
    imported: that module imports pandas at module scope and this tool must run
    on a data node without it. ``test_subset.py`` pins the two implementations
    against each other so the duplication cannot drift silently.
    """
    if not header_line.startswith("#"):
        raise SubsetError(
            "ctrees header line must start with '#', got: {!r}".format(header_line[:80])
        )
    body = header_line.lstrip("#").strip()
    if body.lower().startswith("fields:"):
        body = body[len("fields:") :]
    names = [normalize_column_name(t) for t in re.split(r"[,\s]+", body) if t]
    names = [n for n in names if n]
    if not names:
        raise SubsetError(
            "ctrees header line contains no column names: {!r}".format(header_line[:80])
        )
    return names


def resolve_sample_columns(names: Sequence[str]) -> Dict[str, int]:
    """Column indices for the six values the root sampler reads.

    Case-insensitive first match, mirroring the reference ``match_column_name``;
    a duplicated required column aborts, as it does in the converter's own
    ``resolve_columns``.
    """
    lowered = [n.lower() for n in names]
    wanted = ("scale", "id", "mvir", "jx", "jy", "jz")
    duplicates = sorted({w for w in wanted if lowered.count(w) > 1})
    if duplicates:
        raise SubsetError(
            "duplicate required column(s) in ctrees header: {}".format(", ".join(duplicates))
        )
    indices: Dict[str, int] = {}
    missing: List[str] = []
    for name in wanted:
        try:
            indices[name] = lowered.index(name)
        except ValueError:
            missing.append(name)
    if missing:
        raise SubsetError(
            "missing required column(s) in ctrees header: {}".format(", ".join(sorted(missing)))
        )
    return indices


def read_a_list(path) -> np.ndarray:
    """Load a scale-factor list: one value per non-comment line, ascending."""
    path = Path(path)
    values: List[float] = []
    with open(path, "r") as handle:
        for lineno, raw in enumerate(handle, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            try:
                values.append(float(line.split()[0]))
            except (ValueError, IndexError):
                raise SubsetError(
                    "{}:{}: non-numeric scale factor: {!r}".format(path, lineno, line)
                )
    if not values:
        raise SubsetError("{}: no scale factors found".format(path))
    arr = np.asarray(values, dtype=np.float64)
    # checked before the ordering test, which a NaN would silently satisfy and
    # which would then disable every scale comparison made against this list
    if not np.all(np.isfinite(arr)):
        raise SubsetError("{}: non-finite scale factor".format(path))
    if np.any(np.diff(arr) <= 0.0):
        raise SubsetError("{}: scale factors are not strictly ascending".format(path))
    return arr


def widened_float32(token: bytes, column: str) -> float:
    """Parse one ctrees numeric token the way the production path sees it.

    Text -> float64 (strtod) -> float32 store, returned widened back to float64.
    See the module docstring's numeric contract.

    Non-finite and float32-overflowing values abort, exactly as the converter
    aborts on them (``ctrees_parser.CtreesFileParser._assemble``). Carrying an
    infinity here would silently poison the sampled ``Spin`` bound and the mass
    ranking on data the converter would refuse outright. The overflow warning is
    suppressed because the finiteness check below is the deliberate detector.
    """
    parsed = float(token)
    if not np.isfinite(parsed):
        raise SubsetError("non-finite value in column '{}': {!r}".format(column, token))
    with np.errstate(over="ignore"):
        narrowed = np.float32(parsed)
    if not np.isfinite(narrowed):
        raise SubsetError("float32-overflowing value in column '{}': {!r}".format(column, token))
    return float(narrowed)


def spin_from_widened(mvir: np.ndarray, j: np.ndarray) -> np.ndarray:
    """``Spin = (float)((double)J * (1.0 / (double)Mvir))`` for ``Mvir != 0``.

    Reproduces ``fixups.normalise_spin`` bit-for-bit on the float32 values the
    reader would hold, including the zero-mass carve-out (raw ``J`` is kept).
    """
    mvir32 = np.asarray(mvir, dtype=np.float64).astype(np.float32)
    j32 = np.asarray(j, dtype=np.float64).astype(np.float32)
    out = j32.copy()
    nonzero = np.nonzero(mvir32 != np.float32(0.0))[0]
    if nonzero.size:
        inv = 1.0 / mvir32[nonzero].astype(np.float64)
        with np.errstate(over="ignore"):
            out[nonzero] = (j32[nonzero].astype(np.float64) * inv).astype(np.float32)
    return out


def require_gate_value(name: str, value: float, minimum: float, maximum: float) -> float:
    """Reject a flag value that would silently disable a gate instead of failing.

    Only the arguments that *gate* something are checked this way. A nonsense
    ``--target-trees`` merely selects nothing and is obvious at a glance, but a
    negative ``--bytes-per-halo`` makes every halo estimate negative and quietly
    lets Gate B pass forests it exists to exclude, and a NaN ``--scale-atol``
    makes every scale comparison vacuously true.
    """
    if not np.isfinite(value) or value < minimum or value > maximum:
        raise SubsetError(
            "{} must be finite and within [{}, {}], got {!r}".format(name, minimum, maximum, value)
        )
    return value


def _log(message: str) -> None:
    sys.stderr.write("[subset] {}\n".format(message))
    sys.stderr.flush()


def _write_npy_atomic(path: Path, array: np.ndarray) -> None:
    """Write a .npy through a temp file and rename, so a killed run leaves no
    half-written artifact for the next stage to read."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    # save through a handle: np.save would otherwise append its own '.npy' to a
    # name that does not already end in it
    with open(tmp, "wb") as handle:
        np.save(handle, array, allow_pickle=False)
    os.replace(str(tmp), str(path))


def _write_json_atomic(path: Path, payload: dict) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "w") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")
    os.replace(str(tmp), str(path))


def _assert_unique_sorted(values: np.ndarray, what: str) -> np.ndarray:
    """Return ``values`` sorted, aborting on any duplicate."""
    ordered = np.sort(np.asarray(values, dtype=np.int64))
    if ordered.size > 1:
        dup = np.nonzero(ordered[1:] == ordered[:-1])[0]
        if dup.size:
            raise SubsetError(
                "{}: duplicate id(s); examples: {}".format(what, ordered[dup][:5].tolist())
            )
    return ordered


# ---------------------------------------------------------------------------
# Index-file loading (streaming, numpy-only)
# ---------------------------------------------------------------------------


def load_forests_list(path) -> Tuple[np.ndarray, np.ndarray]:
    """Stream ``forests.list`` into ``(tree_root_id, forest_id)`` int64 arrays."""
    path = Path(path)
    root_blocks: List[np.ndarray] = []
    forest_blocks: List[np.ndarray] = []
    roots: List[int] = []
    forests: List[int] = []

    def flush() -> None:
        if roots:
            root_blocks.append(np.asarray(roots, dtype=np.int64))
            forest_blocks.append(np.asarray(forests, dtype=np.int64))
            del roots[:]
            del forests[:]

    with open(path, "rb") as handle:
        for lineno, raw in enumerate(handle, start=1):
            if not raw.strip() or raw[:1] == b"#":
                continue
            parts = raw.split()
            if len(parts) != 2:
                raise SubsetError(
                    "{}:{}: expected 'TreeRootID ForestID', got: {!r}".format(
                        path, lineno, raw[:80]
                    )
                )
            try:
                roots.append(int(parts[0]))
                forests.append(int(parts[1]))
            except ValueError:
                raise SubsetError(
                    "{}:{}: non-integer forests.list row: {!r}".format(path, lineno, raw[:80])
                )
            if len(roots) >= _PARSE_BLOCK_ROWS:
                flush()
    flush()
    if not root_blocks:
        raise SubsetError("{}: no forest rows found".format(path))
    return np.concatenate(root_blocks), np.concatenate(forest_blocks)


def load_locations(path) -> Tuple[np.ndarray, np.ndarray, np.ndarray, Dict[int, str]]:
    """Stream ``locations.dat`` into ``(tree_root_id, file_id, offset)`` plus the
    ``file_id -> filename`` map.

    The filename is checked on every row rather than only on a file id's first
    appearance: a file id that changes name mid-file is corrupt input, and it
    would silently mis-target the extractor.
    """
    path = Path(path)
    root_blocks: List[np.ndarray] = []
    file_blocks: List[np.ndarray] = []
    offset_blocks: List[np.ndarray] = []
    roots: List[int] = []
    files: List[int] = []
    offsets: List[int] = []
    filemap: Dict[int, str] = {}

    def flush() -> None:
        if roots:
            root_blocks.append(np.asarray(roots, dtype=np.int64))
            file_blocks.append(np.asarray(files, dtype=np.int32))
            offset_blocks.append(np.asarray(offsets, dtype=np.int64))
            del roots[:]
            del files[:]
            del offsets[:]

    with open(path, "rb") as handle:
        for lineno, raw in enumerate(handle, start=1):
            if not raw.strip() or raw[:1] == b"#":
                continue
            parts = raw.split()
            if len(parts) != 4:
                raise SubsetError(
                    "{}:{}: expected 'TreeRootID FileID Offset Filename', got: {!r}".format(
                        path, lineno, raw[:80]
                    )
                )
            try:
                root = int(parts[0])
                file_id = int(parts[1])
                offset = int(parts[2])
            except ValueError:
                raise SubsetError(
                    "{}:{}: non-integer locations.dat row: {!r}".format(path, lineno, raw[:80])
                )
            if file_id < 0:
                raise SubsetError("{}:{}: negative FileID {}".format(path, lineno, file_id))
            if offset < 0:
                raise SubsetError("{}:{}: negative offset {}".format(path, lineno, offset))
            name = parts[3].decode("utf-8", "replace")
            known = filemap.get(file_id)
            if known is None:
                filemap[file_id] = name
            elif known != name:
                raise SubsetError(
                    "{}:{}: FileID {} maps to both {!r} and {!r}".format(
                        path, lineno, file_id, known, name
                    )
                )
            roots.append(root)
            files.append(file_id)
            offsets.append(offset)
            if len(roots) >= _PARSE_BLOCK_ROWS:
                flush()
    flush()
    if not root_blocks:
        raise SubsetError("{}: no location rows found".format(path))
    return (
        np.concatenate(root_blocks),
        np.concatenate(file_blocks),
        np.concatenate(offset_blocks),
        filemap,
    )


def load_filesizes(path) -> Dict[str, int]:
    """Load the ``stat`` inventory: ``<path>\\t<size>`` per line, keyed by basename.

    Written with GNU ``stat --printf='%n\\t%s\\n'`` -- ``-c``/``--format`` does not
    interpret backslash escapes and would emit a literal ``\\t``. Whitespace
    separation is accepted as a fallback so a BSD-``stat`` inventory also loads.
    """
    path = Path(path)
    sizes: Dict[str, int] = {}
    with open(path, "r") as handle:
        for lineno, raw in enumerate(handle, start=1):
            line = raw.rstrip("\n")
            if not line.strip():
                continue
            if "\t" in line:
                name_part, _, size_part = line.rpartition("\t")
            else:
                pieces = line.rsplit(None, 1)
                if len(pieces) != 2:
                    raise SubsetError(
                        "{}:{}: expected '<path><TAB><size>', got: {!r}".format(path, lineno, line)
                    )
                name_part, size_part = pieces
            try:
                size = int(size_part)
            except ValueError:
                raise SubsetError("{}:{}: non-integer size {!r}".format(path, lineno, size_part))
            name = os.path.basename(name_part.strip())
            if not name:
                raise SubsetError("{}:{}: empty filename".format(path, lineno))
            if name in sizes and sizes[name] != size:
                raise SubsetError(
                    "{}:{}: conflicting sizes for {!r} ({} and {})".format(
                        path, lineno, name, sizes[name], size
                    )
                )
            sizes[name] = size
    if not sizes:
        raise SubsetError("{}: no file sizes found".format(path))
    return sizes


# ---------------------------------------------------------------------------
# Stage 1 -- plan-candidates
# ---------------------------------------------------------------------------


def compute_tree_extents(
    root_ids: np.ndarray,
    file_ids: np.ndarray,
    offsets: np.ndarray,
    file_sizes: np.ndarray,
) -> Tuple[np.ndarray, np.ndarray]:
    """Byte extent of every tree body, and the (file_id, offset) sort order.

    A tree's body runs from its recorded offset -- which points at the first data
    row, not at the ``#tree`` line -- to the start of the next tree's ``#tree``
    marker; the last tree in a file runs to the file size. Returns
    ``(order, extent_in_sorted_order)``.
    """
    order = np.lexsort((offsets, file_ids))
    sorted_files = file_ids[order]
    sorted_offsets = offsets[order]
    sorted_roots = root_ids[order]

    marker_len = tree_marker_lengths(sorted_roots)
    # written twice on purpose: every slot first gets the next tree's start, then
    # the last tree of each file is corrected to the file size below. Dropping
    # either write silently corrupts one tree per file.
    body_end = np.empty(sorted_offsets.shape, dtype=np.int64)

    has_next_in_file = np.zeros(sorted_offsets.shape, dtype=bool)
    if sorted_files.size > 1:
        has_next_in_file[:-1] = sorted_files[:-1] == sorted_files[1:]
    body_end[:-1] = sorted_offsets[1:] - marker_len[1:]
    if body_end.size:
        body_end[-1] = 0
    last_in_file = ~has_next_in_file
    body_end[last_in_file] = file_sizes[sorted_files[last_in_file]]

    extent = body_end - sorted_offsets
    bad = extent <= 0
    if bad.any():
        idx = np.nonzero(bad)[0][:5]
        examples = [
            "(file_id={}, root={}, offset={}, body_end={})".format(
                int(sorted_files[i]), int(sorted_roots[i]), int(sorted_offsets[i]), int(body_end[i])
            )
            for i in idx
        ]
        raise SubsetError(
            "{} tree(s) have a non-positive byte extent; examples: {}".format(
                int(bad.sum()), ", ".join(examples)
            )
        )
    return order, extent


def cmd_plan_candidates(args: argparse.Namespace) -> int:
    index_dir = Path(args.index)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    started = time.time()
    _log("loading {}".format(index_dir / "locations.dat"))
    loc_roots, loc_files, loc_offsets, filemap = load_locations(index_dir / "locations.dat")
    _log("  {:,} location rows, {:,} distinct files".format(loc_roots.size, len(filemap)))

    _log("loading {}".format(index_dir / "forests.list"))
    for_roots, for_forests = load_forests_list(index_dir / "forests.list")
    _log("  {:,} forests.list rows".format(for_roots.size))

    # One-to-one coverage between the two index files, the same invariant
    # scatter.validate_root_coverage() will later enforce against the data.
    loc_sorted = _assert_unique_sorted(loc_roots, "locations.dat TreeRootID")
    for_sorted = _assert_unique_sorted(for_roots, "forests.list TreeRootID")
    if loc_sorted.size != for_sorted.size or not np.array_equal(loc_sorted, for_sorted):
        only_loc = np.setdiff1d(loc_sorted, for_sorted, assume_unique=True)
        only_for = np.setdiff1d(for_sorted, loc_sorted, assume_unique=True)
        raise SubsetError(
            "index files disagree: {} root(s) only in locations.dat (e.g. {}), "
            "{} root(s) only in forests.list (e.g. {})".format(
                only_loc.size,
                only_loc[:5].tolist(),
                only_for.size,
                only_for[:5].tolist(),
            )
        )
    del loc_sorted, for_sorted

    _log("loading {}".format(index_dir / "filesizes.tsv"))
    sizes_by_name = load_filesizes(index_dir / "filesizes.tsv")

    max_file_id = int(loc_files.max())
    file_sizes = np.zeros(max_file_id + 1, dtype=np.int64)
    missing_sizes = []
    for file_id, name in sorted(filemap.items()):
        size = sizes_by_name.get(os.path.basename(name))
        if size is None:
            missing_sizes.append(name)
        else:
            file_sizes[file_id] = size
    if missing_sizes:
        raise SubsetError(
            "{} file(s) referenced by locations.dat are absent from filesizes.tsv; "
            "examples: {}".format(len(missing_sizes), missing_sizes[:5])
        )

    # forest id per tree, via a sorted join on root id
    _log("joining trees to forests")
    for_order = np.argsort(for_roots, kind="stable")
    sorted_for_roots = for_roots[for_order]
    positions = np.searchsorted(sorted_for_roots, loc_roots)
    forest_of_tree = for_forests[for_order][positions]
    del for_order, sorted_for_roots, positions, for_roots, for_forests

    _log("computing per-tree byte extents")
    order, extent_sorted = compute_tree_extents(loc_roots, loc_files, loc_offsets, file_sizes)

    tree_table = np.zeros(loc_roots.size, dtype=TREE_TABLE_DTYPE)
    tree_table["tree_root_id"] = loc_roots[order]
    tree_table["forest_id"] = forest_of_tree[order]
    tree_table["file_id"] = loc_files[order]
    tree_table["offset"] = loc_offsets[order]
    tree_table["extent"] = extent_sorted
    del loc_roots, loc_files, loc_offsets, forest_of_tree, order, extent_sorted

    _log("aggregating {:,} trees into forests".format(tree_table.size))
    forest_order = np.argsort(tree_table["forest_id"], kind="stable")
    ordered_forests = tree_table["forest_id"][forest_order]
    ordered_extents = tree_table["extent"][forest_order]
    unique_forests, starts, counts = np.unique(
        ordered_forests, return_index=True, return_counts=True
    )
    total_bytes = (
        np.add.reduceat(ordered_extents, starts)
        if unique_forests.size
        else np.zeros(0, dtype=np.int64)
    )
    forest_table = np.zeros(unique_forests.size, dtype=FOREST_TABLE_DTYPE)
    forest_table["forest_id"] = unique_forests
    forest_table["n_trees"] = counts
    forest_table["total_bytes"] = total_bytes
    del forest_order, ordered_forests, ordered_extents, unique_forests, starts, counts

    m = int(args.m) if args.m else tree_table.size
    m = min(m, tree_table.size)
    _log("selecting the top {:,} trees by byte extent".format(m))
    if m == tree_table.size:
        cand_idx = np.arange(tree_table.size)
    else:
        cand_idx = np.argpartition(tree_table["extent"], tree_table.size - m)[-m:]
    cand_idx = cand_idx[np.argsort(tree_table["extent"][cand_idx], kind="stable")[::-1]]

    candidates = np.zeros(cand_idx.size, dtype=CANDIDATES_DTYPE)
    for name in ("tree_root_id", "forest_id", "file_id", "offset", "extent"):
        candidates[name] = tree_table[name][cand_idx]

    _write_npy_atomic(out_dir / "tree_table.npy", tree_table)
    _write_npy_atomic(out_dir / "forest_table.npy", forest_table)
    _write_npy_atomic(out_dir / "candidates.npy", candidates)
    _write_json_atomic(
        out_dir / "filemap.json",
        {str(k): v for k, v in sorted(filemap.items())},
    )
    _write_json_atomic(
        out_dir / "plan_candidates.json",
        {
            "index_dir": str(index_dir),
            "n_trees": int(tree_table.size),
            "n_forests": int(forest_table.size),
            "n_files": len(filemap),
            "max_file_id": max_file_id,
            "file_ids_contiguous": sorted(filemap) == list(range(max_file_id + 1)),
            "m_candidates": int(candidates.size),
            "total_bytes": int(tree_table["extent"].sum()),
            "largest_forest_n_trees": int(forest_table["n_trees"].max()),
            "largest_forest_id": int(
                forest_table["forest_id"][int(np.argmax(forest_table["n_trees"]))]
            ),
            "elapsed_seconds": round(time.time() - started, 1),
        },
    )
    _log(
        "wrote tree_table ({:,} trees), forest_table ({:,} forests), candidates ({:,}) to {}".format(
            tree_table.size, forest_table.size, candidates.size, out_dir
        )
    )
    return 0


# ---------------------------------------------------------------------------
# Stage 2 -- sample-roots
# ---------------------------------------------------------------------------


def read_marked_row(handle, offset: int, marker: bytes) -> bytes:
    """Read a tree's ``#tree`` marker and its first data row in one read.

    ``locations.dat`` records the offset of the first DATA row, so the marker
    occupies exactly the bytes before it. Reading both together lets the sampler
    assert against the marker the data actually carries rather than against the
    root id the index claims -- nothing else in the pipeline checks that the two
    agree, and it costs no extra seek on a shared parallel filesystem.

    Passing an empty ``marker`` reads a plain line at ``offset``. Reading to the
    newline rather than assuming a fixed width is required: ctrees rows have no
    fixed length. Returns the data row without its newline.
    """
    if offset < len(marker):
        raise SubsetError(
            "offset {} is too small to hold the preceding marker {!r}".format(offset, marker)
        )
    handle.seek(offset - len(marker))
    parts: List[bytes] = []
    size = len(marker) + 4096
    while True:
        chunk = handle.read(size)
        if not chunk:
            break
        parts.append(chunk)
        blob = b"".join(parts)
        if len(blob) >= len(marker):
            if not blob.startswith(marker):
                raise SubsetError(
                    "offset {} is not immediately preceded by its own marker: expected {!r}, "
                    "found {!r}".format(offset, marker, blob[: len(marker)])
                )
            newline = blob.find(b"\n", len(marker))
            if newline >= 0:
                return blob[len(marker) : newline]
        size = min(size * 2, 1 << 20)
    raise SubsetError("no complete data row at offset {}".format(offset))


def read_ctrees_header_line(handle) -> str:
    return read_marked_row(handle, 0, b"").decode("utf-8", "replace")


def cmd_sample_roots(args: argparse.Namespace) -> int:
    candidates = np.load(args.candidates, allow_pickle=False)
    if candidates.dtype != CANDIDATES_DTYPE:
        raise SubsetError(
            "{}: unexpected dtype {} (expected {})".format(
                args.candidates, candidates.dtype, CANDIDATES_DTYPE
            )
        )
    with open(args.filemap, "r") as handle:
        filemap = {int(k): v for k, v in json.load(handle).items()}

    require_gate_value("--scale-atol", args.scale_atol, 0.0, np.inf)
    a_list = read_a_list(args.a_list)
    final_scale = float(a_list[-1])
    trees_dir = Path(args.trees)
    out_path = Path(args.out)

    # Sequential locality: a scattered read pattern on a shared parallel
    # filesystem is far more expensive than an ordered one.
    order = np.lexsort((candidates["offset"], candidates["file_id"]))
    values = np.zeros(candidates.size, dtype=ROOT_VALUES_DTYPE)

    started = time.time()
    handle = None
    open_file_id = None
    columns: Dict[str, int] = {}
    n_columns = 0
    done = 0
    try:
        for idx in order:
            file_id = int(candidates["file_id"][idx])
            if file_id != open_file_id:
                if handle is not None:
                    handle.close()
                name = filemap.get(file_id)
                if name is None:
                    raise SubsetError("no filename recorded for FileID {}".format(file_id))
                handle = open(trees_dir / os.path.basename(name), "rb")
                open_file_id = file_id
                header_names = parse_header_columns(read_ctrees_header_line(handle))
                columns = resolve_sample_columns(header_names)
                n_columns = len(header_names)

            root_id = int(candidates["tree_root_id"][idx])
            offset = int(candidates["offset"][idx])
            # read the marker too, so the row is checked against the id the data
            # carries rather than against the one locations.dat claims
            line = read_marked_row(handle, offset, tree_marker_bytes(root_id))
            tokens = line.split()
            if len(tokens) != n_columns:
                raise SubsetError(
                    "{} root {}: row at offset {} has {} token(s), header declares {}".format(
                        filemap[file_id], root_id, offset, len(tokens), n_columns
                    )
                )

            row_id = int(tokens[columns["id"]])
            if row_id != root_id:
                raise SubsetError(
                    "{} offset {}: row id {} != '#tree' marker root id {} -- the "
                    "first-data-row-is-root premise does not hold for this dataset".format(
                        filemap[file_id], offset, row_id, root_id
                    )
                )
            row_scale = float(tokens[columns["scale"]])
            # NaN fails every comparison, so it would slip through the tolerance
            # test below; the production parser rejects a non-finite scale outright
            if not np.isfinite(row_scale):
                raise SubsetError(
                    "{} root {}: non-finite scale {!r}".format(
                        filemap[file_id], root_id, tokens[columns["scale"]]
                    )
                )
            if abs(row_scale - final_scale) > args.scale_atol:
                raise SubsetError(
                    "{} root {}: row scale {!r} is not the final a_list scale {!r} "
                    "(atol {})".format(
                        filemap[file_id], root_id, row_scale, final_scale, args.scale_atol
                    )
                )

            # written at the candidate's own row so root_values.npy stays
            # row-aligned with candidates.npy as well as keyed by root id
            values["tree_root_id"][idx] = root_id
            for name in ("mvir", "jx", "jy", "jz"):
                values[name][idx] = widened_float32(tokens[columns[name]], name)

            done += 1
            if args.progress_every and done % args.progress_every == 0:
                rate = done / max(time.time() - started, 1e-9)
                _log("sampled {:,}/{:,} roots ({:.0f}/s)".format(done, candidates.size, rate))
    finally:
        if handle is not None:
            handle.close()

    _assert_unique_sorted(values["tree_root_id"], "sampled tree root ids")
    _write_npy_atomic(out_path, values)
    _log(
        "sampled {:,} root rows in {:.1f}s -> {}".format(
            values.size, time.time() - started, out_path
        )
    )
    return 0


# ---------------------------------------------------------------------------
# Proxy calibration -- the gate that sets M
# ---------------------------------------------------------------------------


def spearman_rank_correlation(x: np.ndarray, y: np.ndarray) -> float:
    """Spearman's rho with average ranks for ties (numpy-only)."""
    x = np.asarray(x, dtype=np.float64)
    y = np.asarray(y, dtype=np.float64)
    if x.size != y.size:
        raise SubsetError("spearman: mismatched lengths")
    if x.size < 2:
        return float("nan")
    rx = _average_ranks(x)
    ry = _average_ranks(y)
    rx -= rx.mean()
    ry -= ry.mean()
    denom = np.sqrt((rx * rx).sum() * (ry * ry).sum())
    if denom == 0.0:
        return float("nan")
    return float((rx * ry).sum() / denom)


def _average_ranks(values: np.ndarray) -> np.ndarray:
    """Ranks 1..n, with each run of equal values sharing its mean rank."""
    order = np.argsort(values, kind="stable")
    ordered = values[order]
    starts = np.flatnonzero(np.r_[True, ordered[1:] != ordered[:-1]])
    ends = np.r_[starts[1:], ordered.size]
    # a run spanning [start, end) holds ranks start+1 .. end, whose mean is
    # (start + end + 1) / 2
    ranks = np.empty(values.size, dtype=np.float64)
    ranks[order] = np.repeat((starts + ends + 1) / 2.0, ends - starts)
    return ranks


def forest_max_root_mvir(forest_ids: np.ndarray, mvir: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
    """Per-forest maximum of a per-tree value. Returns (forest_ids, max)."""
    order = np.argsort(forest_ids, kind="stable")
    ordered_forests = forest_ids[order]
    ordered_values = mvir[order]
    unique, starts = np.unique(ordered_forests, return_index=True)
    if unique.size == 0:
        return unique, np.zeros(0, dtype=np.float64)
    return unique, np.maximum.reduceat(ordered_values, starts)


def recovery_fraction(
    tree_forests: np.ndarray,
    tree_extents: np.ndarray,
    tree_mvir: np.ndarray,
    depth: float,
    top_k: int,
) -> Tuple[float, int]:
    """Fraction of the true top-``top_k`` forests a byte-extent prefilter recovers.

    "True" ranks every forest by its maximum root ``Mvir`` over all of its trees.
    The prefilter keeps the top ``ceil(depth * n_trees)`` trees by byte extent,
    ranks the forests present in that pool by their maximum root ``Mvir`` *within
    the pool*, takes the top ``top_k``, and reports the overlap with the true set.
    """
    n_trees = tree_forests.size
    m = int(np.ceil(depth * n_trees))
    m = max(1, min(m, n_trees))

    true_forests, true_max = forest_max_root_mvir(tree_forests, tree_mvir)
    k = min(top_k, true_forests.size)
    true_top = set(true_forests[np.argsort(true_max, kind="stable")[::-1][:k]].tolist())

    pool = np.argpartition(tree_extents, n_trees - m)[-m:]
    pool_forests, pool_max = forest_max_root_mvir(tree_forests[pool], tree_mvir[pool])
    pool_k = min(k, pool_forests.size)
    pool_top = set(pool_forests[np.argsort(pool_max, kind="stable")[::-1][:pool_k]].tolist())

    if not true_top:
        return float("nan"), m
    return len(true_top & pool_top) / len(true_top), m


def cmd_calibrate_proxy(args: argparse.Namespace) -> int:
    require_gate_value("--min-recovery", args.min_recovery, 0.0, 1.0)
    tree_table = np.load(args.tree_table, allow_pickle=False)
    root_values = np.load(args.root_values, allow_pickle=False)

    # keyed join: every tree in the table must have been sampled
    order = np.argsort(root_values["tree_root_id"], kind="stable")
    sampled_roots = root_values["tree_root_id"][order]
    table_roots = tree_table["tree_root_id"]
    positions = np.searchsorted(sampled_roots, table_roots)
    if positions.max(initial=0) >= sampled_roots.size or not np.array_equal(
        sampled_roots[np.clip(positions, 0, sampled_roots.size - 1)], table_roots
    ):
        raise SubsetError(
            "calibrate-proxy needs root values for EVERY tree in the table "
            "(sample-roots with --m 0 / all candidates); {:,} sampled vs {:,} trees".format(
                sampled_roots.size, table_roots.size
            )
        )
    mvir = root_values["mvir"][order][positions]

    rho = spearman_rank_correlation(tree_table["extent"].astype(np.float64), mvir)
    _log("Spearman rho(byte extent, root Mvir) over {:,} trees = {:.4f}".format(mvir.size, rho))

    results = []
    chosen: Optional[dict] = None
    for depth in args.depths:
        fraction, m = recovery_fraction(
            tree_table["forest_id"], tree_table["extent"], mvir, depth, args.top_k
        )
        row = {
            "relative_depth": depth,
            "m_trees": m,
            "recovery_fraction": round(float(fraction), 4),
            "meets_gate": bool(fraction >= args.min_recovery),
        }
        results.append(row)
        _log(
            "  depth {:.5%} (M={:,}): recovery {:.4f} {}".format(
                depth, m, fraction, "PASS" if row["meets_gate"] else "fail"
            )
        )
        if chosen is None and row["meets_gate"]:
            chosen = row

    payload = {
        "n_trees": int(tree_table.size),
        "n_forests": int(np.unique(tree_table["forest_id"]).size),
        "top_k": args.top_k,
        "min_recovery": args.min_recovery,
        "spearman_rho_extent_vs_root_mvir": round(rho, 6),
        "depths": results,
        "calibrated_depth": chosen["relative_depth"] if chosen else None,
        "gate_passed": chosen is not None,
    }
    if args.production_trees:
        payload["production_trees"] = args.production_trees
        payload["production_m"] = (
            int(np.ceil(chosen["relative_depth"] * args.production_trees)) if chosen else None
        )
    if args.out:
        _write_json_atomic(Path(args.out), payload)
        _log("wrote {}".format(args.out))

    if chosen is None:
        _log(
            "GATE FAILED: no tested depth reaches recovery >= {:.2f}; the byte proxy "
            "is too weak to support a high-mass stratum".format(args.min_recovery)
        )
        return 1
    _log(
        "GATE PASSED at relative depth {:.5%}"
        "{}".format(
            chosen["relative_depth"],
            (
                "  ->  M = {:,} at {:,} production trees".format(
                    payload["production_m"], args.production_trees
                )
                if args.production_trees
                else ""
            ),
        )
    )
    return 0


# ---------------------------------------------------------------------------
# Stage 3 -- finalize
# ---------------------------------------------------------------------------


def half_decade_bins(n_trees: np.ndarray) -> np.ndarray:
    """Half-decade bin index on ``log10(n_trees)`` starting at ``n_trees = 1``."""
    counts = np.asarray(n_trees, dtype=np.float64)
    if np.any(counts < 1):
        raise SubsetError("forest tree counts below 1 cannot be binned")
    return np.floor(2.0 * np.log10(counts) + 1e-12).astype(np.int64)


def representativeness(population_n_trees: np.ndarray, sample_n_trees: np.ndarray) -> dict:
    """Frozen representativeness statistic: population *share* per half-decade
    ``log10(n_trees)`` bin, tail bins pooled rightward, factor-of-two acceptance.

    Comparing shares rather than per-bin sampling rates is deliberate: a uniform
    draw has a trivially uniform sampling rate, so that form of the test would
    pass regardless of the sample and prove nothing.
    """
    pop_bins = half_decade_bins(population_n_trees)
    sam_bins = half_decade_bins(sample_n_trees)

    max_bin = int(pop_bins.max()) if pop_bins.size else 0
    pop_counts = np.bincount(pop_bins, minlength=max_bin + 1).astype(np.int64)
    sam_counts = np.bincount(sam_bins, minlength=max_bin + 1).astype(np.int64)[: max_bin + 1]

    # Pool rightward: a bin holding fewer than BIN_POOLING_MIN_POPULATION
    # forests merges into the next bin up, repeatedly, so a handful of giant
    # forests cannot fail the test on counting noise. The leftover sparse tail
    # has no bin to its right, so it merges into the last pool instead. Pooling
    # reads the POPULATION only -- never the sample -- which is what keeps the
    # statistic falsifiable.
    pooled: List[List[int]] = []  # [lo_bin, hi_bin, pop, sam]
    carry_pop = 0
    carry_sam = 0
    lo: Optional[int] = None
    for b in range(max_bin + 1):
        if lo is None:
            lo = b
        carry_pop += int(pop_counts[b])
        carry_sam += int(sam_counts[b])
        if carry_pop >= BIN_POOLING_MIN_POPULATION:
            pooled.append([lo, b, carry_pop, carry_sam])
            carry_pop = 0
            carry_sam = 0
            lo = None
    if carry_pop or carry_sam:
        if pooled:
            pooled[-1][1] = max_bin
            pooled[-1][2] += carry_pop
            pooled[-1][3] += carry_sam
        else:
            pooled.append([lo if lo is not None else 0, max_bin, carry_pop, carry_sam])

    pop_total = int(pop_counts.sum())
    sam_total = int(sam_counts.sum())
    rows = []
    worst_ratio = 1.0
    all_represented = True
    for lo_bin, hi_bin, pop, sam in pooled:
        if pop == 0:
            continue
        pop_share = pop / pop_total if pop_total else 0.0
        sam_share = sam / sam_total if sam_total else 0.0
        if sam == 0:
            all_represented = False
            ratio = float("inf")
        else:
            ratio = max(sam_share / pop_share, pop_share / sam_share)
            worst_ratio = max(worst_ratio, ratio)
        rows.append(
            {
                "log10_n_trees_range": [lo_bin / 2.0, (hi_bin + 1) / 2.0],
                "population_forests": pop,
                "sampled_forests": sam,
                "population_share": round(pop_share, 6),
                "sampled_share": round(sam_share, 6),
                "share_ratio": None if ratio == float("inf") else round(ratio, 4),
            }
        )
    passed = all_represented and worst_ratio <= BIN_SHARE_MAX_RATIO
    return {
        "bins": rows,
        "every_populated_bin_represented": all_represented,
        "worst_share_ratio": round(worst_ratio, 4),
        "max_allowed_share_ratio": BIN_SHARE_MAX_RATIO,
        "passed": bool(passed),
        "population_median_n_trees": float(np.median(population_n_trees)),
        "population_p90_n_trees": float(np.percentile(population_n_trees, 90)),
        "sample_median_n_trees": float(np.median(sample_n_trees)) if sample_n_trees.size else None,
        "sample_p90_n_trees": (
            float(np.percentile(sample_n_trees, 90)) if sample_n_trees.size else None
        ),
        "note": (
            "Validates the FOREST-SIZE distribution -- the driver of reader workload "
            "and of the orphan statistics behind C and G. It is a proxy for, not a "
            "direct test of, low halo mass, which no index file carries."
        ),
    }


def forest_positions(forest_ids_sorted: np.ndarray, wanted: np.ndarray) -> np.ndarray:
    """Positions of ``wanted`` forest ids within the ascending forest table.

    Position-based lookup throughout ``finalize`` is deliberate: a Python
    ``{forest_id: index}`` dict would need well over 10 GB at production forest
    counts, for a mapping ``searchsorted`` already provides for free.
    """
    if forest_ids_sorted.size == 0:
        raise SubsetError("empty forest table")
    pos = np.clip(np.searchsorted(forest_ids_sorted, wanted), 0, forest_ids_sorted.size - 1)
    if not np.array_equal(forest_ids_sorted[pos], wanted):
        missing = np.asarray(wanted)[forest_ids_sorted[pos] != np.asarray(wanted)]
        raise SubsetError(
            "{} forest id(s) absent from the forest table; examples: {}".format(
                missing.size, missing[:5].tolist()
            )
        )
    return pos


def cmd_finalize(args: argparse.Namespace) -> int:
    tree_table = np.load(args.tree_table, allow_pickle=False)
    forest_table = np.load(args.forest_table, allow_pickle=False)
    candidates = np.load(args.candidates, allow_pickle=False)
    root_values = np.load(args.root_values, allow_pickle=False)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    with open(args.filemap, "r") as handle:
        filemap = {int(k): v for k, v in json.load(handle).items()}
    n_files = len(filemap)

    forest_ids = forest_table["forest_id"]
    n_forests = forest_table.size
    require_gate_value("--bytes-per-halo", args.bytes_per_halo, np.finfo(float).tiny, np.inf)
    est_halos = np.rint(forest_table["total_bytes"] / args.bytes_per_halo).astype(np.int64)
    gate_a = forest_table["n_trees"] <= args.max_trees_per_forest
    gate_b = est_halos <= args.max_halos_per_forest
    tractable = gate_a & gate_b
    if not tractable.any():
        raise SubsetError("no forest passes both tractability gates")
    _log(
        "tractable forests: {:,} of {:,} (Gate A excludes {:,}, Gate B excludes {:,})".format(
            int(tractable.sum()), n_forests, int((~gate_a).sum()), int((~gate_b).sum())
        )
    )

    biggest = int(np.argmax(forest_table["n_trees"]))
    super_forest = {
        "forest_id": int(forest_ids[biggest]),
        "n_trees": int(forest_table["n_trees"][biggest]),
        "projected_reader_allocation_bytes": int(
            int(forest_table["n_trees"][biggest]) * args.tree_alloc_bytes
        ),
        "estimated_halos": int(est_halos[biggest]),
        "excluded_by_gate_a": bool(not gate_a[biggest]),
        "excluded_by_gate_b": bool(not gate_b[biggest]),
    }

    # --- measured root values, joined onto the candidate pool by root id ---
    join_order = np.argsort(root_values["tree_root_id"], kind="stable")
    sampled_roots = root_values["tree_root_id"][join_order]
    if sampled_roots.size == 0:
        raise SubsetError("root_values.npy is empty")
    positions = np.clip(
        np.searchsorted(sampled_roots, candidates["tree_root_id"]), 0, sampled_roots.size - 1
    )
    if not np.array_equal(sampled_roots[positions], candidates["tree_root_id"]):
        raise SubsetError(
            "root_values.npy does not cover every candidate root "
            "({:,} sampled vs {:,} candidates)".format(sampled_roots.size, candidates.size)
        )
    cand_mvir = root_values["mvir"][join_order][positions]
    cand_j = [root_values[name][join_order][positions] for name in ("jx", "jy", "jz")]

    # Sampled z=0 Spin lower bound: max |J_k|/Mvir over sampled rows with
    # non-zero Mvir. Zero-mass rows carry raw J by the reference carve-out and
    # are not on the relation at all, so they are excluded from this figure.
    #
    # They are reported separately rather than dropped, because the two answer
    # different questions. The figure above bounds the physical spin parameter;
    # but a zero-mass halo's Spin FIELD holds un-normalised J, and the D7
    # [-1000, 1000] range check is applied to the field. A bound drawn only from
    # the on-relation rows therefore says nothing about what that check will see.
    nonzero = cand_mvir.astype(np.float32) != np.float32(0.0)
    sampled_spin_max = (
        float(max(np.abs(spin_from_widened(cand_mvir[nonzero], j[nonzero])).max() for j in cand_j))
        if nonzero.any()
        else None
    )
    zero_mass = ~nonzero
    sampled_zero_mass_raw_j_max = (
        float(max(np.abs(j[zero_mass].astype(np.float64).astype(np.float32)).max() for j in cand_j))
        if zero_mass.any()
        else None
    )

    measured_forests, measured_max_mvir = forest_max_root_mvir(candidates["forest_id"], cand_mvir)

    selected = np.zeros(n_forests, dtype=bool)

    # --- high-mass supplement: top K tractable forests by measured root Mvir ---
    supplement_pos = np.zeros(0, dtype=np.int64)
    if args.k > 0 and measured_forests.size:
        measured_pos = forest_positions(forest_ids, measured_forests)
        keep = tractable[measured_pos]
        ranked = measured_pos[keep][np.argsort(measured_max_mvir[keep], kind="stable")[::-1]]
        supplement_pos = ranked[: args.k]
    selected[supplement_pos] = True
    supplement_trees = int(forest_table["n_trees"][supplement_pos].sum())
    _log(
        "high-mass supplement: {:,} forests, {:,} trees".format(
            supplement_pos.size, supplement_trees
        )
    )

    # --- random stratum: fixed-seed uniform draw over the tractable population ---
    pool = np.nonzero(tractable & ~selected)[0]
    shuffled = np.random.default_rng(args.seed).permutation(pool)
    remaining = max(args.target_trees - supplement_trees, 0)
    random_pos = np.zeros(0, dtype=np.int64)
    if remaining and shuffled.size:
        cumulative = np.cumsum(forest_table["n_trees"][shuffled])
        take = min(int(np.searchsorted(cumulative, remaining, side="left")) + 1, shuffled.size)
        random_pos = shuffled[:take]
    selected[random_pos] = True
    _log(
        "random stratum: {:,} forests, {:,} trees (target {:,})".format(
            random_pos.size, int(forest_table["n_trees"][random_pos].sum()), remaining
        )
    )

    # --- file-coverage closure (C4), with complete forests only ---
    file_of_tree = tree_table["file_id"]
    if np.any(np.diff(file_of_tree.astype(np.int64)) < 0):
        raise SubsetError(
            "{}: not sorted by file_id; regenerate it with plan-candidates".format(args.tree_table)
        )
    file_bounds = np.searchsorted(file_of_tree, np.arange(n_files + 1, dtype=np.int32))
    forest_pos_of_tree = forest_positions(forest_ids, tree_table["forest_id"])

    closure_added: List[dict] = []
    for _ in range(args.max_closure_rounds):
        covered = np.zeros(n_files, dtype=bool)
        covered[np.unique(file_of_tree[selected[forest_pos_of_tree]])] = True
        missing = np.nonzero(~covered)[0]
        if missing.size == 0:
            break
        _log("closing {:,} uncovered file id(s) with complete forests".format(missing.size))
        for file_id in missing.tolist():
            touching = np.unique(
                forest_pos_of_tree[file_bounds[file_id] : file_bounds[file_id + 1]]
            )
            if selected[touching].any():
                # an addition earlier in this same round already closed this file:
                # forests span files (C8), so one forest can close several at once
                continue
            options = touching[tractable[touching] & ~selected[touching]]
            if options.size == 0:
                raise SubsetError(
                    "file id {} ({}) is touched only by intractable forests, so no "
                    "complete-forest closure exists for it. This is a blocker, not a "
                    "warning: adding a lone tree would create a partial forest, which "
                    "converts differently. See SHIN-UCHUU-CONVERSION-PLAN.md, 'the "
                    "file-coverage rule'".format(file_id, filemap.get(file_id, "?"))
                )
            smallest = int(options[int(np.argmin(forest_table["n_trees"][options]))])
            selected[smallest] = True
            closure_added.append(
                {
                    "file_id": int(file_id),
                    "forest_id": int(forest_ids[smallest]),
                    "n_trees": int(forest_table["n_trees"][smallest]),
                }
            )
    else:
        covered = np.zeros(n_files, dtype=bool)
        covered[np.unique(file_of_tree[selected[forest_pos_of_tree]])] = True
        if not covered.all():
            raise SubsetError(
                "file-coverage closure did not converge in {} rounds".format(
                    args.max_closure_rounds
                )
            )

    # --- build the selection and re-run the gates over the final set ---
    member = selected[forest_pos_of_tree]
    sel_rows = np.nonzero(member)[0]
    selection = np.zeros(sel_rows.size, dtype=SELECTION_DTYPE)
    for name in ("file_id", "tree_root_id", "forest_id", "offset", "extent"):
        selection[name] = tree_table[name][sel_rows]
    selection = selection[np.lexsort((selection["offset"], selection["file_id"]))]

    sel_pos = np.nonzero(selected)[0]
    sel_n_trees = forest_table["n_trees"][sel_pos]
    sel_halos = est_halos[sel_pos]
    largest = int(np.argmax(sel_n_trees))

    covered_files = np.unique(selection["file_id"])
    coverage_complete = covered_files.size == n_files and int(covered_files.max()) == n_files - 1
    cube_root = int(round(n_files ** (1.0 / 3.0)))

    supplement_halos = int(est_halos[supplement_pos].sum())
    total_halos = int(sel_halos.sum())
    supplement_fraction = supplement_halos / total_halos if total_halos else 0.0
    rep = representativeness(
        forest_table["n_trees"][tractable], forest_table["n_trees"][random_pos]
    )

    assertions = {
        "gate_a_tree_count": {
            "description": "no selected forest exceeds the per-forest tree-count cap",
            "cap": int(args.max_trees_per_forest),
            "largest_selected_forest_id": int(forest_ids[sel_pos[largest]]),
            "largest_selected_n_trees": int(sel_n_trees[largest]),
            "largest_selected_reader_allocation_bytes": int(
                int(sel_n_trees[largest]) * args.tree_alloc_bytes
            ),
            "passed": bool(int(sel_n_trees.max()) <= args.max_trees_per_forest),
        },
        "gate_b_halo_count": {
            "description": "no selected forest exceeds the per-forest halo-count cap",
            "cap": int(args.max_halos_per_forest),
            "largest_selected_estimated_halos": int(sel_halos.max()),
            "passed": bool(int(sel_halos.max()) <= args.max_halos_per_forest),
        },
        "file_coverage": {
            "description": (
                "every source file id contributes at least one selected tree, ids are "
                "contiguous from 0, and the file count is a perfect cube"
            ),
            "n_files": n_files,
            "files_covered": int(covered_files.size),
            "contiguous_from_zero": bool(coverage_complete),
            "cube_root": cube_root,
            "is_perfect_cube": bool(cube_root**3 == n_files),
            "forests_added_for_closure": closure_added,
            "passed": bool(coverage_complete and cube_root**3 == n_files),
        },
        "representativeness": rep,
        "super_forest_excluded": {
            "description": (
                "the largest forest is excluded whenever it is intractable, with the "
                "projected reader allocation that excludes it recorded as the reason"
            ),
            "super_forest": super_forest,
            "selected": bool(selected[biggest]),
            "passed": bool(tractable[biggest] or not selected[biggest]),
        },
    }
    gate_names = (
        "gate_a_tree_count",
        "gate_b_halo_count",
        "file_coverage",
        "super_forest_excluded",
        "representativeness",
    )
    all_passed = all(assertions[name]["passed"] for name in gate_names)

    manifest = {
        "seed": args.seed,
        "target_trees": args.target_trees,
        "k_supplement": args.k,
        "bytes_per_halo": args.bytes_per_halo,
        "tree_alloc_bytes": args.tree_alloc_bytes,
        "population": {
            "n_forests": int(n_forests),
            "n_trees": int(tree_table.size),
            "n_tractable_forests": int(tractable.sum()),
            "excluded_forests": int((~tractable).sum()),
            "excluded_forest_halo_fraction": round(
                float(est_halos[~tractable].sum()) / max(float(est_halos.sum()), 1.0), 6
            ),
        },
        "selection": {
            "n_forests": int(sel_pos.size),
            "n_trees": int(selection.size),
            "estimated_halos": total_halos,
            "estimated_ascii_bytes": int(selection["extent"].sum()),
        },
        "strata": {
            "random": {
                "n_forests": int(random_pos.size),
                "estimated_halos": int(est_halos[random_pos].sum()),
            },
            "high_mass_supplement": {
                "n_forests": int(supplement_pos.size),
                "estimated_halos": supplement_halos,
                "halo_fraction": round(supplement_fraction, 6),
                "halo_fraction_warn_threshold": args.supplement_halo_fraction,
                "within_balance_rule": bool(supplement_fraction <= args.supplement_halo_fraction),
                "note": (
                    "The supplement biases C/N and G/N upward, which is conservative for a "
                    "memory ceiling while it stays small. Above the threshold, raise the "
                    "random stratum rather than cutting K."
                ),
            },
            "coverage_closure": {"n_forests": len(closure_added)},
        },
        "sampled_z0_spin_max": sampled_spin_max,
        "sampled_z0_zero_mass_rows": int(zero_mass.sum()),
        "sampled_z0_zero_mass_raw_j_max": sampled_zero_mass_raw_j_max,
        "sampled_z0_zero_mass_raw_j_max_note": (
            "Zero-mass roots keep raw J in the Spin FIELD by the reference carve-out, so the "
            "D7 [-1000, 1000] range check sees these values too even though they are not on "
            "the J/Mvir relation. null means no sampled z=0 root had Mvir == 0."
        ),
        "sampled_z0_spin_max_note": (
            "max |J_k|/Mvir over sampled z=0 root rows with non-zero Mvir. A rigorous "
            "LOWER bound on the box's z=0 Spin maximum, not an upper bound and not an "
            "extremum; if it exceeds 1000, D7 is refuted on the spot."
        ),
        "assertions": assertions,
        "all_assertions_passed": bool(all_passed),
    }

    _write_npy_atomic(out_dir / "selection.npy", selection)
    _write_json_atomic(out_dir / "selection.json", manifest)
    _write_json_atomic(out_dir / "filemap.json", {str(k): v for k, v in sorted(filemap.items())})

    _log(
        "selected {:,} forests / {:,} trees / ~{:,} halos".format(
            sel_pos.size, selection.size, total_halos
        )
    )
    for name in gate_names:
        _log("  {}: {}".format(name, "PASS" if assertions[name]["passed"] else "FAIL"))
    if not manifest["strata"]["high_mass_supplement"]["within_balance_rule"]:
        _log(
            "  BALANCE RULE: the supplement holds {:.1%} of subset halos (> {:.0%}); raise "
            "the random stratum rather than cutting K".format(
                supplement_fraction, args.supplement_halo_fraction
            )
        )
    return 0 if all_passed else 1


# ---------------------------------------------------------------------------
# Stage 4 -- extract
# ---------------------------------------------------------------------------


def locate_header(handle, path: str) -> Tuple[bytes, int, int, int]:
    """Return ``(header_bytes, count_start, count_end, first_marker_pos)``.

    ``header_bytes`` are the file's bytes before its first ``#tree`` marker;
    ``count_start``/``count_end`` bracket the bare tree-count line's digits
    within them (excluding its newline), so it can be rewritten at exactly its
    original field width.
    """
    handle.seek(0)
    probe = handle.read(_HEADER_PROBE)
    marker = b"#tree "
    pos = -1
    if probe.startswith(marker):
        pos = 0
    else:
        needle = b"\n" + marker
        found = probe.find(needle)
        if found >= 0:
            pos = found + 1
    if pos < 0:
        raise SubsetError(
            "{}: no '#tree' marker within the first {} bytes".format(path, len(probe))
        )
    header = probe[:pos]

    count_start = count_end = -1
    line_start = 0
    while line_start < len(header):
        line_end = header.find(b"\n", line_start)
        if line_end < 0:
            line_end = len(header)
        line = header[line_start:line_end]
        stripped = line.strip()
        if stripped and not stripped.startswith(b"#"):
            if len(stripped.split()) != 1:
                raise SubsetError(
                    "{}: unexpected non-comment header line before the first '#tree' "
                    "marker: {!r}".format(path, line[:80])
                )
            try:
                int(stripped)
            except ValueError:
                raise SubsetError("{}: non-integer tree-count line: {!r}".format(path, line[:80]))
            if count_start >= 0:
                raise SubsetError("{}: more than one bare tree-count line".format(path))
            count_start = line_start
            count_end = line_end
        line_start = line_end + 1
    if count_start < 0:
        raise SubsetError(
            "{}: no bare tree-count line before the first '#tree' marker".format(path)
        )
    return header, count_start, count_end, pos


def _require_data_row_tail(handle, path: str, n_columns: int) -> None:
    """Extractor precondition: the source file must end with a newline **after a
    final data row**, carrying no trailer.

    The last tree in a file has its body taken as ``[offset, file_size)``, so both
    halves matter and neither implies the other: a missing newline truncates that
    body mid-row, while a trailer such as ``END`` is copied into it and only
    surfaces when the converter rejects the row's token count, long after the
    transfer is paid for.
    """
    handle.seek(0, os.SEEK_END)
    size = handle.tell()
    if size == 0:
        raise SubsetError("{}: empty source file".format(path))
    handle.seek(max(size - _TAIL_PROBE, 0))
    tail = handle.read()
    if not tail.endswith(b"\n"):
        raise SubsetError(
            "{}: does not end with a newline after its final data row -- the last "
            "tree's byte extent would be wrong".format(path)
        )
    last = tail.rstrip(b"\n").rsplit(b"\n", 1)[-1]
    if last.startswith(b"#") or len(last.split()) != n_columns:
        raise SubsetError(
            "{}: final line is not a data row ({} token(s), header declares {}): {!r} -- a "
            "trailer would be copied into the last tree's body".format(
                path, len(last.split()), n_columns, last[:80]
            )
        )


#: One emitted file's per-tree bookkeeping, held only while that file is verified.
_ExtractedFile = Tuple[List[int], List[int], List[int], List[str]]


def _extract_one_file(src_path: Path, dst_path: Path, rows: np.ndarray) -> _ExtractedFile:
    """Emit one subset ``tree_*.dat``: header, rewritten count line, then each
    selected tree's own ``#tree`` marker followed by its body bytes.

    Returns ``(root_ids, new_offsets, extents, source_body_md5s)`` for the
    verification pass, which runs immediately afterwards so this bookkeeping
    never accumulates across files.
    """
    with open(src_path, "rb") as src:
        n_columns = len(parse_header_columns(read_ctrees_header_line(src)))
        _require_data_row_tail(src, str(src_path), n_columns)
        header, count_start, count_end, _ = locate_header(src, str(src_path))
        width = count_end - count_start
        new_count = str(rows.size).encode("ascii")
        if len(new_count) > width:
            raise SubsetError(
                "{}: rewritten tree count {} does not fit the original {}-byte count "
                "field".format(src_path, rows.size, width)
            )
        new_header = header[:count_start] + new_count.ljust(width) + header[count_end:]

        roots: List[int] = []
        offsets: List[int] = []
        extents: List[int] = []
        digests: List[str] = []
        tmp_path = dst_path.with_name(dst_path.name + ".tmp")
        with open(tmp_path, "wb") as dst:
            dst.write(new_header)
            for row in rows:
                root = int(row["tree_root_id"])
                extent = int(row["extent"])
                marker = tree_marker_bytes(root)
                # Check the SOURCE marker before copying. Only the top-M candidates
                # are ever sampled, so for every other selected tree -- the bulk of
                # the subset -- this is the one place the index entry is tested
                # against the data it points at. Getting it wrong relabels one
                # tree's halos as another's, which converts cleanly and is wrong
                # only in the science. Reading the marker leaves the handle exactly
                # at the body, so it costs no extra seek.
                if int(row["offset"]) < len(marker):
                    raise SubsetError(
                        "{}: offset {} is too small to hold tree {}'s marker".format(
                            src_path, int(row["offset"]), root
                        )
                    )
                src.seek(int(row["offset"]) - len(marker))
                found = src.read(len(marker))
                if found != marker:
                    raise SubsetError(
                        "{}: tree {} is not preceded by its own marker in the source: expected "
                        "{!r}, found {!r} -- locations.dat disagrees with the data".format(
                            src_path, root, marker, found
                        )
                    )
                dst.write(marker)
                offsets.append(dst.tell())
                md5 = hashlib.md5()
                remaining = extent
                while remaining > 0:
                    chunk = src.read(min(_COPY_CHUNK, remaining))
                    if not chunk:
                        raise SubsetError(
                            "{}: unexpected EOF while copying tree {} ({} byte(s) "
                            "short)".format(src_path, root, remaining)
                        )
                    md5.update(chunk)
                    dst.write(chunk)
                    remaining -= len(chunk)
                roots.append(root)
                extents.append(extent)
                digests.append(md5.hexdigest())
    os.replace(str(tmp_path), str(dst_path))
    return roots, offsets, extents, digests


#: Searched for inside every emitted body. A body must never contain a line-start
#: '#tree' marker: if one is there, an extent ran past the next tree and the body
#: swallowed it. The md5 check cannot see this -- the same wrong bytes were both
#: copied and hashed -- so the emitted structure has to be checked directly.
_LINE_START_MARKER = b"\n#tree "


def _verify_one_file(path: Path, extracted: _ExtractedFile) -> List[str]:
    """Stream one emitted file once, checking marker placement, body md5s,
    data-row well-formedness, the rewritten count line, and that the emitted
    trees account for the whole file.

    The last two together are what make the count claim real. Comparing the
    rewritten count against the number of trees we meant to write proves nothing
    on its own; it becomes evidence once no body contains a marker and the
    verified bodies consume the file exactly, because the emitted marker count is
    then necessarily the number of trees checked here.
    """
    roots, offsets, extents, digests = extracted
    failures: List[str] = []
    file_size = path.stat().st_size
    overlap = len(_LINE_START_MARKER) - 1
    with open(path, "rb") as handle:
        header, count_start, count_end, _ = locate_header(handle, str(path))
        declared = int(header[count_start:count_end].strip())
        if declared != len(roots):
            failures.append(
                "{}: rewritten count {} != {} selected trees".format(
                    path.name, declared, len(roots)
                )
            )
        header_names = parse_header_columns(read_ctrees_header_line(handle))
        n_columns = len(header_names)
        id_column = resolve_sample_columns(header_names)["id"]

        for root, offset, extent, expected_md5 in zip(roots, offsets, extents, digests):
            marker = tree_marker_bytes(root)
            handle.seek(offset - len(marker))
            if handle.read(len(marker)) != marker:
                failures.append(
                    "{}: tree {} is not immediately preceded by its '#tree' line".format(
                        path.name, root
                    )
                )
                continue
            md5 = hashlib.md5()
            remaining = extent
            first_chunk = b""
            tail = b""
            swallowed = False
            while remaining > 0:
                chunk = handle.read(min(_COPY_CHUNK, remaining))
                if not chunk:
                    break
                if not first_chunk:
                    first_chunk = chunk
                # the marker can straddle a chunk boundary, so re-search the seam
                if _LINE_START_MARKER in chunk or _LINE_START_MARKER in tail + chunk[:overlap]:
                    swallowed = True
                tail = chunk[-overlap:]
                md5.update(chunk)
                remaining -= len(chunk)
            if remaining != 0:
                failures.append(
                    "{}: tree {} body is {} byte(s) short".format(path.name, root, remaining)
                )
                continue
            if swallowed:
                failures.append(
                    "{}: tree {} body contains a '#tree' marker -- its byte extent ran past "
                    "the next tree and swallowed it".format(path.name, root)
                )
            elif tail[-1:] != b"\n":
                # a correct body ends on its last data row's newline. Any other
                # final byte means the extent stopped mid-row, or overran into the
                # next marker by less than its full length -- which leaves '\n##tree'
                # rather than '\n#tree' and so slips past the scan above.
                failures.append(
                    "{}: tree {} body ends with {!r}, not a newline -- its byte extent is "
                    "wrong".format(path.name, root, tail[-1:])
                )
            if md5.hexdigest() != expected_md5:
                failures.append(
                    "{}: tree {} body md5 differs from its source range".format(path.name, root)
                )
            tokens = first_chunk.split(b"\n", 1)[0].split()
            if first_chunk.startswith(b"#") or len(tokens) != n_columns:
                failures.append(
                    "{}: tree {} body does not begin with a well-formed data row "
                    "({} token(s), header declares {})".format(
                        path.name, root, len(tokens), n_columns
                    )
                )
            else:
                # the body's first row is the tree's root, so its id must be the
                # root we labelled it with -- the other half of the marker check
                try:
                    row_id = int(tokens[id_column])
                except ValueError:
                    row_id = None
                if row_id is None:
                    failures.append(
                        "{}: tree {} body's first row has a non-integer id {!r}".format(
                            path.name, root, tokens[id_column]
                        )
                    )
                elif row_id != root:
                    failures.append(
                        "{}: tree {} body's first row has id {} -- the extracted bytes belong "
                        "to a different tree".format(path.name, root, row_id)
                    )
        # trees are verified in ascending offset order and the last body runs to
        # EOF, so anything left over is content the selection does not account for
        if roots and handle.tell() != file_size:
            failures.append(
                "{}: {} byte(s) after the last verified tree body -- the emitted file holds "
                "content the selection does not account for".format(
                    path.name, file_size - handle.tell()
                )
            )
    return failures


def cmd_extract(args: argparse.Namespace) -> int:
    selection_dir = Path(args.selection)
    selection = np.load(selection_dir / "selection.npy", allow_pickle=False)
    with open(selection_dir / "filemap.json", "r") as handle:
        filemap = {int(k): v for k, v in json.load(handle).items()}
    src_dir = Path(args.trees)
    out_dir = Path(args.out)
    # Emitting into the source directory would replace every tree_*.dat and both
    # index files with the subset, destroying an irreplaceable dataset. Resolved
    # so a symlink cannot smuggle the same directory past the comparison.
    if out_dir.exists() and out_dir.resolve() == src_dir.resolve():
        raise SubsetError(
            "--out is the source directory ({}); extraction would overwrite the source "
            "dataset".format(src_dir)
        )
    out_dir.mkdir(parents=True, exist_ok=True)
    # Tree files are replaced one at a time but the report is written only at the
    # end, so a run that fails part way would otherwise leave a passing report
    # describing contents that have already been partly overwritten.
    stale_report = out_dir / "extract_report.json"
    if stale_report.exists():
        stale_report.unlink()

    n_files = len(filemap)
    if sorted(filemap) != list(range(n_files)):
        raise SubsetError("source file ids are not contiguous from 0 (count {})".format(n_files))
    cube_root = int(round(n_files ** (1.0 / 3.0)))
    if cube_root**3 != n_files:
        raise SubsetError(
            "file count {} is not a perfect cube ({}^3 = {}); read_locations() will "
            "reject it".format(n_files, cube_root, cube_root**3)
        )

    selection = selection[np.lexsort((selection["offset"], selection["file_id"]))]
    bounds = np.searchsorted(selection["file_id"], np.arange(n_files + 1, dtype=np.int32))

    failures: List[str] = []
    n_verified = 0
    started = time.time()

    # The index files are written as extraction proceeds: buffering 5e6 rows to
    # write them at the end would blow the sub-1 GB budget this stage is chosen
    # for. Row order does not matter -- the reader sorts both files itself.
    loc_tmp = out_dir / "locations.dat.tmp"
    for_tmp = out_dir / "forests.list.tmp"
    with open(loc_tmp, "w") as loc_handle, open(for_tmp, "w") as for_handle:
        loc_handle.write("#TreeRootID FileID Offset Filename\n")
        for_handle.write("#TreeRootID ForestID\n")

        for file_id in range(n_files):
            name = os.path.basename(filemap[file_id])
            rows = selection[int(bounds[file_id]) : int(bounds[file_id + 1])]
            if rows.size == 0:
                raise SubsetError(
                    "file id {} ({}) has no selected trees; every source file must "
                    "contribute at least one, because read_locations() asserts file-id "
                    "contiguity and cubeness".format(file_id, name)
                )
            dst_path = out_dir / name
            extracted = _extract_one_file(src_dir / name, dst_path, rows)
            file_failures = _verify_one_file(dst_path, extracted)
            failures.extend(file_failures)
            n_verified += len(extracted[0]) - len(file_failures)

            for root, offset in zip(extracted[0], extracted[1]):
                loc_handle.write("{} {} {} {}\n".format(root, file_id, offset, name))
            for root, forest in zip(rows["tree_root_id"].tolist(), rows["forest_id"].tolist()):
                for_handle.write("{} {}\n".format(root, forest))

            if args.progress_every and (file_id + 1) % args.progress_every == 0:
                _log(
                    "extracted {:,}/{:,} files in {:.1f}s".format(
                        file_id + 1, n_files, time.time() - started
                    )
                )
    os.replace(str(loc_tmp), str(out_dir / "locations.dat"))
    os.replace(str(for_tmp), str(out_dir / "forests.list"))

    failures.extend(_verify_index_files(out_dir, selection["tree_root_id"]))

    report = {
        "n_files": n_files,
        "n_trees": int(selection.size),
        "bytes": int(selection["extent"].sum()),
        "elapsed_seconds": round(time.time() - started, 1),
        "checks": [
            "roots appear exactly once in forests.list and locations.dat",
            "file ids contiguous from 0, all present, count a perfect cube",
            "rewritten count line equals the '#tree' marker count",
            "no emitted body contains a '#tree' marker, and the bodies consume the "
            "whole file -- together these make the marker count evidence rather "
            "than a restatement of what was requested",
            "emitted body md5 equals the source byte range md5",
            "each offset is preceded by its '#tree' line and begins a data row",
            "each source file ends with a newline (checked before extraction)",
        ],
        "n_trees_verified": n_verified,
        "n_failures": len(failures),
        "failures": failures[:50],
        "passed": not failures,
    }
    _write_json_atomic(out_dir / "extract_report.json", report)
    _log(
        "extracted {:,} trees across {:,} files in {:.1f}s".format(
            selection.size, n_files, time.time() - started
        )
    )
    if failures:
        _log(
            "VERIFICATION FAILED ({} problem(s)) -- do not transfer this subset".format(
                len(failures)
            )
        )
        return 1
    _log("verification passed: {:,} trees checked".format(n_verified))
    return 0


def _verify_index_files(out_dir: Path, expected_roots: np.ndarray) -> List[str]:
    """Every selected root appears exactly once in both emitted index files."""
    failures: List[str] = []
    expected = np.sort(np.asarray(expected_roots, dtype=np.int64))
    loc_roots, _, _, _ = load_locations(out_dir / "locations.dat")
    for_roots, _ = load_forests_list(out_dir / "forests.list")
    for label, roots in (("locations.dat", loc_roots), ("forests.list", for_roots)):
        try:
            if not np.array_equal(_assert_unique_sorted(roots, "subset " + label), expected):
                failures.append("subset {} root set != selected root set".format(label))
        except SubsetError as exc:
            failures.append(str(exc))
    return failures


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="subset.py",
        description="Whole-forest subset selection and extraction for ctrees ASCII data.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p1 = sub.add_parser(
        "plan-candidates",
        help="local: build the per-tree and per-forest tables and the candidate pool",
    )
    p1.add_argument(
        "--index",
        required=True,
        help="directory holding forests.list, locations.dat and filesizes.tsv",
    )
    p1.add_argument("--out", required=True, help="output directory for the tables")
    p1.add_argument("--m", type=int, default=0, help="candidate trees to keep (0 = all)")
    p1.set_defaults(func=cmd_plan_candidates)

    p2 = sub.add_parser("sample-roots", help="remote: read one root row per candidate tree")
    p2.add_argument("--candidates", required=True)
    p2.add_argument("--filemap", required=True, help="filemap.json from plan-candidates")
    p2.add_argument("--trees", required=True, help="directory holding the tree_*.dat files")
    p2.add_argument("--a-list", required=True, help="scale-factor list; its last entry is z=0")
    p2.add_argument("--out", required=True)
    p2.add_argument("--scale-atol", type=float, default=1e-6)
    p2.add_argument("--progress-every", type=int, default=100_000)
    p2.set_defaults(func=cmd_sample_roots)

    p3 = sub.add_parser(
        "calibrate-proxy",
        help="local: measure byte-extent-vs-root-Mvir proxy quality and pick M",
    )
    p3.add_argument("--tree-table", required=True)
    p3.add_argument("--root-values", required=True, help="root values for EVERY tree")
    p3.add_argument("--top-k", type=int, default=200)
    p3.add_argument("--min-recovery", type=float, default=0.90)
    p3.add_argument(
        "--depths",
        type=float,
        nargs="+",
        default=[0.001, 0.002, 0.003, 0.005, 0.0075, 0.01],
        help="relative prefilter depths to test, ascending",
    )
    p3.add_argument("--production-trees", type=int, default=0)
    p3.add_argument("--out", default=None)
    p3.set_defaults(func=cmd_calibrate_proxy)

    p4 = sub.add_parser("finalize", help="local: gates, strata, coverage closure, manifest")
    p4.add_argument("--tree-table", required=True)
    p4.add_argument("--forest-table", required=True)
    p4.add_argument("--candidates", required=True)
    p4.add_argument("--root-values", required=True)
    p4.add_argument("--filemap", required=True)
    p4.add_argument("--out", required=True)
    p4.add_argument("--target-trees", type=int, required=True)
    p4.add_argument("--k", type=int, default=200, help="high-mass supplement size")
    p4.add_argument("--seed", type=int, required=True)
    p4.add_argument("--max-trees-per-forest", type=int, default=DEFAULT_MAX_TREES_PER_FOREST)
    p4.add_argument("--max-halos-per-forest", type=int, default=DEFAULT_MAX_HALOS_PER_FOREST)
    p4.add_argument("--bytes-per-halo", type=float, default=DEFAULT_BYTES_PER_HALO)
    p4.add_argument("--tree-alloc-bytes", type=int, default=DEFAULT_TREE_ALLOC_BYTES)
    p4.add_argument(
        "--supplement-halo-fraction", type=float, default=DEFAULT_SUPPLEMENT_HALO_FRACTION
    )
    p4.add_argument("--max-closure-rounds", type=int, default=32)
    p4.set_defaults(func=cmd_finalize)

    p5 = sub.add_parser("extract", help="remote: stream the selected byte ranges into a subset")
    p5.add_argument(
        "--selection", required=True, help="directory holding selection.npy and filemap.json"
    )
    p5.add_argument("--trees", required=True, help="source tree directory")
    p5.add_argument("--out", required=True, help="destination directory")
    p5.add_argument("--progress-every", type=int, default=100)
    p5.set_defaults(func=cmd_extract)

    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except SubsetError as exc:
        _log("FATAL: {}".format(exc))
        return 2
    except (OSError, ValueError) as exc:
        # a missing artifact or an unparsable one is fatal in the same sense as a
        # SubsetError, and shares its exit code so automation can tell a bad input
        # from a gate that ran and failed. Anything else is a bug and keeps its
        # traceback.
        _log("FATAL: {}: {}".format(type(exc).__name__, exc))
        return 2


if __name__ == "__main__":
    sys.exit(main())
