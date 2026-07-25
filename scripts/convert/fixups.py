"""Phase 3 fix-ups for the ctrees -> snapshot-HDF5 converter (plan Slice 5).

Implements the conversion plan's Phase 3 steps 1-5 on the sorted per-snapshot
arrays: a_list adjacency validation, spin normalisation, Len derivation, and
the ``fix_flybys``/``fix_upid`` equivalents. Reference semantics replicated
exactly (resolved decision D12):

- spin: ``J[k] * (1.0 / (double)Mvir)`` in float64, cast to float32, only where
  ``Mvir != 0`` — multiply-by-reciprocal, matching apply_ctrees_value_conventions
  (src/io/tree/read_ctrees_ascii.c:96-122) bit for bit;
- Len: C ``round()`` half-away-from-zero of ``Mvir_native * 1e-10 / PartMass``
  with the reference finiteness/negativity/INT_MAX aborts (same file);
- fix_flybys: per forest at that FOREST'S max snapshot — zero ``pid == -1``
  centrals aborts, one returns unchanged, multiple demote to a sole survivor
  chosen by strict-greater Mvir in ascending-id scan order
  (src/io/tree/ctrees/ctrees_utils.c:318-412);
- fix_upid: centrals get ``upid = id``; satellite upid chains are followed to
  depth 30 with the reference pid fallback, and every resolved satellite gets
  BOTH ``upid`` and ``pid`` set to the ultimate central's id
  (src/io/tree/ctrees/ctrees_utils.c:414-509 and find_fof_halo at 722-787).

fix_flybys runs strictly before fix_upid (reference execution order,
read_ctrees_ascii.c:692-700). Chain construction, ranks, and identity fields
are Slice 6. All aborts carry counts and concrete examples — never repair.

After this stage the ``Jx``/``Jy``/``Jz`` fields of the fixed records carry the
normalised Spin components (raw J only where ``Mvir == 0``, per the reference
carve-out).
"""

import os
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import RECORD_DTYPE, ConverterError  # noqa: E402
from scatter import A_LIST_ATOL, Manifest, file_md5, id_checksum, load_a_list  # noqa: E402

#: Fixed-record dtype: the frozen scratch fields plus the Slice 5 outputs.
#: Little-endian, packed, itemsize 120. Jx/Jy/Jz hold normalised Spin after
#: the fix-up stage (see module docstring).
FIXED_RECORD_DTYPE = np.dtype(
    [(name, RECORD_DTYPE.fields[name][0].str) for name in RECORD_DTYPE.names]
    + [("Len", "<i4"), ("MostBoundID", "<i8")],
    align=False,
)

#: Human-readable dtype identity recorded in every fixed-file manifest entry.
FIXED_DTYPE_TAG = "ctrees-fixed-v1/itemsize=120/" + ",".join(
    "{}:{}".format(name, FIXED_RECORD_DTYPE.fields[name][0].str)
    for name in FIXED_RECORD_DTYPE.names
)

#: Reference upid-chain depth limit (ctrees_utils.c find_fof_halo).
MAX_UPID_CHAIN_DEPTH = 30

#: Expected particle-mass units string in simulation_info.yaml; the Len formula
#: keeps the 1e10-units value, so any other units would silently corrupt Len.
PARTICLE_MASS_UNITS = "1e10 Msun/h"

#: Catalog native mass unit (Msun/h) expressed in the reference mass unit
#: (1e10 Msun/h). This is the SAME factor the generated tree accessor bakes into
#: mimic_tree_get_HaloMass for a native-Msun/h catalog (see
#: scripts/generate_properties.py:_linear_conversion_expr, which derives it from
#: core_properties.yaml reference_units and formats it to full float64
#: precision). Defining it once here — the converter's frozen-units home — keeps
#: the Len derivation (Mvir_native * NATIVE_TO_REF_MASS / PartMass) and the
#: cross-check Mvir reconstruction from drifting apart or from the C model.
NATIVE_TO_REF_MASS = 1e-10

#: Reference mass unit (1e10 Msun/h) expressed in native Msun/h — the reciprocal
#: of NATIVE_TO_REF_MASS, defined independently as the exact literal so the value
#: matches the multiplication used when the header attribute was written. Used to
#: stamp/verify the ``particle_mass_msun_h`` header attribute (a 1e10-Msun/h
#: particle mass rendered in Msun/h); shared by the writer and the cross-check's
#: header-consistency guard so the round-trip is bit-for-bit.
REF_TO_NATIVE_MASS = 1e10

_INT32_MAX = float(np.iinfo(np.int32).max)


def fixed_scratch_name(snap: int) -> str:
    return "snap_{:03d}_fixed.bin".format(snap)


def _log(message: str) -> None:
    print(message, file=sys.stderr)


def load_particle_mass(path) -> float:
    """Load simulation.particle_mass from simulation_info.yaml.

    The value must be positive and finite (plan Slice 5 startup requirement)
    and declared in 1e10 Msun/h — the units the Len formula is frozen in.
    """
    path = Path(path)
    with open(path) as handle:
        data = yaml.safe_load(handle)
    try:
        node = data["simulation"]["particle_mass"]
        value = float(node["value"])
    except (KeyError, TypeError, ValueError):
        raise ConverterError("{}: missing or malformed simulation.particle_mass.value".format(path))
    units = node.get("units") if isinstance(node, dict) else None
    if units != PARTICLE_MASS_UNITS:
        raise ConverterError(
            "{}: particle_mass units {!r} != required {!r} — the Len formula is frozen "
            "in 1e10 Msun/h".format(path, units, PARTICLE_MASS_UNITS)
        )
    if not (np.isfinite(value) and value > 0.0):
        raise ConverterError(
            "{}: particle_mass must be positive and finite, got {}".format(path, value)
        )
    return value


def round_half_away_from_zero(values: np.ndarray) -> np.ndarray:
    """C ``round()`` semantics for non-negative float64 input.

    NumPy's rint rounds exact .5 ties to even (banker's rounding); C round()
    rounds them away from zero. Non-tie values agree, so only exact ties are
    corrected. Callers abort on negative input before rounding.
    """
    rounded = np.rint(values)
    ties = (values - np.floor(values)) == 0.5
    return np.where(ties, np.floor(values) + 1.0, rounded)


def derive_len(mvir: np.ndarray, particle_mass: float, context: str) -> Tuple[np.ndarray, int]:
    """Len = round(Mvir_native * 1e-10 / PartMass), replicating the reference.

    Matches apply_ctrees_value_conventions: float32 Mvir widened to float64,
    the derived count validated (finite, non-negative, <= INT32_MAX) BEFORE
    rounding and int32 conversion, C round() half-away-from-zero. Len == 0 is
    preserved and counted, never repaired.
    """
    len_particles = mvir.astype(np.float64) * NATIVE_TO_REF_MASS / particle_mass
    bad = ~np.isfinite(len_particles) | (len_particles < 0.0) | (len_particles > _INT32_MAX)
    if bad.any():
        rows = np.nonzero(bad)[0][:5]
        examples = [
            "(Mvir={!r}, derived={!r})".format(float(mvir[r]), float(len_particles[r]))
            for r in rows
        ]
        raise ConverterError(
            "{}: {} invalid derived particle count(s) (non-finite, negative, or > INT32_MAX); "
            "examples: {}".format(context, int(bad.sum()), ", ".join(examples))
        )
    len32 = round_half_away_from_zero(len_particles).astype(np.int32)
    return len32, int((len32 == 0).sum())


def normalise_spin(records: np.ndarray) -> None:
    """Spin[k] = (float)((double)J[k] * (1.0 / (double)Mvir)) where Mvir != 0.

    Multiply-by-reciprocal in float64 with a float32 result cast, exactly as
    apply_ctrees_value_conventions computes it (bit-exactness is a frozen
    comparison rule). Zero-mass halos keep their raw J — the reference
    carve-out. A float32-overflowing result is carried as the reference's
    (float) cast would carry it, never aborted here.

    Note: for float32 J and Mvir, reciprocal-multiply and direct division are
    provably identical at the float32 output. The true quotient is a ratio of
    24-bit significands, so it can never lie within 2^-49 (relative) of a
    float32 rounding midpoint unless it equals one exactly, while the two
    float64 computation paths differ by at most ~2^-51 — they can never
    straddle a float32 boundary. The reciprocal form is kept for line-by-line
    correspondence with the C source.
    """
    nonzero = np.nonzero(records["Mvir"] != np.float32(0.0))[0]
    if nonzero.size == 0:
        return
    inv_mvir = 1.0 / records["Mvir"][nonzero].astype(np.float64)
    for component in ("Jx", "Jy", "Jz"):
        with np.errstate(over="ignore"):
            records[component][nonzero] = (
                records[component][nonzero].astype(np.float64) * inv_mvir
            ).astype(np.float32)


def validate_adjacency(records: np.ndarray, snap: int, a_list: np.ndarray, context: str) -> None:
    """Step 1: every desc_scale must match a_list[snap + 1] within atol.

    ctrees guarantees adjacency via its own phantom halos; a violation is
    corrupt input with no repair policy. The final a_list snapshot must have
    all ``desc_id == -1``.
    """
    final_snap = len(a_list) - 1
    has_desc = records["desc_id"] != -1
    if snap == final_snap:
        if has_desc.any():
            rows = np.nonzero(has_desc)[0][:5]
            examples = [
                "(id={}, desc_id={})".format(int(records["id"][r]), int(records["desc_id"][r]))
                for r in rows
            ]
            raise ConverterError(
                "{}: final snapshot {} has {} halo(s) with desc_id != -1; examples: {}".format(
                    context, snap, int(has_desc.sum()), ", ".join(examples)
                )
            )
        return

    desc_scale = records["desc_scale"][has_desc]
    if desc_scale.size == 0:
        return
    ids = records["id"][has_desc]
    desc_ids = records["desc_id"][has_desc]
    unique_scales = np.unique(desc_scale)
    matched_snap = np.empty(unique_scales.size, dtype=np.int64)
    matched_ok = np.empty(unique_scales.size, dtype=bool)
    for i, scale in enumerate(unique_scales):
        nearest = int(np.argmin(np.abs(a_list - scale)))
        matched_snap[i] = nearest
        matched_ok[i] = abs(a_list[nearest] - scale) <= A_LIST_ATOL
    scale_slot = np.searchsorted(unique_scales, desc_scale)
    unknown = ~matched_ok[scale_slot]
    if unknown.any():
        rows = np.nonzero(unknown)[0][:5]
        examples = [
            "(id={}, desc_id={}, desc_scale={})".format(
                int(ids[r]), int(desc_ids[r]), float(desc_scale[r])
            )
            for r in rows
        ]
        raise ConverterError(
            "{}: snapshot {} has {} descendant link(s) whose desc_scale matches no a_list "
            "entry within atol {}; examples: {}".format(
                context, snap, int(unknown.sum()), A_LIST_ATOL, ", ".join(examples)
            )
        )
    desc_snap = matched_snap[scale_slot]
    wrong = desc_snap != snap + 1
    if wrong.any():
        rows = np.nonzero(wrong)[0][:5]
        examples = [
            "(id={}, desc_id={}, desc_scale={}, maps to snapshot {}, expected {})".format(
                int(ids[r]), int(desc_ids[r]), float(desc_scale[r]), int(desc_snap[r]), snap + 1
            )
            for r in rows
        ]
        raise ConverterError(
            "{}: snapshot {} has {} non-adjacent descendant link(s); examples: {}".format(
                context, snap, int(wrong.sum()), ", ".join(examples)
            )
        )


def fix_flybys_snapshot(records: np.ndarray, snap: int, forests_at_max: np.ndarray) -> int:
    """fix_flybys equivalent for the forests whose max snapshot is ``snap``.

    Per forest over its halos at this snapshot (ctrees_utils.c:318-412): zero
    ``pid == -1`` centrals aborts (the reference errors on corrupt input);
    exactly one returns unchanged; multiple demote — the sole survivor is the
    strict-greater-Mvir central in ascending-id scan order (records are
    id-sorted, matching the reference's within-snapshot scan), every other
    forest member at this snapshot gets ``upid`` rewritten to the survivor,
    and demoted centrals additionally get ``pid`` rewritten and MostBoundID
    negated. Returns the number of demoted centrals.
    """
    forests_at_max = np.asarray(forests_at_max, dtype=np.int64)
    if forests_at_max.size == 0:
        return 0
    member_rows = np.nonzero(np.isin(records["forest_id"], forests_at_max))[0]
    present = np.unique(records["forest_id"][member_rows])
    absent = np.setdiff1d(forests_at_max, present, assume_unique=False)
    if absent.size:
        raise ConverterError(
            "snapshot {}: {} forest(s) recorded with max snapshot {} have no halos here; "
            "examples: {}".format(snap, absent.size, snap, absent[:5].tolist())
        )

    # group member rows by forest; within a group, row order is ascending id
    order = np.argsort(records["forest_id"][member_rows], kind="stable")
    member_rows = member_rows[order]
    group_forests = records["forest_id"][member_rows]
    starts = np.nonzero(np.r_[True, group_forests[1:] != group_forests[:-1]])[0]
    is_central = records["pid"][member_rows] == -1
    # reduceat on a bool array would stay bool (logical OR); count in int64
    central_counts = np.add.reduceat(is_central.astype(np.int64), starts)

    zero = central_counts == 0
    if zero.any():
        bad_forests = group_forests[starts][zero][:5].tolist()
        raise ConverterError(
            "snapshot {}: {} forest(s) have zero pid == -1 centrals at their max snapshot "
            "(corrupt input, reference fix_flybys errors); forest id examples: {}".format(
                snap, int(zero.sum()), bad_forests
            )
        )

    demoted_total = 0
    bounds = np.r_[starts, member_rows.size]
    for g in np.nonzero(central_counts > 1)[0]:
        members = member_rows[bounds[g] : bounds[g + 1]]
        central_rows = members[records["pid"][members] == -1]
        # ascending-id scan with strict > == first occurrence of the maximum
        survivor = central_rows[np.argmax(records["Mvir"][central_rows])]
        fof_id = records["id"][survivor]
        others = members[members != survivor]
        records["upid"][others] = fof_id
        demoted = central_rows[central_rows != survivor]
        records["pid"][demoted] = fof_id
        records["MostBoundID"][demoted] = -records["MostBoundID"][demoted]
        demoted_total += demoted.size
    return demoted_total


def fix_upid_snapshot(records: np.ndarray, snap: int) -> None:
    """fix_upid equivalent within one snapshot (ctrees_utils.c:414-509).

    Centrals get ``upid = id`` (pid stays -1). Satellites are then processed
    sequentially in ascending-id order with IN-PLACE rewrites, exactly like
    the reference scan at ctrees_utils.c:442-503: a later satellite's chain
    that reaches an already-resolved satellite sees its rewritten
    ``upid``/``pid`` (path compression), which is what lets the reference
    accept descending-id chains longer than the per-satellite lookup limit.

    Per chain step: the upid target counts as found only within the origin
    satellite's forest (the reference resolves inside per-forest arrays, so a
    same-id halo in another forest is invisible to it); a not-found upid
    falls back to the current halo's pid (conversion plan Phase 3 step 5b);
    neither found is an unresolved failure. Per satellite, at most
    MAX_UPID_CHAIN_DEPTH + 1 lookups are permitted — find_fof_halo enters
    with calldepth 0..30 inclusive (ctrees_utils.c:733-740 fails only at 31)
    and every entry performs one lookup. Every resolved satellite gets BOTH
    ``upid`` and ``pid`` set to the ultimate central's id.

    Failures are collected across the whole snapshot (failed satellites are
    never rewritten) and reported together with counts and per-hop examples.
    """
    ids = records["id"]
    n = ids.size
    upid = records["upid"]
    pid = records["pid"]
    forest = records["forest_id"]
    central = pid == -1
    upid[central] = ids[central]
    satellite_rows = np.nonzero(~central)[0]  # records are id-sorted: ascending id

    def _lookup(target: int, origin_forest: int) -> int:
        pos = int(np.searchsorted(ids, target))
        if pos < n and ids[pos] == target and forest[pos] == origin_forest:
            return pos
        return -1

    failures: List[str] = []
    n_failures = 0
    for row in satellite_rows:
        origin_forest = int(forest[row])
        cur = int(row)
        resolved = -1
        failure = None
        for _ in range(MAX_UPID_CHAIN_DEPTH + 1):
            target = _lookup(int(upid[cur]), origin_forest)
            if target < 0:
                # reference fallback: follow the current halo's pid instead
                target = _lookup(int(pid[cur]), origin_forest)
                if target < 0:
                    failure = (
                        "(origin id={}, at id={}, upid={}, pid={}, forest={}: neither "
                        "target present within the forest)".format(
                            int(ids[row]),
                            int(ids[cur]),
                            int(upid[cur]),
                            int(pid[cur]),
                            origin_forest,
                        )
                    )
                    break
            if pid[target] == -1:
                resolved = ids[target]
                break
            cur = target
        if resolved != -1:
            upid[row] = resolved
            pid[row] = resolved
            continue
        if failure is None:
            failure = "(origin id={}, at id={}, forest={}: chain exceeds depth {})".format(
                int(ids[row]), int(ids[cur]), origin_forest, MAX_UPID_CHAIN_DEPTH
            )
        n_failures += 1
        if len(failures) < 5:
            failures.append(failure)
    if n_failures:
        raise ConverterError(
            "snapshot {}: {} satellite upid chain(s) unresolved (missing targets or depth > {}); "
            "examples: {}".format(snap, n_failures, MAX_UPID_CHAIN_DEPTH, ", ".join(failures))
        )


def apply_fixups_snapshot(
    records: np.ndarray,
    snap: int,
    a_list: np.ndarray,
    particle_mass: float,
    forests_at_max: np.ndarray,
    context: str = "fixups",
) -> Tuple[np.ndarray, Dict[str, int]]:
    """Run Phase 3 steps 1-5 on one snapshot's sorted records.

    Returns the fixed-record array (FIXED_RECORD_DTYPE) and the per-snapshot
    stats. fix_flybys runs strictly before fix_upid (reference order).
    """
    fixed = np.zeros(records.size, dtype=FIXED_RECORD_DTYPE)
    for name in RECORD_DTYPE.names:
        fixed[name] = records[name]

    validate_adjacency(fixed, snap, a_list, context)
    normalise_spin(fixed)
    fixed["Len"], len_zero = derive_len(fixed["Mvir"], particle_mass, context)
    # MostBoundID carries the ctrees id (convert_ctrees_to_lht); fix_flybys
    # negates it for demoted centrals
    fixed["MostBoundID"] = fixed["id"]
    demoted = fix_flybys_snapshot(fixed, snap, forests_at_max)
    fix_upid_snapshot(fixed, snap)
    return fixed, {"rows": int(fixed.size), "flyby_demotions": demoted, "len_zero_count": len_zero}


def verify_mostboundid_invariant(records: np.ndarray, context: str) -> None:
    """Ids are never modified by the fix-up stage, so |MostBoundID| == id must
    hold for every fixed record; abort with count and examples otherwise."""
    bad = np.abs(records["MostBoundID"]) != records["id"]
    if bad.any():
        rows = np.nonzero(bad)[0][:5]
        examples = [
            "(row={}, id={}, MostBoundID={})".format(
                int(r), int(records["id"][r]), int(records["MostBoundID"][r])
            )
            for r in rows
        ]
        raise ConverterError(
            "{}: {} halo(s) violate |MostBoundID| == id after fix-ups — sign corrections "
            "must preserve the ctrees id; examples: {}".format(
                context, int(bad.sum()), ", ".join(examples)
            )
        )


def _forests_at_max_by_snap(forest_max_table: np.ndarray) -> Dict[int, np.ndarray]:
    """Invert the per-forest max-snapshot table into snap -> forest ids."""
    if forest_max_table.ndim != 2 or forest_max_table.shape[1] != 2:
        raise ConverterError(
            "forest max-snapshot table has shape {}, expected (n, 2)".format(forest_max_table.shape)
        )
    by_snap: Dict[int, np.ndarray] = {}
    forests = forest_max_table[:, 0]
    max_snaps = forest_max_table[:, 1]
    for snap in np.unique(max_snaps).tolist():
        by_snap[int(snap)] = forests[max_snaps == snap]
    return by_snap


def run_fixups(
    workdir,
    a_list_path,
    simulation_info_path,
    snapshots: Optional[Sequence[int]] = None,
) -> Manifest:
    """Apply the fix-up stage to every sorted snapshot (or the given subset).

    The a_list must be byte-identical to the one the scatter stage validated
    observed pairs against (manifest identity binding); simulation_info.yaml
    is bound the same way, recorded here if scatter did not record it.
    Re-running skips snapshots already fixed after verifying their artifacts.
    """
    manifest = Manifest.load_or_create(workdir)
    if not manifest.path.exists():
        raise ConverterError("{}: no manifest found; run scatter first".format(workdir))

    a_list, a_list_md5 = load_a_list(a_list_path)
    provenance = manifest.data["provenance"]
    recorded = provenance.get("a_list", {}).get("md5")
    if recorded != a_list_md5:
        raise ConverterError(
            "{}: a_list content md5 {} != manifest-recorded {} — the fix-up stage must use "
            "the a_list the scatter stage validated against".format(
                a_list_path, a_list_md5, recorded
            )
        )
    sim_info_md5 = file_md5(simulation_info_path)
    recorded_info = provenance.get("simulation_info")
    if recorded_info is None:
        provenance["simulation_info"] = {
            "path": str(Path(simulation_info_path).resolve()),
            "md5": sim_info_md5,
        }
        manifest.save()
    elif recorded_info.get("md5") != sim_info_md5:
        raise ConverterError(
            "{}: simulation_info content md5 {} != manifest-recorded {} — "
            "refusing to mix metadata across runs".format(
                simulation_info_path, sim_info_md5, recorded_info.get("md5")
            )
        )
    particle_mass = load_particle_mass(simulation_info_path)

    forest_max_path = Path(manifest.workdir) / "forest_max_snap.npy"
    manifest.verify_intermediate(forest_max_path, "forest max-snapshot table")
    forests_by_snap = _forests_at_max_by_snap(np.load(forest_max_path))

    if snapshots is None:
        snapshots = sorted(int(s) for s in manifest.data["snapshots"])
    for snap in snapshots:
        fix_one_snapshot(manifest, snap, a_list, particle_mass, forests_by_snap)
    return manifest


def fix_one_snapshot(
    manifest: Manifest,
    snap: int,
    a_list: np.ndarray,
    particle_mass: float,
    forests_by_snap: Dict[int, np.ndarray],
) -> None:
    """Fix one snapshot: verify input, apply steps 1-5, write + verify output."""
    entry = manifest.data["snapshots"].get(str(snap))
    if entry is None:
        raise ConverterError("snapshot {}: no manifest entry; run scatter first".format(snap))
    if entry.get("status") == "fixed":
        manifest.verify_intermediate(entry["fixed_file"], "fixed snapshot scratch")
        return
    if entry.get("status") != "sorted":
        raise ConverterError(
            "snapshot {}: unexpected status {!r}; run sort first".format(snap, entry.get("status"))
        )

    sorted_path = Path(entry["sorted_file"])
    manifest.verify_intermediate(sorted_path, "sorted snapshot scratch")
    records = np.fromfile(sorted_path, dtype=RECORD_DTYPE)
    if len(records) != entry["rows"]:
        raise ConverterError(
            "{}: has {} rows, manifest records {}".format(sorted_path, len(records), entry["rows"])
        )

    fixed, stats = apply_fixups_snapshot(
        records,
        snap,
        a_list,
        particle_mass,
        forests_by_snap.get(snap, np.empty(0, dtype=np.int64)),
        context=str(sorted_path),
    )

    fixed_path = sorted_path.parent / fixed_scratch_name(snap)
    fixed.tofile(fixed_path)

    # verify the fixed file against the manifest totals before recording it;
    # ids are never modified by the fix-up stage, so the id checksum and the
    # |MostBoundID| == id invariant must both hold
    reread = np.fromfile(fixed_path, dtype=FIXED_RECORD_DTYPE)
    if len(reread) != entry["rows"]:
        raise ConverterError(
            "{}: fixed file has {} rows, manifest records {}".format(
                fixed_path, len(reread), entry["rows"]
            )
        )
    checksum = id_checksum(reread["id"])
    if checksum != entry["id_checksum"]:
        raise ConverterError(
            "{}: fixed-file id checksum {} != manifest checksum {}".format(
                fixed_path, checksum, entry["id_checksum"]
            )
        )
    verify_mostboundid_invariant(reread, str(fixed_path))

    manifest.register_intermediate(
        fixed_path, "snapshot-fixed", rows=int(len(reread)), dtype_tag=FIXED_DTYPE_TAG
    )
    entry["fixed_file"] = str(fixed_path.resolve())
    entry["flyby_demotions"] = stats["flyby_demotions"]
    entry["len_zero_count"] = stats["len_zero_count"]
    entry["status"] = "fixed"
    manifest.save()
    _log(
        "fixups: snapshot {} — {} rows, {} flyby demotion(s), {} Len==0 halo(s)".format(
            snap, stats["rows"], stats["flyby_demotions"], stats["len_zero_count"]
        )
    )
