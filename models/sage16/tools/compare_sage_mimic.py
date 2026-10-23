#!/usr/bin/env python3
"""
Compare native SAGE HDF5 output with Mimic `sage16` HDF5 output, galaxy by galaxy.

A reporting tool: it prints a table and writes a JSON record of measured statistics, and
leaves verdict thresholds to the caller.

Native layouts
  SAGE : `model_<N>.hdf5` with `Snap_<snap>/<field>` datasets, or the master `model.hdf5`
         with `Core_<N>/Snap_<snap>/<field>` external links into the shards.
  Mimic: `model_<NNN>.hdf5` with a compound `Snap<snap:03d>/Galaxies` dataset, or the master
         `model.hdf5` with `Snap<snap:03d>/File<NNN>/Galaxies` external links.
  Shards are used when present; the master only if there are none.

Matching: the exact float32-bit tuple (Posx, Posy, Posz, Len, Type) is primary, cross-checked
against SAGE `SimulationHaloIndex` <-> Mimic `MostBoundID`.  A key matches only when unique on
both sides; duplicated keys are excluded and counted, and the agreement of the two keys is
reported.  Every field present on both sides is then compared over the matched galaxies
(explicit renames, vectors, a composite and a unit transform are mapped below); fields with no
counterpart are listed.

Statistics: frac_bit_identical (the wider float type is rounded to float32 first;
frac_identical_unrounded is the exact-equality fraction without rounding), median/p99/max
relative difference, frac_within_tol (rdiff <= RTOL or |a-b| <= ATOL), and exact equality for
integer fields.  Within-tolerance and bit-identical fractions are different statistics: a
one-ULP difference is within tolerance but not bit-identical.  Never quote one as the other.

Usage
    python compare_sage_mimic.py --sage-dir DIR --mimic-dir DIR --snapshots 63 37 --out r.json
    python compare_sage_mimic.py --sage-dir DIR --self-test --snapshots 63 --out self.json

`--snapshots` takes numbers (space or comma separated) or `all`.  `--self-test` compares the
SAGE directory with itself, so every property must be 100% bit-identical.  Exit status is 2 if
the inputs cannot be read or any requested snapshot fails, else 0.
"""

import argparse
import glob
import json
import os
import sys

import h5py
import numpy as np

# The only tolerances: defaults for --rtol/--atol, and a reporting threshold for "large"
RTOL = 1e-6
ATOL = 1e-10
LARGE_RDIFF = 1e-3
N_LIST = 20  # cap on unmatched / worst galaxies listed in the JSON

# SAGE name -> Mimic name
RENAMES = {
    "DiskRadius": "DiskScaleRadius",
    "EjectedMass": "EjectedGas",
    "IntraClusterStars": "ICS",
    "MetalsEjectedMass": "MetalsEjectedGas",
    "MetalsIntraClusterStars": "MetalsICS",
    "OutflowRate": "SupernovaOutflowRate",
}
# SAGE scalar name -> (Mimic vector field, component)
VECTORS = {
    "Posx": ("Pos", 0),
    "Posy": ("Pos", 1),
    "Posz": ("Pos", 2),
    "Velx": ("Vel", 0),
    "Vely": ("Vel", 1),
    "Velz": ("Vel", 2),
    "Spinx": ("Spin", 0),
    "Spiny": ("Spin", 1),
    "Spinz": ("Spin", 2),
}
# SAGE rounds each term to float32 before summing, so this pair cannot be bit-comparable
COMPOSITES = {("SfrDisk", "SfrBulge"): "StarFormationRate"}
# SAGE writes Myr with a negative "unset" sentinel (-978028.5); Mimic writes Gyr with 0
MERGER_TIME_FIELDS = ("TimeOfLastMajorMerger", "TimeOfLastMinorMerger")
# Secondary matching key, also compared as an integer field
ID_KEY = ("SimulationHaloIndex", "MostBoundID")


# --- File discovery and loading ---
def _shard_sort_key(path):
    """Sort `model_<N>.hdf5` numerically by N."""
    stem = os.path.splitext(os.path.basename(path))[0]
    tail = stem.rsplit("_", 1)[-1]
    return (int(tail), path) if tail.isdigit() else (10**9, path)


def discover_files(directory):
    """Return (files, layout, warnings): shards if present, else the master file."""
    shards = sorted(glob.glob(os.path.join(directory, "model_[0-9]*.hdf5")), key=_shard_sort_key)
    master = os.path.join(directory, "model.hdf5")
    warnings = []
    if shards:
        if os.path.exists(master):
            try:
                with h5py.File(master, "r") as f:
                    n_core = sum(1 for k in f if k.startswith("Core_"))
                    if n_core == 0:  # Mimic master: count File<NNN> groups of any snapshot
                        for k in f:
                            if k.startswith("Snap") and isinstance(f[k], h5py.Group):
                                n_core = sum(1 for kk in f[k] if kk.startswith("File"))
                                break
                if n_core and n_core != len(shards):
                    warnings.append(
                        f"{directory}: master lists {n_core} cores/files but {len(shards)} shards found"
                    )
            except OSError as exc:
                warnings.append(f"{directory}: cannot inspect master file ({exc})")
        return shards, "shards", warnings
    if os.path.exists(master):
        return [master], "master", warnings
    raise FileNotFoundError(f"no model_<N>.hdf5 or model.hdf5 in {directory}")


def available_snapshots(files, fmt):
    """Snapshot numbers present in the given files (fmt: 'sage' or 'mimic')."""
    found = set()
    for path in files:
        with h5py.File(path, "r") as f:
            for key in f:
                if fmt == "sage":
                    if key.startswith("Snap_"):
                        found.add(int(key[5:]))
                    elif key.startswith("Core_"):
                        found.update(int(k[5:]) for k in f[key] if k.startswith("Snap_"))
                elif key.startswith("Snap") and key[4:].isdigit():
                    found.add(int(key[4:]))
    return sorted(found)


def load_sage(files, snapshot):
    """Return ({field: ndarray in stored dtype}, n_files_with_data) for one snapshot."""
    parts, used, schema = [], 0, None
    for path in files:
        with h5py.File(path, "r") as f:
            groups = []
            if f"Snap_{snapshot}" in f:
                groups.append(f[f"Snap_{snapshot}"])
            else:
                groups.extend(
                    f[c][f"Snap_{snapshot}"]
                    for c in sorted(f)
                    if c.startswith("Core_") and f"Snap_{snapshot}" in f[c]
                )
            for g in groups:
                names = sorted(k for k in g if isinstance(g[k], h5py.Dataset))
                if schema is None:
                    schema = names
                elif names != schema:
                    raise ValueError(f"SAGE field set differs between files ({path})")
                lengths = {int(g[k].shape[0]) for k in names}
                if len(lengths) != 1:
                    raise ValueError(f"SAGE datasets of unequal length in {path}")
                if lengths.pop() > 0:
                    parts.append({k: g[k][...] for k in names})
                    used += 1
    if schema is None:
        raise KeyError(f"Snap_{snapshot} not found")
    if not parts:
        return {k: np.empty(0) for k in schema}, 0
    return {k: np.concatenate([p[k] for p in parts]) for k in schema}, used


def load_mimic(files, snapshot):
    """Return ({field: ndarray (vector fields are (N,3))}, n_files_with_data)."""
    tag = f"Snap{snapshot:03d}"
    parts, used, dtype = [], 0, None
    found = False
    for path in files:
        with h5py.File(path, "r") as f:
            if tag not in f:
                continue
            found = True
            g = f[tag]
            dsets = (
                [g["Galaxies"]]
                if "Galaxies" in g
                else [
                    g[k]["Galaxies"]
                    for k in sorted(g)
                    if k.startswith("File") and "Galaxies" in g[k]
                ]
            )
            for d in dsets:
                if dtype is None:
                    dtype = d.dtype
                elif d.dtype != dtype:
                    raise ValueError(f"Mimic record dtype differs between files ({path})")
                if d.shape[0] > 0:
                    parts.append(d[...])
                    used += 1
    if not found:
        raise KeyError(f"{tag} not found")
    rec = np.concatenate(parts) if parts else np.empty(0, dtype=dtype)
    return {n: rec[n] for n in rec.dtype.names}, used


# --- Matching ---
def tuple_key(px, py, pz, length, typ):
    """Exact matching key: float32 bit patterns of Pos, plus Len and Type (int32)."""
    key = np.empty(
        len(px), dtype=[("x", "<u4"), ("y", "<u4"), ("z", "<u4"), ("l", "<i4"), ("t", "<i4")]
    )
    for name, arr in (("x", px), ("y", py), ("z", pz)):
        key[name] = np.ascontiguousarray(arr, dtype="<f4").view("<u4")
    key["l"] = length
    key["t"] = typ
    return key


def match_keys(ka, kb):
    """Match rows whose key is unique on BOTH sides; return index arrays ia, ib and statistics."""
    na, nb = len(ka), len(kb)
    uniq, inv = np.unique(np.concatenate([ka, kb]), return_inverse=True)
    inv = inv.ravel()
    ca = np.bincount(inv[:na], minlength=len(uniq))
    cb = np.bincount(inv[na:], minlength=len(uniq))
    first_a = np.full(len(uniq), -1, dtype=np.int64)
    first_b = np.full(len(uniq), -1, dtype=np.int64)
    first_a[inv[:na][::-1]] = np.arange(na)[::-1]  # lowest row index wins
    first_b[inv[na:][::-1]] = np.arange(nb)[::-1]
    ok = (ca == 1) & (cb == 1)
    ia, ib = first_a[ok], first_b[ok]
    order = np.argsort(ia)
    ia, ib = ia[order], ib[order]
    return {
        "ia": ia,
        "ib": ib,
        "stats": {
            "n_sage": na,
            "n_mimic": nb,
            "n_unique_keys_sage": int(np.sum(ca > 0)),
            "n_unique_keys_mimic": int(np.sum(cb > 0)),
            "n_duplicated_keys_sage": int(np.sum(ca > 1)),
            "n_rows_in_duplicated_keys_sage": int(ca[ca > 1].sum()),
            "n_duplicated_keys_mimic": int(np.sum(cb > 1)),
            "n_rows_in_duplicated_keys_mimic": int(cb[cb > 1].sum()),
            "is_unique": bool(np.all(ca <= 1) and np.all(cb <= 1)),
            "n_matched": int(len(ia)),
            "match_rate_sage": len(ia) / na if na else None,
            "match_rate_mimic": len(ib) / nb if nb else None,
            "n_sage_only": int(na - len(ia)),
            "n_mimic_only": int(nb - len(ib)),
        },
    }


def key_agreement(m_tuple, m_id):
    """Compare the (SAGE row -> Mimic row) assignments of the two matchings."""
    a = dict(zip(m_tuple["ia"].tolist(), m_tuple["ib"].tolist()))
    b = dict(zip(m_id["ia"].tolist(), m_id["ib"].tolist()))
    both = a.keys() & b.keys()
    same = sum(1 for k in both if a[k] == b[k])
    return {
        "n_matched_by_both": len(both),
        "n_same_partner": same,
        "n_different_partner": len(both) - same,
        "n_matched_by_tuple_only": len(a.keys() - b.keys()),
        "n_matched_by_id_only": len(b.keys() - a.keys()),
    }


# --- Field mapping ---
def build_pairs(sage, mimic, self_test=False):
    """Return (pairs, sage_only, mimic_only): comparable full-length arrays and unmapped fields."""
    pairs = []
    s_used, m_used = set(), set()

    def add(label, kind, s_names, m_name, a, b, note="", bit_comparable=True, sentinel=None):
        pairs.append(
            dict(
                label=label,
                kind=kind,
                sage=list(s_names),
                mimic=m_name,
                a=a,
                b=b,
                note=note,
                bit_comparable=bit_comparable,
                sentinel=sentinel,
            )
        )
        s_used.update(s_names)
        m_used.add(m_name.split("[")[0])

    if self_test:  # identity mapping over every SAGE field
        for name in sorted(sage):
            add(name, "identity", [name], name, sage[name], mimic[name])
        return pairs, [], []

    if ID_KEY[0] in sage and ID_KEY[1] in mimic:
        add(ID_KEY[0], "id_key", [ID_KEY[0]], ID_KEY[1], sage[ID_KEY[0]], mimic[ID_KEY[1]])

    for terms, m_name in COMPOSITES.items():
        if all(t in sage for t in terms) and m_name in mimic:
            total = sage[terms[0]].astype(np.float32)
            for t in terms[1:]:
                total = total + sage[t].astype(np.float32)
            add(
                " + ".join(terms),
                "composite",
                terms,
                m_name,
                total,
                mimic[m_name],
                note="float32 sum of SAGE terms; not bit-comparable by construction",
                bit_comparable=False,
            )

    for s_name, (m_name, comp) in VECTORS.items():
        if s_name in sage and m_name in mimic:
            add(
                s_name,
                "vector",
                [s_name],
                f"{m_name}[{comp}]",
                sage[s_name],
                mimic[m_name][:, comp],
            )
    m_used.update(m for m, _ in VECTORS.values() if m in mimic)

    # Sentinel (<0) maps to 0; otherwise float32 Myr / 1000 -> Gyr
    for name in MERGER_TIME_FIELDS:
        if name in sage and name in mimic:
            raw = sage[name].astype(np.float32)
            sentinel = raw < 0
            conv = np.where(sentinel, np.float32(0.0), raw / np.float32(1000.0)).astype(np.float32)
            add(
                name,
                "unit_transform",
                [name],
                name,
                conv,
                mimic[name],
                note="SAGE Myr -> Gyr (float32 /1000); SAGE value<0 (sentinel) -> 0",
                sentinel=sentinel,
            )

    for s_name, m_name in RENAMES.items():
        if s_name in sage and m_name in mimic:
            add(
                s_name,
                "rename",
                [s_name],
                m_name,
                sage[s_name],
                mimic[m_name],
                note=f"renamed to {m_name}",
            )

    for name in sorted(set(sage) - s_used):
        if name in mimic and name not in m_used:
            add(name, "direct", [name], name, sage[name], mimic[name])

    return pairs, sorted(set(sage) - s_used), sorted(set(mimic) - m_used)


# --- Property comparison ---
def _json_float(x):
    """float -> JSON-safe value ('inf'/'nan' as strings)."""
    x = float(x)
    return x if np.isfinite(x) else ("inf" if np.isinf(x) and x > 0 else "nan")


def _quantiles(r):
    """Return (median, 99th percentile, max) of r, or Nones when r is empty."""
    if len(r) == 0:
        return None, None, None
    with np.errstate(invalid="ignore"):  # percentile interpolation next to inf
        return float(np.median(r)), float(np.percentile(r, 99)), float(np.max(r))


def _bits(x):
    """Integer bit patterns of a float array, with -0.0 normalised to +0.0."""
    x = x + x.dtype.type(0.0)  # -0.0 + 0.0 = +0.0; NaN stays NaN
    return x.view(np.uint32 if x.dtype.itemsize == 4 else np.uint64)


def compare_pair(a, b, rtol, atol, sentinel=None, bit_comparable=True):
    """Return (stats, outside-tolerance mask, not-bit-identical mask) for matched a (SAGE), b."""
    n = len(a)
    stats = {
        "n": n,
        "dtype_sage": str(a.dtype),
        "dtype_mimic": str(b.dtype),
        "bit_comparable": bit_comparable,
    }
    a_int, b_int = np.issubdtype(a.dtype, np.integer), np.issubdtype(b.dtype, np.integer)

    if a_int and b_int:
        eq = a == b
        stats.update(
            is_integer=True,
            frac_bit_identical=float(eq.mean()) if n else None,
            n_not_identical=int((~eq).sum()),
            max_abs_diff=int(np.max(np.abs(a.astype(np.int64) - b.astype(np.int64)))) if n else 0,
            frac_within_tol=float(eq.mean()) if n else None,
        )
        return stats, ~eq, ~eq

    # An int/float mix is compared as float64
    stats["is_integer"] = False
    if a_int or b_int:
        a, b = a.astype(np.float64), b.astype(np.float64)
        stats["note_mixed_int_float"] = True
    cmp_dtype = np.float32 if min(a.dtype.itemsize, b.dtype.itemsize) == 4 else np.float64
    stats["compare_dtype"] = np.dtype(cmp_dtype).name
    a_c, b_c = a.astype(cmp_dtype), b.astype(cmp_dtype)

    eq_bits = (_bits(a_c) == _bits(b_c)) | (np.isnan(a_c) & np.isnan(b_c))
    if a.dtype != b.dtype:  # exact equality without rounding the wider side
        a64, b64 = a.astype(np.float64), b.astype(np.float64)
        stats["frac_identical_unrounded"] = (
            float(((a64 == b64) | (np.isnan(a64) & np.isnan(b64))).mean()) if n else None
        )

    x, y = a_c.astype(np.float64), b_c.astype(np.float64)
    both_nan = np.isnan(x) & np.isnan(y)
    equal = (x == y) | both_nan
    with np.errstate(invalid="ignore", divide="ignore"):
        diff = np.abs(x - y)
        r = diff / np.maximum(np.abs(x), np.abs(y))
    r[equal] = 0.0
    nonfinite_mismatch = ~equal & ~np.isfinite(r)
    r[nonfinite_mismatch] = np.inf
    within = equal | (r <= rtol) | (diff <= atol)
    differing = ~eq_bits

    med, p99, mx = _quantiles(r)
    med_d, p99_d, mx_d = _quantiles(r[differing])
    stats.update(
        frac_bit_identical=float(eq_bits.mean()) if n else None,
        n_not_identical=int(differing.sum()),
        median_rdiff=_json_float(med) if med is not None else None,
        p99_rdiff=_json_float(p99) if p99 is not None else None,
        max_rdiff=_json_float(mx) if mx is not None else None,
        median_rdiff_differing=_json_float(med_d) if med_d is not None else None,
        p99_rdiff_differing=_json_float(p99_d) if p99_d is not None else None,
        max_rdiff_differing=_json_float(mx_d) if mx_d is not None else None,
        frac_within_tol=float(within.mean()) if n else None,
        n_outside_tol=int((~within).sum()),
        n_rdiff_gt_large=int((r > LARGE_RDIFF).sum()),
        n_nonfinite_mismatch=int(nonfinite_mismatch.sum()),
    )
    if sentinel is not None:  # SAGE-sentinel pairs that map onto Mimic's 0
        sent_pair = sentinel & (b_c == 0)
        stats["n_sentinel_pairs"] = int(sent_pair.sum())
        stats["n_sage_sentinel"] = int(sentinel.sum())
        rest = ~sent_pair
        stats["frac_bit_identical_excl_sentinel_pairs"] = (
            float(eq_bits[rest].mean()) if rest.any() else None
        )
    return stats, ~within, differing


# --- Per-snapshot driver ---
def unmatched_rows(data, idx, sage_side):
    """Describe up to N_LIST unmatched rows (position, Len, Type, Mvir, ID)."""
    rows = []
    for i in idx[:N_LIST]:
        if sage_side:
            pos = [data["Posx"][i], data["Posy"][i], data["Posz"][i]]
            ident = data["SimulationHaloIndex"][i]
        else:
            pos = list(data["Pos"][i])
            ident = data["MostBoundID"][i]
        rows.append(
            {
                "row": int(i),
                "Type": int(data["Type"][i]),
                "Len": int(data["Len"][i]),
                "Mvir": float(data["Mvir"][i]),
                "Pos": [float(p) for p in pos],
                "halo_id": int(ident),
            }
        )
    return rows


def count_by_type(types):
    """Return {galaxy Type: count}."""
    u, c = np.unique(types, return_counts=True)
    return {int(k): int(v) for k, v in zip(u, c)}


def compare_snapshot(snap, sage_files, mimic_files, rtol, atol, self_test):
    """Match and compare one snapshot; return the JSON-ready result dict."""
    sage, n_sage_files = load_sage(sage_files, snap)
    if self_test:
        mimic, n_mimic_files = sage, n_sage_files
        m_pos = [mimic[k] for k in ("Posx", "Posy", "Posz")]
        m_id = mimic["SimulationHaloIndex"]
    else:
        mimic, n_mimic_files = load_mimic(mimic_files, snap)
        m_pos = [mimic["Pos"][:, i] for i in range(3)]
        m_id = mimic["MostBoundID"]
    n_s, n_m = len(sage["Type"]), len(mimic["Type"])
    res = {
        "snapshot": snap,
        "n_sage": n_s,
        "n_mimic": n_m,
        "count_difference": n_s - n_m,
        "n_files_with_data": {"sage": n_sage_files, "mimic": n_mimic_files},
        "counts_by_type": {
            "sage": count_by_type(sage["Type"]),
            "mimic": count_by_type(mimic["Type"]),
        },
    }

    kt_s = tuple_key(sage["Posx"], sage["Posy"], sage["Posz"], sage["Len"], sage["Type"])
    kt_m = tuple_key(m_pos[0], m_pos[1], m_pos[2], mimic["Len"], mimic["Type"])
    m_tuple = match_keys(kt_s, kt_m)
    m_idk = match_keys(sage["SimulationHaloIndex"].astype(np.int64), m_id.astype(np.int64))
    if m_tuple["stats"]["is_unique"]:
        primary, chosen = "tuple", m_tuple
    elif m_idk["stats"]["is_unique"]:
        primary, chosen = "halo_id", m_idk
    else:
        primary, chosen = "tuple_unique_rows_only", m_tuple
    res["matching"] = {
        "primary": primary,
        "tuple": m_tuple["stats"],
        "halo_id": m_idk["stats"],
        "agreement_tuple_vs_halo_id": key_agreement(m_tuple, m_idk),
    }
    ia, ib = chosen["ia"], chosen["ib"]
    matched_s = np.zeros(n_s, bool)
    matched_s[ia] = True
    matched_m = np.zeros(n_m, bool)
    matched_m[ib] = True
    res["unmatched"] = {
        "sage_only": unmatched_rows(sage, np.flatnonzero(~matched_s), True),
        "mimic_only": unmatched_rows(mimic, np.flatnonzero(~matched_m), self_test),
    }

    pairs, sage_only, mimic_only = build_pairs(sage, mimic, self_test)
    props, n_out = [], np.zeros(len(ia), np.int32)
    n_notid = np.zeros(len(ia), np.int32)
    out_names = [[] for _ in range(len(ia))]
    for p in pairs:
        sent = p["sentinel"][ia] if p["sentinel"] is not None else None
        st, outside, notid = compare_pair(
            p["a"][ia], p["b"][ib], rtol, atol, sent, p["bit_comparable"]
        )
        st.update(
            label=p["label"],
            kind=p["kind"],
            sage_fields=p["sage"],
            mimic_field=p["mimic"],
            note=p["note"],
        )
        props.append(st)
        if p["bit_comparable"]:
            n_notid += notid
        n_out += outside
        for g in np.flatnonzero(outside):
            out_names[g].append(p["label"])
    res["properties"] = props
    res["sage_only_fields"] = sage_only
    res["mimic_only_fields"] = mimic_only
    worst = np.argsort(-n_out, kind="stable")[:N_LIST]
    res["galaxies"] = {
        "n_matched": int(len(ia)),
        "n_with_any_property_outside_tol": int((n_out > 0).sum()),
        "n_with_any_property_not_bit_identical": int((n_notid > 0).sum()),
        "worst": [
            {
                "sage_row": int(ia[g]),
                "mimic_row": int(ib[g]),
                "Type": int(sage["Type"][ia[g]]),
                "halo_id": int(sage["SimulationHaloIndex"][ia[g]]),
                "n_properties_outside_tol": int(n_out[g]),
                "properties": out_names[g],
            }
            for g in worst
            if n_out[g] > 0
        ],
    }
    return res


# --- Output ---
def fmt_pct(x):
    """Format a fraction as a percentage column."""
    return "   n/a " if x is None else f"{100.0 * x:7.3f}"


def fmt_num(x):
    """Format a statistic (number, 'inf'/'nan' string or None) for a table column."""
    if x is None:
        return "      -"
    if isinstance(x, str):
        return f"{x:>9s}"
    return f"{x:9.2e}"


def print_snapshot(res):
    """Print the compact per-snapshot table."""
    mt, mi, ag = (
        res["matching"]["tuple"],
        res["matching"]["halo_id"],
        res["matching"]["agreement_tuple_vs_halo_id"],
    )
    print(
        f"\n=== Snapshot {res['snapshot']}: SAGE {res['n_sage']:,}  Mimic {res['n_mimic']:,}  "
        f"(diff {res['count_difference']:+d})   types SAGE {res['counts_by_type']['sage']} "
        f"Mimic {res['counts_by_type']['mimic']}"
    )
    for name, m in (("tuple ", mt), ("haloID", mi)):
        print(
            f"  match {name}: matched {m['n_matched']:,}  rate SAGE {fmt_pct(m['match_rate_sage'])}% "
            f"Mimic {fmt_pct(m['match_rate_mimic'])}%  dup keys S/M "
            f"{m['n_duplicated_keys_sage']}/{m['n_duplicated_keys_mimic']}  "
            f"unmatched S/M {m['n_sage_only']}/{m['n_mimic_only']}"
        )
    print(
        f"  agreement: both {ag['n_matched_by_both']:,}  same partner {ag['n_same_partner']:,}  "
        f"different {ag['n_different_partner']}  tuple-only {ag['n_matched_by_tuple_only']}  "
        f"id-only {ag['n_matched_by_id_only']}   PRIMARY = {res['matching']['primary']}"
    )
    print(
        f"  {'property':<28s}{'kind':<15s}{'%ident':>8s}{'median':>10s}{'p99':>10s}{'max':>10s}"
        f"{'%in-tol':>9s}{'#out':>7s}"
    )
    for p in res["properties"]:
        flag = "" if p["bit_comparable"] else " *"
        print(
            f"  {p['label']:<28s}{p['kind']:<15s}{fmt_pct(p['frac_bit_identical']):>8s}"
            f"{fmt_num(p.get('median_rdiff')):>10s}{fmt_num(p.get('p99_rdiff')):>10s}"
            f"{fmt_num(p.get('max_rdiff')):>10s}{fmt_pct(p['frac_within_tol']):>9s}"
            f"{p.get('n_outside_tol', p['n_not_identical']):>7d}{flag}"
        )
    if res["sage_only_fields"]:
        print(f"  SAGE-only fields : {', '.join(res['sage_only_fields'])}")
    if res["mimic_only_fields"]:
        print(f"  Mimic-only fields: {', '.join(res['mimic_only_fields'])}")
    g = res["galaxies"]
    print(
        f"  galaxies: {g['n_with_any_property_outside_tol']:,}/{g['n_matched']:,} have a property "
        f"outside tolerance; {g['n_with_any_property_not_bit_identical']:,} not fully bit-identical "
        f"(* = not bit-comparable by construction, excluded from the latter)"
    )


def parse_snapshots(tokens):
    """Parse --snapshots tokens into a list of ints, or 'all'."""
    if len(tokens) == 1 and tokens[0].lower() == "all":
        return "all"
    out = []
    for t in tokens:
        out.extend(int(x) for x in t.split(",") if x.strip())
    return out


def main(argv=None):
    """Command-line entry point; return the process exit status."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sage-dir", required=True)
    ap.add_argument("--mimic-dir")
    ap.add_argument("--snapshots", nargs="+", required=True, help="numbers or 'all'")
    ap.add_argument("--out", required=True, help="JSON output path")
    ap.add_argument("--rtol", type=float, default=RTOL)
    ap.add_argument("--atol", type=float, default=ATOL)
    ap.add_argument(
        "--self-test", action="store_true", help="compare the SAGE directory with itself"
    )
    args = ap.parse_args(argv)
    if not args.self_test and not args.mimic_dir:
        ap.error("--mimic-dir is required unless --self-test")

    try:
        sage_files, sage_layout, warns = discover_files(args.sage_dir)
        if args.self_test:
            mimic_files, mimic_layout = sage_files, sage_layout
        else:
            mimic_files, mimic_layout, w2 = discover_files(args.mimic_dir)
            warns += w2
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    for w in warns:
        print(f"WARNING: {w}", file=sys.stderr)

    avail_s = available_snapshots(sage_files, "sage")
    avail_m = avail_s if args.self_test else available_snapshots(mimic_files, "mimic")
    wanted = parse_snapshots(args.snapshots)
    if wanted == "all":
        wanted = sorted(set(avail_s) & set(avail_m))

    report = {
        "sage_dir": args.sage_dir,
        "mimic_dir": args.mimic_dir,
        "self_test": args.self_test,
        "layout": {"sage": sage_layout, "mimic": mimic_layout},
        "files": {
            "sage": [os.path.basename(f) for f in sage_files],
            "mimic": [os.path.basename(f) for f in mimic_files],
        },
        "warnings": warns,
        "rtol": args.rtol,
        "atol": args.atol,
        "large_rdiff": LARGE_RDIFF,
        "snapshots_available": {"sage": avail_s, "mimic": avail_m},
        "snapshots": [],
        "errors": [],
    }
    print(
        f"SAGE : {args.sage_dir}  [{sage_layout}, {len(sage_files)} file(s)]  snapshots {avail_s[0]}..{avail_s[-1]}"
        if avail_s
        else f"SAGE : {args.sage_dir}  [no snapshots]"
    )
    if not args.self_test:
        print(
            f"Mimic: {args.mimic_dir}  [{mimic_layout}, {len(mimic_files)} file(s)]  snapshots {avail_m}"
        )
    for snap in wanted:
        try:
            res = compare_snapshot(
                snap, sage_files, mimic_files, args.rtol, args.atol, args.self_test
            )
        except (KeyError, ValueError, OSError) as exc:
            msg = f"snapshot {snap}: {exc}"
            print(f"ERROR: {msg}", file=sys.stderr)
            report["errors"].append(msg)
            continue
        report["snapshots"].append(res)
        print_snapshot(res)

    with open(args.out, "w") as fh:
        json.dump(report, fh, indent=1, allow_nan=False)
    print(f"\nWrote {args.out}")
    return 2 if report["errors"] else 0


if __name__ == "__main__":
    sys.exit(main())
