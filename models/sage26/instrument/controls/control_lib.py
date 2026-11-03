"""Shared machinery for the T1 parity instrument's controls.

The control generators (``make_fixture.py``, ``alter_map.py``, ``to_mimic_layout.py``,
``perturb.py``) build inputs for ``compare_sage26.py`` and, for each control, an
``expect.json`` holding the comparator's command line and the outcome the control
expects. ``run_control.py`` then runs the comparator and checks its JSON against it.

This module holds what they share:

- ``Mapping``: one SAGE26 dataset paired with its Mimic property. The generators build
  the pairing themselves (the synthetic fixture from literal tables, the
  reference-derived controls from ``field_map.yaml``); nothing here reads the map.
- ``evaluate``: the control's own computation of the expected per-snapshot counts and
  verdict under the README's rules 5, 6 and 8. It is written separately from the
  comparator and does not match galaxies by key: each candidate row carries the
  reference row it was built from (its *origin*), or -1 when the control made it
  unmatched on purpose, so the expected pairing is known by construction.
- Writers for a SAGE26-layout reference file and a Mimic-layout candidate directory.

Every array a control builds is in *reference row order* (candidate row j was built
from reference row ``origin[j]``); ``write_candidate`` alone shuffles rows into
partitions, so the comparator never sees the construction order.
"""

import hashlib
import json
import os
from fractions import Fraction

import h5py
import numpy as np
from numpy.lib.recfunctions import repack_fields

#: T1's eight snapshots, in the comparator's default order.
T1_SNAPSHOTS = (63, 37, 32, 27, 23, 20, 18, 16)

#: Rule 6 and rule 8 constants, restated here so the expectation is the control's own.
REL_TOL = 1e-6
ZERO_BAND = 1e-10
COUNT_TOLERANCE = Fraction(1, 1000)
MATCH_MINIMUM = Fraction(999, 1000)
FLOAT_GATE = Fraction(9, 10)
INT_GATE = Fraction(995, 1000)

#: The fixed SAGE26 creation values of four uncompared datasets (rule 7).
CREATION_VALUES = {"mergeType": 0, "g_max": 0, "mergeIntoID": -1, "mergeIntoSnapNum": -1}

#: Seed of the documented shuffle that assigns candidate rows to partitions.
PARTITION_SEED = 20261103


class Mapping:
    """One compared SAGE26 dataset and the Mimic property it is compared with."""

    def __init__(self, ds, kind, sage26_type, mimic, component, mimic_type, factor):
        self.ds = ds
        self.kind = kind
        self.sage26_type = np.dtype(sage26_type)
        self.mimic = mimic
        self.component = component
        self.mimic_type = np.dtype(mimic_type)
        self.factor = float(factor)

    @property
    def wider(self):
        """True when Mimic stores the property wider than SAGE26 (rule 11)."""
        return self.mimic_type.itemsize > self.sage26_type.itemsize


def mimic_dtype(mappings):
    """The Mimic ``Galaxies`` compound type holding every mapped property once."""
    fields, seen = [], set()
    for mp in mappings:
        if mp.mimic in seen:
            continue
        seen.add(mp.mimic)
        shape = (3,) if mp.component is not None else ()
        fields.append((mp.mimic, mp.mimic_type.newbyteorder("<"), shape))
    return np.dtype(fields)


# ---------------------------------------------------------------------------
# The control's own evaluation
# ---------------------------------------------------------------------------


def _within(r, m):
    """Rule 6's within-tolerance test, pair by pair, on float64 arrays."""
    nan_r, nan_m = np.isnan(r), np.isnan(m)
    inf_r, inf_m = np.isinf(r), np.isinf(m)
    finite = ~(nan_r | inf_r) & ~(nan_m | inf_m)
    with np.errstate(all="ignore"):
        ok = nan_r & nan_m
        ok = ok | (inf_r & inf_m & (np.sign(r) == np.sign(m)))
        ok = ok | (finite & (r == 0) & (np.abs(m) <= ZERO_BAND))
        ok = ok | (finite & (r != 0) & (np.abs(m - r) <= REL_TOL * np.abs(r)))
    return ok


def _significant(r, m):
    """Rule 5's M_sig cohort."""
    with np.errstate(all="ignore"):
        return (np.abs(r) > ZERO_BAND) | (np.abs(m) > ZERO_BAND) | ~np.isfinite(r) | ~np.isfinite(m)


def _bits(values, dtype):
    """Bit patterns at ``dtype`` with -0.0 read as +0.0."""
    v = np.asarray(values).astype(dtype)
    v = np.where(v == 0, dtype.type(0), v)
    return v.view(np.uint32 if dtype.itemsize == 4 else np.uint64)


def candidate_column(cand, mp):
    """The candidate's stored values for one mapping (one component of a vector)."""
    col = cand[mp.mimic]
    return col[:, mp.component] if mp.component is not None else col


def _python_number(value):
    if isinstance(value, (np.integer, int)):
        return int(value)
    value = float(value)
    if np.isnan(value):
        return "nan"
    if np.isinf(value):
        return "inf" if value > 0 else "-inf"
    return value


def evaluate_snapshot(snap, ref, cand, origin, mappings, uncompared):
    """Expected table for one snapshot.

    ``ref`` maps SAGE26 dataset names to arrays; ``cand`` is the candidate's structured
    array; ``origin[j]`` is the reference row candidate row j pairs with, or -1.
    Returns ``(table, failed_threshold_names)``.
    """
    n_ref = int(ref["SnapNum"].shape[0])
    n_cand = int(cand.shape[0])
    ci = np.nonzero(origin >= 0)[0]
    ri = origin[ci]
    if np.unique(ri).size != ri.size:
        raise AssertionError("control construction pairs a reference row twice")
    matched = int(ri.size)
    failed = []
    if abs(n_cand - n_ref) * COUNT_TOLERANCE.denominator > COUNT_TOLERANCE.numerator * n_ref:
        failed.append("count")
    if matched * MATCH_MINIMUM.denominator < MATCH_MINIMUM.numerator * n_ref:
        failed.append("matching_vs_n_ref")
    if matched * MATCH_MINIMUM.denominator < MATCH_MINIMUM.numerator * n_cand:
        failed.append("matching_vs_n_cand")

    properties = {}
    for mp in mappings:
        r_stored = ref[mp.ds][ri]
        m_stored = candidate_column(cand, mp)[ci]
        if mp.kind == "int":
            equal = int((r_stored.astype(np.int64) == m_stored.astype(np.int64)).sum())
            properties[mp.ds] = {"exact_equal": equal}
            if equal * INT_GATE.denominator < INT_GATE.numerator * matched:
                failed.append("int:%s" % mp.ds)
            continue
        r = r_stored.astype(np.float64)
        with np.errstate(all="ignore"):
            m_unrounded = m_stored.astype(np.float64) * np.float64(mp.factor)
            if mp.sage26_type.itemsize == 4:
                m = m_unrounded.astype(np.float32).astype(np.float64)
            else:
                m = m_unrounded
        within = _within(r, m)
        sig = _significant(r, m)
        n_within = int(within.sum())
        m_sig = int(sig.sum())
        n_within_sig = int((within & sig).sum())
        n_bits = int((_bits(r_stored, mp.sage26_type) == _bits(m, mp.sage26_type)).sum())
        entry = {
            "within_tolerance": n_within,
            "M_sig": m_sig,
            "within_tolerance_sig": n_within_sig,
            "bit_identical": n_bits,
        }
        if mp.wider:
            entry["within_tolerance_unrounded"] = int(_within(r, m_unrounded).sum())
        properties[mp.ds] = entry
        if n_within * FLOAT_GATE.denominator < FLOAT_GATE.numerator * matched:
            failed.append("float:%s:over_M" % mp.ds)
        if m_sig and n_within_sig * FLOAT_GATE.denominator < FLOAT_GATE.numerator * m_sig:
            failed.append("float:%s:over_M_sig" % mp.ds)

    summaries = {}
    for ds in uncompared:
        v = ref[ds]
        s = {
            "nonzero": int((v != 0).sum()),
            "min": _python_number(v.min()),
            "max": _python_number(v.max()),
        }
        if ds in CREATION_VALUES:
            s["creation_value"] = CREATION_VALUES[ds]
            s["differ_from_creation"] = int((v != CREATION_VALUES[ds]).sum())
        summaries[ds] = s

    table = {
        "n_ref": n_ref,
        "n_cand": n_cand,
        "matched": matched,
        "unmatched_reference": n_ref - matched,
        "unmatched_candidate": n_cand - matched,
        "properties": properties,
        "uncompared": summaries,
    }
    return table, failed


def evaluate(snapshots, mappings, uncompared):
    """Expected tables, failures and verdict for a pass-or-fail control.

    ``snapshots`` is an ordered list of ``(snap, ref, cand, origin)``.
    """
    tables, failures = {}, []
    for snap, ref, cand, origin in snapshots:
        table, failed = evaluate_snapshot(snap, ref, cand, origin, mappings, uncompared)
        tables[str(snap)] = table
        failures.extend("Snap %d: %s" % (snap, name) for name in failed)
    return {"tables": tables, "failures": failures, "verdict": "fail" if failures else "pass"}


# ---------------------------------------------------------------------------
# Writers
# ---------------------------------------------------------------------------


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def write_reference(path, snapshots, order):
    """Write a SAGE26-layout worker file: ``Snap_<N>`` groups of 1-D datasets.

    ``snapshots`` maps a snapshot number to ``{dataset: array}``; ``order`` is the
    dataset order to write.
    """
    with h5py.File(path, "w") as f:
        for snap, data in snapshots.items():
            group = f.create_group("Snap_%d" % snap)
            for ds in order:
                group.create_dataset(ds, data=data[ds])


def partition_rows(n, partitions, snap):
    """The documented shuffle: the rows partition p receives, for a table of n rows.

    Rows are permuted with ``numpy.random.default_rng(PARTITION_SEED + snap)
    .permutation(n)`` and the permutation is cut into ``partitions`` contiguous,
    near-equal chunks with ``numpy.array_split``; partition p holds chunk p, in order.
    """
    perm = np.random.default_rng(PARTITION_SEED + snap).permutation(n)
    return np.array_split(perm, partitions)


def write_candidate(
    directory,
    tables,
    partitions,
    basename="model",
    skip_partition=None,
    extra_partition=False,
    drop_field=None,
):
    """Write a Mimic-layout candidate: ``<basename>_<NNN>.hdf5`` partition files.

    ``tables`` maps a snapshot number to the candidate's structured array (rows in any
    order); each table's rows are spread over the partitions by ``partition_rows``.
    Options build the partition-defect controls: ``skip_partition`` leaves one file
    unwritten, ``extra_partition`` also writes ``<basename>_<K>.hdf5`` (a copy of the
    last partition's tables), and ``drop_field = (partition, snap, field)`` writes that
    one partition's table without that field.
    """
    os.makedirs(directory)
    contents = {p: {} for p in range(partitions)}
    for snap, table in tables.items():
        for p, rows in enumerate(partition_rows(table.shape[0], partitions, snap)):
            part = table[rows]
            if drop_field is not None and drop_field[:2] == (p, snap):
                keep = [n for n in part.dtype.names if n != drop_field[2]]
                part = repack_fields(part[keep])
            contents[p][snap] = part
    if extra_partition:
        contents[partitions] = contents[partitions - 1]
    for p, snaps in contents.items():
        if p == skip_partition:
            continue
        with h5py.File(os.path.join(directory, "%s_%03d.hdf5" % (basename, p)), "w") as f:
            for snap, part in snaps.items():
                f.create_dataset("Snap%03d/Galaxies" % snap, data=part)


def write_expectation(control_dir, document):
    """Write ``expect.json`` for one control."""
    with open(os.path.join(control_dir, "expect.json"), "w", encoding="utf-8") as fh:
        json.dump(document, fh, indent=1, allow_nan=False)
        fh.write("\n")


def expectation(name, description, comparator, reference, reference_sha256, candidate, **kw):
    """The common skeleton of an ``expect.json`` document.

    Keyword arguments: ``partitions`` (int), ``snapshots`` (None for the default, or a
    comma-separated string), and ``expect`` (the expected-outcome block read by
    ``run_control.py``).
    """
    return {
        "control": name,
        "description": description,
        "comparator": os.path.abspath(comparator),
        "args": {
            "reference": os.path.abspath(reference),
            "reference_sha256": reference_sha256,
            "candidate": os.path.abspath(candidate),
            "partitions": kw["partitions"],
            "snapshots": kw.get("snapshots"),
        },
        "expect": kw["expect"],
    }
