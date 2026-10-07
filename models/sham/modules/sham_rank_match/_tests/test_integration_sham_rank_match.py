#!/usr/bin/env python3
"""
sham_rank_match - Integration Test

Validates the rank match through the real executable and the HDF5 output, read per
UniqueGalaxyID, on the committed micro-Uchuu horizontal fixture (three forests over six
snapshots, BoxSize 100 Mpc/h, h = 0.6774) run from the shipped
models/sham/input/sham_micro-uchuu-ascii-horizontal.yaml:

  - every assigned row's log10 StellarMass (physical Msun) matches the independent
    double-precision reference (sham_rank_match_reference.py) for its rank within 1e-4, where
    the rank orders the Type 0/1 rows with ShamVpeak >= ShamMinVpeak by descending ShamVpeak
    and then ascending UniqueGalaxyID across the whole snapshot;
  - every masked candidate and every row below the ShamMinVpeak floor has ShamGhost 1 and
    StellarMass 0 (a derivative run file with a raised floor and a raised mass floor
    exercises both), and the audit line's candidate, assigned and masked counts agree;
  - no Type 2 row is ever written, and the galaxies the fixture's snapshot-4 halos 0 and 2
    lose to their snapshot-5 merger target (core demotes them to Type 2, the module retires
    them) are written at snapshot 4 and absent at snapshot 5;
  - every written row's ShamVpeak equals the maximum Vmax along its branch, recomputed from the
    fixture's own FirstProgenitor links and Vmax columns (the module's peak history, end to end);
  - repeating a run is bitwise identical on the Galaxies datasets;
  - exact ShamVpeak ties between different FoF groups rank by ascending UniqueGalaxyID, on a
    derivative copy of the fixture whose Vmax is flattened through h5py in the test's temp
    dir;
  - startup rejects an output snapshot above the redshift window and a missing pre_timestep
    entry; every run is leak-free.

The pure-Python case at the top checks that the rank-density table embedded in
test_unit_sham_rank_match.c (between its SHAM_DECIMAL_REFERENCE markers) is exactly what the
reference script computes, so the C table cannot drift from its oracle; it needs no executable
and runs under every MODEL/SIMULATION pair. Every fixture case reports NA under any
simulation other than micro-uchuu-ascii-horizontal (make tests-snapshot-global
builds that pair and runs them all).
"""

import json
import math
import re
import shutil
import sys
import tempfile
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from framework import (  # noqa: E402
    MIMIC_EXE,
    TestNotApplicable,
    TestSkipped,
    check_no_memory_leaks,
    compiled_model,
    compiled_simulation,
    run_mimic,
    run_test_suite,
)
from sham_rank_match_reference import (  # noqa: E402
    CASES,
    LOG_MASS_DECIMALS,
    PUBLISHED,
    case_table,
    cumulative_density,
    inverse_log10_mass,
)

UNIT_TEST_SOURCE = Path(__file__).resolve().parent / "test_unit_sham_rank_match.c"

#: One C table row: {"name", h_sim, density, assigned, log10 M*}.
ROW_PATTERN = re.compile(r'\{"([\w-]+)", ([^,]+), ([^,]+), ([01]), ([^}]+)\}')


TEMP_DIR = None

PACKAGE_SIMULATION = "micro-uchuu-ascii-horizontal"
RUN_FILE = REPO_ROOT / "models" / "sham" / "input" / f"sham_{PACKAGE_SIMULATION}.yaml"
SIMULATION_DIR = REPO_ROOT / "simulations" / PACKAGE_SIMULATION
FIXTURE_DIR = SIMULATION_DIR / "_tests" / "data" / "generic"
SIMULATION_CONFIG = SIMULATION_DIR / "_tests" / "input" / "test_simulation.yaml"

NUM_SNAPSHOTS = 6

#: The target parameters, in the order of the reference's PUBLISHED tuple.
PARAMETER_NAMES = (
    "ShamTargetLogMstar",
    "ShamTargetPhi1",
    "ShamTargetAlpha1",
    "ShamTargetPhi2",
    "ShamTargetAlpha2",
)

#: Tolerance on log10 M* [physical Msun] between the module and the reference.
LOG_MASS_TOLERANCE = 1.0e-4

#: SHAM_MASS_UNIT_MSUN in sham_rank_match.h: StellarMass is written in units of 1e10 Msun/h.
MASS_UNIT_MSUN = 1.0e10

#: Derivative run parameters. ShamMinVpeak 194.5 keeps the fixture's 214, 202 and 195 km/s
#: peaks at snapshot 4 and drops its 194 and 193 km/s ones below the completeness floor.
#: ShamTargetLogMassFloor 11.6 puts the floor density n(>10^11.6 Msun) = 4.84e-7 Mpc^-3
#: between rank 1 (4.66e-7) and rank 2 (7.77e-7) of the fixture's 100 Mpc/h box, so the third
#: snapshot-4 candidate is masked while the first two are assigned.
FLOORED_PARAMETERS = {"ShamMinVpeak": 194.5, "ShamTargetLogMassFloor": 11.6}

#: The flat Vmax [km/s] of the tie fixture: every halo of every snapshot has the same peak.
TIE_VMAX = 200.0

AUDIT_LINE = re.compile(
    r"^SHAM audit z=(-?\d+\.\d{4}) candidates=(\d+) assigned=(\d+) masked=(\d+)$",
    re.MULTILINE,
)

#: log10 M* of a rank density depends only on (h, density); each costs 60 quadratures.
_INVERSE_CACHE = {}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def require_fixture_package():
    """Require sham x the fixture package (NA under another simulation).

    The module's tests register only under MODEL=sham, so a different model means a misconfigured
    direct run and stays a loud SKIP; a different simulation is a registered pair these cases
    were not written for, which is not applicable.
    """
    if compiled_model() != "sham":
        raise TestSkipped(f"selected model is {compiled_model()!r}, not sham")
    if compiled_simulation() != PACKAGE_SIMULATION:
        raise TestNotApplicable(
            f"selected pair is MODEL={compiled_model()} "
            f"SIMULATION={compiled_simulation()}, not the sham x {PACKAGE_SIMULATION} pair these "
            f"cases are written against (make tests-snapshot-global builds and runs it)"
        )
    if not MIMIC_EXE.exists():
        raise FileNotFoundError(f"Mimic executable not found at {MIMIC_EXE}")
    assert RUN_FILE.exists(), f"missing run file {RUN_FILE}"


def simulation_constants():
    """(BoxSize [Mpc/h], h_sim) of the fixture's simulation config."""
    with open(SIMULATION_CONFIG, "r") as handle:
        simulation = yaml.safe_load(handle)["simulation"]
    return float(simulation["box_size"]["value"]), float(simulation["cosmology"]["hubble_h"])


def write_run(name, parameters=None, modules=None, input_overrides=None):
    """Copy the shipped run file into the temp dir with a private output directory.

    The copy keeps the shipped file's repository-relative simulation.config, so every run
    starts from the repository root as the file documents. ``modules`` replaces the module
    phases (the parameters block is kept); ``parameters`` updates single parameters;
    ``input_overrides`` goes into the run file's input section, which wins over the simulation
    config's.
    """
    with open(RUN_FILE, "r") as handle:
        config = yaml.safe_load(handle)
    output_dir = TEMP_DIR / name
    output_dir.mkdir(parents=True, exist_ok=True)
    config["output"]["output_directory"] = str(output_dir)
    if modules is not None:
        params = config["modules"]["parameters"]
        config["modules"] = dict(modules)
        config["modules"]["parameters"] = params
    if parameters is not None:
        config["modules"]["parameters"].update(parameters)
    if input_overrides is not None:
        config.setdefault("input", {}).update(input_overrides)
    run_file = TEMP_DIR / f"{name}.yaml"
    with open(run_file, "w") as handle:
        yaml.safe_dump(config, handle, default_flow_style=False, sort_keys=False)
    return (
        run_file,
        output_dir,
        config["output"]["output_filename"],
        config["modules"]["parameters"],
    )


def run_ok(run_file, what):
    """Run Mimic, require success and a clean leak report, and return its stdout."""
    returncode, stdout, stderr = run_mimic(run_file, cwd=REPO_ROOT)
    assert returncode == 0, f"{what}: the run should complete (rc={returncode}):\n{stdout}{stderr}"
    assert check_no_memory_leaks(stdout, stderr), f"{what}: the run reported a memory leak"
    return stdout


def read_galaxies(output_dir, stem):
    """Every written snapshot's Galaxies dataset: {snapshot: rows} (one file per snapshot)."""
    import h5py

    found = {}
    for path in sorted(Path(output_dir).glob(f"{stem}_*.hdf5")):
        with h5py.File(path, "r") as handle:
            for group_name in handle:
                if group_name.startswith("Snap"):
                    found[int(group_name[4:])] = handle[group_name]["Galaxies"][()]
    assert found, f"no snapshot files written under {output_dir}"
    return found


def all_field_bytes(galaxies):
    """Each snapshot's field values as bytes (exact), ignoring row padding."""
    return {
        snap: tuple((name, rows[name].tobytes()) for name in rows.dtype.names)
        for snap, rows in galaxies.items()
    }


def reference_log_mass(h_sim, density):
    """log10 M* [physical Msun] at ``density`` from the double-precision reference (cached)."""
    key = (h_sim, density)
    if key not in _INVERSE_CACHE:
        _INVERSE_CACHE[key] = inverse_log10_mass(h_sim, density)
    return _INVERSE_CACHE[key]


def rank_order(rows, min_vpeak):
    """Candidate row indices, best rank first: descending ShamVpeak, ties ascending ID."""
    candidates = [
        index
        for index in range(len(rows))
        if int(rows["Type"][index]) in (0, 1) and float(rows["ShamVpeak"][index]) >= min_vpeak
    ]
    candidates.sort(key=lambda i: (-float(rows["ShamVpeak"][i]), int(rows["UniqueGalaxyID"][i])))
    return candidates


def assert_population(rows, parameters, snapshot):
    """Assert the whole rank-match contract on one snapshot's rows; return (cand, assigned, masked).

    Every row is Type 0 or 1 (a Type 2 row is never written). The candidates are ranked by
    descending ShamVpeak and then ascending UniqueGalaxyID; rank r has the density
    (r + 0.5) / BoxSize^3 * h^3 [Mpc^-3], which is masked when it exceeds the reference's
    n(>10^ShamTargetLogMassFloor) and otherwise has the reference's log10 M* within the
    tolerance. Masked candidates and every non-candidate have ShamGhost 1 and StellarMass 0.
    """
    import numpy as np

    what = f"snapshot {snapshot}"
    box_size, h_sim = simulation_constants()
    published = tuple(parameters[name] for name in PARAMETER_NAMES) == PUBLISHED
    assert published, f"{what}: the target parameters must be the published ones"
    assert float(parameters["ShamTargetHubble"]) == 0.7, "the reference's published h is 0.7"
    ids = rows["UniqueGalaxyID"]
    assert len(np.unique(ids)) == len(ids), f"{what}: UniqueGalaxyIDs must be unique"
    types = set(int(t) for t in np.unique(rows["Type"]))
    assert types <= {0, 1}, f"{what}: Types {sorted(types)} written; a Type 2 or 3 row must not be"

    floor_density = cumulative_density(
        h_sim, float(parameters["ShamTargetLogMassFloor"]) * math.log(10.0)
    )
    ranked = rank_order(rows, float(parameters["ShamMinVpeak"]))
    candidate_set = set(ranked)
    assigned = masked = 0
    for rank, index in enumerate(ranked):
        galaxy_id = int(ids[index])
        density = (rank + 0.5) / box_size**3 * h_sim**3
        stellar_mass = float(rows["StellarMass"][index])
        ghost = int(rows["ShamGhost"][index])
        if density > floor_density:
            masked += 1
            assert ghost == 1 and stellar_mass == 0.0, (
                f"{what}: masked rank {rank} (ID {galaxy_id}) must have ShamGhost 1 and "
                f"StellarMass 0, not {ghost} and {stellar_mass}"
            )
            continue
        assigned += 1
        assert ghost == 0, f"{what}: assigned rank {rank} (ID {galaxy_id}) has ShamGhost {ghost}"
        assert stellar_mass > 0.0, f"{what}: assigned rank {rank} (ID {galaxy_id}) has no mass"
        measured = math.log10(stellar_mass * MASS_UNIT_MSUN / h_sim)
        expected = reference_log_mass(h_sim, density)
        assert abs(measured - expected) <= LOG_MASS_TOLERANCE, (
            f"{what}: rank {rank} (ID {galaxy_id}) has log10 M* {measured:.6f}, reference "
            f"{expected:.6f} at density {density:.6e} Mpc^-3"
        )
    for index in range(len(rows)):
        if index in candidate_set:
            continue
        assert int(rows["ShamGhost"][index]) == 1 and float(rows["StellarMass"][index]) == 0.0, (
            f"{what}: ID {int(ids[index])} is below the ShamMinVpeak floor "
            f"(ShamVpeak {float(rows['ShamVpeak'][index])}) and must have ShamGhost 1 and "
            f"StellarMass 0"
        )
    return len(ranked), assigned, masked


def assert_run(stdout, galaxies, parameters):
    """Assert every snapshot of a run and that the audit lines report the same counts.

    Returns the per-snapshot (candidates, assigned, masked) tuples.
    """
    assert sorted(galaxies) == list(range(NUM_SNAPSHOTS)), f"snapshots {sorted(galaxies)}"
    counts = [assert_population(galaxies[snap], parameters, snap) for snap in sorted(galaxies)]
    audits = [tuple(int(v) for v in line[1:]) for line in AUDIT_LINE.findall(stdout)]
    assert audits == counts, f"audit lines {audits} disagree with the written rows {counts}"
    return counts


def demoted_fixture_halos():
    """Snapshot-4 halo indices that a snapshot-5 halo absorbs without being its main progenitor.

    Read from the fixture's own links: a halo is demoted when its Descendant's FirstProgenitor
    is another halo.
    """
    import h5py

    with h5py.File(FIXTURE_DIR / "snapshot_004.h5", "r") as early:
        descendant = early["halos/Descendant"][()]
    with h5py.File(FIXTURE_DIR / "snapshot_005.h5", "r") as late:
        first_progenitor = late["halos/FirstProgenitor"][()]
    return [
        halo
        for halo, target in enumerate(descendant)
        if target >= 0 and first_progenitor[target] != halo
    ]


def fixture_positions(snapshot, halos):
    """The comoving Pos rows of ``halos`` in the fixture's ``snapshot`` file, as tuples."""
    import h5py

    with h5py.File(FIXTURE_DIR / f"snapshot_{snapshot:03d}.h5", "r") as handle:
        positions = handle["halos/Pos"][()]
    return [tuple(float(v) for v in positions[halo]) for halo in halos]


def tie_fixture(destination):
    """Copy the fixture to ``destination`` with every halo's Vmax set to TIE_VMAX; return it.

    With one peak value everywhere, every candidate ties, so the rank is decided entirely by
    ascending UniqueGalaxyID across FoF groups.
    """
    import h5py

    shutil.copytree(FIXTURE_DIR, destination)
    for snap in range(NUM_SNAPSHOTS):
        with h5py.File(Path(destination) / f"snapshot_{snap:03d}.h5", "r+") as handle:
            vmax = handle["halos/Vmax"]
            if vmax.shape[0]:
                vmax[...] = TIE_VMAX
    return Path(destination)


def fixture_branch_peaks(galaxies):
    """The maximum Vmax along each written row's branch, recomputed from the fixture's links.

    Returns {(snapshot, row index): (maximum Vmax, own Vmax)}. A row is matched to its fixture
    halo by its comoving Pos. Its branch is the FirstProgenitor chain of that halo, walked back
    while the progenitor halo also has a written row: core gives a halo the galaxy of its main
    progenitor only when that progenitor carried one, so the chain stops where the galaxy was
    created (a first-snapshot central, or a satellite that became a central). Every snapshot of
    the fixture run is an output snapshot, so "has a written row" is known for the whole chain.
    """
    import h5py

    tables = {}
    for snap in sorted(galaxies):
        with h5py.File(FIXTURE_DIR / f"snapshot_{snap:03d}.h5", "r") as handle:
            halos = handle["halos"]
            tables[snap] = {
                "pos": [tuple(float(v) for v in row) for row in halos["Pos"][()]],
                "vmax": [float(v) for v in halos["Vmax"][()]],
                "first_progenitor": [int(v) for v in halos["FirstProgenitor"][()]],
            }
    halo_of_row = {}
    for snap, rows in galaxies.items():
        positions = tables[snap]["pos"]
        assert len(set(positions)) == len(positions), f"snapshot {snap}: halo positions repeat"
        for index in range(len(rows)):
            position = tuple(float(v) for v in rows["Pos"][index])
            assert position in positions, f"snapshot {snap}: row {index} matches no fixture halo"
            halo_of_row[(snap, index)] = positions.index(position)
    has_row = {(snap, halo) for (snap, _index), halo in halo_of_row.items()}

    peaks = {}
    for (snap, index), halo in halo_of_row.items():
        own = tables[snap]["vmax"][halo]
        peak = own
        step_snap, step_halo = snap, halo
        while step_snap > 0:
            progenitor = tables[step_snap]["first_progenitor"][step_halo]
            if progenitor < 0 or (step_snap - 1, progenitor) not in has_row:
                break
            step_snap, step_halo = step_snap - 1, progenitor
            peak = max(peak, tables[step_snap]["vmax"][step_halo])
        peaks[(snap, index)] = (peak, own)
    return peaks


def test_reference_matches_unit_table():
    """
    Test that the unit test's reference table is the reference script's output.

    Expected: every row of the C table between its SHAM_DECIMAL_REFERENCE markers has
              the reference's case name, h_sim and rank density (C literals parsed with
              float()), the reference's mask/assign outcome, and, when assigned, the
              reference's log10 M* formatted to LOG_MASS_DECIMALS places; masked rows
              carry 0.0. The table has at least eight cases.
    """
    source = UNIT_TEST_SOURCE.read_text()
    block = source.split("SHAM_DECIMAL_REFERENCE_BEGIN")[1].split("SHAM_DECIMAL_REFERENCE_END")[0]
    rows = ROW_PATTERN.findall(block)
    table = {
        name: {
            "params": (float(hubble), float(density)),
            "outcome": "assign" if flag == "1" else "mask",
            "log_mass": log_mass.strip(),
        }
        for name, hubble, density, flag, log_mass in rows
    }
    assert len(rows) == len(table), "case names in the C table must be unique"
    assert len(table) >= 8, f"the C table has {len(table)} cases; at least eight are required"

    reference = case_table()
    reference_params = {name: (hubble, density) for name, hubble, density in CASES}
    assert list(table) == list(reference), f"C cases {list(table)} != reference {list(reference)}"
    for name, (outcome, log_mass) in reference.items():
        row = table[name]
        params_message = f"{name}: C (h, density) {row['params']} != {reference_params[name]}"
        assert row["params"] == reference_params[name], params_message
        assert row["outcome"] == outcome, f"{name}: C table says {row['outcome']}, not {outcome}"
        if outcome == "assign":
            mass_message = f"{name}: C log10 M* {row['log_mass']} != reference {log_mass}"
            assert row["log_mass"] == log_mass, mass_message
            assert len(log_mass.split(".")[1]) == LOG_MASS_DECIMALS
        else:
            assert row["log_mass"] == "0.0", f"{name}: a masked row carries 0.0"
    print(json.dumps({name: f"{o} {m}" for name, (o, m) in reference.items()}))


# ---------------------------------------------------------------------------
# Fixture tests (sham x micro-uchuu-ascii-horizontal)
# ---------------------------------------------------------------------------


def test_fixture_assigned_masses_match_the_reference():
    """
    Test the shipped fixture run against the independent reference, per UniqueGalaxyID.

    Expected: the run succeeds without leaks; at every snapshot every row is Type 0 or 1 with a
              unique ID; each candidate (ShamVpeak >= 80, ranked by descending ShamVpeak then
              ascending ID) has ShamGhost 0 and log10 M* within 1e-4 of the reference for its
              rank density (r + 0.5) / 100^3 * 0.6774^3; no row is masked (the fixture's few
              halos sit far above the floor density); the audit lines report the written
              counts; the last populated snapshot has five candidates with rank 0 at the
              hand-checked log10 M* = 11.65462.
    """
    require_fixture_package()
    run_file, output_dir, stem, parameters = write_run("shipped")
    stdout = run_ok(run_file, "shipped fixture run")
    galaxies = read_galaxies(output_dir, stem)
    counts = assert_run(stdout, galaxies, parameters)

    assert counts[4] == (5, 5, 0), f"snapshot 4 candidates/assigned/masked {counts[4]}"
    assert all(masked == 0 for _c, _a, masked in counts), f"unexpected masking {counts}"
    box_size, h_sim = simulation_constants()
    top = max(range(len(galaxies[4])), key=lambda i: float(galaxies[4]["ShamVpeak"][i]))
    top_mass = math.log10(float(galaxies[4]["StellarMass"][top]) * MASS_UNIT_MSUN / h_sim)
    assert abs(top_mass - 11.65462) <= LOG_MASS_TOLERANCE, f"rank 0 log10 M* {top_mass}"
    print(f"  ✓ candidates/assigned/masked per snapshot {counts} match the reference")


def test_fixture_floors_mask_and_ghost_rows():
    """
    Test the masking and completeness-floor rules on a derivative run file.

    Setup: ShamMinVpeak 194.5 and ShamTargetLogMassFloor 11.6 (the floor density sits between
           the fixture's rank-1 and rank-2 densities).
    Expected: the run succeeds without leaks; snapshot 4 has three candidates of which the first
              two are assigned and the third is masked, and its two rows below the completeness
              floor are not candidates; every masked row and every row below the floor has
              ShamGhost 1 and StellarMass 0; assigned rows match the reference; the audit lines
              report the same counts; the run has at least one masked, one below-floor and one
              assigned row (the case is not vacuous).
    """
    require_fixture_package()
    run_file, output_dir, stem, parameters = write_run("floored", parameters=FLOORED_PARAMETERS)
    stdout = run_ok(run_file, "floored fixture run")
    galaxies = read_galaxies(output_dir, stem)
    counts = assert_run(stdout, galaxies, parameters)

    assert counts[4] == (3, 2, 1), f"snapshot 4 candidates/assigned/masked {counts[4]}"
    below_floor = sum(len(rows) for rows in galaxies.values()) - sum(c for c, _a, _m in counts)
    assert below_floor > 0, "no written row lies below the completeness floor (vacuous)"
    assert sum(a for _c, a, _m in counts) > 0, "nothing is assigned (vacuous)"
    print(f"  ✓ counts {counts}: {below_floor} rows below the floor; masking and ghost flags hold")


def test_fixture_merged_centrals_are_retired():
    """
    Test the retirement path end to end on the shipped fixture run.

    Expected: the fixture's snapshot-4 halos 0 and 2 are the FoF centrals absorbed by
              snapshot-5 halo 0 without being its main progenitor (read from the fixture's
              links); their galaxies, found by Pos, are written at snapshot 4 and absent at
              snapshot 5, where core demotes them to Type 2 and the module retires them to
              Type 3; the galaxy of halo 1 (the main progenitor) is written at both; no Type 2
              row appears at any snapshot.
    """
    require_fixture_package()
    demoted = demoted_fixture_halos()
    assert demoted == [0, 2], f"the fixture's demoted snapshot-4 halos are {demoted}, not [0, 2]"

    run_file, output_dir, stem, _parameters = write_run("retired")
    run_ok(run_file, "retirement run")
    galaxies = read_galaxies(output_dir, stem)

    def id_at(position):
        matches = [
            int(row["UniqueGalaxyID"])
            for row in galaxies[4]
            if tuple(float(v) for v in row["Pos"]) == position
        ]
        assert len(matches) == 1, f"position {position} matches {len(matches)} snapshot-4 rows"
        return matches[0]

    later_ids = {int(i) for i in galaxies[5]["UniqueGalaxyID"]}
    for halo, position in zip(demoted, fixture_positions(4, demoted)):
        galaxy_id = id_at(position)
        assert galaxy_id not in later_ids, f"halo {halo}'s galaxy {galaxy_id} survived snapshot 5"
    (survivor,) = fixture_positions(4, [1])
    assert id_at(survivor) in later_ids, "the main progenitor's galaxy must survive snapshot 5"
    for snap, rows in galaxies.items():
        assert 2 not in {int(t) for t in rows["Type"]}, f"snapshot {snap} wrote a Type 2 row"
    print(f"  ✓ the galaxies of snapshot-4 halos {demoted} are written at 4 and absent at 5")


def test_fixture_peak_history_is_the_branch_maximum():
    """
    Test ShamVpeak end to end against the fixture's own merger-tree links.

    Expected: the run succeeds without leaks; at every snapshot every written row's ShamVpeak
              equals the maximum Vmax along its branch (the FirstProgenitor chain back to the
              snapshot where its galaxy was created), recomputed from the fixture's Vmax and
              link datasets rather than read from the module; the case is not vacuous: at least
              one row's peak exceeds its own snapshot's Vmax, so the history, not just the
              current value, is exercised. Covers the peak ratchet through core inheritance for
              Type 0 rows; the fixture has no Type 1 galaxy.
    """
    require_fixture_package()
    run_file, output_dir, stem, _parameters = write_run("peaks")
    run_ok(run_file, "peak history run")
    galaxies = read_galaxies(output_dir, stem)
    peaks = fixture_branch_peaks(galaxies)

    carried = 0
    for (snap, index), (peak, own) in sorted(peaks.items()):
        measured = float(galaxies[snap]["ShamVpeak"][index])
        galaxy_id = int(galaxies[snap]["UniqueGalaxyID"][index])
        assert measured == peak, (
            f"snapshot {snap}: ID {galaxy_id} has ShamVpeak {measured}, but the maximum Vmax "
            f"along its branch in the fixture is {peak}"
        )
        carried += peak > own
    assert carried > 0, "no row carries a peak above its own Vmax (vacuous)"
    print(f"  ✓ ShamVpeak equals the branch maximum for {len(peaks)} rows ({carried} carried)")


def test_fixture_repeated_runs_are_bitwise_identical():
    """
    Test that two runs of the shipped run file give identical Galaxies datasets.

    Expected: every snapshot's Galaxies dataset is byte-identical field by field.
    """
    require_fixture_package()
    outputs = []
    for label in ("repeat_a", "repeat_b"):
        run_file, output_dir, stem, _parameters = write_run(label)
        run_ok(run_file, label)
        outputs.append(all_field_bytes(read_galaxies(output_dir, stem)))
    assert outputs[0].keys() == outputs[1].keys(), "repeated runs must write the same snapshots"
    assert outputs[0] == outputs[1], "repeated runs must be bitwise identical"
    print("  ✓ two runs are bitwise identical at every snapshot")


def test_fixture_cross_fof_ties_rank_by_unique_id():
    """
    Test the tie rule across FoF groups on a derivative fixture with a flat Vmax.

    Setup: a copy of the fixture's snapshot files in the temp dir with Vmax set to 200 km/s for
           every halo through h5py, and the run file's input.simulation_dir and
           snapshot_list_file pointed at it, so every ShamVpeak is 200.
    Expected: the run succeeds without leaks; at every snapshot all rows have ShamVpeak 200 and
              their masses follow the reference with ranks assigned by ascending UniqueGalaxyID
              (so StellarMass falls as the ID rises); at snapshot 4 the rows come from at least
              three distinct FoF groups and their output order differs from ID order, so the
              ranking is global rather than per FoF or by row.
    """
    require_fixture_package()
    import numpy as np

    fixture = tie_fixture(TEMP_DIR / "tie_fixture")
    overrides = {
        "simulation_dir": str(fixture),
        "snapshot_list_file": str(fixture / "micro-uchuu-fixture.a_list"),
    }
    run_file, output_dir, stem, parameters = write_run("ties", input_overrides=overrides)
    stdout = run_ok(run_file, "tie fixture run")
    galaxies = read_galaxies(output_dir, stem)
    assert_run(stdout, galaxies, parameters)

    for snap, rows in galaxies.items():
        assert np.all(rows["ShamVpeak"] == TIE_VMAX), f"snapshot {snap}: peaks are not all tied"
        order = np.argsort(rows["UniqueGalaxyID"])
        masses = rows["StellarMass"][order]
        assert np.all(np.diff(masses) < 0.0), f"snapshot {snap}: masses {masses} not by ID rank"
    rows = galaxies[4]
    assert len(np.unique(rows["UniqueCentralGalaxyID"])) >= 3, "fewer than three FoF groups"
    ids = [int(i) for i in rows["UniqueGalaxyID"]]
    assert ids != sorted(ids), "output order equals ID order, so the tie check is vacuous"
    print(f"  ✓ {len(ids)} tied rows at snapshot 4 rank by ascending UniqueGalaxyID across FoFs")


def test_fixture_invalid_configurations_are_rejected_at_startup():
    """
    Test that the module's startup contract is enforced before any snapshot is loaded.

    Expected: with ShamTargetRedshiftMax 0.1 (the fixture's first snapshots lie at z = 0.19 and
              0.14) and with no pre_timestep entry, the run fails at startup naming the rule,
              loads no snapshot and writes no master file.
    """
    require_fixture_package()
    cases = {
        "redshift_window": (
            {"parameters": {"ShamTargetRedshiftMax": 0.1}},
            None,
            "above ShamTargetRedshiftMax = 0.1",
        ),
        "missing_pre_timestep": (
            {},
            {"post_snapshot": [{"sham_rank_match": "process_snapshot"}]},
            "must be configured exactly once in modules.pre_timestep as process_full_halo",
        ),
    }
    for label, (extra, modules, message) in cases.items():
        run_file, output_dir, stem, _parameters = write_run(
            f"reject_{label}", parameters=extra.get("parameters"), modules=modules
        )
        returncode, stdout, stderr = run_mimic(run_file, cwd=REPO_ROOT)
        output = stdout + stderr
        assert returncode != 0, f"{label}: the configuration must be rejected:\n{output}"
        assert message in output, f"{label}: expected '{message}':\n{output}"
        assert "Loaded snapshot" not in output, f"{label}: the rejection must precede any snapshot"
        assert not (Path(output_dir) / f"{stem}.hdf5").exists(), f"{label}: no master file"
    print(f"  ✓ {len(cases)} invalid configurations fail at startup")


def main():
    global TEMP_DIR
    TEMP_DIR = Path(tempfile.mkdtemp(prefix="mimic_sham_rank_match_"))
    try:
        tests = [
            test_reference_matches_unit_table,
            test_fixture_assigned_masses_match_the_reference,
            test_fixture_floors_mask_and_ghost_rows,
            test_fixture_merged_centrals_are_retired,
            test_fixture_peak_history_is_the_branch_maximum,
            test_fixture_repeated_runs_are_bitwise_identical,
            test_fixture_cross_fof_ties_rank_by_unique_id,
            test_fixture_invalid_configurations_are_rejected_at_startup,
        ]
        return run_test_suite(tests, "sham_rank_match (test_integration_sham_rank_match.py)")
    finally:
        shutil.rmtree(TEMP_DIR, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
