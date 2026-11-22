"""Consistent-Trees ASCII forests to L-Halo records, the way sage-model's
``convert_trees_to_lhalo`` wrote them.

This module reproduces sage-model's ``convert_trees_to_lhalo`` as the code stood at
sage-model commit ``9218e11`` (2022-06-11), with exactly two differences (below). Every line
reference in this docstring and in the code is to a file at that commit, read with
``git -C <sage-model> show 9218e11:<path>``.

Terms. An *ASCII tree* is one ``#tree`` block of a ``tree_*.dat`` file. A *forest* is the
set of ASCII trees sharing a ``ForestID`` in ``forests.list``; it is one L-Halo "tree" of
the output files.

Steps and the sage-model code each reproduces
---------------------------------------------
Reading the tree tables (``read_forest_table``):

* ``read_forests_list`` -- ``read_forests``, ``src/io/ctrees_utils.c`` lines 24-66, with
  ``getnumlines``, ``src/core_utils.c`` lines 127-157: lines whose first byte is ``#`` are
  skipped; every other line holds ``TreeRootID ForestID``.
* ``read_locations`` -- ``read_locations``, ``src/io/ctrees_utils.c`` lines 69-190: lines
  ``TreeRootID FileID Offset Filename``; the line count must equal ``forests.list``'s;
  offsets and file ids must be non-negative; a file id is opened at its first line as the
  directory of ``locations.dat`` (the path up to its last ``/``) joined with that line's
  ``Filename``; the file-id table grows only by one doubling from 2000 entries, so a file
  id equal to the table size, or at least twice it, is refused (lines 130-151); the file
  ids must be contiguous from 0 (lines 171-182) and their count a perfect cube (lines
  183-187).
* ``pair_forests`` -- ``assign_forest_ids``, ``src/io/ctrees_utils.c`` lines 278-294, with
  the sorts of lines 193-204 and 257-260: both tables sorted by tree id and paired by
  position, refused where the ids disagree. Then the trees are sorted by (``ForestID``,
  ``FileID``, ``Offset``) (``sort_locations_on_fid_file_offset``, lines 236-255 and
  272-275, called at ``src/io/read_tree_consistentrees_ascii.c`` line 66) and forests are
  the runs of equal ``ForestID``, counted from a previous id of -1
  (``read_tree_consistentrees_ascii.c`` lines 68-79 and 105-120), so leading trees with
  ``ForestID`` -1 belong to no forest and are dropped, as sage-model skips them; a forest
  count of ``INT_MAX`` or more is refused (lines 76-79).

Reading forests (``read_forests`` for a run of forests parsed together, ``read_forest``
for one):

* ``read_column_map`` -- ``parse_header_ctrees``, ``src/io/parse_ctrees.h`` lines 166-321,
  with ``match_column_name``, lines 111-140, on the first line of one tree file (the file
  ``get_forests_filename_ctr_ascii`` names, ``read_tree_consistentrees_ascii.c`` lines
  21-25 and 308-314: ``SimulationDir/TreeName``, a parameter here). The line is read as
  ``fgets`` reads it (at most 1239 bytes); it must begin with ``#``; its ``strsep`` count
  on ``" ,"`` must equal its count of non-empty tokens on ``" ,\\n#"``; a token must be
  shorter than 64 bytes; a column's name is its token up to ``(``; a requested name
  matches the first column whose name equals it ignoring ASCII case. The requested names
  and their types are those of ``read_tree_consistentrees_ascii.c`` lines 242-292:
  ``scale`` and ``desc_scale`` float64; ``id``, ``desc_id``, ``pid``, ``upid`` int64;
  ``snap_num`` or ``snap_idx`` int32 (when both are found the later column wins, as the
  later column is parsed last); every other column float32.
* ``tree_text`` -- ``read_single_tree_ctrees``, ``src/io/parse_ctrees.h`` lines 429-515:
  rows run from the tree's offset until a line beginning with ``#`` or the end of the
  file; an offset at a ``#`` line or at the end gives no rows; a final line without a
  newline is not parsed.
* ``parse_rows`` -- ``parse_line_ctrees``, ``src/io/parse_ctrees.h`` lines 323-426, its
  number parsing at lines 389-418: ``strtod`` for float64, ``strtoll`` for int64,
  ``(int32_t) strtol`` for int32 and ``strtof`` for float32. Rows are parsed with
  ``numpy.loadtxt``, whose float parse is correctly rounded (CPython's ``dtoa``), so the
  float64 columns equal ``strtod``. A float32 column is parsed to float64 and narrowed;
  that rounds twice, which can differ from ``strtof``'s single correctly rounded
  conversion only when the float64 lands exactly on a float32 rounding boundary (a
  midpoint between two adjacent float32 values, or float32's overflow threshold). Those
  entries are found vectorised; where the decimal text equals the boundary exactly
  (``decimal.Decimal``; common, e.g. ``3.27000e+09`` is a float32 midpoint) the cast's
  tie to even is ``strtof``'s, and otherwise the text is rounded once, exactly, with
  ``fractions.Fraction`` (``strtof_exact``: nearest, ties to even). So every float32
  equals ``strtof``'s bits.

Converting one forest (``convert_forest``; ``load_forest_ctrees``,
``src/io/read_tree_consistentrees_ascii.c`` lines 325-424):

* ``_convert_conventions`` -- ``convert_ctrees_conventions_to_lht``,
  ``read_tree_consistentrees_ascii.c`` lines 426-463, called per ASCII tree at line 372:
  ``Spin`` = float32(``J`` x (1.0 / unscaled ``Mvir``)) in double; ``Mvir``, ``M_Mean200``
  (``M200b``), ``M_TopHat`` (``M200c``) = float32(mass x ``1e-10``) in double;
  ``Len = (int) roundf(Mvir * (1.0 / PartMass))`` (line 441): the float32 scaled ``Mvir``
  times the double reciprocal gives a double, narrowed to float32 by the call to
  ``roundf``, which rounds half away from zero; ``FileNr = -1``; ``SubhaloIndex = (int)
  (prev_N + nhalos)``, the forest's row count before the tree plus the tree's own, so
  constant within a tree; ``SubHalfMass = -1.0f``; ``SnapNum = snap_num + 0``; every link
  -1. The record (``RECORD_DTYPE``, 104 bytes) is ``struct halo_data``,
  ``src/core_simulation.h``, which is the layout of Mimic's
  ``simulations/mini-uchuu/halo_properties.yaml`` (``Mvir`` is Mimic's ``M_Crit200``).
* ``_check_flybys`` -- ``fix_flybys``, ``src/io/ctrees_utils.c`` lines 297-381 (called at
  ``read_tree_consistentrees_ascii.c`` line 398), without its writes (difference 1).
* ``_fix_upid`` -- ``fix_upid``, ``src/io/ctrees_utils.c`` lines 384-465, with
  ``find_fof_halo``, lines 679-735 (called at ``read_tree_consistentrees_ascii.c`` line
  407): every FoF's ``upid`` becomes its own ``id``, and every other halo's ``upid`` and
  ``pid`` become the id of the FoF reached by following ``upid`` from host to host; the
  largest ``SnapNum`` must be non-negative.
* ``_assign_mergertree_indices`` -- ``assign_mergertree_indices``,
  ``src/io/ctrees_utils.c`` lines 467-676 (called at ``read_tree_consistentrees_ascii.c``
  line 413): the forest sorted by (``scale`` descending, ``upid``, ``pid``, ``id``) is the
  output order (lines 486-506); FoF links (lines 508-578); a descendant's snapshot is the
  highest whose scale (that of the snapshot's last halo in the sorted order) lies within
  ``1e-4`` of ``desc_scale`` and its index the halo with ``desc_id`` among that snapshot's
  sorted range (lines 581-618); progenitor chains (lines 621-662): the first progenitor
  met opens the chain, and a later one becomes ``FirstProgenitor`` only when strictly
  more massive (float32 scaled ``Mvir``) than the current first, else it is appended, so
  the chain is not sorted by mass (masses 1, 3, 2 met in that order give 3, 1, 2).

The two differences from sage-model
-----------------------------------
1. In ``fix_flybys`` every operation is kept -- its sort of the forest by (``scale``
   descending, ``id``), its search for the last scale and for the most massive FoF there
   (the survivor), and its refusals when the last scale holds no FoF or no survivor is
   found -- except the writes that alter data: the negation of ``MostBoundID`` and the
   setting of ``pid`` to the survivor's id for the other FoFs at the last scale (lines
   370-371), and the setting of ``upid`` to the survivor's id for **every** other halo at
   the last scale, subhalos of other FoF groups included (line 377). Several FoF
   centrals at a forest's last snapshot therefore stay centrals with positive
   ``MostBoundID`` and their own FoF groups.
2. Each halo's ``MostBoundID`` is its own row's ``id``. sage-model passes the forest's
   first row as ``info`` for every ASCII tree (``read_tree_consistentrees_ascii.c``
   lines 357-372, used at line 449), so halo *j* of a later tree got the id of the
   forest's *j*-th row, *j* the halo's position within its own ASCII tree.

Writing the output files (``main``; ``convert_trees_to_lhalo``, ``src/sage.c`` lines
358-528, with the forest split of ``setup_forests_io_ctrees``,
``read_tree_consistentrees_ascii.c`` lines 64-124):

* ``file_forest_range`` -- lines 83-101 and 107-124 of ``read_tree_consistentrees_ascii.c``:
  with F forests and N output files (sage-model's ``NTasks``), file *t* holds the
  contiguous forests ``floor(F/N) * t + min(t, F mod N)`` onwards, ``floor(F/N)`` of them
  plus one when ``t < F mod N``. A task with no forests is refused there (lines 121-124),
  so sage-model's run fails whenever N > F; this module refuses such a run whole (exit 2),
  which is stricter than sage-model, whose tasks below F would still have written files.
* ``check_int32_count`` -- ``PWRITE_64BIT_TO_32BIT``, ``src/sage.c`` lines 421-444 (and
  the forest-count check of lines 400-404): a count above ``INT_MAX`` is refused.
* ``write_lhalo_forests`` -- ``src/sage.c`` lines 446-508: ``int32 Ntrees`` (the file's
  forest count), ``int32 totNHalos``, ``int32 TreeNHalos[Ntrees]`` (each forest's halo
  count), then each forest's records in turn, little-endian; the file is named
  ``<tree-name>.<t>`` (``"%s%s.%d"``, line 447). The header line of the tree files is that
  of ``tree_0_0_0.dat`` beside ``locations.dat`` (``SimulationDir/TreeName``, the file
  ``get_forests_filename_ctr_ascii`` names; sage-model's ``TreeName`` run parameter, fixed
  here at ``tree_0_0_0.dat``).

The command line (``main``) writes output files A to B of N, each under a temporary name
unique to it, renamed when complete, with a JSON sidecar; it refuses the whole range
before writing when any output or sidecar exists. ``CTREES-TO-LHALO.md`` documents it::

    ctrees_to_lhalo.py --forests-list PATH --locations PATH --n-files N --files A-B
        --particle-mass X --tree-name NAME --out DIR

Exit status 0 on success, 2 on a read, parse, consistency or count error (naming the
file), 3 on a usage error or a refused overwrite.

Particle mass from ``Len`` (``particle_mass_interval``). ``PartMass`` enters only ``Len``
(line 441), so the masses that reproduce the ``Len`` of a set of written records form an
interval of doubles, not a point. ``particle_mass_interval`` finds its ends exactly from
the records' ``Mvir`` and ``Len`` (bisection over double bit patterns, under the sequence
above: float32 ``Mvir`` times the double ``1.0 / PartMass``, narrowed to float32, rounded
half away from zero), and ``shortest_decimal`` names the decimal with the fewest
significant digits inside it (nearest the midpoint among those).

Order independence. sage-model's sorts are heap sorts over total orders (ids are unique
within a forest; this module refuses a forest with a repeated id), so the output order is
the final sort's whatever the intermediate orders, and the sorts are done with
``numpy.lexsort``. ``fix_upid`` resolves halos in its sorted order and rewrites
``upid``/``pid`` as it goes; the FoF reached from any halo is the same whether a host on
the way was already rewritten or not, so the hosts are resolved here all at once by id.

Inputs. Consistent-Trees ASCII forests as staged. Where sage-model's behaviour on an
input is undefined, would never terminate, or depends on how its unstable sorts order
equal keys (an empty forest; repeated tree or halo ids; a requested column missing from
the header; a blank, over-long or non-numeric row; a tab, comma, carriage return or
form feed in the rows; a host chain that loops without reaching a FoF; a descendant
snapshot holding no halo), this module refuses with ``CtreesToLhaloError`` instead.
One family of cases goes the other way. sage-model's ``fix_upid`` finds a subhalo's FoF
with ``find_fof_halo`` (``src/io/ctrees_utils.c`` lines 679-735), which looks for each
host in one direction only of the forest sorted by (``scale`` descending, ``id``) --
forward when the host's id exceeds the current halo's, else backward (lines 702-716) --
and returns -1 when that search misses the host (a host at another scale can lie the
other way) or the chain is deeper than its recursion cap of 30 (lines 688-694; as
``fix_upid`` meets the chain, since it rewrites each resolved halo's ``upid`` in its
sorted order, which can shorten a later chain). On -1, ``fix_upid``'s fallback loop
(lines 422-443) succeeds only when the halo's own ``upid`` names a FoF; otherwise it
restarts from that ``upid`` on every pass and never ends. So wherever ``find_fof_halo``
returns -1 and the halo's ``upid`` is not a FoF, sage-model never terminates, while this
module follows the chain by id and returns records. This does not arise in the staged
trees: Consistent-Trees' ``upid`` names the ultimate (most massive) host, a FoF, so the
fallback always succeeds there. Behaviour on other inputs is unspecified.

Every refusal raises ``CtreesToLhaloError``, which names the file (or forest) and
carries ``exit_status`` 2. The module never uses ``multiprocessing``.
"""

from __future__ import annotations

import argparse
import functools
import hashlib
import io
import json
import math
import mmap
import os
import re
import sys
import tempfile
import warnings
from dataclasses import dataclass
from datetime import datetime, timezone
from decimal import Decimal
from fractions import Fraction

import numpy as np

__all__ = [
    "SAGE_MODEL_COMMIT",
    "EXIT_STATUS_DATA",
    "RECORD_DTYPE",
    "ROW_DTYPE",
    "CtreesToLhaloError",
    "ColumnMap",
    "ForestTable",
    "TreeFiles",
    "parse_header_line",
    "read_column_map",
    "column_map_from_header",
    "read_forests_list",
    "read_locations",
    "pair_forests",
    "read_forest_table",
    "tree_text",
    "parse_rows",
    "strtof_exact",
    "read_forests",
    "read_forest",
    "forest_source",
    "convert_forest",
    "EXIT_STATUS_OK",
    "EXIT_STATUS_USAGE",
    "HEADER_TREE_FILE",
    "header_tree_file",
    "check_int32_count",
    "file_forest_range",
    "write_lhalo_forests",
    "sage_len",
    "count_reproduced",
    "shortest_decimal",
    "ParticleMassInterval",
    "particle_mass_interval",
    "read_lhalo_mvir_and_len",
    "particle_mass_interval_of_files",
    "main",
]

SAGE_MODEL_COMMIT = "9218e11"
EXIT_STATUS_OK = 0
EXIT_STATUS_DATA = 2
EXIT_STATUS_USAGE = 3
INT_MAX = 2**31 - 1
# The tree file whose first line gives the column map: sage-model's SimulationDir/TreeName run
# parameter (read_tree_consistentrees_ascii.c lines 21-25), fixed here at tree_0_0_0.dat.
HEADER_TREE_FILE = "tree_0_0_0.dat"
# Forests parsed together in one batch when writing a file (memory, not semantics).
_FORESTS_PER_BATCH = 4096
DBL_MAX = float(np.finfo(np.float64).max)

# fgets(linebuf, PARSE_CTREES_MAXBUFSIZE=1240) and pread(.., 1239, ..): parse_ctrees.h
# lines 31, 188-189 and 442-449. A row longer than this (newline included) never fits
# sage-model's read window, which then re-reads the same bytes forever.
_MAX_LINE_BYTES = 1239
# PARSE_CTREES_MAX_COLNAME_LEN, parse_ctrees.h line 28 (token length checked at 235).
_MAX_COLNAME_LEN = 64
# numfiles_allocated, ctrees_utils.c line 89.
_INITIAL_FILE_TABLE = 2000
# max_epsilon_scale, ctrees_utils.c line 590.
_MAX_EPSILON_SCALE = 1.0e-4
# Bytes that would make a whitespace split differ from sage-model's " ," strsep.
_FORBIDDEN_ROW_BYTES = (b"\t", b",", b"\r", b"\x0b", b"\x0c", b"\x00")

RECORD_DTYPE = np.dtype(
    [
        ("Descendant", "<i4"),
        ("FirstProgenitor", "<i4"),
        ("NextProgenitor", "<i4"),
        ("FirstHaloInFOFgroup", "<i4"),
        ("NextHaloInFOFgroup", "<i4"),
        ("Len", "<i4"),
        ("M_Mean200", "<f4"),
        ("Mvir", "<f4"),
        ("M_TopHat", "<f4"),
        ("Pos", "<f4", (3,)),
        ("Vel", "<f4", (3,)),
        ("VelDisp", "<f4"),
        ("Vmax", "<f4"),
        ("Spin", "<f4", (3,)),
        ("MostBoundID", "<i8"),
        ("SnapNum", "<i4"),
        ("FileNr", "<i4"),
        ("SubhaloIndex", "<i4"),
        ("SubHalfMass", "<f4"),
    ]
)
assert RECORD_DTYPE.itemsize == 104

# The requested columns in sage-model's order, read_tree_consistentrees_ascii.c lines
# 242-292: (header name, row field, row type). snap_num and snap_idx share SnapNum.
_REQUESTED = (
    (b"scale", "scale", "<f8"),
    (b"id", "id", "<i8"),
    (b"desc_scale", "desc_scale", "<f8"),
    (b"desc_id", "desc_id", "<i8"),
    (b"pid", "pid", "<i8"),
    (b"upid", "upid", "<i8"),
    (b"mvir", "mvir", "<f4"),
    (b"vrms", "vrms", "<f4"),
    (b"vmax", "vmax", "<f4"),
    (b"x", "x", "<f4"),
    (b"y", "y", "<f4"),
    (b"z", "z", "<f4"),
    (b"vx", "vx", "<f4"),
    (b"vy", "vy", "<f4"),
    (b"vz", "vz", "<f4"),
    (b"Jx", "Jx", "<f4"),
    (b"Jy", "Jy", "<f4"),
    (b"Jz", "Jz", "<f4"),
    (b"snap_num", "snap_num", "<i4"),
    (b"snap_idx", "snap_num", "<i4"),
    (b"M200b", "M200b", "<f4"),
    (b"M200c", "M200c", "<f4"),
)

# One forest's Consistent-Trees rows, with sage-model's types.
ROW_DTYPE = np.dtype(list(dict((field, kind) for _, field, kind in _REQUESTED).items()))


class CtreesToLhaloError(Exception):
    """A refusal of sage-model's conversion: names its file or forest; exit status 2."""

    exit_status = EXIT_STATUS_DATA

    def __init__(self, source, message: str):
        self.source = str(source)
        self.message = message
        super().__init__(f"{self.source}: {message}")


# --------------------------------------------------------------------------------------
# Header and column map
# --------------------------------------------------------------------------------------


@dataclass(frozen=True)
class ColumnMap:
    """The data columns to parse: (column number, row field, row type), ascending.

    ``source`` is the tree file whose header gave the map.
    """

    columns: tuple
    source: str


def parse_header_line(line: bytes, source) -> list:
    """Column names of a Consistent-Trees header line, as ``parse_header_ctrees`` reads them.

    ``line`` is the line as ``fgets`` returns it (at most 1239 bytes, newline included
    when it fits); ``parse_ctrees.h`` lines 189-272.
    """
    if not line:
        raise CtreesToLhaloError(source, "could not read the first line (the header)")
    if line[:1] != b"#":
        raise CtreesToLhaloError(source, f"the header's first character is {line[:1]!r}, not '#'")
    # strsep(" ,") counts every token, empty ones included (lines 207-214).
    totncols = line.count(b" ") + line.count(b",") + 1
    names = []
    for token in line.replace(b",", b" ").replace(b"\n", b" ").replace(b"#", b" ").split(b" "):
        if not token:
            continue
        if len(token) >= _MAX_COLNAME_LEN:
            raise CtreesToLhaloError(
                source, f"header token {token!r} is not shorter than {_MAX_COLNAME_LEN} bytes"
            )
        names.append(token.split(b"(", 1)[0])
    if len(names) != totncols:
        raise CtreesToLhaloError(
            source,
            f"the header has {totncols} columns by its separators but {len(names)} names",
        )
    return names


def read_column_map(tree_file) -> ColumnMap:
    """The column map from the first line of ``tree_file`` (``parse_header_ctrees``).

    A requested column the header lacks is refused (sage-model would leave the field
    uninitialised); at least one of ``snap_num`` and ``snap_idx`` is needed.
    """
    source = os.fspath(tree_file)
    try:
        with open(source, "rb") as handle:
            line = handle.readline(_MAX_LINE_BYTES)
    except OSError as exc:
        raise CtreesToLhaloError(source, f"could not open the file ({exc.strerror})") from None
    return column_map_from_header(line, source)


def column_map_from_header(line: bytes, source) -> ColumnMap:
    """The column map of a header line as ``fgets`` reads it (``parse_header_ctrees``)."""
    names = [name.lower() for name in parse_header_line(line, source)]
    chosen = {}
    for wanted, field, kind in _REQUESTED:
        wanted_lower = wanted.lower()
        column = next((j for j, name in enumerate(names) if name == wanted_lower), None)
        if column is None:
            continue
        # Columns are parsed in ascending order, so for a shared field the later wins.
        if field not in chosen or column > chosen[field][0]:
            chosen[field] = (column, field, kind)
    missing = [field for field in ROW_DTYPE.names if field not in chosen]
    if missing:
        raise CtreesToLhaloError(source, f"the header lacks the columns {missing}")
    return ColumnMap(columns=tuple(sorted(chosen.values())), source=source)


# --------------------------------------------------------------------------------------
# forests.list and locations.dat
# --------------------------------------------------------------------------------------


def _read_bytes(path) -> bytes:
    try:
        with open(path, "rb") as handle:
            return handle.read()
    except OSError as exc:
        raise CtreesToLhaloError(path, f"can't open the file ({exc.strerror})") from None


def _data_region(raw: bytes):
    """Where the data lines of ``raw`` begin: ``(buffer, skiprows, start)``.

    Lines whose first byte is ``#`` are not data (the files' comment rule). Leading
    comment lines are skipped in place, without copying ``raw``; a comment line further
    in (never seen in the staged tables) makes a filtered copy, returned with
    ``skiprows`` and ``start`` 0.
    """
    start = skip = 0
    while raw.startswith(b"#", start):
        newline = raw.find(b"\n", start)
        start = len(raw) if newline < 0 else newline + 1
        skip += 1
    if raw.find(b"\n#", start) >= 0:
        lines = raw[start:].splitlines(True)
        return b"".join(line for line in lines if not line.startswith(b"#")), 0, 0
    return raw, skip, start


def _count_lines(buffer: bytes, start: int) -> int:
    tail = 1 if len(buffer) > start and not buffer.endswith(b"\n") else 0
    return buffer.count(b"\n", start) + tail


def _lines_at(buffer: bytes, start: int, wanted) -> dict:
    """The bytes of data lines ``wanted`` (numbered from 0 at byte ``start``), newline cut.

    Scans for newlines in 64 MiB chunks so a large table is never expanded in memory.
    """
    wanted = sorted(set(int(k) for k in wanted))
    found = {}
    chunk = 1 << 26
    line, line_start, position, w = 0, start, start, 0
    while w < len(wanted) and position < len(buffer):
        end = min(position + chunk, len(buffer))
        view = np.frombuffer(buffer, dtype=np.uint8, count=end - position, offset=position)
        newlines = position + np.flatnonzero(view == ord("\n"))
        while w < len(wanted) and wanted[w] < line + newlines.size:
            j = wanted[w] - line
            begin = line_start if j == 0 else int(newlines[j - 1]) + 1
            found[wanted[w]] = buffer[begin : int(newlines[j])]
            w += 1
        if newlines.size:
            line_start = int(newlines[-1]) + 1
        line += newlines.size
        position = end
    while w < len(wanted) and wanted[w] == line:
        found[wanted[w]] = buffer[line_start:]
        w += 1
    return found


def _loadtxt(buffer: bytes, dtype, usecols, source, what: str, skiprows: int = 0):
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            return np.loadtxt(
                io.BytesIO(buffer),
                dtype=dtype,
                usecols=usecols,
                comments=None,
                ndmin=1,
                skiprows=skiprows,
            )
    except (ValueError, UserWarning) as exc:
        raise CtreesToLhaloError(source, f"could not parse {what}: {exc}") from None


def _read_table(path, dtype, usecols, what: str):
    """A table's data lines parsed by ``usecols``, and ``(buffer, start)`` of its lines."""
    source = os.fspath(path)
    buffer, skip, start = _data_region(_read_bytes(source))
    nlines = _count_lines(buffer, start)
    if nlines == 0:
        return np.zeros(0, dtype=dtype), buffer, start
    table = _loadtxt(buffer, dtype, usecols, source, what, skip)
    if len(table) != nlines:
        raise CtreesToLhaloError(source, "a data line is blank")
    return table, buffer, start


def read_forests_list(path):
    """``(tree_root_id, forest_id)`` from ``forests.list`` (``read_forests``)."""
    dtype = np.dtype([("tree", "<i8"), ("forest", "<i8")])
    table, _, _ = _read_table(path, dtype, (0, 1), "a 'TreeRootID ForestID' line")
    return table["tree"].copy(), table["forest"].copy()


def _locations_dirname(path: str) -> str:
    """The directory part of ``path`` as ``read_locations`` cuts it (lines 77-84)."""
    cut = path.rfind("/")
    return path if cut < 0 else path[:cut]


def _check_file_table_growth(file_id: np.ndarray, source) -> None:
    """sage-model's file-descriptor table (``read_locations``, lines 128-151).

    The table starts at 2000 entries. A line whose file id exceeds the current size
    doubles it, once for that line; the id must then be below the size. So an id equal to
    the current size is refused, an id at least twice it is refused, and the table can
    double again on a later line with a larger id.
    """
    allocated = _INITIAL_FILE_TABLE
    start = 0
    while True:
        beyond = np.flatnonzero(file_id[start:] >= allocated)
        if beyond.size == 0:
            return
        line = start + int(beyond[0])
        fid = int(file_id[line])
        if fid > allocated:
            allocated *= 2
        if fid >= allocated:
            raise CtreesToLhaloError(
                source,
                f"line {line}: file id {fid} is beyond sage-model's file table of "
                f"{allocated} entries",
            )
        start = line + 1


def read_locations(path, ntrees: int):
    """``(tree_root_id, file_id, offset, tree_files)`` from ``locations.dat``.

    ``tree_files[f]`` is the path of file id ``f``: the directory of ``path`` joined with
    the ``Filename`` on the first line naming ``f`` (``read_locations``).
    """
    source = os.fspath(path)
    dtype = np.dtype([("tree", "<i8"), ("file", "<i4"), ("offset", "<i8"), ("name", "S1")])
    table, buffer, start = _read_table(
        source, dtype, (0, 1, 2, 3), "a 'TreeRootID FileID Offset Filename' line"
    )
    if len(table) != ntrees:
        raise CtreesToLhaloError(source, f"{len(table)} trees, but forests.list holds {ntrees}")
    tree, file_id, offset = table["tree"].copy(), table["file"].copy(), table["offset"].copy()
    del table
    for name, values in (("offset", offset), ("file id", file_id)):
        bad = np.flatnonzero(values < 0)
        if bad.size:
            raise CtreesToLhaloError(
                source, f"line {int(bad[0])}: {name} {int(values[bad[0]])} is negative"
            )
    _check_file_table_growth(file_id, source)

    unique_ids, first_line = np.unique(file_id, return_index=True)
    lines = _lines_at(buffer, start, first_line)
    del buffer
    dirname = _locations_dirname(source)
    tree_files = {}
    for fid, line in zip(unique_ids.tolist(), first_line.tolist(), strict=True):
        filename = lines[line].split()[3].decode("utf-8", "surrogateescape")
        tree_path = f"{dirname}/{filename}"
        try:
            os.close(os.open(tree_path, os.O_RDONLY))
        except OSError as exc:
            raise CtreesToLhaloError(
                tree_path, f"could not open the tree file ({exc.strerror})"
            ) from None
        tree_files[fid] = tree_path

    numfiles = len(tree_files)
    max_fileid = int(unique_ids[-1]) if numfiles else 0
    if max_fileid + 1 != numfiles:
        raise CtreesToLhaloError(
            source,
            f"file ids run to {max_fileid} but {numfiles} files are named; "
            "the file ids are not contiguous from 0",
        )
    cube_root = float(np.cbrt(float(numfiles)))
    box_divisions = int(np.floor(cube_root + 0.5))  # C round(): half away from zero
    if box_divisions**3 != numfiles:
        raise CtreesToLhaloError(source, f"{numfiles} tree files is not a perfect cube")
    return tree, file_id, offset, tuple(tree_files[f] for f in range(numfiles))


@dataclass(frozen=True)
class ForestTable:
    """The trees in (ForestID, FileID, Offset) order, grouped into forests.

    Forest ``k`` is trees ``forest_start[k]`` to ``forest_start[k + 1] - 1``.
    """

    tree_root_id: np.ndarray
    forest_id: np.ndarray
    file_id: np.ndarray
    offset: np.ndarray
    forest_start: np.ndarray
    tree_files: tuple

    @property
    def nforests(self) -> int:
        return len(self.forest_start) - 1

    def trees(self, forest: int) -> slice:
        return slice(int(self.forest_start[forest]), int(self.forest_start[forest + 1]))


def _check_forest_count(nforests: int, source) -> None:
    """sage-model requires fewer than ``INT_MAX`` forests (read_tree_consistentrees_ascii.c
    lines 76-79)."""
    if nforests >= INT_MAX:
        raise CtreesToLhaloError(
            source, f"{nforests} forests cannot be counted in a 32-bit integer"
        )


def pair_forests(
    forests_tree, forests_forest, locations_tree, file_id, offset, tree_files, sources
) -> ForestTable:
    """Pair the tables by tree id and group the trees into forests (``assign_forest_ids``)."""
    forests_source, locations_source = sources
    by_tree_f = np.argsort(forests_tree, kind="stable")
    by_tree_l = np.argsort(locations_tree, kind="stable")
    sorted_f = forests_tree[by_tree_f]
    sorted_l = locations_tree[by_tree_l]
    disagree = np.flatnonzero(sorted_f != sorted_l)
    if disagree.size:
        i = int(disagree[0])
        raise CtreesToLhaloError(
            locations_source,
            f"tree roots[{i}] = {int(sorted_f[i])} in {forests_source} does not equal "
            f"the tree root {int(sorted_l[i])} in locations",
        )
    repeated = np.flatnonzero(sorted_f[1:] == sorted_f[:-1])
    if repeated.size:
        raise CtreesToLhaloError(
            forests_source, f"tree id {int(sorted_f[repeated[0]])} appears more than once"
        )
    forest = np.empty_like(forests_forest)
    forest[by_tree_l] = forests_forest[by_tree_f]
    order = np.lexsort((offset, file_id, forest))
    tree, forest, file_id, offset = (
        locations_tree[order],
        forest[order],
        file_id[order],
        offset[order],
    )
    previous = np.concatenate(([-1], forest[:-1]))
    starts = np.flatnonzero(forest != previous)
    _check_forest_count(starts.size, forests_source)
    first = int(starts[0]) if starts.size else len(forest)
    keep = slice(first, None)
    return ForestTable(
        tree_root_id=tree[keep],
        forest_id=forest[keep],
        file_id=file_id[keep],
        offset=offset[keep],
        forest_start=np.concatenate((starts - first, [len(forest) - first])).astype(np.int64),
        tree_files=tuple(tree_files),
    )


def read_forest_table(forests_list, locations) -> ForestTable:
    """Read and pair ``forests.list`` and ``locations.dat`` (``setup_forests_io_ctrees``)."""
    forests_tree, forests_forest = read_forests_list(forests_list)
    locations_tree, file_id, offset, tree_files = read_locations(locations, len(forests_tree))
    return pair_forests(
        forests_tree,
        forests_forest,
        locations_tree,
        file_id,
        offset,
        tree_files,
        (os.fspath(forests_list), os.fspath(locations)),
    )


# --------------------------------------------------------------------------------------
# Tree rows
# --------------------------------------------------------------------------------------


def tree_text(buffer, offset: int) -> bytes:
    """The rows of the tree at ``offset`` of ``buffer`` (``read_single_tree_ctrees``).

    From ``offset`` up to a line beginning with ``#`` or the end of the file; an
    unterminated final line is dropped, as sage-model parses a line only at its newline.
    """
    if offset >= len(buffer) or buffer[offset : offset + 1] == b"#":
        return b""
    stop = buffer.find(b"\n#", offset)
    text = buffer[offset : len(buffer) if stop < 0 else stop + 1]
    return text[: text.rfind(b"\n") + 1]


class TreeFiles:
    """Read-only memory maps of the tree files, opened on first use."""

    def __init__(self, tree_files):
        self.tree_files = tuple(tree_files)
        self._maps = {}

    def buffer(self, file_id: int):
        mapped = self._maps.get(file_id)
        if mapped is None:
            path = self.tree_files[file_id]
            try:
                with open(path, "rb") as handle:
                    if os.fstat(handle.fileno()).st_size == 0:
                        mapped = b""
                    else:
                        mapped = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
            except OSError as exc:
                raise CtreesToLhaloError(path, f"could not read ({exc.strerror})") from None
            self._maps[file_id] = mapped
        return mapped

    def tree_text(self, file_id: int, offset: int) -> bytes:
        return tree_text(self.buffer(file_id), offset)

    def close(self) -> None:
        for mapped in self._maps.values():
            if isinstance(mapped, mmap.mmap):
                mapped.close()
        self._maps.clear()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def strtof_exact(token: bytes) -> np.float32:
    """``strtof`` of a decimal token: its value rounded once to float32, ties to even."""
    exact = Fraction(token.decode("ascii"))
    with np.errstate(over="ignore"):
        nearest = np.float32(float(token))
        candidates = {
            nearest,
            np.nextafter(nearest, np.float32(-np.inf)),
            np.nextafter(nearest, np.float32(np.inf)),
        }

    def value(candidate):
        if np.isinf(candidate):
            # float32 overflows to infinity from 2**128 - 2**103 on: round as if 2**128.
            return Fraction(2**128) if candidate > 0 else -Fraction(2**128)
        return Fraction(float(candidate))

    def key(candidate):
        return (abs(value(candidate) - exact), int(candidate.view(np.uint32)) & 1)

    best = np.float32(min(candidates, key=key))
    if best == 0:
        best = np.float32(-0.0) if exact < 0 or token.lstrip().startswith(b"-") else best
    return best


def _narrow_like_strtof(values: np.ndarray, token_of) -> np.ndarray:
    """float32 of correctly rounded float64 ``values`` (any shape), as ``strtof`` rounds.

    The float64-to-float32 cast agrees with ``strtof`` except where the float64 is
    exactly a float32 rounding boundary: a midpoint between adjacent float32 values, or
    the overflow threshold. There the decimal is compared exactly with the boundary
    (``decimal.Decimal``); when it equals the boundary the cast's tie to even is
    ``strtof``'s too, and otherwise (double rounding) ``strtof_exact`` rounds it once.
    ``token_of(index)`` gives the decimal text of the entry at ``index``.
    """
    with np.errstate(invalid="ignore", over="ignore"):
        narrowed = values.astype(np.float32)
        widened = narrowed.astype(np.float64)
        toward = np.where(values > widened, np.float32(np.inf), np.float32(-np.inf))
        neighbour = np.nextafter(narrowed, toward.astype(np.float32))
        midpoint = (widened + neighbour.astype(np.float64)) * 0.5
        on_midpoint = (widened != values) & (values == midpoint)
        overflowed = np.isinf(narrowed) & np.isfinite(values)
    for index in zip(*np.nonzero(on_midpoint), strict=True):
        token = token_of(index)
        if Decimal(token.decode("ascii")) != Decimal(float(values[index])):
            narrowed[index] = strtof_exact(token)
    for index in zip(*np.nonzero(overflowed), strict=True):
        narrowed[index] = strtof_exact(token_of(index))
    return narrowed


@functools.lru_cache(maxsize=8)
def _parse_layout(column_map: ColumnMap):
    """The ``loadtxt`` dtype and columns for a map: one subarray field per parse type.

    float32 fields are parsed as float64 (then narrowed as ``strtof`` would), so the
    float32 columns form one block narrowed in a single pass.
    """
    groups = {}
    for column, field, kind in column_map.columns:
        parse_kind = "<f8" if kind == "<f4" else kind
        key = "f4" if kind == "<f4" else parse_kind
        groups.setdefault(key, (parse_kind, [], []))
        groups[key][1].append(column)
        groups[key][2].append(field)
    dtype = np.dtype([(key, kind, (len(cols),)) for key, (kind, cols, _) in groups.items()])
    usecols = tuple(c for _, cols, _ in groups.values() for c in cols)
    layout = {key: (tuple(cols), tuple(fields)) for key, (_, cols, fields) in groups.items()}
    return dtype, usecols, layout


def parse_rows(text: bytes, column_map: ColumnMap, source=None) -> np.ndarray:
    """Parse tree rows (``parse_line_ctrees``) into a ``ROW_DTYPE`` array."""
    source = column_map.source if source is None else source
    if not text:
        return np.zeros(0, dtype=ROW_DTYPE)
    for forbidden in _FORBIDDEN_ROW_BYTES:
        if forbidden in text:
            raise CtreesToLhaloError(source, f"a row holds the byte {forbidden!r}")
    newlines = np.flatnonzero(np.frombuffer(text, dtype=np.uint8) == ord("\n"))
    if newlines.size == 0 or newlines[-1] != len(text) - 1:
        raise CtreesToLhaloError(source, "the rows do not end with a newline")
    longest = int(np.diff(newlines, prepend=-1).max())
    if longest > _MAX_LINE_BYTES:
        raise CtreesToLhaloError(
            source, f"a row of {longest} bytes exceeds sage-model's {_MAX_LINE_BYTES}"
        )
    dtype, usecols, layout = _parse_layout(column_map)
    parsed = _loadtxt(text, dtype, usecols, source, "a row")
    if len(parsed) != newlines.size:
        raise CtreesToLhaloError(source, "a row is blank")

    rows = np.empty(len(parsed), dtype=ROW_DTYPE)
    lines = []
    for key, (cols, fields) in layout.items():
        block = parsed[key]
        if key == "f4":

            def token_of(index, cols=cols):
                if not lines:
                    lines.extend(text.split(b"\n"))
                row, j = index
                return lines[row].split()[cols[j]]

            block = _narrow_like_strtof(block, token_of)
        for j, field in enumerate(fields):
            rows[field] = block[:, j]
    return rows


def read_forests(table: ForestTable, forests, files: TreeFiles, column_map: ColumnMap):
    """Rows of the forests numbered ``forests``, parsed together, in sage-model's order.

    Each forest's ASCII trees in (ForestID, FileID, Offset) order (``load_forest_ctrees``,
    lines 357-373), each tree's rows in file order. Returns ``(rows, tree_nhalos,
    forest_nhalos)``: the rows of the forests in turn, each of their trees' row counts,
    and each forest's row count. An empty batch gives three empty arrays.
    """
    forests = np.atleast_1d(np.asarray(forests, dtype=np.int64))
    if forests.size == 0:
        empty = np.zeros(0, dtype=np.int64)
        return np.zeros(0, dtype=ROW_DTYPE), empty, empty.copy()
    starts = table.forest_start[forests]
    ntrees = table.forest_start[forests + 1] - starts
    first_tree = np.cumsum(ntrees) - ntrees
    trees = np.repeat(starts - first_tree, ntrees) + np.arange(int(ntrees.sum()))
    texts = [
        files.tree_text(int(f), int(o))
        for f, o in zip(table.file_id[trees], table.offset[trees], strict=True)
    ]
    tree_nhalos = np.array([t.count(b"\n") for t in texts], dtype=np.int64)
    forest_nhalos = np.add.reduceat(tree_nhalos, first_tree)
    try:
        rows = parse_rows(b"".join(texts), column_map, forest_source(table, int(forests[0])))
    except CtreesToLhaloError:
        if forests.size == 1:
            raise
        for forest in forests.tolist():  # name the forest that fails
            read_forests(table, [forest], files, column_map)
        raise
    return rows, tree_nhalos, forest_nhalos


def forest_source(table: ForestTable, forest: int) -> str:
    """How refusals name forest ``forest``: its first tree's file, number and ForestID."""
    tree = int(table.forest_start[forest])
    path = table.tree_files[int(table.file_id[tree])]
    return f"{path} (forest {forest}, ForestID {int(table.forest_id[tree])})"


def read_forest(table: ForestTable, forest: int, files: TreeFiles, column_map: ColumnMap):
    """One forest's rows and per-tree row counts, in sage-model's read order."""
    rows, tree_nhalos, _ = read_forests(table, [forest], files, column_map)
    return rows, tree_nhalos


# --------------------------------------------------------------------------------------
# The forest conversion
# --------------------------------------------------------------------------------------


def _roundf(values: np.ndarray) -> np.ndarray:
    """C ``roundf`` on float32: nearest integer, halves away from zero."""
    with np.errstate(invalid="ignore"):
        whole = np.trunc(values)
        fraction = values - whole  # exact in float32
        step = np.where(np.abs(fraction) >= np.float32(0.5), np.sign(values), np.float32(0))
    return whole + step.astype(np.float32)


def _convert_conventions(rows: np.ndarray, tree_nhalos: np.ndarray, part_mass: float):
    """``convert_ctrees_conventions_to_lht`` (lines 426-463), MostBoundID from own row."""
    n = len(rows)
    records = np.empty(n, dtype=RECORD_DTYPE)
    with np.errstate(divide="ignore", invalid="ignore", over="ignore"):
        inv_halo_mass = 1.0 / rows["mvir"].astype(np.float64)
        for k, column in enumerate(("Jx", "Jy", "Jz")):
            records["Spin"][:, k] = (rows[column].astype(np.float64) * inv_halo_mass).astype(
                np.float32
            )
        mvir = (rows["mvir"].astype(np.float64) * 1e-10).astype(np.float32)
        records["Mvir"] = mvir
        records["M_Mean200"] = (rows["M200b"].astype(np.float64) * 1e-10).astype(np.float32)
        records["M_TopHat"] = (rows["M200c"].astype(np.float64) * 1e-10).astype(np.float32)
        inv_part_mass = 1.0 / float(part_mass)
        narrowed = (mvir.astype(np.float64) * inv_part_mass).astype(np.float32)
        records["Len"] = _roundf(narrowed).astype(np.int32)
    records["FileNr"] = -1
    records["SubhaloIndex"] = np.repeat(np.cumsum(tree_nhalos), tree_nhalos).astype(np.int32)
    records["SubHalfMass"] = np.float32(-1.0)
    records["MostBoundID"] = rows["id"]  # difference 2: sage-model used the forest's row j
    for name in (
        "Descendant",
        "FirstProgenitor",
        "NextProgenitor",
        "FirstHaloInFOFgroup",
        "NextHaloInFOFgroup",
    ):
        records[name] = -1
    records["SnapNum"] = rows["snap_num"] + np.int32(0)
    for k, column in enumerate(("x", "y", "z")):
        records["Pos"][:, k] = rows[column]
    for k, column in enumerate(("vx", "vy", "vz")):
        records["Vel"][:, k] = rows[column]
    records["VelDisp"] = rows["vrms"]
    records["Vmax"] = rows["vmax"]
    return records


def _check_flybys(rows: np.ndarray, mvir: np.ndarray, source) -> None:
    """``fix_flybys`` (lines 297-381) without its writes (difference 1)."""
    n = len(rows)
    order = np.lexsort((rows["id"], -rows["scale"]))  # (scale descending, id)
    scale = rows["scale"][order]
    pid = rows["pid"][order]
    max_scale = scale[0]
    below = scale[1:] < max_scale
    end = 1 + int(np.argmax(below)) if below.any() else n
    last_halo_with_max_scale = end - 1 if end >= 2 else 1
    num_fofs = int(pid[0] == -1) + int(np.count_nonzero(pid[1:end] == -1))
    if num_fofs == 0:
        raise CtreesToLhaloError(
            source,
            f"no FoF halo (pid == -1) at the forest's last scale {max_scale!r} "
            f"(tree root id {int(rows['id'][order[0]])})",
        )
    if num_fofs == 1:
        return
    window = order[: last_halo_with_max_scale + 1]
    masses = mvir[window]
    candidate = (pid[: last_halo_with_max_scale + 1] == -1) & (masses > np.float32(-1.0))
    if not candidate.any():
        raise CtreesToLhaloError(source, "there must be at least one FOF halo")
    survivor = int(np.flatnonzero(candidate)[np.argmax(masses[candidate])])
    if int(rows["id"][window[survivor]]) == -1:
        raise CtreesToLhaloError(source, "there must be at least one FOF halo")
    # sage-model now demotes every other FoF at the last scale to the survivor and
    # points every other halo there at it; difference 1 leaves them unchanged.


def _fix_upid(rows: np.ndarray, snapnum: np.ndarray, source):
    """``fix_upid`` (lines 384-465) with ``find_fof_halo``: post-fix ``upid``, ``pid``."""
    ids = rows["id"]
    pid = rows["pid"].copy()
    upid = rows["upid"].copy()
    fof = pid == -1
    upid[fof] = ids[fof]

    by_id = np.argsort(ids, kind="stable")
    sorted_ids = ids[by_id]
    repeated = np.flatnonzero(sorted_ids[1:] == sorted_ids[:-1])
    if repeated.size:
        raise CtreesToLhaloError(
            source, f"halo id {int(sorted_ids[repeated[0]])} appears more than once"
        )

    def locate(targets, halos):
        place = np.minimum(np.searchsorted(sorted_ids, targets), len(sorted_ids) - 1)
        found = sorted_ids[place] == targets
        if not found.all():
            k = int(np.flatnonzero(~found)[0])
            raise CtreesToLhaloError(
                source,
                f"could not locate the FOF halo for halo id {int(ids[halos[k]])}: "
                f"no halo has id {int(targets[k])}",
            )
        return by_id[place]

    subs = np.flatnonzero(~fof)
    if subs.size:
        host = locate(upid[subs], subs)
        pending = np.flatnonzero(~fof[host])
        steps = 0
        while pending.size:
            steps += 1
            if steps > len(rows):
                k = int(pending[0])
                raise CtreesToLhaloError(
                    source,
                    f"the host chain of halo id {int(ids[subs[k]])} never reaches a FOF halo",
                )
            host[pending] = locate(rows["upid"][host[pending]], subs[pending])
            pending = pending[~fof[host[pending]]]
        upid[subs] = ids[host]
        pid[subs] = ids[host]

    max_snapnum = max(-1, int(snapnum.max()))
    if max_snapnum < 0:
        raise CtreesToLhaloError(source, "every SnapNum is negative")
    return upid, pid, max_snapnum


def _assign_mergertree_indices(rows, records, upid, pid, max_snapnum, source):
    """``assign_mergertree_indices`` (lines 467-676) on the post-fix ``upid``/``pid``."""
    n = len(rows)
    nsnapshots = max_snapnum + 1
    order = np.lexsort((rows["id"], pid, upid, -rows["scale"]))
    records = records[order]
    ids = rows["id"][order]
    pid = pid[order]
    upid = upid[order]
    scale = rows["scale"][order]
    desc_scale = rows["desc_scale"][order]
    desc_id = rows["desc_id"][order]
    index = np.arange(n, dtype=np.int64)

    snap = records["SnapNum"].astype(np.int64)
    bad = np.flatnonzero((snap < 0) | (snap >= nsnapshots))
    if bad.size:
        raise CtreesToLhaloError(
            source, f"snapnum = {int(snap[bad[0]])} is outside range [0, {nsnapshots})"
        )

    # FoF groups (lines 508-578): a FoF opens its group; each other halo must belong to
    # the last FoF met and is appended to its chain.
    is_fof = pid == -1
    last_fof = np.maximum.accumulate(np.where(is_fof, index, -1))
    subs = np.flatnonzero(~is_fof)
    if subs.size:
        orphan = subs[last_fof[subs] < 0]
        if orphan.size:
            raise CtreesToLhaloError(
                source,
                f"processing subhalo id {int(ids[orphan[0]])} before any FOF halo",
            )
        stray = subs[upid[subs] != ids[last_fof[subs]]]
        if stray.size:
            raise CtreesToLhaloError(
                source,
                f"the sort did not place the FOF before the subs (halo id "
                f"{int(ids[stray[0]])}, upid {int(upid[stray[0]])})",
            )
    records["FirstHaloInFOFgroup"] = last_fof
    next_in_group = np.full(n, -1, dtype=np.int64)
    next_in_group[:-1] = np.where(is_fof[1:], -1, index[1:])
    records["NextHaloInFOFgroup"] = next_in_group

    # Snapshot ranges (lines 517-521): the scale of a snapshot is that of its last halo.
    present, first_at = np.unique(snap, return_index=True)
    last_at = n - 1 - np.unique(snap[::-1], return_index=True)[1]
    start_scale = np.full(nsnapshots, -1, dtype=np.int64)
    end_scale = np.full(nsnapshots, -1, dtype=np.int64)
    scales = np.full(nsnapshots, DBL_MAX)
    start_scale[present] = first_at
    end_scale[present] = last_at
    scales[present] = scale[last_at]

    with_desc = np.flatnonzero(desc_id != -1)
    if with_desc.size == 0:
        return records

    # Descendant snapshot (lines 586-597): the highest within 1e-4 of desc_scale.
    distinct, inverse = np.unique(desc_scale[with_desc], return_inverse=True)
    with np.errstate(invalid="ignore", over="ignore"):
        distance = np.abs(scales[None, :] - distinct[:, None])
        stops = ~(distance > _MAX_EPSILON_SCALE)
    from_top = stops[:, ::-1]
    found_snap = from_top.any(axis=1)
    chosen = nsnapshots - 1 - np.argmax(from_top, axis=1)
    within = found_snap & (distance[np.arange(len(distinct)), chosen] <= _MAX_EPSILON_SCALE)
    if not within.all():
        k = int(np.flatnonzero(~within)[0])
        halo = with_desc[int(np.flatnonzero(inverse == k)[0])]
        raise CtreesToLhaloError(
            source,
            f"could not locate desc_snapnum for halo id {int(ids[halo])} "
            f"(desc_scale {float(distinct[k])!r})",
        )
    desc_snap = chosen[inverse]
    if (start_scale[desc_snap] < 0).any():
        halo = with_desc[int(np.flatnonzero(start_scale[desc_snap] < 0)[0])]
        raise CtreesToLhaloError(
            source, f"the descendant snapshot of halo id {int(ids[halo])} holds no halo"
        )

    # Descendant index (lines 600-618): the halo with desc_id in that snapshot's range.
    by_id = np.argsort(ids, kind="stable")
    sorted_ids = ids[by_id]
    place = np.minimum(np.searchsorted(sorted_ids, desc_id[with_desc]), n - 1)
    desc_loc = by_id[place]
    ok = (
        (sorted_ids[place] == desc_id[with_desc])
        & (desc_loc >= start_scale[desc_snap])
        & (desc_loc <= end_scale[desc_snap])
    )
    if not ok.all():
        halo = with_desc[int(np.flatnonzero(~ok)[0])]
        raise CtreesToLhaloError(
            source,
            f"could not find descendant id {int(desc_id[halo])} of halo id {int(ids[halo])} "
            f"at snapshot {int(desc_snap[int(np.flatnonzero(~ok)[0])])}",
        )
    records["Descendant"][with_desc] = desc_loc

    # Progenitor chains (lines 621-662), progenitors met in index order. A progenitor is
    # promoted to the front iff strictly more massive than the chain's current front,
    # which is the first progenitor or the last promoted one: iff more massive than every
    # progenitor met before it (a NaN mass is never more massive, and a NaN first
    # progenitor is never displaced).
    by_desc = np.argsort(desc_loc, kind="stable")
    progenitor = with_desc[by_desc]
    descendant = desc_loc[by_desc]
    opens = np.concatenate(([True], descendant[1:] != descendant[:-1]))
    group = np.cumsum(opens) - 1
    ngroups = int(group[-1]) + 1
    mass = records["Mvir"][progenitor]
    nan_mass = np.isnan(mass)
    mass_key = np.where(nan_mass, np.float32(-np.inf), mass)
    by_mass = np.lexsort((progenitor, -mass_key, group))
    rank = (ngroups - group).astype(np.int64) * (n + 1) + progenitor
    ranked = rank[by_mass]
    record_breaker = np.empty(len(progenitor), dtype=bool)
    record_breaker[by_mass] = np.minimum.accumulate(ranked) == ranked
    nan_front = np.zeros(ngroups, dtype=bool)
    nan_front[group[opens & nan_mass]] = True
    promoted = record_breaker & ~opens & ~nan_mass & ~nan_front[group]
    place_class = np.where(opens, 1, np.where(promoted, 0, 2))
    place_key = np.where(promoted, -progenitor, progenitor)
    chain_order = np.lexsort((place_key, place_class, group))
    chain = progenitor[chain_order]
    chain_group = group[chain_order]
    head = np.concatenate(([True], chain_group[1:] != chain_group[:-1]))
    tail = np.concatenate((chain_group[1:] != chain_group[:-1], [True]))
    records["FirstProgenitor"][descendant[opens]] = chain[head]
    following = np.concatenate((chain[1:], [-1]))
    following[tail] = -1
    records["NextProgenitor"][chain] = following
    return records


def convert_forest(rows, tree_nhalos, part_mass: float, source="<forest>") -> np.ndarray:
    """One forest's L-Halo records, as sage-model's ``load_forest_ctrees`` returns them.

    ``rows`` is a ``ROW_DTYPE`` array of the forest's rows in sage-model's read order (its
    ASCII trees in (ForestID, FileID, Offset) order, each tree's rows in file order);
    ``tree_nhalos`` the row count of each of those trees; ``part_mass`` sage-model's
    ``PartMass`` (1e10 Msun/h). Returns ``RECORD_DTYPE`` records in sage-model's output
    order, with the module's two differences.
    """
    rows = np.asarray(rows)
    if rows.dtype != ROW_DTYPE:
        raise TypeError(f"rows must have ROW_DTYPE, not {rows.dtype}")
    tree_nhalos = np.asarray(tree_nhalos, dtype=np.int64)
    if (tree_nhalos < 0).any() or int(tree_nhalos.sum()) != len(rows):
        raise ValueError("tree_nhalos must be non-negative and sum to the row count")
    if len(rows) == 0:
        raise CtreesToLhaloError(source, "the forest holds no halo")
    if len(rows) >= INT_MAX:
        raise CtreesToLhaloError(source, f"{len(rows)} halos cannot be indexed by an int")
    records = _convert_conventions(rows, tree_nhalos, part_mass)
    _check_flybys(rows, records["Mvir"], source)
    upid, pid, max_snapnum = _fix_upid(rows, records["SnapNum"], source)
    return _assign_mergertree_indices(rows, records, upid, pid, max_snapnum, source)


# --------------------------------------------------------------------------------------
# Output files
# --------------------------------------------------------------------------------------


def header_tree_file(locations) -> str:
    """The tree file whose first line gives the column map: ``tree_0_0_0.dat`` beside
    ``locations.dat`` (sage-model's ``SimulationDir/TreeName``)."""
    return f"{_locations_dirname(os.fspath(locations))}/{HEADER_TREE_FILE}"


def check_int32_count(value: int, what: str, source) -> None:
    """``PWRITE_64BIT_TO_32BIT`` (``src/sage.c`` lines 421-444): refuse above ``INT_MAX``."""
    if int(value) > INT_MAX:
        raise CtreesToLhaloError(
            source, f"{what} = {int(value)} does not fit inside 32 bits (max {INT_MAX})"
        )


def file_forest_range(nforests: int, n_files: int, file_number: int, source) -> tuple:
    """Forests ``[start, stop)`` of output file ``file_number`` of ``n_files``.

    ``setup_forests_io_ctrees`` (``read_tree_consistentrees_ascii.c`` lines 83-124): each
    file holds ``floor(F/N)`` contiguous forests, plus one for the first ``F mod N`` files.
    ``n_files`` greater than ``nforests`` is refused (sage-model refuses a task with no
    forests, lines 121-124, so its run fails; this refusal covers every file of the run).
    """
    if n_files < 1 or not 0 <= file_number < n_files:
        raise ValueError(f"file {file_number} is not one of {n_files} files")
    check_int32_count(n_files, "the number of output files", source)
    if n_files > nforests:
        raise CtreesToLhaloError(
            source,
            f"{n_files} output files but {nforests} forests: a file would hold no forest, "
            "which sage-model's setup_forests_io_ctrees refuses",
        )
    per_file, remainder = divmod(int(nforests), int(n_files))
    count = per_file + (1 if file_number < remainder else 0)
    start = per_file * file_number + min(file_number, remainder)
    check_int32_count(count, "the number of forests in a file", source)
    return start, start + count


def write_lhalo_forests(
    handle, table: ForestTable, start: int, stop: int, files, column_map, part_mass, source
):
    """Write forests ``[start, stop)`` as one L-Halo file to the binary ``handle``.

    ``convert_trees_to_lhalo`` (``src/sage.c`` lines 446-508): ``int32 Ntrees``,
    ``int32 totNHalos``, ``int32 TreeNHalos[Ntrees]``, then each forest's records in turn,
    little-endian. ``handle`` must be positioned at 0 of an empty, seekable file. Returns
    each forest's halo count (int64).
    """
    nforests = stop - start
    check_int32_count(nforests, "the number of forests in the file", source)
    handle.write(bytes(8 + 4 * nforests))  # sage-model seeks past the header (line 461)
    forest_nhalos = np.zeros(nforests, dtype=np.int64)
    for batch_start in range(start, stop, _FORESTS_PER_BATCH):
        batch = np.arange(batch_start, min(stop, batch_start + _FORESTS_PER_BATCH))
        rows, tree_nhalos, per_forest = read_forests(table, batch, files, column_map)
        ntrees = table.forest_start[batch + 1] - table.forest_start[batch]
        row = tree = 0
        for forest, nhalos, nt in zip(
            batch.tolist(), per_forest.tolist(), ntrees.tolist(), strict=True
        ):
            name = forest_source(table, forest)
            records = convert_forest(
                rows[row : row + nhalos], tree_nhalos[tree : tree + nt], part_mass, name
            )
            check_int32_count(len(records), "the number of halos in a forest", name)
            handle.write(np.ascontiguousarray(records, dtype=RECORD_DTYPE).tobytes())
            forest_nhalos[forest - start] = len(records)
            row += nhalos
            tree += nt
    total = int(forest_nhalos.sum())
    check_int32_count(total, "the number of halos in the file", source)
    handle.seek(0)
    handle.write(np.array([nforests, total], dtype="<i4").tobytes())
    handle.write(forest_nhalos.astype("<i4").tobytes())
    handle.seek(0, os.SEEK_END)
    return forest_nhalos


# --------------------------------------------------------------------------------------
# Particle mass from Len
# --------------------------------------------------------------------------------------

# Bit patterns of the smallest positive (subnormal) double and of the largest finite one.
# For non-negative doubles the order of the int64 bit patterns is the order of the values.
_MIN_POSITIVE_BITS = 1
_DBL_MAX_BITS = int(np.float64(DBL_MAX).view(np.int64))


def _double_of_bits(bits: int) -> np.float64:
    return np.int64(bits).view(np.float64)


def _first_true(predicate, low: int, high: int):
    """The smallest of ``low..high`` where ``predicate`` (False, then True) holds, or None."""
    if not predicate(high):
        return None
    while low < high:
        middle = (low + high) // 2
        if predicate(middle):
            high = middle
        else:
            low = middle + 1
    return low


def _last_true(predicate, low: int, high: int):
    """The largest of ``low..high`` where ``predicate`` (True, then False) holds, or None."""
    if not predicate(low):
        return None
    while low < high:
        middle = (low + high + 1) // 2
        if predicate(middle):
            low = middle
        else:
            high = middle - 1
    return low


def sage_len(mvir, inv_part_mass) -> np.ndarray:
    """``roundf(Mvir * inv_part_mass)`` as float32, the sequence of ``_convert_conventions``.

    ``read_tree_consistentrees_ascii.c`` line 441: the float32 scaled ``Mvir`` times the
    double ``inv_part_mass`` gives a double, narrowed to float32 and rounded half away from
    zero. The values are whole numbers (``inf`` on overflow); ``Len`` is their ``int``.
    """
    with np.errstate(over="ignore", invalid="ignore"):
        product = np.asarray(mvir, dtype=np.float32).astype(np.float64) * np.float64(inv_part_mass)
        return _roundf(product.astype(np.float32))


def count_reproduced(mvir, length, part_mass: float) -> int:
    """How many halos' ``Len`` sage-model's arithmetic gives back with ``part_mass``."""
    with np.errstate(divide="ignore"):
        inv_part_mass = np.float64(1.0) / np.float64(part_mass)
    got = sage_len(mvir, inv_part_mass).astype(np.float64)
    return int(np.count_nonzero(got == np.asarray(length, dtype=np.float64)))


def shortest_decimal(low: float, high: float) -> str:
    """The decimal with the fewest significant digits whose value lies in ``[low, high]``.

    ``0 < low <= high``, both finite. Among the shortest, the one nearest the interval's
    midpoint (``(low + high) / 2``, exactly); of two equally near, the smaller. Exact
    (``fractions.Fraction``). Searching the last digit's place from the leading digit of
    ``high`` downwards, the first place with a multiple of it in the interval gives the
    shortest decimals (a power of ten in the interval is found at its own place, before any
    longer decimal), and at most ten candidates, since the place above held none.
    """
    if not (0 < low <= high and math.isfinite(high)):
        raise ValueError(f"shortest_decimal needs 0 < low <= high, finite: {low!r}, {high!r}")
    low_q, high_q = Fraction(low), Fraction(high)
    middle = (low_q + high_q) / 2
    place = Decimal(high).adjusted()
    while True:
        unit = Fraction(10) ** place
        first = math.ceil(low_q / unit)
        last = math.floor(high_q / unit)
        if first <= last:
            nearest = min(range(first, last + 1), key=lambda n: (abs(n * unit - middle), n))
            return format(Decimal(nearest).scaleb(place), "f")
        place -= 1


@dataclass(frozen=True)
class ParticleMassInterval:
    """The particle masses (doubles) that reproduce every ``Len`` of a set of halos.

    ``low`` and ``high`` are the smallest and largest double ``PartMass`` under which
    sage-model's arithmetic gives every halo its ``Len``; every double between them does
    too, and none outside. Both are ``None`` when no double does. ``value`` is the shortest
    decimal in the interval (``shortest_decimal``). ``n_checked`` is the number of halos;
    ``n_reproduced`` how many of them ``value`` reproduces (all, unless the interval is
    empty, when it is 0).
    """

    low: float | None
    high: float | None
    value: str | None
    n_checked: int
    n_reproduced: int

    @property
    def empty(self) -> bool:
        return self.low is None

    def contains(self, part_mass: float) -> bool:
        """Whether the double ``part_mass`` lies in the interval."""
        return not self.empty and self.low <= float(part_mass) <= self.high


def particle_mass_interval(mvir, length) -> ParticleMassInterval:
    """The interval of particle masses that reproduces every halo's ``Len`` from its ``Mvir``.

    ``mvir`` are float32 scaled masses (1e10 Msun/h) and ``length`` the ``Len`` sage-model
    wrote for them (``read_tree_consistentrees_ascii.c`` line 441; ``sage_len``). The
    search is exact, never sampled. With ``r`` the double ``1.0 / PartMass``, each halo's
    ``Len`` is non-decreasing in ``r`` (``Mvir >= 0``; each rounding step is monotone), so
    "every ``Len`` is at least its value" holds from some ``r`` upwards and "every ``Len``
    is at most its value" up to some ``r``: both bounds are found by bisection over the
    bit patterns of the positive doubles. ``1.0 / PartMass`` rounded is non-increasing in
    ``PartMass``, so the masses whose ``r`` lies between the bounds are again an interval,
    whose ends are found the same way. Halos with the same (``Mvir``, ``Len``) are checked
    once. ``Mvir`` must be finite and non-negative (a negative mass would reverse the
    order); there must be at least one halo.
    """
    mvir = np.asarray(mvir)
    length = np.asarray(length)
    if mvir.dtype != np.float32 or mvir.ndim != 1 or length.shape != mvir.shape:
        raise ValueError("particle_mass_interval needs equal-length 1-d float32 Mvir and Len")
    if not np.issubdtype(length.dtype, np.integer):
        raise ValueError("particle_mass_interval needs integer Len")
    n_checked = len(mvir)
    if n_checked == 0:
        raise ValueError("particle_mass_interval needs at least one halo")
    if not np.all(np.isfinite(mvir)) or np.any(mvir < 0):
        raise ValueError("particle_mass_interval needs finite, non-negative Mvir")
    # One key per (Mvir, Len): Mvir's bits (below 2**31) above Len's as uint32.
    key = (mvir.view(np.uint32).astype(np.uint64) << np.uint64(32)) | (
        length.astype(np.int64) & 0xFFFFFFFF
    ).astype(np.uint64)
    key = np.unique(key)
    unique_mvir = (key >> np.uint64(32)).astype(np.uint32).view(np.float32)
    unique_length = (key & np.uint64(0xFFFFFFFF)).astype(np.uint32).view(np.int32)
    unique_length = unique_length.astype(np.float64)

    def every_len_at_least(bits):
        return bool(np.all(sage_len(unique_mvir, _double_of_bits(bits)) >= unique_length))

    def every_len_at_most(bits):
        return bool(np.all(sage_len(unique_mvir, _double_of_bits(bits)) <= unique_length))

    empty = ParticleMassInterval(None, None, None, n_checked, 0)
    r_low = _first_true(every_len_at_least, _MIN_POSITIVE_BITS, _DBL_MAX_BITS)
    r_high = _last_true(every_len_at_most, _MIN_POSITIVE_BITS, _DBL_MAX_BITS)
    if r_low is None or r_high is None or r_low > r_high:
        return empty
    r_low, r_high = _double_of_bits(r_low), _double_of_bits(r_high)

    def reciprocal(bits):
        with np.errstate(over="ignore", divide="ignore"):
            return np.float64(1.0) / _double_of_bits(bits)

    low = _first_true(lambda bits: reciprocal(bits) <= r_high, _MIN_POSITIVE_BITS, _DBL_MAX_BITS)
    high = _last_true(lambda bits: reciprocal(bits) >= r_low, _MIN_POSITIVE_BITS, _DBL_MAX_BITS)
    if low is None or high is None or low > high:
        return empty
    low, high = float(_double_of_bits(low)), float(_double_of_bits(high))
    value = shortest_decimal(low, high)
    return ParticleMassInterval(
        low, high, value, n_checked, count_reproduced(mvir, length, float(value))
    )


def read_lhalo_mvir_and_len(path) -> tuple:
    """The ``Mvir`` (float32) and ``Len`` (int32) of every record of an L-Halo file.

    The file is ``int32 Ntrees``, ``int32 totNHalos``, ``int32 TreeNHalos[Ntrees]`` and
    ``totNHalos`` 104-byte records (``write_lhalo_forests``); a file whose counts or size
    disagree with that is refused (``ValueError``, naming it).
    """
    path = os.fspath(path)
    size = os.path.getsize(path)
    with open(path, "rb") as handle:
        head = np.frombuffer(handle.read(8), dtype="<i4")
        if len(head) != 2 or head[0] < 0 or head[1] < 0:
            raise ValueError(f"{path}: not an L-Halo file (header)")
        ntrees, nhalos = int(head[0]), int(head[1])
        counts = np.frombuffer(handle.read(4 * ntrees), dtype="<i4")
    if len(counts) != ntrees or int(counts.astype(np.int64).sum()) != nhalos:
        raise ValueError(f"{path}: TreeNHalos does not sum to totNHalos {nhalos}")
    offset = 8 + 4 * ntrees
    if size != offset + RECORD_DTYPE.itemsize * nhalos:
        raise ValueError(f"{path}: {size} bytes, not {offset} + 104 x {nhalos}")
    if nhalos == 0:
        return np.empty(0, dtype=np.float32), np.empty(0, dtype=np.int32)
    records = np.memmap(path, dtype=RECORD_DTYPE, mode="r", offset=offset, shape=(nhalos,))
    try:
        return records["Mvir"].astype(np.float32), records["Len"].astype(np.int32)
    finally:
        del records


def particle_mass_interval_of_files(paths) -> ParticleMassInterval:
    """``particle_mass_interval`` over every record of the L-Halo files ``paths`` together."""
    pairs = [read_lhalo_mvir_and_len(path) for path in paths]
    mvir = np.concatenate([p[0] for p in pairs]) if pairs else np.empty(0, np.float32)
    length = np.concatenate([p[1] for p in pairs]) if pairs else np.empty(0, np.int32)
    return particle_mass_interval(mvir, length)


# --------------------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------------------


class _UsageError(Exception):
    """A usage error or a refused overwrite: exit status 3."""


class _ArgumentParser(argparse.ArgumentParser):
    """argparse with its usage exit (status 2) turned into a ``_UsageError`` (status 3)."""

    def error(self, message):
        raise _UsageError(message)


def _build_parser() -> argparse.ArgumentParser:
    parser = _ArgumentParser(
        prog="ctrees_to_lhalo.py",
        description=(
            "Convert Consistent-Trees ASCII forests to L-Halo binary files as sage-model's "
            f"convert_trees_to_lhalo did at {SAGE_MODEL_COMMIT}, with two differences "
            "(see CTREES-TO-LHALO.md). Writes output files A to B of N."
        ),
    )
    parser.add_argument("--forests-list", required=True, help="the forests.list file")
    parser.add_argument(
        "--locations",
        required=True,
        help="the locations.dat file; tree files resolve from its directory",
    )
    parser.add_argument("--n-files", required=True, help="N, the number of output files")
    parser.add_argument(
        "--files", required=True, help="A-B: write output files A to B inclusive (0-based)"
    )
    parser.add_argument(
        "--particle-mass", required=True, help="PartMass in 1e10 Msun/h (no default)"
    )
    parser.add_argument("--tree-name", required=True, help="output files are <tree-name>.<t>")
    parser.add_argument("--out", required=True, help="an existing output directory")
    return parser


@dataclass(frozen=True)
class _Options:
    forests_list: str
    locations: str
    n_files: int
    first: int
    last: int
    particle_mass: float
    particle_mass_text: str
    tree_name: str
    out: str


def _parse_options(argv) -> _Options:
    args = _build_parser().parse_args(argv)
    if not re.fullmatch(r"[0-9]+", args.n_files) or int(args.n_files) < 1:
        raise _UsageError(f"--n-files must be a positive integer, not {args.n_files!r}")
    n_files = int(args.n_files)
    match = re.fullmatch(r"([0-9]+)-([0-9]+)", args.files)
    if match is None:
        raise _UsageError(f"--files must be A-B, not {args.files!r}")
    first, last = int(match.group(1)), int(match.group(2))
    if first > last:
        raise _UsageError(f"--files {args.files}: A is greater than B")
    if last >= n_files:
        raise _UsageError(f"--files {args.files}: B is not below --n-files {n_files}")
    try:
        particle_mass = float(args.particle_mass)
    except ValueError:
        particle_mass = math.nan
    if not (math.isfinite(particle_mass) and particle_mass > 0):
        raise _UsageError(
            f"--particle-mass must be a positive finite number, not {args.particle_mass!r}"
        )
    name = args.tree_name
    if not name or "/" in name or "\0" in name or name in (".", ".."):
        raise _UsageError(f"--tree-name must be a file name, not {name!r}")
    if not os.path.isdir(args.out):
        raise _UsageError(f"--out {args.out!r} is not an existing directory")
    return _Options(
        forests_list=args.forests_list,
        locations=args.locations,
        n_files=n_files,
        first=first,
        last=last,
        particle_mass=particle_mass,
        particle_mass_text=args.particle_mass,
        tree_name=name,
        out=args.out,
    )


def _output_paths(options: _Options, file_number: int) -> tuple:
    path = os.path.join(options.out, f"{options.tree_name}.{file_number}")
    return path, path + ".json"


def _script_sha256() -> str:
    with open(os.path.abspath(__file__), "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def _file_entry(path) -> dict:
    return {"path": os.path.abspath(path), "size_bytes": os.stat(path).st_size}


def _now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="microseconds")


def _publish(temporary: str, final: str) -> None:
    """Rename a completed temporary file to its final name, refusing an existing one.

    The check and the rename are two steps; processes given disjoint ranges never share
    a final name, which is what makes concurrent runs safe.
    """
    if os.path.lexists(final):
        raise _UsageError(f"{final} appeared while it was being written; not overwritten")
    os.replace(temporary, final)


def _write_output_file(options, file_number, table, start, stop, files, column_map, inputs):
    """Write output file ``file_number`` and its sidecar, each via a per-file temporary."""
    final, sidecar = _output_paths(options, file_number)
    started = _now()
    prefix = f".{options.tree_name}.{file_number}."
    temporaries = []
    try:
        descriptor, data_temporary = tempfile.mkstemp(
            prefix=prefix, suffix=".partial", dir=options.out
        )
        temporaries.append(data_temporary)
        with os.fdopen(descriptor, "wb") as handle:
            forest_nhalos = write_lhalo_forests(
                handle,
                table,
                start,
                stop,
                files,
                column_map,
                options.particle_mass,
                final,
            )
            handle.flush()
            os.fsync(handle.fileno())
        first_tree, end_tree = int(table.forest_start[start]), int(table.forest_start[stop])
        used_files = np.unique(table.file_id[first_tree:end_tree]).tolist()
        record = {
            "tool": "ctrees_to_lhalo.py",
            "script_sha256": inputs["script_sha256"],
            "sage_model_commit": SAGE_MODEL_COMMIT,
            "inputs": {
                "forests_list": inputs["forests_list"],
                "locations": inputs["locations"],
                "header_tree_file": inputs["header_tree_file"],
                "tree_files": [
                    {"file_id": fid, **_file_entry(table.tree_files[fid])} for fid in used_files
                ],
            },
            "output": {
                "path": os.path.abspath(final),
                "size_bytes": os.stat(data_temporary).st_size,
                "tree_name": options.tree_name,
                "file_number": file_number,
                "n_files": options.n_files,
            },
            "forest_range": {
                "start": start,
                "stop": stop,
                "total_forests": table.nforests,
                "first_forest_id": int(table.forest_id[first_tree]),
                "last_forest_id": int(table.forest_id[end_tree - 1]),
            },
            "nforests": stop - start,
            "nhalos": int(forest_nhalos.sum()),
            "particle_mass": options.particle_mass,
            "particle_mass_argument": options.particle_mass_text,
            "started_utc": started,
            "finished_utc": _now(),
        }
        descriptor, json_temporary = tempfile.mkstemp(
            prefix=prefix, suffix=".json.partial", dir=options.out
        )
        temporaries.append(json_temporary)
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(record, handle, indent=2)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        _publish(data_temporary, final)
        _publish(json_temporary, sidecar)
    finally:
        for temporary in temporaries:
            if os.path.lexists(temporary):
                os.unlink(temporary)


def main(argv=None) -> int:
    """The command line; returns the exit status (0, 2 or 3). See ``CTREES-TO-LHALO.md``."""
    prog = "ctrees_to_lhalo.py"
    try:
        options = _parse_options(argv)
    except _UsageError as exc:
        _build_parser().print_usage(sys.stderr)
        print(f"{prog}: error: {exc}", file=sys.stderr)
        return EXIT_STATUS_USAGE
    numbers = range(options.first, options.last + 1)
    existing = [p for t in numbers for p in _output_paths(options, t) if os.path.lexists(p)]
    if existing:
        print(
            f"{prog}: error: refusing to overwrite {len(existing)} existing output(s), "
            f"first {existing[0]}; nothing written",
            file=sys.stderr,
        )
        return EXIT_STATUS_USAGE
    try:
        check_int32_count(options.n_files, "the number of output files", "--n-files")
        table = read_forest_table(options.forests_list, options.locations)
        header = header_tree_file(options.locations)
        column_map = read_column_map(header)
        ranges = [
            file_forest_range(table.nforests, options.n_files, t, options.forests_list)
            for t in numbers
        ]
        inputs = {
            "script_sha256": _script_sha256(),
            "forests_list": _file_entry(options.forests_list),
            "locations": _file_entry(options.locations),
            "header_tree_file": _file_entry(header),
        }
        with TreeFiles(table.tree_files) as files:
            for t, (start, stop) in zip(numbers, ranges, strict=True):
                _write_output_file(options, t, table, start, stop, files, column_map, inputs)
                print(
                    f"{prog}: wrote {_output_paths(options, t)[0]} (forests {start}-{stop - 1})",
                    file=sys.stderr,
                )
    except CtreesToLhaloError as exc:
        print(f"{prog}: error: {exc}", file=sys.stderr)
        return exc.exit_status
    except OSError as exc:  # an input that vanished, or a failed write
        print(f"{prog}: error: {exc.filename}: {exc.strerror or exc}", file=sys.stderr)
        return EXIT_STATUS_DATA
    except _UsageError as exc:
        print(f"{prog}: error: {exc}", file=sys.stderr)
        return EXIT_STATUS_USAGE
    return EXIT_STATUS_OK


if __name__ == "__main__":
    sys.exit(main())
