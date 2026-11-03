#!/usr/bin/env python3
"""T1 parity instrument: compare a Mimic ``sage26`` run against SAGE26 galaxy by galaxy.

The reference is one SAGE26 worker file (``model_0.hdf5``: groups ``Snap_<N>`` of
one-dimensional datasets). The candidate is a Mimic HDF5 run directory holding K
partition files ``<basename>_<NNN>.hdf5``, each with ``Snap<NNN>/Galaxies`` compound
tables. Galaxies are matched one to one per snapshot by their position and type, every
mapped property is compared under the rules stated in this directory's README.md, and
the run ends with a numerical verdict:

    pass   every threshold holds at every compared snapshot       exit status 0
    fail   at least one threshold does not hold                   exit status 1
    error  the instrument cannot measure (no verdict tables)      exit status 2

The SAGE26-to-Mimic pairing is read from ``field_map.yaml`` beside this file. The
instrument depends only on the Python standard library, NumPy and h5py, so the field
map is written in, and parsed as, a strict YAML subset (see ``parse_field_map_text``).

The README is the specification: its numbered comparison rules are cited below as
"rule N", and its "Readings settled by the PM" section fixes the points the rules
leave open.
"""

import argparse
import csv
import hashlib
import json
import math
import os
import re
import sys
from fractions import Fraction

import h5py
import numpy as np

# ---------------------------------------------------------------------------
# Constants fixed by the comparison rules
# ---------------------------------------------------------------------------

#: SHA-256 of the T1 reference worker file (rule 10).
T1_REFERENCE_SHA256 = "19977093c0497339b954cb4fc0417ff249fde0de794319c224a8b9ea9e001d18"

#: T1's eight snapshots, the default ``--snapshots`` (rule 10).
T1_SNAPSHOTS = (63, 37, 32, 27, 23, 20, 18, 16)

#: Relative tolerance of rule 6.
REL_TOL = 1e-6

#: Absolute zero band of rule 6 (r = 0) and of the M_sig cohort (rule 5).
ZERO_BAND = 1e-10

#: Rule 8 thresholds, as exact rationals.
COUNT_TOLERANCE = Fraction(1, 1000)  # |N_cand - N_ref| <= 0.001 N_ref
MATCH_MINIMUM = Fraction(999, 1000)  # M >= 0.999 N_ref and M >= 0.999 N_cand
FLOAT_GATE = Fraction(9, 10)  # >= 90% within tolerance, over M and over M_sig
INT_GATE = Fraction(995, 1000)  # >= 99.5% exactly equal, over M

#: The eight uncompared SAGE26 datasets (rule 7) and, where SAGE26's creation value is
#: fixed, that value.
UNCOMPARED_DATASETS = (
    "GalaxyIndex",
    "CentralGalaxyIndex",
    "SAGEHaloIndex",
    "SAGETreeIndex",
    "mergeType",
    "mergeIntoID",
    "mergeIntoSnapNum",
    "g_max",
)
CREATION_VALUES = {"mergeType": 0, "g_max": 0, "mergeIntoID": -1, "mergeIntoSnapNum": -1}

#: The datasets the matching key and the snapshot check need (rules 3 and 4).
KEY_POSITION_DATASETS = ("Posx", "Posy", "Posz")

EXIT_PASS, EXIT_FAIL, EXIT_ERROR = 0, 1, 2

FIELD_MAP_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "field_map.yaml")

STORAGE_TYPES = {
    "int32": ("i", 4),
    "int64": ("i", 8),
    "float32": ("f", 4),
    "float64": ("f", 8),
}


class InstrumentError(Exception):
    """The instrument cannot measure: the verdict is ``error`` (rule 9)."""


# ---------------------------------------------------------------------------
# Field map
# ---------------------------------------------------------------------------

_KEY_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_BARE_RE = re.compile(r"[A-Za-z0-9_.+\-]+")
_MAPPING_KEYS = {
    "kind",
    "mimic",
    "component",
    "source",
    "sage26_type",
    "mimic_type",
    "factor",
    "unit_check",
    "factor_citation",
}
_REQUIRED_MAPPING_KEYS = {"kind", "mimic", "source", "sage26_type", "mimic_type", "factor"}


def _parse_scalar(text, where):
    """Parse a field-map value: a double-quoted JSON string or a bare token."""
    if text.startswith('"'):
        try:
            value = json.loads(text)
        except ValueError as exc:
            raise InstrumentError("%s: malformed quoted string (%s)" % (where, exc)) from exc
        if not isinstance(value, str):
            raise InstrumentError("%s: quoted value is not a string" % where)
        return value
    if not _BARE_RE.fullmatch(text):
        raise InstrumentError(
            "%s: value %r is neither a bare token nor a quoted string" % (where, text)
        )
    return text


def parse_field_map_text(text, name="field_map.yaml"):
    """Parse the field map's strict YAML subset into ``{dataset: {key: str}}``.

    Accepted grammar (a subset of YAML, so the file is also valid YAML):
    full-line ``#`` comments and blank lines; ``format: 1`` and ``fields:`` at column
    0; one ``<Dataset>:`` per entry at column 2; ``<key>: <value>`` at column 4, the
    value a bare token ``[A-Za-z0-9_.+-]+`` or a double-quoted JSON string. Anything
    else (tabs, other indentation, inline comments, duplicate keys) is rejected, so a
    malformed map is an ``error``, never a silent misreading.
    """
    fmt = None
    fields = {}
    in_fields = False
    current = None
    for lineno, raw in enumerate(text.splitlines(), start=1):
        where = "%s:%d" % (name, lineno)
        if "\t" in raw:
            raise InstrumentError("%s: tab character" % where)
        line = raw.rstrip()
        if not line or line.lstrip().startswith("#"):
            continue
        indent = len(line) - len(line.lstrip(" "))
        body = line[indent:]
        key, sep, rest = body.partition(":")
        if not sep or not _KEY_RE.fullmatch(key):
            raise InstrumentError("%s: expected '<key>:' (got %r)" % (where, body))
        rest = rest.strip()
        if indent == 0:
            if key == "format" and fmt is None:
                fmt = _parse_scalar(rest, where)
            elif key == "fields" and not in_fields and rest == "":
                in_fields = True
            else:
                raise InstrumentError("%s: unexpected top-level key %r" % (where, key))
        elif indent == 2 and in_fields:
            if rest != "":
                raise InstrumentError("%s: dataset entry %r must open a block" % (where, key))
            if key in fields:
                raise InstrumentError("%s: duplicate entry for dataset %r" % (where, key))
            current = fields[key] = {}
        elif indent == 4 and current is not None:
            if key in current:
                raise InstrumentError("%s: duplicate key %r" % (where, key))
            if rest == "":
                raise InstrumentError("%s: key %r has no value" % (where, key))
            current[key] = _parse_scalar(rest, where)
        else:
            raise InstrumentError("%s: unexpected indentation %d" % (where, indent))
    if fmt != "1":
        raise InstrumentError("%s: 'format: 1' is required" % name)
    if not fields:
        raise InstrumentError("%s: no 'fields' entries" % name)
    return fields


def validate_field_map(raw):
    """Check the parsed map against rules 3, 4, 6 and 7 and return typed entries.

    Each returned entry is a dict: ``{"uncompared": reason}`` or ``{"kind", "mimic",
    "component" (int or None), "source", "sage26_type", "mimic_type", "factor"
    (float)}``.
    """
    entries = {}
    for ds, e in raw.items():
        if "uncompared" in e:
            if set(e) != {"uncompared"}:
                raise InstrumentError("field map %s: an uncompared entry takes no other key" % ds)
            entries[ds] = {"uncompared": e["uncompared"]}
            continue
        unknown = set(e) - _MAPPING_KEYS
        missing = _REQUIRED_MAPPING_KEYS - set(e)
        if unknown or missing:
            raise InstrumentError(
                "field map %s: unknown keys %s, missing keys %s"
                % (ds, sorted(unknown), sorted(missing))
            )
        kind = e["kind"]
        if kind not in ("float", "int"):
            raise InstrumentError("field map %s: kind must be float or int" % ds)
        for side in ("sage26_type", "mimic_type"):
            if e[side] not in STORAGE_TYPES:
                raise InstrumentError(
                    "field map %s: %s %r is not one of %s"
                    % (ds, side, e[side], sorted(STORAGE_TYPES))
                )
            if STORAGE_TYPES[e[side]][0] != kind[0]:
                # Rule 3: no integer is converted to or from a float.
                raise InstrumentError(
                    "field map %s: %s %s does not suit kind %s" % (ds, side, e[side], kind)
                )
        component = None
        if "component" in e:
            if e["component"] not in ("0", "1", "2"):
                raise InstrumentError("field map %s: component must be 0, 1 or 2" % ds)
            component = int(e["component"])
        if e["source"] not in ("model", "core", "simulation"):
            raise InstrumentError("field map %s: source must be model, core or simulation" % ds)
        try:
            factor = float(e["factor"])
        except ValueError:
            raise InstrumentError(
                "field map %s: factor %r is not a number" % (ds, e["factor"])
            ) from None
        if not math.isfinite(factor) or factor == 0.0:
            raise InstrumentError("field map %s: factor must be finite and non-zero" % ds)
        if kind == "int" and e["factor"] != "1":
            raise InstrumentError("field map %s: an integer property takes factor 1" % ds)
        if e["source"] == "model" and factor != 1.0:
            raise InstrumentError(
                "field map %s: a sage26 package property takes factor 1 (rule 7)" % ds
            )
        if factor != 1.0 and not e.get("factor_citation"):
            raise InstrumentError("field map %s: a factor other than 1 needs factor_citation" % ds)
        if e["source"] in ("core", "simulation") and not e.get("unit_check"):
            raise InstrumentError("field map %s: a %s mapping needs unit_check" % (ds, e["source"]))
        entries[ds] = {
            "kind": kind,
            "mimic": e["mimic"],
            "component": component,
            "source": e["source"],
            "sage26_type": e["sage26_type"],
            "mimic_type": e["mimic_type"],
            "factor": factor,
        }

    uncompared = {ds for ds, e in entries.items() if "uncompared" in e}
    if uncompared != set(UNCOMPARED_DATASETS):
        raise InstrumentError(
            "field map: the uncompared entries must be exactly %s (got %s)"
            % (list(UNCOMPARED_DATASETS), sorted(uncompared))
        )
    for ds, kind in (("SnapNum", "int"), ("Type", "int")):
        if ds not in entries or entries[ds].get("kind") != kind:
            raise InstrumentError("field map: %s must be mapped as an %s property" % (ds, kind))
    for ds in KEY_POSITION_DATASETS:
        e = entries.get(ds)
        if e is None or e.get("kind") != "float" or e.get("sage26_type") != "float32":
            # PM ruling Q6: the key never narrows a stored value.
            raise InstrumentError(
                "field map: %s must be mapped as a float property with sage26_type float32" % ds
            )
    return entries


def load_field_map(path=FIELD_MAP_PATH):
    """Read, parse and validate the field map; return ``(entries, sha256)``."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        raise InstrumentError("cannot read field map %s: %s" % (path, exc)) from exc
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise InstrumentError("field map %s is not UTF-8: %s" % (path, exc)) from exc
    entries = validate_field_map(parse_field_map_text(text, os.path.basename(path)))
    return entries, hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------


def sha256_file(path):
    """SHA-256 of a file, streamed."""
    digest = hashlib.sha256()
    try:
        with open(path, "rb") as fh:
            for block in iter(lambda: fh.read(1 << 22), b""):
                digest.update(block)
    except OSError as exc:
        raise InstrumentError("cannot read %s: %s" % (path, exc)) from exc
    return digest.hexdigest()


def candidate_partition_files(directory, basename, partitions):
    """Rule 2: exactly ``<basename>_000.hdf5`` ... ``<basename>_<K-1>.hdf5``, nothing else."""
    try:
        names = os.listdir(directory)
    except OSError as exc:
        raise InstrumentError("cannot list candidate directory %s: %s" % (directory, exc)) from exc
    pattern = re.compile(re.escape(basename) + r"_.*\.hdf5")
    present = {n for n in names if pattern.fullmatch(n)}
    expected = ["%s_%03d.hdf5" % (basename, i) for i in range(partitions)]
    missing = [n for n in expected if n not in present]
    extra = sorted(present - set(expected))
    if missing or extra:
        raise InstrumentError(
            "candidate directory %s must hold exactly %s ... %s: missing %s, unexpected %s"
            % (directory, expected[0], expected[-1], missing, extra)
        )
    paths = [os.path.join(directory, n) for n in expected]
    for p in paths:
        if not os.path.isfile(p):
            raise InstrumentError("candidate partition %s is not a regular file" % p)
    return paths


def _type_matches(dtype, declared):
    """PM ruling Q5: a storage type is its numeric kind and width; byte order is free."""
    kind, width = STORAGE_TYPES[declared]
    return dtype.kind == kind and dtype.itemsize == width


def _signed_int64(values):
    """Rule 3: widen a signed integer array losslessly to int64."""
    if values.dtype.kind != "i":
        raise InstrumentError("internal: %s is not a signed integer type" % values.dtype)
    return values.astype(np.int64)


def read_reference_snapshot(ref, snap, fmap):
    """Read and check ``Snap_<N>`` (rules 3, 7 and 9); return ``{dataset: array}``."""
    group_name = "Snap_%d" % snap
    if group_name not in ref:
        raise InstrumentError("reference has no group %s" % group_name)
    group = ref[group_name]
    if not isinstance(group, h5py.Group):
        raise InstrumentError("reference %s is not a group" % group_name)
    names = list(group.keys())
    unmapped = sorted(set(names) - set(fmap))
    if unmapped:
        raise InstrumentError(
            "reference %s datasets with no field-map entry: %s" % (group_name, unmapped)
        )
    absent = sorted(set(fmap) - set(names))
    if absent:
        raise InstrumentError(
            "field-map datasets absent from reference %s: %s" % (group_name, absent)
        )
    data = {}
    length = None
    for name in names:
        obj = group[name]
        if not isinstance(obj, h5py.Dataset) or obj.ndim != 1:
            raise InstrumentError(
                "reference %s/%s is not a one-dimensional dataset" % (group_name, name)
            )
        if length is None:
            length = obj.shape[0]
        elif obj.shape[0] != length:
            raise InstrumentError(
                "reference %s/%s has length %d, not %d" % (group_name, name, obj.shape[0], length)
            )
        if obj.dtype.kind not in "iuf":
            raise InstrumentError(
                "reference %s/%s has non-numeric type %s" % (group_name, name, obj.dtype)
            )
        e = fmap[name]
        if "uncompared" not in e and not _type_matches(obj.dtype, e["sage26_type"]):
            raise InstrumentError(
                "reference %s/%s is stored as %s, the field map declares %s"
                % (group_name, name, obj.dtype, e["sage26_type"])
            )
        arr = obj[()]
        data[name] = arr.astype(arr.dtype.newbyteorder("="))
    return data


def read_candidate_snapshot(handles, snap, fmap):
    """Read and check ``Snap<NNN>/Galaxies`` across all partitions (rules 2, 3 and 9).

    Returns ``{mimic_property: array}`` for every mapped Mimic property, concatenated
    over the partitions in partition order (the row order carries no meaning).
    """
    table_name = "Snap%03d/Galaxies" % snap
    layout = None
    tables = []
    for path, handle in handles:
        if table_name not in handle:
            raise InstrumentError("candidate %s has no %s table" % (path, table_name))
        table = handle[table_name]
        if not isinstance(table, h5py.Dataset) or table.dtype.names is None or table.ndim != 1:
            raise InstrumentError(
                "candidate %s %s is not a one-dimensional compound table" % (path, table_name)
            )
        this = [
            (
                n,
                table.dtype.fields[n][0].base.kind,
                table.dtype.fields[n][0].base.itemsize,
                table.dtype.fields[n][0].shape,
            )
            for n in table.dtype.names
        ]
        if layout is None:
            layout = this
        elif this != layout:
            raise InstrumentError(
                "candidate %s %s: compound field names, types or shapes differ from the first partition"
                % (path, table_name)
            )
        tables.append(table)

    needed = {}
    for ds, e in fmap.items():
        if "uncompared" in e:
            continue
        prop = e["mimic"]
        if prop not in tables[0].dtype.names:
            raise InstrumentError(
                "field-map property %s (for %s) absent from candidate %s" % (prop, ds, table_name)
            )
        sub = tables[0].dtype.fields[prop][0]
        want_shape = (3,) if e["component"] is not None else ()
        if sub.shape != want_shape or not _type_matches(sub.base, e["mimic_type"]):
            raise InstrumentError(
                "candidate %s field %s is stored as %s%s, the field map declares %s%s for %s"
                % (table_name, prop, sub.base, sub.shape, e["mimic_type"], want_shape, ds)
            )
        needed[prop] = sub

    columns = {prop: [] for prop in needed}
    for table in tables:
        rows = table[()]
        for prop in needed:
            col = rows[prop]
            columns[prop].append(col.astype(col.dtype.newbyteorder("=")))
    out = {}
    for prop, sub in needed.items():
        parts = columns[prop]
        if parts:
            out[prop] = np.concatenate(parts, axis=0)
        else:
            out[prop] = np.zeros((0,) + sub.shape, dtype=sub.base.newbyteorder("="))
    return out


# ---------------------------------------------------------------------------
# Rule 6 arithmetic
# ---------------------------------------------------------------------------


def candidate_values(cand, entry):
    """Mimic values for one mapped dataset, widened to float64 or int64 (rules 3 and 6).

    For a float: returns ``(m, m_unrounded)`` where ``m_unrounded`` is stored x factor
    in float64 and ``m`` is that rounded (nearest-even) to the SAGE26 dataset's precision
    and widened back. For an int: returns ``(m, None)``, the int64 widening.
    """
    values = cand[entry["mimic"]]
    if entry["component"] is not None:
        values = values[:, entry["component"]]
    if entry["kind"] == "int":
        return _signed_int64(values), None
    with np.errstate(all="ignore"):
        unrounded = values.astype(np.float64) * np.float64(entry["factor"])
        if STORAGE_TYPES[entry["sage26_type"]][1] == 4:
            rounded = unrounded.astype(np.float32).astype(np.float64)
        else:
            rounded = unrounded.copy()
    return rounded, unrounded


def within_tolerance(r, m):
    """Rule 6: the boolean within-tolerance mask for float64 arrays r and m."""
    with np.errstate(all="ignore"):
        both_nan = np.isnan(r) & np.isnan(m)
        same_inf = np.isinf(r) & np.isinf(m) & (r == m)
        zero_ref = (r == 0) & (np.abs(m) <= ZERO_BAND)
        finite_rel = (
            np.isfinite(r) & (r != 0) & np.isfinite(m) & (np.abs(m - r) <= REL_TOL * np.abs(r))
        )
    return both_nan | same_inf | zero_ref | finite_rel


def significant(r, m):
    """Rule 5: the M_sig cohort, pairs outside the zero band or with a non-finite value."""
    with np.errstate(all="ignore"):
        return (np.abs(r) > ZERO_BAND) | (np.abs(m) > ZERO_BAND) | ~np.isfinite(r) | ~np.isfinite(m)


def _zero_normalised_bits(values, dtype):
    """Bit patterns of ``values`` at ``dtype`` with -0.0 read as +0.0 (NaN bits kept)."""
    v = values.astype(dtype)
    v = np.where(v == 0, dtype.type(0), v)
    return v.view(np.uint32 if dtype.itemsize == 4 else np.uint64)


def bit_identical(r_stored, m, sage26_type):
    """Rule 6: equal bit patterns at SAGE26's stored precision after -0.0 normalisation."""
    dtype = np.dtype(sage26_type)
    with np.errstate(all="ignore"):
        return _zero_normalised_bits(r_stored, dtype) == _zero_normalised_bits(m, dtype)


def _stat(values):
    """Median and 99th percentile (NumPy's default linear interpolation; PM ruling Q3)."""
    if values.size == 0:
        return None, None
    return float(np.median(values)), float(np.percentile(values, 99))


# ---------------------------------------------------------------------------
# Matching (rule 4)
# ---------------------------------------------------------------------------


def _key_side(positions, types):
    """Return (keys Nx4 int64, finite mask) for one side: float32 bits of x, y, z and Type."""
    finite = np.ones(types.shape[0], dtype=bool)
    cols = []
    for p in positions:
        p32 = p.astype(np.float32)
        finite &= np.isfinite(p32)
        cols.append(_zero_normalised_bits(p32, np.dtype(np.float32)).astype(np.int64))
    cols.append(types.astype(np.int64))
    keys = np.stack(cols, axis=1) if types.shape[0] else np.zeros((0, 4), dtype=np.int64)
    return keys, finite


def _unique_rows(keys, mask):
    """Indices (into keys) of rows inside ``mask`` whose key occurs once among them."""
    idx = np.nonzero(mask)[0]
    if idx.size == 0:
        return idx, 0
    _, inverse, counts = np.unique(keys[idx], axis=0, return_inverse=True, return_counts=True)
    inverse = inverse.reshape(-1)
    single = counts[inverse] == 1
    return idx[single], int((~single).sum())


def match_galaxies(ref_pos, ref_type, cand_pos, cand_type):
    """Match galaxies one to one by (Posx, Posy, Posz, Type) (rule 4).

    Positions are float32-valued float64 arrays as rule 6 treats each side. Returns
    ``(ref_index, cand_index, side_counts)`` with the pairs sorted by reference row.
    """
    rkeys, rfinite = _key_side(ref_pos, ref_type)
    ckeys, cfinite = _key_side(cand_pos, cand_type)
    rsingle, rdup = _unique_rows(rkeys, rfinite)
    csingle, cdup = _unique_rows(ckeys, cfinite)
    nr = rsingle.size
    if nr and csingle.size:
        both = np.concatenate([rkeys[rsingle], ckeys[csingle]], axis=0)
        _, inverse, counts = np.unique(both, axis=0, return_inverse=True, return_counts=True)
        inverse = inverse.reshape(-1)
        ref_of = np.full(counts.size, -1, dtype=np.int64)
        cand_of = np.full(counts.size, -1, dtype=np.int64)
        ref_of[inverse[:nr]] = rsingle
        cand_of[inverse[nr:]] = csingle
        paired = (ref_of >= 0) & (cand_of >= 0)
        ref_index, cand_index = ref_of[paired], cand_of[paired]
        order = np.argsort(ref_index, kind="stable")
        ref_index, cand_index = ref_index[order], cand_index[order]
    else:
        ref_index = cand_index = np.zeros(0, dtype=np.int64)
    m = ref_index.size
    sides = {
        "reference": {
            "nonfinite_position": int((~rfinite).sum()),
            "duplicate_key": rdup,
            "unmatched": int(ref_type.shape[0] - m),
        },
        "candidate": {
            "nonfinite_position": int((~cfinite).sum()),
            "duplicate_key": cdup,
            "unmatched": int(cand_type.shape[0] - m),
        },
    }
    return ref_index, cand_index, sides


# ---------------------------------------------------------------------------
# Per-snapshot comparison
# ---------------------------------------------------------------------------


def _at_least(count, denominator, fraction):
    """Exact rational test count >= fraction x denominator (rule 8)."""
    return count * fraction.denominator >= fraction.numerator * denominator


def _fraction(count, denominator):
    return None if denominator == 0 else count / denominator


def _scalar(value):
    """A JSON-safe number: non-finite floats become "nan", "inf", "-inf" (rule 11)."""
    if isinstance(value, (np.integer, int)) and not isinstance(value, bool):
        return int(value)
    value = float(value)
    if math.isnan(value):
        return "nan"
    if math.isinf(value):
        return "inf" if value > 0 else "-inf"
    return value


def _check_snapnum(values, snap, side):
    bad = int((values != snap).sum())
    if bad:
        raise InstrumentError(
            "%s Snap %d: %d rows carry another SnapNum (snapshot mix-up)" % (side, snap, bad)
        )


def compare_snapshot(snap, ref, cand, fmap):
    """Compare one snapshot (rules 3 to 8, 11); return ``(table, outlier_blocks)``."""
    n_ref = int(ref["SnapNum"].shape[0])
    sn = fmap["SnapNum"]
    cand_snapnum, _ = candidate_values(cand, sn)
    n_cand = int(cand_snapnum.shape[0])
    _check_snapnum(_signed_int64(ref["SnapNum"]), snap, "reference")
    _check_snapnum(cand_snapnum, snap, "candidate")

    ref_pos = [ref[d].astype(np.float64) for d in KEY_POSITION_DATASETS]
    cand_pos = [candidate_values(cand, fmap[d])[0] for d in KEY_POSITION_DATASETS]
    ref_type = _signed_int64(ref["Type"])
    cand_type = candidate_values(cand, fmap["Type"])[0]
    ri, ci, sides = match_galaxies(ref_pos, ref_type, cand_pos, cand_type)
    m_pairs = int(ri.size)
    if m_pairs == 0:
        raise InstrumentError("Snap %d: no galaxy matched (M = 0)" % snap)

    thresholds = []

    def threshold(name, ok, **detail):
        thresholds.append(dict(name=name, passed=bool(ok), **detail))

    diff = abs(n_cand - n_ref)
    threshold(
        "count",
        diff * COUNT_TOLERANCE.denominator <= COUNT_TOLERANCE.numerator * n_ref,
        n_ref=n_ref,
        n_cand=n_cand,
    )
    threshold(
        "matching_vs_n_ref",
        _at_least(m_pairs, n_ref, MATCH_MINIMUM),
        matched=m_pairs,
        denominator=n_ref,
    )
    threshold(
        "matching_vs_n_cand",
        _at_least(m_pairs, n_cand, MATCH_MINIMUM),
        matched=m_pairs,
        denominator=n_cand,
    )

    properties = {}
    outliers = []
    for ds, e in fmap.items():
        if "uncompared" in e:
            continue
        r_stored = ref[ds][ri]
        m_all, m_unrounded_all = candidate_values(cand, e)
        m = m_all[ci]
        if e["kind"] == "int":
            r = _signed_int64(r_stored)
            equal = r == m
            n_equal = int(equal.sum())
            ok = _at_least(n_equal, m_pairs, INT_GATE)
            threshold("int:%s" % ds, ok, equal=n_equal, denominator=m_pairs)
            properties[ds] = {
                "kind": "int",
                "mimic": e["mimic"],
                "exact_equal": n_equal,
                "exact_equal_fraction": _fraction(n_equal, m_pairs),
                "bit_identical": n_equal,
                "bit_identical_fraction": _fraction(n_equal, m_pairs),
                "passed": ok,
            }
            bad = np.nonzero(~equal)[0]
        else:
            r = r_stored.astype(np.float64)
            within = within_tolerance(r, m)
            sig = significant(r, m)
            n_within = int(within.sum())
            m_sig = int(sig.sum())
            n_within_sig = int((within & sig).sum())
            ok_m = _at_least(n_within, m_pairs, FLOAT_GATE)
            ok_sig = m_sig == 0 or _at_least(n_within_sig, m_sig, FLOAT_GATE)
            threshold("float:%s:over_M" % ds, ok_m, within=n_within, denominator=m_pairs)
            threshold("float:%s:over_M_sig" % ds, ok_sig, within=n_within_sig, denominator=m_sig)
            n_bits = int(bit_identical(r_stored, m, e["sage26_type"]).sum())
            with np.errstate(all="ignore"):
                rel_set = np.isfinite(r) & (r != 0) & np.isfinite(m)
                rel = np.abs(m[rel_set] - r[rel_set]) / np.abs(r[rel_set])
                zero_set = (r == 0) & np.isfinite(m)
                zero_abs = np.abs(m[zero_set])
            rel_med, rel_p99 = _stat(rel)
            zero_med, zero_p99 = _stat(zero_abs)
            prop = {
                "kind": "float",
                "mimic": e["mimic"],
                "component": e["component"],
                "within_tolerance": n_within,
                "within_tolerance_fraction": _fraction(n_within, m_pairs),
                "M_sig": m_sig,
                "within_tolerance_sig": n_within_sig,
                "within_tolerance_sig_fraction": _fraction(n_within_sig, m_sig),
                "bit_identical": n_bits,
                "bit_identical_fraction": _fraction(n_bits, m_pairs),
                "relative_difference": {
                    "pairs": int(rel_set.sum()),
                    "median": rel_med,
                    "p99": rel_p99,
                },
                "abs_candidate_where_reference_zero": {
                    "pairs": int(zero_set.sum()),
                    "pairs_nonfinite_candidate": int(((r == 0) & ~np.isfinite(m)).sum()),
                    "median": zero_med,
                    "p99": zero_p99,
                },
                "passed_over_M": ok_m,
                "passed_over_M_sig": ok_sig,
                "passed": ok_m and ok_sig,
            }
            if STORAGE_TYPES[e["mimic_type"]][1] > STORAGE_TYPES[e["sage26_type"]][1]:
                n_unr = int(within_tolerance(r, m_unrounded_all[ci]).sum())
                prop["within_tolerance_unrounded"] = n_unr
                prop["within_tolerance_unrounded_fraction"] = _fraction(n_unr, m_pairs)
            properties[ds] = prop
            bad = np.nonzero(~within)[0]
        if bad.size:
            outliers.append((ds, e["kind"], ri[bad], r[bad], m[bad]))

    uncompared = {}
    for ds in UNCOMPARED_DATASETS:
        v = ref[ds]
        summary = {
            "nonzero": int((v != 0).sum()),
            "min": _scalar(v.min()) if v.size else None,
            "max": _scalar(v.max()) if v.size else None,
        }
        if ds in CREATION_VALUES:
            summary["creation_value"] = CREATION_VALUES[ds]
            summary["differ_from_creation"] = int((v != CREATION_VALUES[ds]).sum())
        uncompared[ds] = summary

    table = {
        "snapshot": snap,
        "n_ref": n_ref,
        "n_cand": n_cand,
        "matched": m_pairs,
        "sides": sides,
        "thresholds": thresholds,
        "failed_thresholds": [t["name"] for t in thresholds if not t["passed"]],
        "passed": all(t["passed"] for t in thresholds),
        "properties": properties,
        "uncompared": uncompared,
    }
    return table, outliers


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------


def _json_safe(obj):
    if isinstance(obj, dict):
        return {k: _json_safe(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_json_safe(v) for v in obj]
    if isinstance(obj, (bool, np.bool_)):
        return bool(obj)
    if obj is None or isinstance(obj, str):
        return obj
    return _scalar(obj)


def write_json(path, document):
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(_json_safe(document), fh, indent=1, allow_nan=False)
        fh.write("\n")


def _csv_number(value):
    if isinstance(value, (int, np.integer)):
        return str(int(value))
    value = float(value)
    if math.isnan(value):
        return "nan"
    if math.isinf(value):
        return "inf" if value > 0 else "-inf"
    return repr(value)


def write_outliers(path, blocks):
    """Rule 11 and PM ruling Q2: one row per matched pair and property outside tolerance."""
    with open(path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(
            [
                "snapshot",
                "Type",
                "Posx",
                "Posy",
                "Posz",
                "SimulationHaloIndex",
                "property",
                "kind",
                "r",
                "m",
            ]
        )
        for snap, ref, block in blocks:
            for ds, kind, rows, r, m in block:
                typ = ref["Type"][rows]
                px, py, pz = (ref[d][rows] for d in KEY_POSITION_DATASETS)
                shi = ref["SimulationHaloIndex"][rows]
                for i in range(rows.size):
                    writer.writerow(
                        [
                            snap,
                            int(typ[i]),
                            _csv_number(px[i]),
                            _csv_number(py[i]),
                            _csv_number(pz[i]),
                            int(shi[i]),
                            ds,
                            kind,
                            _csv_number(r[i]),
                            _csv_number(m[i]),
                        ]
                    )


def _print_table(tables):
    print(
        "%5s %8s %8s %8s %7s  %s" % ("snap", "N_ref", "N_cand", "M", "result", "failed thresholds")
    )
    for t in tables:
        failed = t["failed_thresholds"]
        shown = ", ".join(failed[:6]) + (
            " ... (+%d)" % (len(failed) - 6) if len(failed) > 6 else ""
        )
        print(
            "%5d %8d %8d %8d %7s  %s"
            % (
                t["snapshot"],
                t["n_ref"],
                t["n_cand"],
                t["matched"],
                "pass" if t["passed"] else "fail",
                shown,
            )
        )


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------


def _snapshot_list(text):
    try:
        snaps = [int(s) for s in text.split(",")]
    except ValueError:
        raise argparse.ArgumentTypeError("snapshots must be comma-separated integers") from None
    if any(s < 0 for s in snaps) or len(set(snaps)) != len(snaps):
        raise argparse.ArgumentTypeError("snapshots must be distinct non-negative integers")
    return snaps


def _positive_int(text):
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError("must be a positive integer") from None
    if value < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return value


def build_parser():
    parser = argparse.ArgumentParser(
        prog="compare_sage26.py",
        description=(
            "Compare a Mimic sage26 HDF5 run (candidate) against one SAGE26 worker file "
            "(reference) galaxy by galaxy under the T1 comparison rules of "
            "models/sage26/instrument/README.md, using field_map.yaml beside this script."
        ),
        epilog=(
            "Exit status: 0 = pass (every threshold holds at every compared snapshot); "
            "1 = fail (at least one threshold does not hold); 2 = error (the instrument "
            "cannot measure: wrong reference digest, incomplete candidate, type mismatch, "
            "unmapped or absent field, snapshot mix-up, M = 0, unreadable file, or a usage "
            "error). An error writes the JSON with the verdict, message and input "
            "identities but no per-snapshot tables, and no outliers CSV. A pass whose "
            "inputs are not T1's (reference digest and all eight snapshots) prints "
            "'pass (not T1-eligible)'."
        ),
    )
    parser.add_argument(
        "--reference", required=True, metavar="FILE", help="SAGE26 worker file (model_0.hdf5)"
    )
    parser.add_argument(
        "--reference-sha256",
        required=True,
        metavar="HEX",
        help="expected SHA-256 of FILE; any other digest is an error before any snapshot is read",
    )
    parser.add_argument(
        "--candidate", required=True, metavar="DIR", help="Mimic HDF5 run output directory"
    )
    parser.add_argument(
        "--partitions",
        required=True,
        metavar="K",
        type=_positive_int,
        help="number of partition files DIR must hold: <basename>_000.hdf5 ... <basename>_<K-1>.hdf5 (8 for mini-Millennium)",
    )
    parser.add_argument(
        "--basename", default="model", help="candidate partition basename (default: model)"
    )
    parser.add_argument(
        "--snapshots",
        type=_snapshot_list,
        default=list(T1_SNAPSHOTS),
        metavar="LIST",
        help="comma-separated snapshots to compare (default: T1's eight, %s)"
        % ",".join(str(s) for s in T1_SNAPSHOTS),
    )
    parser.add_argument("--json", required=True, metavar="OUT.json", help="JSON report to write")
    parser.add_argument(
        "--outliers",
        required=True,
        metavar="OUT.csv",
        help="outliers CSV to write (pass or fail only)",
    )
    return parser


def run(args):
    """Run the comparison.

    Returns ``(exit_status, document, printed_verdict, results)``; ``results`` is
    ``(tables, outlier_blocks)`` for a pass or fail and ``None`` for an error.
    """
    document = {
        "instrument": "compare_sage26.py",
        "verdict": "error",
        "reference": {
            "path": os.path.abspath(args.reference),
            "sha256": None,
            "expected_sha256": args.reference_sha256,
        },
        "snapshots": list(args.snapshots),
        "partitions": args.partitions,
        "candidate": {
            "directory": os.path.abspath(args.candidate),
            "basename": args.basename,
            "files": [],
        },
        "field_map": {"path": FIELD_MAP_PATH, "sha256": None},
        "t1_eligible": False,
    }
    try:
        # Rule 1: reference identity, before any snapshot is read.
        digest = sha256_file(args.reference)
        document["reference"]["sha256"] = digest
        document["t1_eligible"] = digest == T1_REFERENCE_SHA256 and set(args.snapshots) == set(
            T1_SNAPSHOTS
        )
        if digest != args.reference_sha256.strip().lower():
            raise InstrumentError(
                "reference SHA-256 %s does not equal --reference-sha256 %s"
                % (digest, args.reference_sha256)
            )
        fmap, map_digest = load_field_map()
        document["field_map"]["sha256"] = map_digest

        # Rule 2: candidate completeness.
        paths = candidate_partition_files(args.candidate, args.basename, args.partitions)
        document["candidate"]["files"] = [
            {"name": os.path.basename(p), "sha256": sha256_file(p)} for p in paths
        ]

        tables, blocks = [], []
        try:
            ref_file = h5py.File(args.reference, "r")
        except OSError as exc:
            raise InstrumentError("cannot open reference %s: %s" % (args.reference, exc)) from exc
        handles = []
        try:
            for p in paths:
                try:
                    handles.append((p, h5py.File(p, "r")))
                except OSError as exc:
                    raise InstrumentError(
                        "cannot open candidate partition %s: %s" % (p, exc)
                    ) from exc
            for snap in args.snapshots:
                ref = read_reference_snapshot(ref_file, snap, fmap)
                cand = read_candidate_snapshot(handles, snap, fmap)
                table, outliers = compare_snapshot(snap, ref, cand, fmap)
                tables.append(table)
                blocks.append((snap, ref, outliers))
        finally:
            for _, h in handles:
                h.close()
            ref_file.close()
    except InstrumentError as exc:
        document["error"] = str(exc)
        return EXIT_ERROR, document, "error: %s" % exc, None

    passed = all(t["passed"] for t in tables)
    document["verdict"] = "pass" if passed else "fail"
    document["failures"] = [
        "Snap %d: %s" % (t["snapshot"], n) for t in tables for n in t["failed_thresholds"]
    ]
    document["snapshot_tables"] = tables
    if passed:
        printed = "pass" if document["t1_eligible"] else "pass (not T1-eligible)"
    else:
        printed = "fail"
    return (EXIT_PASS if passed else EXIT_FAIL), document, printed, (tables, blocks)


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        status, document, printed, results = run(args)
        if results is not None:
            tables, blocks = results
            write_outliers(args.outliers, blocks)
            _print_table(tables)
        write_json(args.json, document)
    except Exception as exc:  # noqa: BLE001 - any unexpected failure must be 'error', never exit 1
        print("error: internal failure: %s: %s" % (type(exc).__name__, exc))
        try:
            write_json(
                args.json,
                {
                    "instrument": "compare_sage26.py",
                    "verdict": "error",
                    "error": "internal failure: %s" % exc,
                },
            )
        except Exception:  # noqa: BLE001
            pass
        return EXIT_ERROR
    print("verdict: %s" % printed)
    return status


if __name__ == "__main__":
    sys.exit(main())
