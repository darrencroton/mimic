"""Transform small Consistent-Trees ASCII fixtures into their cut form, for the
unmodified C reference reader.

A declared cut forest table (``forests.list`` shape: every tree root mapped to
its forest after the cut) cuts a catalogue by one rule: hosts are resolved
under the original ``forests.list``, then every halo whose resolved central
lies in another forest of the table becomes a central; no host relation is
invented. The C reference reader (``src/io/vertical/ctrees/ctrees_utils.c``,
``fix_upid``) knows nothing of a cut and is never taught one. This tool writes
a source it reads as the cut catalogue instead:

- every data row's ``pid`` and ``upid`` are first canonicalised to the hosts
  resolved under the original partition (the converter's own port of the
  reference resolution, ``fixups.fix_upid_snapshot``): a satellite carries
  its ultimate central's id in both, a central ``pid`` -1 and ``upid`` -1;
- then the cut is applied: a satellite whose central's forest in the table
  differs from its own is written as a central in the raw Consistent-Trees
  convention, ``pid`` -1 and ``upid`` -1, which is what the reference reader
  expects of a central (it sets such a halo's ``upid`` to its own id itself);
- the cut table is written as the new ``forests.list``, and ``locations.dat``
  is regenerated for the written file (each block's offset that of its first
  data row, where the reference reader starts reading).

**One output file.** The reference reader refuses a ``locations.dat`` whose
``FileID`` values are not contiguous from 0, or whose file count is not a
perfect cube (``read_locations``). The tool therefore writes every tree into
one file, :data:`OUTPUT_TREE_FILE` (1 = 1^3), keeping the trees' order: the
given files in turn, each in its own order. The inputs must share one header
line; the output keeps the first file's header and comments, its tree-count
line rewritten to the total, and every other file's lines from its first
``#tree`` marker on. The written layout is checked against both rules before
``locations.dat`` is written, and refused if it breaks either.

Every other byte is kept: the header, comments and the ``#tree`` markers; a
rewritten data row keeps its tokens, separated by single spaces. The table
must hold every ``#tree`` root of the files, and
every invariant of a cut table is checked against the trees' row counts
(``census.cut_table.read_cut_table``). An input whose hosts cannot be
resolved under the original partition is refused, as is an output directory
that is not new or empty.

For small fixtures only: every file is held in memory, as text and as one
record per halo.

Usage:
    mimic_venv/bin/python convert/mimic-convert/tests/tools/transform_ascii_fixture.py \\
        --forests-list <forests.list> --cut-table <table> --output-dir <dir> \\
        <tree file> [<tree file> ...]

Prints one JSON line (the file written and the promotions per snapshot);
exits 2 on a refusal, naming the offending input.
"""

import argparse
import json
import sys
from pathlib import Path
from typing import Dict, List, Sequence

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from census.cut_table import read_cut_table  # noqa: E402
from ctrees_parser import parse_header_line, resolve_columns  # noqa: E402
from errors import ConverterError  # noqa: E402
from fixups import fix_upid_snapshot  # noqa: E402
from scatter import load_forests_list  # noqa: E402

#: The one tree file the tool writes (Consistent-Trees' name for a one-file box).
OUTPUT_TREE_FILE = "tree_0_0_0.dat"

#: One parsed data row: where it is, and the columns the transformation reads.
ROW_DTYPE = np.dtype(
    [
        ("file", "<i8"),
        ("line", "<i8"),
        ("snap", "<i8"),
        ("id", "<i8"),
        ("pid", "<i8"),
        ("upid", "<i8"),
        ("tree_root_id", "<i8"),
        ("forest_id", "<i8"),
    ]
)


class _File:
    """One tree file's lines and the token positions of its host columns."""

    def __init__(self, path: Path):
        self.path = path
        self.lines = path.read_bytes().splitlines(keepends=True)
        if not self.lines:
            raise ConverterError("{}: empty file".format(path))
        if not self.lines[-1].endswith(b"\n"):
            self.lines[-1] += b"\n"
        #: index of the first ``#tree`` marker line (the end of the preamble)
        self.first_tree = next(
            (i for i, raw in enumerate(self.lines) if raw.split()[:1] == [b"#tree"]),
            len(self.lines),
        )
        self.n_trees = sum(1 for raw in self.lines if raw.split()[:1] == [b"#tree"])
        layout = resolve_columns(parse_header_line(self.lines[0].decode("ascii").strip()))
        self.n_columns = len(layout.all_names)
        self.id = layout.indices["id"]
        self.pid = layout.indices["pid"]
        self.upid = layout.indices["upid"]
        self.snap = layout.indices[layout.snapshot_key]


def _read_rows(files: Sequence[_File]) -> np.ndarray:
    """Every data row of every file, with the ``#tree`` root it belongs to."""
    rows: List[tuple] = []
    for file_index, tree_file in enumerate(files):
        root = None
        for line_index, raw in enumerate(tree_file.lines[1:], start=1):
            line = raw.strip()
            if not line:
                continue
            if line.startswith(b"#"):
                parts = line.split()
                if parts[0] == b"#tree":
                    root = int(parts[1])
                continue
            if root is None:
                continue  # the bare tree-count line before the first block
            tokens = line.split()
            if len(tokens) != tree_file.n_columns:
                raise ConverterError(
                    "{}:{}: {} token(s), the header declares {}".format(
                        tree_file.path, line_index + 1, len(tokens), tree_file.n_columns
                    )
                )
            rows.append(
                (
                    file_index,
                    line_index,
                    int(tokens[tree_file.snap]),
                    int(tokens[tree_file.id]),
                    int(tokens[tree_file.pid]),
                    int(tokens[tree_file.upid]),
                    root,
                    0,
                )
            )
    return np.array(rows, dtype=ROW_DTYPE)


def _hosts(rows: np.ndarray, cut_forest: np.ndarray) -> Dict[str, np.ndarray]:
    """The written ``pid``/``upid`` of every row and the promotion mask, one
    snapshot at a time: resolved under the original partition, then cut."""
    pid_out = np.full(rows.size, -1, dtype=np.int64)
    upid_out = np.full(rows.size, -1, dtype=np.int64)
    promoted = np.zeros(rows.size, dtype=bool)
    for snap in np.unique(rows["snap"]).tolist():
        at = np.flatnonzero(rows["snap"] == snap)
        at = at[np.argsort(rows["id"][at], kind="stable")]
        records = rows[at].copy()
        if records.size > 1 and np.any(records["id"][1:] == records["id"][:-1]):
            raise ConverterError("snapshot {}: a halo id occurs more than once".format(snap))
        try:
            fix_upid_snapshot(records, snap)
        except ConverterError as exc:
            raise ConverterError(
                "the hosts cannot be resolved under the original forests.list: {}".format(exc)
            ) from exc
        satellite = np.flatnonzero(records["pid"] != -1)
        central = np.searchsorted(records["id"], records["upid"][satellite])
        forest = cut_forest[at]
        severed = forest[central] != forest[satellite]
        kept = satellite[~severed]
        pid_out[at[kept]] = records["upid"][kept]
        upid_out[at[kept]] = records["upid"][kept]
        promoted[at[satellite[severed]]] = True
    return {"pid": pid_out, "upid": upid_out, "promoted": promoted}


def _rewritten(tree_file: _File, lines_of_rows: np.ndarray, pid, upid) -> List[bytes]:
    """``tree_file``'s lines with each listed data row's host tokens replaced."""
    lines = list(tree_file.lines)
    for line_index, row_pid, row_upid in zip(lines_of_rows.tolist(), pid.tolist(), upid.tolist()):
        raw = lines[line_index]
        body = raw.rstrip(b"\r\n")
        tokens = body.split()
        tokens[tree_file.pid] = str(row_pid).encode("ascii")
        tokens[tree_file.upid] = str(row_upid).encode("ascii")
        lines[line_index] = b" ".join(tokens) + raw[len(body) :]
    return lines


def _preamble(tree_file: _File, n_trees: int) -> List[bytes]:
    """The first file's header and comments, its bare tree-count line (when it
    has one) rewritten to the output's ``n_trees``."""
    lines = []
    for raw in tree_file.lines[: tree_file.first_tree]:
        body = raw.strip()
        if lines and body and not body.startswith(b"#"):
            raw = str(n_trees).encode("ascii") + raw[len(raw.rstrip(b"\r\n")) :]
        lines.append(raw)
    return lines


def locations_rows(path: Path, file_id: int = 0) -> List[tuple]:
    """``(root, FileID, offset, name)`` for each ``#tree`` marker of ``path``,
    the offset that of its block's first data row."""
    rows = []
    position = 0
    for raw in path.read_bytes().splitlines(keepends=True):
        position += len(raw)
        parts = raw.split()
        if parts and parts[0] == b"#tree":
            rows.append((int(parts[1]), file_id, position, path.name))
    return rows


def check_reader_layout(rows: Sequence[tuple]) -> None:
    """The reference reader's ``locations.dat`` rules: ``FileID`` values
    contiguous from 0, and a perfect-cube file count."""
    file_ids = sorted({row[1] for row in rows})
    n_files = len(file_ids)
    if file_ids != list(range(n_files)):
        raise ConverterError("FileID values {} are not contiguous from 0".format(file_ids))
    divisions = int(round(n_files ** (1.0 / 3.0)))
    if divisions**3 != n_files:
        raise ConverterError("{} tree files is not a perfect cube".format(n_files))


def transform(tree_files: Sequence, forests_list, cut_table, output_dir) -> Dict:
    """Write the cut form of ``tree_files`` into ``output_dir`` (module
    docstring). Returns the summary the CLI prints."""
    paths = [Path(path).resolve() for path in tree_files]
    if not paths:
        raise ConverterError("no tree file given")
    names = [path.name for path in paths]
    if len(set(names)) != len(names):
        raise ConverterError("tree file names must be distinct: {}".format(names))
    output_dir = Path(output_dir).resolve()
    if output_dir.exists() and any(output_dir.iterdir()):
        raise ConverterError("{}: the output directory is not empty".format(output_dir))
    if output_dir in {path.parent for path in paths}:
        raise ConverterError("{}: the output directory holds an input".format(output_dir))

    files = [_File(path) for path in paths]
    headers = {tree_file.lines[0].strip() for tree_file in files}
    if len(headers) != 1:
        raise ConverterError("the tree files do not share one header line; they cannot be joined")
    rows = _read_rows(files)
    forest_map = load_forests_list(forests_list)
    rows["forest_id"] = forest_map.lookup_forest_ids(rows["tree_root_id"])
    totals = np.zeros(forest_map.tree_root_ids.size, dtype=np.int64)
    np.add.at(totals, np.searchsorted(forest_map.tree_root_ids, rows["tree_root_id"]), 1)
    table_roots, table_ids = read_cut_table(
        cut_table, forest_map.tree_root_ids, forest_map.forest_ids, totals
    )
    cut_forest = table_ids[np.searchsorted(table_roots, rows["tree_root_id"])]
    hosts = _hosts(rows, cut_forest)

    text = _preamble(files[0], sum(tree_file.n_trees for tree_file in files))
    for file_index, tree_file in enumerate(files):
        mine = rows["file"] == file_index
        lines = _rewritten(tree_file, rows["line"][mine], hosts["pid"][mine], hosts["upid"][mine])
        text.extend(lines[tree_file.first_tree :])

    output_dir.mkdir(parents=True, exist_ok=True)
    out = output_dir / OUTPUT_TREE_FILE
    out.write_bytes(b"".join(text))
    locations = locations_rows(out)
    check_reader_layout(locations)
    (output_dir / "locations.dat").write_text(
        "\n".join(
            ["#TreeRootID FileID Offset Filename"] + ["{} {} {} {}".format(*r) for r in locations]
        )
        + "\n"
    )
    (output_dir / "forests.list").write_bytes(Path(cut_table).read_bytes())
    promoted = rows["snap"][hosts["promoted"]]
    return {
        "output_dir": str(output_dir),
        "tree_file": str(out),
        "promoted_per_snapshot": {
            int(snap): int(count) for snap, count in zip(*np.unique(promoted, return_counts=True))
        },
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--forests-list", required=True, help="the original forests.list")
    parser.add_argument("--cut-table", required=True, help="the cut forest table")
    parser.add_argument("--output-dir", required=True, help="a new or empty directory")
    parser.add_argument("tree_files", nargs="+", help="the tree files, in the order to join them")
    args = parser.parse_args(argv)
    try:
        summary = transform(args.tree_files, args.forests_list, args.cut_table, args.output_dir)
    except (ConverterError, OSError) as exc:
        print("ERROR: {}".format(exc), file=sys.stderr)
        return 2
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
