"""Phase 1 ctrees ASCII parsing for the ctrees -> snapshot-HDF5 converter.

Owns the frozen scratch-record dtype, both Consistent-Trees header dialects
(indexed ``#scale(0) id(1) ...`` primary, ``#fields:`` secondary), ``#tree``
block-marker tracking, chunked pandas reads, and the independent row pre-count.

Reference semantics mirrored here (see docs/dev/MIMIC-CONVERTER-IMPLEMENTATION-PLAN.md):
- column names are truncated at the first ``(`` and matched case-insensitively
  (src/io/tree/ctrees/parse_ctrees.h); ``snap_idx``/``snap_num`` are equivalent
  spellings of the snapshot column (src/io/tree/read_ctrees_ascii.c
  setup_column_info);
- floats are parsed as float64 and cast to float32 at record assembly, matching
  the reference strtod-then-cast parse path;
- duplicate or missing required columns abort; malformed rows abort.
"""

import hashlib
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Set, Tuple

import numpy as np
import pandas as pd

#: Frozen scratch-record dtype: little-endian, packed, itemsize 108 bytes.
#: Field order and widths are contract-frozen by the implementation plan
#: (Slice 2); every scratch-file manifest entry records DTYPE_TAG against it.
RECORD_DTYPE = np.dtype(
    [
        ("id", "<i8"),
        ("desc_id", "<i8"),
        ("desc_scale", "<f8"),
        ("pid", "<i8"),
        ("upid", "<i8"),
        ("snap", "<i4"),
        ("Mvir", "<f4"),
        ("X", "<f4"),
        ("Y", "<f4"),
        ("Z", "<f4"),
        ("VX", "<f4"),
        ("VY", "<f4"),
        ("VZ", "<f4"),
        ("Jx", "<f4"),
        ("Jy", "<f4"),
        ("Jz", "<f4"),
        ("vrms", "<f4"),
        ("vmax", "<f4"),
        ("tree_root_id", "<i8"),
        ("forest_id", "<i8"),
    ],
    align=False,
)

#: Human-readable dtype identity recorded in every manifest.
DTYPE_TAG = "ctrees-scratch-v1/itemsize=108/" + ",".join(
    "{}:{}".format(name, RECORD_DTYPE.fields[name][0].str) for name in RECORD_DTYPE.names
)

DEFAULT_CHUNKSIZE = 1_000_000

#: Required ctrees columns (normalized lowercase) -> parse dtype.
#: The snapshot column is handled separately because it has two spellings.
_INT_COLUMNS = ("id", "desc_id", "pid", "upid")
_FLOAT_COLUMNS = (
    "scale",
    "desc_scale",
    "mvir",
    "vrms",
    "vmax",
    "x",
    "y",
    "z",
    "vx",
    "vy",
    "vz",
    "jx",
    "jy",
    "jz",
)
#: Older ctrees files use snap_num, newer use snap_idx (read_ctrees_ascii.c).
SNAPSHOT_SPELLINGS = ("snap_idx", "snap_num")

#: record field <- ctrees column (both normalized) for direct copies.
_RECORD_FROM_COLUMN = {
    "id": "id",
    "desc_id": "desc_id",
    "desc_scale": "desc_scale",
    "pid": "pid",
    "upid": "upid",
    "Mvir": "mvir",
    "X": "x",
    "Y": "y",
    "Z": "z",
    "VX": "vx",
    "VY": "vy",
    "VZ": "vz",
    "Jx": "jx",
    "Jy": "jy",
    "Jz": "jz",
    "vrms": "vrms",
    "vmax": "vmax",
}


class ConverterError(RuntimeError):
    """Fatal converter failure: the run must abort, never repair silently."""


@dataclass
class PreScan:
    """Marker/count evidence from the pandas-independent pre-scan of one file."""

    path: str
    header_line: str
    n_rows: int
    #: data-row ordinal at which each ``#tree`` block starts (ascending)
    tree_start_rows: np.ndarray
    #: tree root id for each block, aligned with tree_start_rows
    tree_root_ids: np.ndarray
    md5: str
    size: int
    #: st_mtime_ns captured before the scan; the scatter stage re-stats after
    #: the pandas pass and aborts if the source changed between the two reads
    mtime_ns: int
    #: ctrees files carry a bare integer tree-count line between the header
    #: comments and the first '#tree' marker (verified against tree_0_0_0.dat);
    #: None when absent. The scatter stage validates it against the marker count.
    declared_tree_count: Optional[int] = None
    #: physical 0-based line index of the count line (for pandas skiprows)
    count_line_index: Optional[int] = None


def normalize_column_name(token: str) -> str:
    """Truncate at the first '(' — reference parse_ctrees.h suffix stripping."""
    return token.split("(", 1)[0]


def parse_header_line(header_line: str) -> List[str]:
    """Return normalized (suffix-stripped, original-case) column names.

    Accepts the indexed primary dialect (``#scale(0) id(1) ...``) and the
    ``#fields:`` secondary dialect. Tokens are split on whitespace and commas,
    matching the reference delimiter set (space, comma, newline, '#').
    """
    if not header_line.startswith("#"):
        raise ConverterError(
            "ctrees header line must start with '#', got: {!r}".format(header_line[:80])
        )
    body = header_line.lstrip("#").strip()
    if body.lower().startswith("fields:"):
        body = body[len("fields:") :]
    tokens = [t for t in re.split(r"[,\s]+", body) if t]
    names = [normalize_column_name(t) for t in tokens]
    names = [n for n in names if n]
    if not names:
        raise ConverterError(
            "ctrees header line contains no column names: {!r}".format(header_line[:80])
        )
    return names


@dataclass
class ColumnLayout:
    """Resolved required-column indices for one file's header."""

    all_names: List[str]
    #: normalized-lowercase required column -> column index in the file
    indices: Dict[str, int]
    #: which snapshot spelling the file uses (normalized lowercase)
    snapshot_column: str


def resolve_columns(names: List[str]) -> ColumnLayout:
    """Map required columns to file column indices, aborting per the contract.

    Case-insensitive first-match semantics follow the reference
    match_column_name(); unlike the reference, a duplicated required column or
    both snapshot spellings at once abort (plan Slice 2).
    """
    lowered = [n.lower() for n in names]
    counts: Dict[str, int] = {}
    for name in lowered:
        counts[name] = counts.get(name, 0) + 1

    required = list(_INT_COLUMNS) + list(_FLOAT_COLUMNS)
    duplicates = [name for name in required if counts.get(name, 0) > 1]
    duplicates += [s for s in SNAPSHOT_SPELLINGS if counts.get(s, 0) > 1]
    if duplicates:
        raise ConverterError(
            "duplicate required column(s) in ctrees header: {}".format(
                ", ".join(sorted(duplicates))
            )
        )

    snapshot_present = [s for s in SNAPSHOT_SPELLINGS if s in counts]
    if len(snapshot_present) > 1:
        raise ConverterError(
            "ambiguous snapshot column: both {} present in ctrees header".format(
                " and ".join(snapshot_present)
            )
        )
    if not snapshot_present:
        raise ConverterError(
            "missing required column(s) in ctrees header: one of {}".format(
                "/".join(SNAPSHOT_SPELLINGS)
            )
        )
    snapshot_column = snapshot_present[0]

    indices: Dict[str, int] = {}
    missing: List[str] = []
    for wanted in required + [snapshot_column]:
        try:
            indices[wanted] = lowered.index(wanted)
        except ValueError:
            missing.append(wanted)
    if missing:
        raise ConverterError(
            "missing required column(s) in ctrees header: {}".format(", ".join(sorted(missing)))
        )
    return ColumnLayout(all_names=names, indices=indices, snapshot_column=snapshot_column)


def prescan_file(path) -> PreScan:
    """Stream the file once, independent of pandas: header, ``#tree`` markers,
    valid data-row count, size, and md5.

    This is the independent row pre-count of plan review finding 7: the parsed
    row count must equal ``n_rows`` exactly before the file's result is
    accepted. Every data row must also have exactly as many whitespace-separated
    tokens as the header declares columns — an explicit structural invariant
    independent of the pandas column projection.
    """
    path = Path(path)
    stat_before = path.stat()
    md5 = hashlib.md5()
    tree_start_rows: List[int] = []
    tree_root_ids: List[int] = []
    n_rows = 0
    header_line: Optional[str] = None
    expected_tokens: Optional[int] = None
    declared_tree_count: Optional[int] = None
    count_line_index: Optional[int] = None
    with open(path, "rb") as handle:
        for lineno, raw in enumerate(handle, start=1):
            md5.update(raw)
            line = raw.strip()
            if header_line is None:
                if not line.startswith(b"#"):
                    raise ConverterError(
                        "{}: first line must be a '#' header, got: {!r}".format(
                            path, line[:80].decode("utf-8", "replace")
                        )
                    )
                header_line = line.decode("utf-8", "replace")
                expected_tokens = len(parse_header_line(header_line))
                continue
            if not line:
                continue
            if line.startswith(b"#"):
                # a marker's first token must be exactly '#tree' — prefixes
                # like '#treejunk' are ordinary comments, and a real '#tree'
                # marker with wrong arity or a non-integer id aborts
                parts = line.split()
                if parts[0] == b"#tree":
                    if len(parts) != 2:
                        raise ConverterError(
                            "{}:{}: malformed '#tree' marker: {!r}".format(
                                path, lineno, line[:80].decode("utf-8", "replace")
                            )
                        )
                    try:
                        root_id = int(parts[1])
                    except ValueError:
                        raise ConverterError(
                            "{}:{}: non-integer '#tree' root id: {!r}".format(
                                path, lineno, parts[1][:40].decode("utf-8", "replace")
                            )
                        )
                    tree_start_rows.append(n_rows)
                    tree_root_ids.append(root_id)
                continue
            if b"#" in line:
                # pandas comment='#' would silently drop the row tail, hiding
                # it from the structural invariant — never legal in ctrees data
                raise ConverterError(
                    "{}:{}: inline '#' in data row: {!r}".format(
                        path, lineno, line[:80].decode("utf-8", "replace")
                    )
                )
            if not tree_start_rows:
                # before the first '#tree' marker only the bare tree-count
                # line is legal (real ctrees layout, e.g. '561266')
                tokens = line.split()
                if len(tokens) == 1 and declared_tree_count is None:
                    try:
                        declared_tree_count = int(tokens[0])
                    except ValueError:
                        raise ConverterError(
                            "{}:{}: data row before the first '#tree' marker".format(path, lineno)
                        )
                    count_line_index = lineno - 1
                    continue
                raise ConverterError(
                    "{}:{}: data row before the first '#tree' marker".format(path, lineno)
                )
            ntokens = len(line.split())
            if ntokens != expected_tokens:
                raise ConverterError(
                    "{}:{}: malformed data row: {} token(s), header declares {} columns".format(
                        path, lineno, ntokens, expected_tokens
                    )
                )
            n_rows += 1
    if header_line is None:
        raise ConverterError("{}: empty file (no header line)".format(path))
    return PreScan(
        path=str(path),
        header_line=header_line,
        n_rows=n_rows,
        tree_start_rows=np.asarray(tree_start_rows, dtype=np.int64),
        tree_root_ids=np.asarray(tree_root_ids, dtype=np.int64),
        md5=md5.hexdigest(),
        size=stat_before.st_size,
        mtime_ns=stat_before.st_mtime_ns,
        declared_tree_count=declared_tree_count,
        count_line_index=count_line_index,
    )


@dataclass
class ParseResult:
    """Totals accumulated by CtreesFileParser.chunks(); valid once complete."""

    n_rows_parsed: int = 0
    #: observed (snapshot, scale) pairs across the whole file
    observed_pairs: Set[Tuple[int, float]] = field(default_factory=set)
    complete: bool = False


class CtreesFileParser:
    """Chunked parser for one ctrees ASCII file.

    Yields structured arrays in RECORD_DTYPE with ``tree_root_id`` attributed
    from ``#tree`` markers and ``forest_id`` set to -1 (joined by the scatter
    stage). The generator raises ConverterError unless the parsed row count
    equals the independent pre-count exactly.
    """

    def __init__(self, path, chunksize: int = DEFAULT_CHUNKSIZE, prescan: Optional[PreScan] = None):
        self.path = Path(path)
        if chunksize < 1:
            raise ConverterError("chunksize must be >= 1, got {}".format(chunksize))
        self.chunksize = chunksize
        self.prescan = prescan if prescan is not None else prescan_file(self.path)
        self.layout = resolve_columns(parse_header_line(self.prescan.header_line))
        self.result = ParseResult()

    def _read_csv_chunks(self):
        ncols = len(self.layout.all_names)
        pandas_names = ["c{}".format(i) for i in range(ncols)]
        used = {"c{}".format(idx): col for col, idx in self.layout.indices.items()}
        dtype_map = {}
        for pname, col in used.items():
            if col in _INT_COLUMNS or col == self.layout.snapshot_column:
                dtype_map[pname] = np.int64
            else:
                dtype_map[pname] = np.float64
        skiprows = None
        if self.prescan.count_line_index is not None:
            skiprows = [self.prescan.count_line_index]
        return pd.read_csv(
            self.path,
            sep=r"\s+",
            comment="#",
            header=None,
            names=pandas_names,
            usecols=sorted(used, key=lambda n: int(n[1:])),
            dtype=dtype_map,
            chunksize=self.chunksize,
            skiprows=skiprows,
        )

    def _column(self, chunk: pd.DataFrame, name: str) -> np.ndarray:
        return chunk["c{}".format(self.layout.indices[name])].to_numpy()

    def chunks(self) -> Iterator[np.ndarray]:
        prescan = self.prescan
        row_offset = 0
        try:
            reader = self._read_csv_chunks()
        except (pd.errors.ParserError, ValueError) as exc:
            raise ConverterError("{}: malformed ctrees data: {}".format(self.path, exc))
        with reader:
            while True:
                try:
                    chunk = reader.get_chunk()
                except StopIteration:
                    break
                except (pd.errors.ParserError, ValueError) as exc:
                    raise ConverterError("{}: malformed ctrees data: {}".format(self.path, exc))
                # structurally short rows are impossible here (the pre-scan
                # enforces exact per-row token arity); float NaN can only be a
                # literal nan token, which _assemble rejects as non-finite
                n = len(chunk)
                if row_offset + n > prescan.n_rows:
                    raise ConverterError(
                        "{}: parsed more rows ({}) than the independent pre-count ({})".format(
                            self.path, row_offset + n, prescan.n_rows
                        )
                    )
                records = self._assemble(chunk, row_offset)
                row_offset += n
                yield records
        if row_offset != prescan.n_rows:
            raise ConverterError(
                "{}: parsed row count {} != independent pre-count {}".format(
                    self.path, row_offset, prescan.n_rows
                )
            )
        self.result.n_rows_parsed = row_offset
        self.result.complete = True

    def _abort_non_finite(self, what: str, col: str, bad: np.ndarray, row_offset: int) -> None:
        ordinals = (np.nonzero(bad)[0][:5] + row_offset).tolist()
        raise ConverterError(
            "{}: {} value(s) in column '{}' at data-row ordinal(s) {} — "
            "aborting, never repairing".format(self.path, what, col, ordinals)
        )

    def _assemble(self, chunk: pd.DataFrame, row_offset: int) -> np.ndarray:
        n = len(chunk)
        records = np.zeros(n, dtype=RECORD_DTYPE)
        for rec_field, col in _RECORD_FROM_COLUMN.items():
            values = self._column(chunk, col)
            if values.dtype.kind == "f":
                bad = ~np.isfinite(values)
                if bad.any():
                    self._abort_non_finite("non-finite", col, bad, row_offset)
            # float64 parse -> float32 cast at record assembly (reference path);
            # the reference parser rejects non-finite values, and a finite
            # float64 overflowing float32 becomes inf — both abort here (the
            # overflow warning is suppressed because the inf check below is
            # the deliberate detector)
            with np.errstate(over="ignore"):
                cast = values.astype(RECORD_DTYPE[rec_field], copy=False)
            if cast.dtype.kind == "f":
                bad = ~np.isfinite(cast)
                if bad.any():
                    self._abort_non_finite("float32-overflowing", col, bad, row_offset)
            records[rec_field] = cast

        snap64 = self._column(chunk, self.layout.snapshot_column)
        if snap64.size and (
            snap64.min() < np.iinfo(np.int32).min or snap64.max() > np.iinfo(np.int32).max
        ):
            raise ConverterError(
                "{}: snapshot value outside int32 range (min {}, max {})".format(
                    self.path, snap64.min(), snap64.max()
                )
            )
        records["snap"] = snap64.astype(np.int32)

        # #tree attribution: interval lookup over pre-scan marker start rows.
        row_ordinals = np.arange(row_offset, row_offset + n, dtype=np.int64)
        marker_idx = np.searchsorted(self.prescan.tree_start_rows, row_ordinals, side="right") - 1
        records["tree_root_id"] = self.prescan.tree_root_ids[marker_idx]
        records["forest_id"] = -1

        scale64 = self._column(chunk, "scale")
        bad = ~np.isfinite(scale64)
        if bad.any():
            self._abort_non_finite("non-finite", "scale", bad, row_offset)
        pairs = np.unique(np.column_stack((snap64.astype(np.float64), scale64)), axis=0)
        self.result.observed_pairs.update((int(s), float(a)) for s, a in pairs)
        return records


def parse_file(path, chunksize: int = DEFAULT_CHUNKSIZE) -> Tuple[np.ndarray, ParseResult, PreScan]:
    """Parse a whole file into one array (tests / small-file convenience)."""
    parser = CtreesFileParser(path, chunksize=chunksize)
    parts = list(parser.chunks())
    if parts:
        records = np.concatenate(parts)
    else:
        records = np.zeros(0, dtype=RECORD_DTYPE)
    return records, parser.result, parser.prescan
