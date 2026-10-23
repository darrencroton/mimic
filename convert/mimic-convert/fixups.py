"""Phase 3 fix-ups for the ctrees -> horizontal-HDF5 converter.

Implements Phase 3 steps 1-5 on the sorted per-snapshot
arrays: a_list adjacency validation, spin normalisation, Len derivation, and
the ``fix_upid`` equivalent. Reference semantics replicated exactly:

- spin: ``J[k] * (1.0 / (double)Mvir)`` in float64, cast to float32, only where
  ``Mvir != 0`` — multiply-by-reciprocal, matching apply_ctrees_value_conventions
  (src/io/vertical/read_ctrees_ascii.c:96-122) bit for bit;
- Len: C ``round()`` half-away-from-zero of ``Mvir_native * 1e-10 / PartMass``
  with the reference finiteness/negativity/INT_MAX aborts (same file);
- fix_upid: centrals get ``upid = id``; satellite upid chains are followed to
  depth 30 with the reference pid fallback, and every resolved satellite gets
  BOTH ``upid`` and ``pid`` set to the ultimate central's id
  (src/io/vertical/ctrees/ctrees_utils.c:414-509 and find_fof_halo at 722-787).

``fix_flybys`` was removed from both the C reader and this converter: it
demoted every FoF central but the most massive at each forest's final
snapshot, which on Shin-Uchuu put 33% of z=0 galaxies in one FoF group. ``MostBoundID`` is therefore
always positive now — the demotion marker it used to carry no longer exists.
Chain construction, ranks, and identity fields belong to ``links``. All aborts carry
counts and concrete examples — never repair.

After this stage the ``Jx``/``Jy``/``Jz`` fields of the fixed records carry the
normalised Spin components (raw J only where ``Mvir == 0``, per the reference
carve-out).

The scratch layout passes through unchanged apart from the two appended
fields: its declared extras are copied field for field into the fixed record,
and no convention touches them -- an extra that selects the raw
``Jx``/``Jy``/``Jz`` columns keeps the catalog J while the core
``Jx``/``Jy``/``Jz`` are normalised.

**Under a declared cut table** (``cut_map``; the canonical ASCII route only)
hosts are still resolved under the original ``forests.list`` partition, and
only then does :func:`sever_snapshot` install the table: every record's
``forest_id`` becomes its tree's forest in the table (through
``tree_root_id``), and every satellite whose resolved central lies in another
forest of the table becomes a central (``upid = id``, ``pid = -1``). No host
relation is invented. Without a table nothing here changes, and a host in
another forest stays the unresolved failure it always was.
"""

import os
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import ConverterError, ScratchLayout, describe_dtype  # noqa: E402
from scatter import (  # noqa: E402
    A_LIST_ATOL,
    ForestMap,
    Manifest,
    file_md5,
    id_checksum,
    load_a_list,
)

#: Version prefix of the fixed-record dtype tag (see :func:`fixed_layout`).
FIXED_DTYPE_TAG_PREFIX = "ctrees-fixed-v2"


def fixed_record_dtype(scratch_dtype: np.dtype) -> np.dtype:
    """The fixed record for one scratch record: every scratch field, in order,
    then ``Len`` and ``MostBoundID``. Jx/Jy/Jz hold normalised Spin after the
    fix-up stage (see the module docstring)."""
    return np.dtype(
        scratch_dtype.descr + [("Len", "<i4"), ("MostBoundID", "<i8")],
        align=False,
    )


def fixed_layout(layout: ScratchLayout) -> Tuple[np.dtype, str]:
    """``(fixed dtype, fixed dtype tag)`` for a workdir's scratch layout."""
    dtype = fixed_record_dtype(layout.dtype)
    return dtype, describe_dtype(FIXED_DTYPE_TAG_PREFIX, dtype)


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
#: the Len derivation (Mvir_native * NATIVE_TO_REF_MASS / PartMass) from
#: drifting away from the C model.
NATIVE_TO_REF_MASS = 1e-10

#: Reference mass unit (1e10 Msun/h) expressed in native Msun/h — the reciprocal
#: of NATIVE_TO_REF_MASS, defined independently as the exact literal so the value
#: matches the multiplication used when the header attribute was written. Used to
#: stamp/verify the ``particle_mass_msun_h`` header attribute (a 1e10-Msun/h
#: particle mass rendered in Msun/h); the version 3 writer stamps it and
#: re-checks a recorded mass with the same multiplication, so the round-trip is
#: bit-for-bit.
REF_TO_NATIVE_MASS = 1e10

_INT32_MAX = float(np.iinfo(np.int32).max)

#: Shared empty sentinel for "no forest peaks at this snapshot" — passed to
#: verify_fof_centrals_present, whose no-op path checks .size == 0.
_EMPTY_FOREST_IDS = np.empty((0,), dtype=np.int64)


def load_forests_at_max_by_snap(manifest: Manifest) -> Dict[int, np.ndarray]:
    """Load ``forest_max_snap.npy`` (Phase 1's per-forest max-snapshot
    aggregate, produced by scatter's finalize pass) and group forest ids by
    their maximum snapshot.

    Returns ``{snap: sorted unique forest ids whose maximum snapshot is
    exactly snap}``. Verified via the manifest against tampering, like every
    other intermediate this stage reads. This is the sidecar the corrupt-input
    guard consumes; see ``verify_fof_centrals_present``.
    """
    path = Path(manifest.workdir) / "forest_max_snap.npy"
    manifest.verify_intermediate(path, "forest-max-snap sidecar")
    table = np.load(path)
    if table.size == 0:
        return {}
    order = np.argsort(table[:, 1], kind="stable")
    forests = table[order, 0]
    snaps = table[order, 1]
    starts = np.nonzero(np.r_[True, snaps[1:] != snaps[:-1]])[0]
    ends = np.r_[starts[1:], snaps.size]
    result: Dict[int, np.ndarray] = {}
    for start, end in zip(starts.tolist(), ends.tolist()):
        result[int(snaps[start])] = np.sort(forests[start:end])
    return result


def fixed_scratch_name(snap: int) -> str:
    return "snap_{:03d}_fixed.bin".format(snap)


def _log(message: str) -> None:
    print(message, file=sys.stderr)


def load_particle_mass(path) -> float:
    """Load simulation.particle_mass from simulation_info.yaml.

    The value must be positive and finite (checked at startup)
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


def verify_fof_centrals_present(
    records: np.ndarray, snap: int, forests_at_max: np.ndarray, context: str
) -> None:
    """Corrupt-input guard that outlived ``fix_flybys``.

    ``fix_flybys`` used to abort, as a side effect of its own topology scan,
    when a forest had zero ``pid == -1`` (FoF central) halos at its maximum
    scale ("NO FOFs at max scale ... Will crash") — structurally impossible
    for a valid Consistent-Trees forest. That guard died with the function;
    nothing else asserted the property it happened to check. This restores it
    standalone, independent of any demotion or topology-rewriting logic: for
    every forest whose maximum snapshot is ``snap``, at least one of its
    records here must have ``pid == -1``.

    ``forests_at_max`` is the sorted, unique array of forest ids whose
    maximum snapshot (from Phase 1's ``forest_max_snap.npy`` aggregate)
    equals ``snap``; an empty array is a no-op (no forest peaks here).
    """
    if forests_at_max.size == 0:
        return
    central_forests = np.unique(records["forest_id"][records["pid"] == -1])
    missing = np.setdiff1d(forests_at_max, central_forests, assume_unique=True)
    if missing.size:
        examples = missing[:5].tolist()
        raise ConverterError(
            "{}: snapshot {}: {} forest(s) whose maximum snapshot is this one have zero "
            "pid == -1 (FoF central) record(s) here; corrupt input; "
            "example forest id(s): {}".format(context, snap, int(missing.size), examples)
        )


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
    falls back to the current halo's pid (Phase 3 step 5b);
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


def sever_snapshot(records: np.ndarray, snap: int, cut_map: ForestMap) -> int:
    """Install a cut table over one snapshot whose hosts
    :func:`fix_upid_snapshot` has resolved under the original partition.

    Every record's ``forest_id`` becomes its tree's forest in the table
    (``cut_map``, through ``tree_root_id``); every satellite whose resolved
    central (``upid``, a ``pid == -1`` record of this snapshot) now lies in
    another forest becomes a central, ``upid = id`` and ``pid = -1``, the
    post-fix-up form of a central. A satellite whose central stays in its
    forest keeps it. Returns the number promoted.
    """
    ids = records["id"]
    forest = cut_map.lookup_forest_ids(records["tree_root_id"])
    records["forest_id"] = forest
    satellites = np.flatnonzero(records["pid"] != -1)
    if satellites.size == 0:
        return 0
    hosts = records["upid"][satellites]
    central = np.minimum(np.searchsorted(ids, hosts), ids.size - 1)
    unresolved = (ids[central] != hosts) | (records["pid"][central] != -1)
    if unresolved.any():  # pragma: no cover - fix_upid_snapshot resolves every satellite first
        at = satellites[np.flatnonzero(unresolved)[0]]
        raise ConverterError(
            "snapshot {}: satellite id {} has upid {}, which is no central of this "
            "snapshot".format(snap, int(ids[at]), int(records["upid"][at]))
        )
    promoted = satellites[forest[central] != forest[satellites]]
    records["upid"][promoted] = ids[promoted]
    records["pid"][promoted] = -1
    return int(promoted.size)


def apply_fixups_snapshot(
    records: np.ndarray,
    snap: int,
    a_list: np.ndarray,
    particle_mass: float,
    forests_at_max: Optional[np.ndarray] = None,
    context: str = "fixups",
    cut_map: Optional[ForestMap] = None,
) -> Tuple[np.ndarray, Dict[str, int]]:
    """Run Phase 3 steps 1-4 on one snapshot's sorted records.

    ``forests_at_max`` is the sorted array of forest ids whose maximum
    snapshot (per ``forest_max_snap.npy``) equals ``snap``; ``None`` (the
    default, used by direct unit-level calls that do not have that sidecar)
    skips the corrupt-input guard rather than treating it as "no forest
    peaks here" the way an explicit empty array does.

    ``cut_map`` (default None) is a declared cut table, installed by
    :func:`sever_snapshot` once every host is resolved; the stats then count
    its promotions as ``cut_promotions``.

    Returns the fixed-record array (``fixed_record_dtype(records.dtype)``) and
    the per-snapshot stats. Every field of ``records`` is copied, so a record's
    extras arrive in the fixed record untouched by any convention below.
    """
    fixed = np.zeros(records.size, dtype=fixed_record_dtype(records.dtype))
    for name in records.dtype.names:
        fixed[name] = records[name]

    validate_adjacency(fixed, snap, a_list, context)
    normalise_spin(fixed)
    fixed["Len"], len_zero = derive_len(fixed["Mvir"], particle_mass, context)
    # MostBoundID carries the ctrees id and is always positive.
    fixed["MostBoundID"] = fixed["id"]
    # Corrupt-input guard, independent of fix_upid below: every forest
    # peaking at this snapshot must have at least one pid == -1 record here.
    if forests_at_max is not None:
        verify_fof_centrals_present(fixed, snap, forests_at_max, context)
    fix_upid_snapshot(fixed, snap)
    stats = {"rows": int(fixed.size), "len_zero_count": len_zero}
    if cut_map is not None:
        stats["cut_promotions"] = sever_snapshot(fixed, snap, cut_map)
    return fixed, stats


def verify_mostboundid_invariant(records: np.ndarray, context: str) -> None:
    """Ids are never modified by the fix-up stage, so ``MostBoundID`` must equal
    ``id`` for every fixed record; abort with count and examples otherwise."""
    bad = records["MostBoundID"] != records["id"]
    if bad.any():
        rows = np.nonzero(bad)[0][:5]
        examples = [
            "(row={}, id={}, MostBoundID={})".format(
                int(r), int(records["id"][r]), int(records["MostBoundID"][r])
            )
            for r in rows
        ]
        raise ConverterError(
            "{}: {} halo(s) violate MostBoundID == id after fix-ups; "
            "examples: {}".format(context, int(bad.sum()), ", ".join(examples))
        )


def run_fixups(
    workdir,
    a_list_path,
    simulation_info_path,
    snapshots: Optional[Sequence[int]] = None,
    cut_map: Optional[ForestMap] = None,
) -> Manifest:
    """Apply the fix-up stage to every sorted snapshot (or the given subset).

    The a_list must be byte-identical to the one the scatter stage validated
    observed pairs against (manifest identity binding); simulation_info.yaml
    is bound the same way, recorded here if scatter did not record it.
    Re-running skips snapshots already fixed after verifying their artifacts.

    This stage deletes nothing: the sorted scratch stays in the workdir.

    ``cut_map`` is the workdir's cut table (``scatter.load_cut_table``), which
    is required exactly when scatter bound one: its md5 must be the bound
    one, so no snapshot of a workdir is fixed with and another without it.
    """
    manifest = Manifest.load(workdir)
    bound_cut = (manifest.data["provenance"].get("cut_table") or {}).get("md5")
    given_cut = None if cut_map is None else cut_map.md5
    if bound_cut != given_cut:
        raise ConverterError(
            "{}: the fix-up stage was given cut table md5 {} but scatter bound {}".format(
                manifest.path, given_cut, bound_cut
            )
        )

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
    forests_at_max_by_snap = load_forests_at_max_by_snap(manifest)

    if snapshots is None:
        snapshots = sorted(int(s) for s in manifest.data["snapshots"])
    for snap in snapshots:
        fix_one_snapshot(
            manifest,
            snap,
            a_list,
            particle_mass,
            forests_at_max_by_snap.get(snap, _EMPTY_FOREST_IDS),
            cut_map=cut_map,
        )
    return manifest


def fix_one_snapshot(
    manifest: Manifest,
    snap: int,
    a_list: np.ndarray,
    particle_mass: float,
    forests_at_max: np.ndarray = _EMPTY_FOREST_IDS,
    cut_map: Optional[ForestMap] = None,
) -> None:
    """Fix one snapshot: verify input, apply steps 1-4 (and the cut table's
    severance when ``cut_map`` is given), write + verify output."""
    entry = manifest.data["snapshots"].get(str(snap))
    if entry is None:
        raise ConverterError("snapshot {}: no manifest entry; run scatter first".format(snap))
    status = entry.get("status")
    if status == "fixed":
        # This stage's output is already on the record and must still be on
        # disk, so it is verified outright before the skip.
        manifest.verify_intermediate(entry["fixed_file"], "fixed snapshot scratch")
        return
    if status != "sorted":
        raise ConverterError(
            "snapshot {}: unexpected status {!r}; run sort first".format(snap, status)
        )

    sorted_path = Path(entry["sorted_file"])
    registered = manifest.verify_intermediate(sorted_path, "sorted snapshot scratch")
    if registered.get("dtype_tag") != manifest.layout.dtype_tag:
        raise ConverterError(
            "{}: sorted scratch dtype tag {!r} != this workdir's {!r} — refusing to fix".format(
                sorted_path, registered.get("dtype_tag"), manifest.layout.dtype_tag
            )
        )
    records = np.fromfile(sorted_path, dtype=manifest.layout.dtype)
    if len(records) != entry["rows"]:
        raise ConverterError(
            "{}: has {} rows, manifest records {}".format(sorted_path, len(records), entry["rows"])
        )

    fixed, stats = apply_fixups_snapshot(
        records,
        snap,
        a_list,
        particle_mass,
        forests_at_max=forests_at_max,
        context=str(sorted_path),
        cut_map=cut_map,
    )

    fixed_path = sorted_path.parent / fixed_scratch_name(snap)
    fixed.tofile(fixed_path)

    # verify the fixed file against the manifest totals before recording it;
    # ids are never modified by the fix-up stage, so the id checksum and the
    # |MostBoundID| == id invariant must both hold
    fixed_dtype, fixed_tag = fixed_layout(manifest.layout)
    if fixed.dtype != fixed_dtype:  # pragma: no cover - fixed_record_dtype guarantees it
        raise ConverterError("{}: fixed records are not in the workdir layout".format(fixed_path))
    reread = np.fromfile(fixed_path, dtype=fixed_dtype)
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
        fixed_path, "snapshot-fixed", rows=int(len(reread)), dtype_tag=fixed_tag
    )
    entry["fixed_file"] = str(fixed_path.resolve())
    entry["len_zero_count"] = stats["len_zero_count"]
    if cut_map is not None:
        entry["cut_promotions"] = stats["cut_promotions"]
    entry["status"] = "fixed"
    manifest.save()
    _log(
        "fixups: snapshot {} — {} rows, {} Len==0 halo(s)".format(
            snap, stats["rows"], stats["len_zero_count"]
        )
    )
