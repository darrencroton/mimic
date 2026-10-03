#!/usr/bin/env python3
"""
sham_global_rank - Integration Test

Validates the snapshot-wide rank through the real executable on the committed
micro-Uchuu horizontal fixture (simulations/micro-uchuu-ascii-horizontal/_tests/data/
generic: three forests over six snapshots, BoxSize 100 Mpc/h, snapshot 0 empty,
two Type 2 orphans at snapshot 5, no Vmax ties), run from the shipped example
models/sham/input/sham_global_micro-uchuu-ascii-horizontal.yaml (M0 = 8,
n0 = 1e-6 (Mpc/h)^-3, alpha = 1, so n0 * BoxSize^3 = 1 and rank r receives
8 / (r + 0.5)):

  - every written snapshot's StellarMass, read per UniqueGalaxyID from the HDF5
    output, is the independent rank oracle of its ShamVpeak values (descending,
    ties by ascending ID, Types 0/1/2, zero peaks unranked), which proves the
    callback's writes reach the output;
  - peaks persist and are inherited: Types 0/1 ratchet up from Vmax, Type 2
    orphans keep the peak of their last resolved snapshot;
  - two runs are bitwise identical;
  - on temporary derivatives of the fixture (a copy in the test's own temp dir in
    which only snapshot 5 Vmax values are edited through h5py), an exact
    cross-FoF tie goes to the lower ID and a higher proxy in one FoF moves the
    ranks of every other FoF;
  - configuring sham_assign_stellar_mass alongside fails at startup, and a rank
    mass above the module's range aborts the run without a master file;
  - the numerical edge table in test_unit_sham_global_rank.c is the 60-digit
    Decimal reference of sham_global_rank_reference.py.

Run directly with MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal after building
that pair with TEST_BUILD=yes (make tests-snapshot-global-sham does both). Every case
except the pure-Python Decimal cross-check reports a configuration SKIP under any other
simulation package: the default sham pair, mini-millennium, is vertical and cannot run
a post_snapshot module.
"""

import json
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
    TestSkipped,
    check_no_memory_leaks,
    compiled_model,
    compiled_simulation,
    run_mimic,
    run_test_suite,
    selected_package_is_horizontal,
)
from sham_global_rank_reference import EDGE_CASES, edge_table  # noqa: E402

TEMP_DIR = None

#: The only package whose committed fixture these cases are written against.
FIXTURE_SIMULATION = "micro-uchuu-ascii-horizontal"

EXAMPLE_RUN_FILE = (
    REPO_ROOT / "models" / "sham" / "input" / (f"sham_global_{FIXTURE_SIMULATION}.yaml")
)
UNIT_TEST_SOURCE = Path(__file__).resolve().parent / "test_unit_sham_global_rank.c"
FIXTURE_DIR = REPO_ROOT / "simulations" / FIXTURE_SIMULATION / "_tests" / "data" / "generic"

#: Example target: M0 / (r + 0.5) because n0 * BoxSize^3 = 1e-6 * 100^3 = 1.
MASS_SCALE = 8.0

#: Independent-oracle bound in float ULPs (see test_unit_sham_global_rank.c).
MAX_ORACLE_ULPS = 2

NUM_SNAPSHOTS = 6
FINAL_SNAPSHOT = NUM_SNAPSHOTS - 1

#: Snapshot 5 of the committed fixture, ranked by hand from its links: the two
#: Type 0 survivors of forest 0 keep peaks 202 (ID 1000000005) and 193 (ID
#: 1000000003), forest 1's central keeps 214 (ID 2000000002), and the two
#: non-main-branch progenitors of snapshot 4 become Type 2 orphans that carry 195
#: (ID 1000000006, its snapshot 3 Vmax) and 194 (ID 1000000004). The snapshot 4/5
#: Vmax = 215/213 satellite has no galaxy (it never had a progenitor central).
FINAL_SNAPSHOT_ORACLE = {
    2000000002: (0, 214.0, 0),
    1000000005: (0, 202.0, 1),
    1000000006: (2, 195.0, 2),
    1000000004: (2, 194.0, 3),
    1000000003: (0, 193.0, 4),
}

#: Fields the module resets for every entry (besides the two stellar-mass fields).
RESET_FIELDS = ("BulgeMass", "MetalsStellarMass", "MetalsBulgeMass", "StarFormationRate")

LEGACY_PARAMETERS = {
    "ShamLogM1": 11.590,
    "ShamN": 0.0351,
    "ShamBeta": 1.376,
    "ShamGamma": 0.608,
    "ShamUseScatter": 1,
    "ShamScatterDex": 0.20,
    "ShamMinMpeak": 0.10,
    "ShamMinVpeak": 80.0,
    "ShamMaxStellarBaryonFraction": 0.17,
    "ShamOrphanMaxAgeMyr": 3000.0,
}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def require_fixture_package():
    """Raise the configuration SKIP unless the selected pair can run these cases."""
    if compiled_simulation() != FIXTURE_SIMULATION or compiled_model() != "sham":
        layout = "horizontal" if selected_package_is_horizontal() else "vertical"
        raise TestSkipped(
            f"configuration SKIP: selected package {compiled_simulation()} is {layout} and not "
            f"the {FIXTURE_SIMULATION} fixture these cases are written against (MODEL="
            f"{compiled_model()}); sham_global_rank runs only in post_snapshot under the "
            f"horizontal driver (run make tests-snapshot-global-sham)"
        )
    if not MIMIC_EXE.exists():
        raise FileNotFoundError(f"Mimic executable not found at {MIMIC_EXE}")


def write_run(name, parameters=None, modules=None, simulation_dir=None):
    """Copy the example run file into the temp dir with a private output directory.

    The copy keeps the example's repository-relative simulation.config, so every run
    starts from the repository root exactly as the example documents.
    """
    with open(EXAMPLE_RUN_FILE, "r") as handle:
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
    if simulation_dir is not None:
        config["input"] = {"simulation_dir": str(simulation_dir)}
    run_file = TEMP_DIR / f"{name}.yaml"
    with open(run_file, "w") as handle:
        yaml.safe_dump(config, handle, default_flow_style=False, sort_keys=False)
    return run_file, output_dir, config["output"]["output_filename"]


def run_ok(run_file, what):
    returncode, stdout, stderr = run_mimic(run_file, cwd=REPO_ROOT)
    assert returncode == 0, f"{what}: the run should complete (rc={returncode}):\n{stdout}{stderr}"
    assert check_no_memory_leaks(stdout, stderr), f"{what}: the run reported a memory leak"
    return stdout


def snapshot_rows(output_dir, stem, snap):
    import h5py

    with h5py.File(Path(output_dir) / f"{stem}_{snap:03d}.hdf5", "r") as handle:
        return handle[f"Snap{snap:03d}"]["Galaxies"][()]


def all_snapshot_rows(output_dir, stem):
    return [snapshot_rows(output_dir, stem, snap) for snap in range(NUM_SNAPSHOTS)]


def float32_bits(value):
    import numpy as np

    return int(np.array([value], dtype=np.float32).view(np.uint32)[0])


def expected_rank_mass(rank):
    """The independent double evaluation M0 / (r + 0.5), rounded to float32."""
    return MASS_SCALE / (rank + 0.5)


def assert_mass_close(got, expected, what):
    ulps = abs(float32_bits(got) - float32_bits(expected))
    assert ulps <= MAX_ORACLE_ULPS, f"{what}: got {got!r}, expected {expected!r} ({ulps} ULPs)"


def assert_snapshot_ranked(rows, what):
    """StellarMass of every row is the brute-force rank oracle of the ShamVpeak values."""
    ids = [int(value) for value in rows["UniqueGalaxyID"]]
    assert len(set(ids)) == len(ids), f"{what}: UniqueGalaxyIDs must be unique"
    assert set(int(t) for t in rows["Type"]) <= {0, 1, 2}, f"{what}: only Types 0/1/2"
    eligible = [
        (float(row["ShamVpeak"]), int(row["UniqueGalaxyID"]))
        for row in rows
        if float(row["ShamVpeak"]) > 0.0
    ]
    order = sorted(eligible, key=lambda item: (-item[0], item[1]))
    rank_of = {galaxy_id: rank for rank, (_vpeak, galaxy_id) in enumerate(order)}
    for row in rows:
        galaxy_id = int(row["UniqueGalaxyID"])
        mass = row["StellarMass"]
        no_scatter_bits = float32_bits(row["ShamStellarMassNoScatter"])
        assert no_scatter_bits == float32_bits(mass), f"{what}: ID {galaxy_id} mass fields differ"
        assert float(row["ShamScatterDex"]) == 0.0, f"{what}: ID {galaxy_id} ShamScatterDex"
        for field in RESET_FIELDS:
            assert float(row[field]) == 0.0, f"{what}: ID {galaxy_id} {field} must be reset"
        if galaxy_id in rank_of:
            assert_mass_close(
                mass, expected_rank_mass(rank_of[galaxy_id]), f"{what}: ID {galaxy_id}"
            )
        else:
            assert float(mass) == 0.0, f"{what}: unranked ID {galaxy_id} must have zero mass"
    return rank_of


def per_id(rows):
    return {int(row["UniqueGalaxyID"]): row for row in rows}


def galaxy_field_bytes(rows):
    """Each field's values as bytes (exact, NaN payloads included), ignoring row padding."""
    return tuple((name, rows[name].tobytes()) for name in rows.dtype.names)


def hdf5_contents(path):
    """Every object's attributes and (dtype, shape, bytes) data in an HDF5 file, by name."""
    import h5py
    import numpy as np

    found = {}
    with h5py.File(path, "r") as handle:

        def visit(item_name, item):
            attrs = tuple(sorted((k, np.asarray(v).tobytes()) for k, v in item.attrs.items()))
            data = None
            if isinstance(item, h5py.Dataset):
                data = (item.dtype.str, item.shape, item[()].tobytes())
            found[item_name] = (attrs, data)

        handle.visititems(visit)
        found["/"] = tuple(sorted((k, np.asarray(v).tobytes()) for k, v in handle.attrs.items()))
    return found


def assert_early_snapshots_unchanged(rows, base, what):
    """Snapshots 0-4 of a snapshot-5-only derivative match the committed-fixture run bytewise."""
    for snap in range(FINAL_SNAPSHOT):
        unchanged = galaxy_field_bytes(rows[snap]) == galaxy_field_bytes(base[snap])
        assert unchanged, f"{what}: snapshot {snap} must not change"


def make_derivative(name, edits):
    """Copy the committed fixture into the temp dir and edit snapshot 5 Vmax values only.

    edits maps an existing snapshot 5 Vmax value (unique in that slab) to its
    replacement. Every other dataset, attribute and file is checked to be the
    committed fixture's own; fixture_manifest.json is copied untouched.
    """
    import h5py
    import numpy as np

    derivative = TEMP_DIR / name
    shutil.copytree(FIXTURE_DIR, derivative)
    with h5py.File(derivative / f"snapshot_{FINAL_SNAPSHOT:03d}.h5", "r+") as handle:
        vmax = handle["halos"]["Vmax"]
        values = vmax[()]
        for old, new in edits.items():
            matches = np.flatnonzero(values == np.float32(old))
            assert len(matches) == 1, f"derivative: Vmax {old} must appear once at snapshot 5"
            vmax[int(matches[0])] = np.float32(new)

    for original in sorted(FIXTURE_DIR.iterdir()):
        copy = derivative / original.name
        if original.suffix != ".h5":
            assert copy.read_bytes() == original.read_bytes(), f"derivative: {original.name}"
            continue
        before, after = hdf5_contents(original), hdf5_contents(copy)
        assert before.keys() == after.keys(), f"derivative: {original.name} object set changed"
        changed = sorted(key for key in before if before[key] != after[key])
        allowed = ["halos/Vmax"] if original.name == f"snapshot_{FINAL_SNAPSHOT:03d}.h5" else []
        assert changed == allowed, f"derivative: {original.name} changed {changed}"
    return derivative


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_example_ranks_every_snapshot_globally():
    """
    Test that the example run assigns every snapshot's rank masses, visible in the output.

    Expected: the run succeeds without leaks; snapshot 0 is empty; every snapshot's
              StellarMass per UniqueGalaxyID equals 8 / (r + 0.5) for its brute-force
              rank within two float ULPs (zero for unranked entries); the final snapshot
              matches the hand-ranked oracle (two Type 2 orphans ranked); EnabledModules
              and Parameters record the module and its three parameters; the run logs
              "Loaded snapshot" (the positive control for the startup-rejection checks).
    """
    import h5py

    require_fixture_package()
    run_file, output_dir, stem = write_run("example")
    stdout = run_ok(run_file, "example run")
    assert "SHAM global rank initialized" in stdout, "the module must initialize"
    # Positive control: the startup-rejection tests assert this string is absent, which is
    # only meaningful if a successful run under the same harness does print it.
    assert "Loaded snapshot" in stdout, "a successful run must log each loaded snapshot"

    rows = all_snapshot_rows(output_dir, stem)
    assert len(rows[0]) == 0, "snapshot 0 of the fixture is empty"
    ranked = 0
    for snap, snap_rows in enumerate(rows):
        ranked += len(assert_snapshot_ranked(snap_rows, f"snapshot {snap}"))
    assert ranked > 0, "the example must rank galaxies"

    final = per_id(rows[FINAL_SNAPSHOT])
    assert set(final) == set(FINAL_SNAPSHOT_ORACLE), f"final snapshot IDs {sorted(final)}"
    for galaxy_id, (galaxy_type, vpeak, rank) in FINAL_SNAPSHOT_ORACLE.items():
        row = final[galaxy_id]
        assert int(row["Type"]) == galaxy_type, f"ID {galaxy_id} Type {row['Type']}"
        assert float(row["ShamVpeak"]) == vpeak, f"ID {galaxy_id} ShamVpeak {row['ShamVpeak']}"
        assert_mass_close(row["StellarMass"], expected_rank_mass(rank), f"final ID {galaxy_id}")
    assert float(final[2000000002]["StellarMass"]) == 16.0, "rank 0 receives exactly 16"

    with h5py.File(Path(output_dir) / f"{stem}.hdf5", "r") as handle:
        run_properties = handle["RunProperties"]
        enabled = [
            tuple(field.decode() for field in (r["module_name"], r["phase"], r["processing_mode"]))
            for r in run_properties["EnabledModules"][()]
        ]
        parameters = run_properties["Parameters"][()]
    assert enabled == [("sham_global_rank", "post_snapshot", "process_snapshot")], enabled
    recorded = {
        row[parameters.dtype.names[0]].decode(): row[parameters.dtype.names[1]].decode()
        for row in parameters
    }
    for name in ("ShamGlobalMassScale", "ShamGlobalNumberDensity", "ShamGlobalSlope"):
        assert name in recorded, f"Parameters must record {name}: {recorded}"
    print(f"  ✓ {ranked} ranked entries across {NUM_SNAPSHOTS} snapshots match the oracle")


def test_peaks_persist_and_are_inherited():
    """
    Test that ShamVpeak follows the peak rules across snapshots in the written output.

    Expected: a galaxy's first Type 0/1 appearance has ShamVpeak = Vmax; later Type 0/1
              appearances have max(previous ShamVpeak, Vmax); Type 2 keeps its previous
              peak exactly; Type 0/1 ShamMpeak is float32(max(previous ShamMpeak, Mvir))
              (float32(Mvir) at first appearance); Type 2 ShamMpeak is unchanged.
    """
    import numpy as np

    require_fixture_package()
    run_file, output_dir, stem = write_run("peaks")
    run_ok(run_file, "peaks run")

    previous = {}
    checked_orphans = 0
    for snap, snap_rows in enumerate(all_snapshot_rows(output_dir, stem)):
        current = per_id(snap_rows)
        for galaxy_id, row in current.items():
            vpeak, mpeak = float(row["ShamVpeak"]), float(row["ShamMpeak"])
            if galaxy_id not in previous:
                assert int(row["Type"]) in (0, 1), f"snapshot {snap}: ID {galaxy_id} new orphan"
                assert vpeak == float(row["Vmax"]), f"snapshot {snap}: ID {galaxy_id} first peak"
                first_mpeak = (
                    f"snapshot {snap}: ID {galaxy_id} first ShamMpeak {mpeak} != float32(Mvir)"
                )
                assert mpeak == float(np.float32(row["Mvir"])), first_mpeak
                continue
            old = previous[galaxy_id]
            if int(row["Type"]) == 2:
                checked_orphans += 1
                assert vpeak == float(old["ShamVpeak"]), f"snapshot {snap}: orphan {galaxy_id}"
                assert mpeak == float(old["ShamMpeak"]), f"snapshot {snap}: orphan {galaxy_id}"
            else:
                expected = max(float(old["ShamVpeak"]), float(row["Vmax"]))
                assert vpeak == expected, f"snapshot {snap}: ID {galaxy_id} ShamVpeak {vpeak}"
                previous_mpeak = float(old["ShamMpeak"])
                expected_mpeak = float(np.float32(max(previous_mpeak, float(row["Mvir"]))))
                assert mpeak == expected_mpeak, (
                    f"snapshot {snap}: ID {galaxy_id} ShamMpeak {mpeak} != float32(max(previous "
                    f"{previous_mpeak}, Mvir {float(row['Mvir'])}))"
                )
        previous = current
    assert checked_orphans >= 2, "the fixture's final snapshot carries two orphans"
    assert float(previous[1000000006]["ShamVpeak"]) == 195.0, "orphan keeps its snapshot 3 peak"
    print(f"  ✓ peaks follow the ratchet and {checked_orphans} orphan entries keep theirs")


def test_repeated_runs_are_bitwise_identical():
    """
    Test that two runs of the example give identical output.

    Expected: every snapshot's Galaxies datasets are byte-identical field by field.
    """
    require_fixture_package()
    outputs = []
    for label in ("repeat_a", "repeat_b"):
        run_file, output_dir, stem = write_run(label)
        run_ok(run_file, label)
        outputs.append([galaxy_field_bytes(rows) for rows in all_snapshot_rows(output_dir, stem)])
    assert outputs[0] == outputs[1], "repeated runs must be bitwise identical"
    print("  ✓ two runs are bitwise identical at every snapshot")


def test_derivative_cross_fof_tie_goes_to_lower_id():
    """
    Test the tie key end to end on a temporary derivative of the fixture.

    Expected: with the snapshot 5 Vmax of ID 1000000005 (forest 0) raised from 201 to
              214, it ties forest 1's ID 2000000002 at ShamVpeak 214; the lower ID wins
              rank 0 (16) and the other gets rank 1 (16/3); snapshots 0-4 are
              bitwise identical to the committed-fixture run.
    """
    require_fixture_package()
    derivative = make_derivative("fixture_tie", {201.0: 214.0})
    run_file, output_dir, stem = write_run("tie", simulation_dir=derivative)
    run_ok(run_file, "tie run")
    base_file, base_dir, _ = write_run("tie_base")
    run_ok(base_file, "tie base run")

    rows = all_snapshot_rows(output_dir, stem)
    base = all_snapshot_rows(base_dir, stem)
    assert_early_snapshots_unchanged(rows, base, "tie")
    final = per_id(rows[FINAL_SNAPSHOT])
    assert float(final[1000000005]["ShamVpeak"]) == float(final[2000000002]["ShamVpeak"]) == 214.0
    assert_snapshot_ranked(rows[FINAL_SNAPSHOT], "tie snapshot 5")
    assert float(final[1000000005]["StellarMass"]) == 16.0, "the lower ID wins the tie"
    assert_mass_close(final[2000000002]["StellarMass"], expected_rank_mass(1), "tie loser")
    print("  ✓ the cross-FoF tie is broken by ascending UniqueGalaxyID")


def test_derivative_higher_proxy_moves_other_fofs():
    """
    Test that the rank is global: a higher proxy in one FoF moves every other FoF.

    Expected: with the snapshot 5 Vmax of ID 1000000003 (forest 0) raised from 191 to
              300, it takes rank 0 and every other galaxy, including forest 1's
              ID 2000000002, moves down exactly one rank; snapshots 0-4 are bitwise
              identical to the committed-fixture run.
    """
    require_fixture_package()
    derivative = make_derivative("fixture_higher", {191.0: 300.0})
    run_file, output_dir, stem = write_run("higher", simulation_dir=derivative)
    run_ok(run_file, "higher-proxy run")
    base_file, base_dir, _ = write_run("higher_base")
    run_ok(base_file, "higher-proxy base run")

    rows = all_snapshot_rows(output_dir, stem)
    assert_early_snapshots_unchanged(rows, all_snapshot_rows(base_dir, stem), "higher proxy")
    final = per_id(rows[FINAL_SNAPSHOT])
    assert float(final[1000000003]["StellarMass"]) == 16.0, "the raised galaxy takes rank 0"
    for galaxy_id, (_type, _vpeak, rank) in FINAL_SNAPSHOT_ORACLE.items():
        if galaxy_id == 1000000003:
            continue
        assert_mass_close(
            final[galaxy_id]["StellarMass"], expected_rank_mass(rank + 1), f"ID {galaxy_id}"
        )
    print("  ✓ a higher proxy in forest 0 lowers forest 1's rank")


def test_legacy_sham_alongside_is_rejected_at_startup():
    """
    Test that the two SHAM prescriptions cannot be configured together.

    Expected: with sham_assign_stellar_mass in post_timestep as well, the run fails at
              startup naming both modules and writes no master file.
    """
    require_fixture_package()
    modules = {
        "post_timestep": [{"sham_assign_stellar_mass": "process_full_halo"}],
        "post_snapshot": [{"sham_global_rank": "process_snapshot"}],
    }
    run_file, output_dir, stem = write_run("legacy", parameters=LEGACY_PARAMETERS, modules=modules)
    returncode, stdout, stderr = run_mimic(run_file, cwd=REPO_ROOT)
    output = stdout + stderr
    assert returncode != 0, f"both prescriptions must be rejected:\n{output}"
    assert "independent stellar-mass prescriptions" in output, output
    assert "Loaded snapshot" not in output, "the rejection must precede any snapshot"
    assert not (Path(output_dir) / f"{stem}.hdf5").exists(), "no master file may be written"
    print("  ✓ sham_assign_stellar_mass alongside sham_global_rank fails at startup")


def test_invalid_parameters_fail_at_startup():
    """
    Test that non-finite or out-of-domain parameters fail init before any snapshot.

    Expected: each run fails with the parameter named and no snapshot loaded.
    """
    require_fixture_package()
    cases = {
        "mass_nan": {"ShamGlobalMassScale": "nan"},
        "mass_inf": {"ShamGlobalMassScale": "inf"},
        "mass_overflow": {"ShamGlobalMassScale": "1e400"},
        "mass_above": {"ShamGlobalMassScale": 100001.0},
        "density_low": {"ShamGlobalNumberDensity": 1.0e-13},
        "slope_high": {"ShamGlobalSlope": 11.0},
    }
    for label, parameters in cases.items():
        run_file, _output_dir, _stem = write_run(f"invalid_{label}", parameters=parameters)
        returncode, stdout, stderr = run_mimic(run_file, cwd=REPO_ROOT)
        output = stdout + stderr
        assert returncode != 0, f"{label}: the run must fail:\n{output}"
        assert next(iter(parameters)) in output, f"{label}: the parameter must be named"
        assert "Loaded snapshot" not in output, f"{label}: init must fail before any snapshot"
    print(f"  ✓ {len(cases)} invalid parameter sets fail at startup")


def test_rank_mass_above_range_aborts_the_run():
    """
    Test that a rank mass above 100000 fails the snapshot and the run.

    Expected: with M0 = 100000 the first populated snapshot's rank 0 needs 200000; the
              run fails naming rank, UniqueGalaxyID, ln M and the parameters, and leaves
              no master file.
    """
    require_fixture_package()
    run_file, output_dir, stem = write_run("too_massive", parameters={"ShamGlobalMassScale": 1e5})
    returncode, stdout, stderr = run_mimic(run_file, cwd=REPO_ROOT)
    output = stdout + stderr
    assert returncode != 0, f"an out-of-range rank mass must fail the run:\n{output}"
    match = re.search(r"rank 0 \(UniqueGalaxyID (\d+)\) has ln M = ([\d.e+-]+) above", output)
    assert match, f"the diagnostic must name rank, ID and ln M:\n{output}"
    assert re.search(r"M0=100000 n0=\S+ alpha=1 BoxSize=100\)", output), output
    assert not (Path(output_dir) / f"{stem}.hdf5").exists(), "no master file may be written"
    print(f"  ✓ rank 0 (ID {match.group(1)}) above the range aborts the run")


def test_decimal_reference_matches_unit_table():
    """
    Test that the unit test's numerical edge table is the 60-digit Decimal reference.

    Expected: every row of the C table between its SHAM_DECIMAL_REFERENCE markers has
              the case name, all four parameters (M0, n0 and alpha as strings parsed
              with float(), BoxSize as a double literal) equal to the reference's
              EDGE_CASES values, and the outcome and float32 bits that
              sham_global_rank_reference.py computes. Pure Python: it needs no
              executable, so it runs under every simulation pair.
    """
    source = UNIT_TEST_SOURCE.read_text()
    block = source.split("SHAM_DECIMAL_REFERENCE_BEGIN")[1].split("SHAM_DECIMAL_REFERENCE_END")[0]
    rows = re.findall(
        r'\{"(\w+)", "([^"]*)", "([^"]*)", "([^"]*)", ([^,]+), (\d), (0x[0-9A-F]+|0)u\}', block
    )
    table = {
        name: {
            "params": tuple(float(value) for value in (m0, n0, alpha, box)),
            "outcome": "pass" if flag == "1" else "fail",
            "bits": int(bits, 16),
        }
        for name, m0, n0, alpha, box, flag, bits in rows
    }
    reference = edge_table()
    reference_params = {name: tuple(params) for name, *params, rank in EDGE_CASES}
    assert all(rank == 0 for *_head, rank in EDGE_CASES), "the C table evaluates rank 0 only"
    assert table.keys() == reference.keys(), f"edge cases {sorted(table)} != {sorted(reference)}"
    for name, (outcome, bits) in reference.items():
        params_message = (
            f"{name}: C parameters {table[name]['params']} != reference {reference_params[name]}"
        )
        assert table[name]["params"] == reference_params[name], params_message
        assert table[name]["outcome"] == outcome, f"{name}: C table says {table[name]['outcome']}"
        if outcome == "pass":
            c_bits = table[name]["bits"]
            assert c_bits == bits, f"{name}: C bits {c_bits:#x} != {bits:#x}"
    print(json.dumps({name: f"{o} {b}" for name, (o, b) in reference.items()}))


def main():
    global TEMP_DIR
    TEMP_DIR = Path(tempfile.mkdtemp(prefix="mimic_sham_global_rank_"))
    try:
        tests = [
            test_example_ranks_every_snapshot_globally,
            test_peaks_persist_and_are_inherited,
            test_repeated_runs_are_bitwise_identical,
            test_derivative_cross_fof_tie_goes_to_lower_id,
            test_derivative_higher_proxy_moves_other_fofs,
            test_legacy_sham_alongside_is_rejected_at_startup,
            test_invalid_parameters_fail_at_startup,
            test_rank_mass_above_range_aborts_the_run,
            test_decimal_reference_matches_unit_table,
        ]
        return run_test_suite(tests, "sham_global_rank (test_integration_sham_global_rank.py)")
    finally:
        shutil.rmtree(TEMP_DIR, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
