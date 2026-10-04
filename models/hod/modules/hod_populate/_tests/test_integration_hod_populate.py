#!/usr/bin/env python3
"""
hod_populate - Integration Test

Validates the HOD population through the real executable and the HDF5 output, read per
UniqueGalaxyID, under the two packages the model ships run files for:

  hod x micro-uchuu-ascii-horizontal  the committed fixture (three forests over six snapshots,
      BoxSize 100 Mpc/h, hosts of 10^11.2 to 10^12 Msun/h), run from the shipped
      models/hod/input/hod_micro-uchuu-ascii-horizontal.yaml. With the shipped Mr < -20
      parameters those hosts almost never host a satellite, so the created-row cases run a
      temporary derivative of that run file in the test's own temp dir with lower mass
      parameters (HODLogMmin 11.8, HODLogM0 10.0, HODLogM1 10.8: some hosts keep a central,
      some do not, and the present ones draw many satellites). The shipped run file itself
      keeps the published values and has its own validity case.
  hod x mini-millennium  the vertical run file models/hod/input/hod_mini-millennium.yaml on the
      package's eight tree files, which has no post_snapshot phase and so no audit.

Each case reads every written snapshot's Galaxies datasets and proves the output contract: every
created row is a Type 2 row with a negative UniqueGalaxyID, HODGhost 0 and a
UniqueCentralGalaxyID naming a Type 0 host with HODGhost 0 (so a host with HODGhost 1 has no
satellite); every Type 1 row has HODGhost 1; no Type 2 row survives from a previous snapshot
(on the horizontal fixture the snapshot number is decoded from the created ID); repeating a
run is bitwise identical on the Galaxies datasets; a different HODSeed changes a draw; a run that
names only the last fixture snapshot is bitwise identical there to the all-snapshot run
(earlier snapshots are not written, so their reset is proved by the unit test); conflicting
module configurations fail at startup; and every run is leak-free.

Run directly with MODEL=hod SIMULATION=<pair> after building that pair with TEST_BUILD=yes
(make tests-snapshot-global-hod does both for the fixture pair and passes --cases fixture;
make MODEL=hod SIMULATION=mini-millennium tests-integration runs every case, the vertical ones
for real). Every case reports a configuration SKIP under any other pair. Fixture cases are
named test_fixture_*, vertical cases test_vertical_*.
"""

import argparse
import re
import shutil
import sys
import tempfile
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    MIMIC_EXE,
    TestSkipped,
    check_no_memory_leaks,
    compiled_model,
    compiled_simulation,
    run_mimic,
    run_test_suite,
)

TEMP_DIR = None

FIXTURE_SIMULATION = "micro-uchuu-ascii-horizontal"
VERTICAL_SIMULATION = "mini-millennium"

FIXTURE_RUN_FILE = REPO_ROOT / "models" / "hod" / "input" / f"hod_{FIXTURE_SIMULATION}.yaml"
VERTICAL_RUN_FILE = REPO_ROOT / "models" / "hod" / "input" / f"hod_{VERTICAL_SIMULATION}.yaml"
VERTICAL_TREE_FILES = [
    REPO_ROOT / "simulations" / VERTICAL_SIMULATION / "snapshots" / f"trees_063.{index}"
    for index in range(8)
]

NUM_SNAPSHOTS = 6
FINAL_SNAPSHOT = NUM_SNAPSHOTS - 1

#: MAX_CREATED_RECORDS_PER_HOST in src/include/constants.h, the created-ID radix.
ID_RADIX = 1024

#: Mass parameters for the fixture derivative: with these, host masses of 10^11.3 to 10^12 give
#: <Ncen> from about 0.001 to 0.9 (a mix of present and absent centrals) and large satellite
#: means (lambda = ((M - 10^10) / 10^10.8)^1.06).
LOW_MASS_PARAMETERS = {"HODLogMmin": 11.8, "HODLogM0": 10.0, "HODLogM1": 10.8}

AUDIT_LINE = re.compile(
    r"^HOD audit z=(-?\d+\.\d{4}) hosts=(\d+) n_gal expected=(\S+) realised=(\S+) "
    r"f_sat expected=(\S+) realised=(\S+)$",
    re.MULTILINE,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def require_package(simulation, run_file):
    """Raise the configuration SKIP unless the selected pair is hod x ``simulation``."""
    if compiled_model() != "hod" or compiled_simulation() != simulation:
        raise TestSkipped(
            f"configuration SKIP: selected pair is MODEL={compiled_model()} "
            f"SIMULATION={compiled_simulation()}, not the hod x {simulation} pair these cases "
            f"are written against (run make tests-snapshot-global-hod for the fixture pair, "
            f"make MODEL=hod SIMULATION=mini-millennium tests-integration for the vertical one)"
        )
    if not MIMIC_EXE.exists():
        raise FileNotFoundError(f"Mimic executable not found at {MIMIC_EXE}")
    assert run_file.exists(), f"missing run file {run_file}"


def require_fixture_package():
    require_package(FIXTURE_SIMULATION, FIXTURE_RUN_FILE)


def require_vertical_package():
    require_package(VERTICAL_SIMULATION, VERTICAL_RUN_FILE)
    missing = [path.name for path in VERTICAL_TREE_FILES if not path.exists()]
    if missing:
        raise TestSkipped(
            f"data SKIP: the mini-Millennium tree files {missing} are absent from "
            f"simulations/{VERTICAL_SIMULATION}/snapshots (./scripts/first_run.sh fetches them)"
        )


def write_run(base_file, name, parameters=None, modules=None, snapshot_list=None):
    """Copy a shipped run file into the temp dir with a private output directory.

    The copy keeps the shipped file's repository-relative simulation.config, so every run
    starts from the repository root exactly as the file documents. ``modules`` replaces the
    module phases (the parameters block is kept); ``parameters`` updates single parameters.
    """
    with open(base_file, "r") as handle:
        config = yaml.safe_load(handle)
    output_dir = TEMP_DIR / name
    output_dir.mkdir(parents=True, exist_ok=True)
    config["output"]["output_directory"] = str(output_dir)
    if snapshot_list is not None:
        config["output"]["snapshot_list"] = list(snapshot_list)
    if modules is not None:
        params = config["modules"]["parameters"]
        config["modules"] = dict(modules)
        config["modules"]["parameters"] = params
    if parameters is not None:
        config["modules"]["parameters"].update(parameters)
    run_file = TEMP_DIR / f"{name}.yaml"
    with open(run_file, "w") as handle:
        yaml.safe_dump(config, handle, default_flow_style=False, sort_keys=False)
    return run_file, output_dir, config["output"]["output_filename"]


def run_ok(run_file, what):
    """Run Mimic, require success and a clean leak report, and return its stdout."""
    returncode, stdout, stderr = run_mimic(run_file, cwd=REPO_ROOT)
    assert returncode == 0, f"{what}: the run should complete (rc={returncode}):\n{stdout}{stderr}"
    assert check_no_memory_leaks(stdout, stderr), f"{what}: the run reported a memory leak"
    return stdout


def read_galaxies(output_dir, stem):
    """Every written (file, snapshot) Galaxies dataset: {(file name, snapshot): rows}.

    A horizontal run writes one file per output snapshot, a vertical run one file per tree
    file, each holding every output snapshot; this reads both layouts.
    """
    import h5py

    found = {}
    for path in sorted(Path(output_dir).glob(f"{stem}_*.hdf5")):
        with h5py.File(path, "r") as handle:
            for group_name in handle:
                if group_name.startswith("Snap"):
                    found[(path.name, int(group_name[4:]))] = handle[group_name]["Galaxies"][()]
    assert found, f"no snapshot files written under {output_dir}"
    return found


def galaxy_field_bytes(rows):
    """Each field's values as bytes (exact, NaN payloads included), ignoring row padding."""
    return tuple((name, rows[name].tobytes()) for name in rows.dtype.names)


def all_field_bytes(galaxies):
    return {key: galaxy_field_bytes(rows) for key, rows in galaxies.items()}


def snapshots_of(galaxies):
    """{snapshot: [rows of every file at that snapshot]} with files in name order."""
    import numpy as np

    grouped = {}
    for (_name, snap), rows in sorted(galaxies.items()):
        grouped.setdefault(snap, []).append(rows)
    return {snap: np.concatenate(parts) for snap, parts in grouped.items()}


def decode_created_id(galaxy_id):
    """Split a created ID -(1 + ordinal + 1024 * host_key) into (host_key, ordinal)."""
    assert galaxy_id < 0, f"ID {galaxy_id} is not in the created namespace"
    magnitude = -int(galaxy_id) - 1
    return magnitude // ID_RADIX, magnitude % ID_RADIX


def rows_per_unit_of(stdout):
    """The run-wide largest slab (horizontal) or forest bound the identity INFO line reports."""
    match = re.search(r"rows_per_unit=(\d+)", stdout)
    assert match, f"the run must log the created-record identity space:\n{stdout}"
    return int(match.group(1))


def assert_output_contract(rows, what, rows_per_unit=None, snapshot=None):
    """Assert the scaffold/sample contract on one snapshot's rows; return created-row count.

    With ``rows_per_unit`` and ``snapshot`` (horizontal runs, where the host key is
    ``row + rows_per_unit * snapshot``) every created ID must also decode to this snapshot, which
    proves no created row of an earlier snapshot survived.
    """
    import numpy as np

    ids = rows["UniqueGalaxyID"]
    assert len(np.unique(ids)) == len(ids), f"{what}: UniqueGalaxyIDs must be unique"
    types = rows["Type"]
    ghosts = rows["HODGhost"]
    assert set(int(t) for t in np.unique(types)) <= {0, 1, 2}, f"{what}: only Types 0/1/2"
    assert set(int(g) for g in np.unique(ghosts)) <= {0, 1}, f"{what}: HODGhost is 0 or 1"
    assert np.all(ghosts[types == 1] == 1), f"{what}: every Type 1 row is scaffold (HODGhost 1)"

    sample_hosts = {int(i) for i in ids[(types == 0) & (ghosts == 0)]}
    created = rows[types == 2]
    per_host = {}
    for row in created:
        galaxy_id = int(row["UniqueGalaxyID"])
        assert galaxy_id < 0, f"{what}: Type 2 row {galaxy_id} is not a created row (survivor)"
        assert int(row["HODGhost"]) == 0, f"{what}: created row {galaxy_id} must be a sample row"
        host = int(row["UniqueCentralGalaxyID"])
        assert host in sample_hosts, (
            f"{what}: created row {galaxy_id} names host {host}, which is not a Type 0 row with "
            f"HODGhost 0 (a host with HODGhost 1 must have no satellite)"
        )
        per_host[host] = per_host.get(host, 0) + 1
        host_key, ordinal = decode_created_id(galaxy_id)
        if rows_per_unit is not None:
            assert host_key // rows_per_unit == snapshot, (
                f"{what}: created row {galaxy_id} decodes to snapshot "
                f"{host_key // rows_per_unit}, so it survived from an earlier snapshot"
            )
    assert all(count <= ID_RADIX for count in per_host.values()), f"{what}: radix exceeded"
    return len(created)


# ---------------------------------------------------------------------------
# Fixture tests (hod x micro-uchuu-ascii-horizontal)
# ---------------------------------------------------------------------------


def test_fixture_created_rows_follow_the_output_contract():
    """
    Test the scaffold/sample output contract on the fixture derivative with lowered masses.

    Expected: the run succeeds without leaks; at every snapshot every created row is a Type 2
              row with a negative ID that decodes to that snapshot, HODGhost 0 and a host that
              is a Type 0 row with HODGhost 0; every Type 1 row has HODGhost 1; the two
              tree-born Type 2 orphans of the fixture's last snapshot do not survive; created
              rows exist at the last snapshot, which also has a Type 0 host with HODGhost 1
              (absent central, hence no satellite); the audit logs one line per snapshot.
    """
    require_fixture_package()
    run_file, output_dir, stem = write_run(
        FIXTURE_RUN_FILE, "fixture_contract", parameters=LOW_MASS_PARAMETERS
    )
    stdout = run_ok(run_file, "fixture derivative run")
    rows_per_unit = rows_per_unit_of(stdout)

    populations = snapshots_of(read_galaxies(output_dir, stem))
    assert sorted(populations) == list(range(NUM_SNAPSHOTS)), sorted(populations)
    created_total = 0
    for snap, rows in populations.items():
        created_total += assert_output_contract(rows, f"snapshot {snap}", rows_per_unit, snap)

    final = populations[FINAL_SNAPSHOT]
    created_final = int((final["Type"] == 2).sum())
    ghost_hosts = int(((final["Type"] == 0) & (final["HODGhost"] == 1)).sum())
    sample_hosts = int(((final["Type"] == 0) & (final["HODGhost"] == 0)).sum())
    assert created_final > 0, "the last snapshot must carry created rows"
    assert sample_hosts > 0 and ghost_hosts > 0, (
        f"the derivative must exercise present and absent centrals at the last snapshot "
        f"(sample hosts {sample_hosts}, ghost hosts {ghost_hosts})"
    )
    audits = AUDIT_LINE.findall(stdout)
    assert len(audits) == NUM_SNAPSHOTS, f"one audit line per output snapshot: {audits}"
    print(
        f"  ✓ {created_total} created rows over {NUM_SNAPSHOTS} snapshots follow the contract "
        f"({created_final} at the last, with {ghost_hosts} ghost host(s))"
    )


def test_fixture_shipped_run_file_is_valid():
    """
    Test that the shipped fixture run file, with the published Mr < -20 values, runs cleanly.

    Expected: the run succeeds without leaks; every snapshot satisfies the output contract
              (the fixture's low-mass hosts yield few or no satellites at these parameters, so
              this does not require created rows); one audit line per snapshot with the
              documented format; the two tree-born orphans of the last snapshot are gone.
    """
    require_fixture_package()
    run_file, output_dir, stem = write_run(FIXTURE_RUN_FILE, "fixture_shipped")
    stdout = run_ok(run_file, "shipped fixture run")
    rows_per_unit = rows_per_unit_of(stdout)

    populations = snapshots_of(read_galaxies(output_dir, stem))
    for snap, rows in populations.items():
        assert_output_contract(rows, f"snapshot {snap}", rows_per_unit, snap)
    final = populations[FINAL_SNAPSHOT]
    assert len(final) > 0 and bool((final["Type"] != 2).all()), "no orphan survives at snapshot 5"
    audits = AUDIT_LINE.findall(stdout)
    assert len(audits) == NUM_SNAPSHOTS, f"one audit line per output snapshot: {audits}"
    print(f"  ✓ the shipped run file logs {len(audits)} audit lines and satisfies the contract")


def test_fixture_repeated_runs_are_bitwise_identical():
    """
    Test that two runs of the fixture derivative give identical Galaxies datasets.

    Expected: every snapshot's Galaxies dataset is byte-identical field by field.
    """
    require_fixture_package()
    outputs = []
    for label in ("repeat_a", "repeat_b"):
        run_file, output_dir, stem = write_run(
            FIXTURE_RUN_FILE, label, parameters=LOW_MASS_PARAMETERS
        )
        run_ok(run_file, label)
        outputs.append(all_field_bytes(read_galaxies(output_dir, stem)))
    assert outputs[0].keys() == outputs[1].keys(), "repeated runs must write the same snapshots"
    assert outputs[0] == outputs[1], "repeated runs must be bitwise identical"
    print("  ✓ two runs are bitwise identical at every snapshot")


def test_fixture_changing_the_seed_changes_a_draw():
    """
    Test that HODSeed enters the draws.

    Expected: with HODSeed 2 instead of 1 the Galaxies datasets differ in at least one
              snapshot, and the seeded run still satisfies the output contract.
    """
    require_fixture_package()
    outputs = []
    for seed in (1, 2):
        parameters = dict(LOW_MASS_PARAMETERS, HODSeed=seed)
        run_file, output_dir, stem = write_run(
            FIXTURE_RUN_FILE, f"seed_{seed}", parameters=parameters
        )
        stdout = run_ok(run_file, f"seed {seed} run")
        galaxies = read_galaxies(output_dir, stem)
        rows_per_unit = rows_per_unit_of(stdout)
        for snap, rows in snapshots_of(galaxies).items():
            assert_output_contract(rows, f"seed {seed} snapshot {snap}", rows_per_unit, snap)
        outputs.append(all_field_bytes(galaxies))
    differing = [key for key in outputs[0] if outputs[0][key] != outputs[1].get(key)]
    assert differing, "a different HODSeed must change at least one snapshot's draws"
    print(f"  ✓ HODSeed 2 changes {len(differing)} of {len(outputs[0])} snapshots")


def test_fixture_last_snapshot_only_run_matches_the_full_run():
    """
    Test that an output list naming only the last snapshot reproduces its rows exactly.

    Expected: the run writes only the last fixture snapshot, carries created rows there, and
              its Galaxies dataset is byte-identical to the last snapshot of the run that
              writes every snapshot (draws depend on the snapshot and host, not on which
              earlier snapshots were written; an unlisted snapshot only retires and resets).
    """
    require_fixture_package()
    full_file, full_dir, stem = write_run(
        FIXTURE_RUN_FILE, "list_full", parameters=LOW_MASS_PARAMETERS
    )
    run_ok(full_file, "all-snapshot run")
    last_file, last_dir, _ = write_run(
        FIXTURE_RUN_FILE,
        "list_last",
        parameters=LOW_MASS_PARAMETERS,
        snapshot_list=[FINAL_SNAPSHOT],
    )
    last_stdout = run_ok(last_file, "last-snapshot run")

    full = read_galaxies(full_dir, stem)
    last = read_galaxies(last_dir, stem)
    assert [snap for (_name, snap) in last] == [FINAL_SNAPSHOT], f"written snapshots {sorted(last)}"
    rows = snapshots_of(last)[FINAL_SNAPSHOT]
    created = assert_output_contract(
        rows, "last-snapshot run", rows_per_unit_of(last_stdout), FINAL_SNAPSHOT
    )
    assert created > 0, "the last-snapshot run must write created rows"
    full_last = [rows for (_name, snap), rows in full.items() if snap == FINAL_SNAPSHOT]
    assert len(full_last) == 1, "the full run writes the last snapshot once"
    identical = galaxy_field_bytes(rows) == galaxy_field_bytes(full_last[0])
    assert identical, "the last-snapshot run must be bitwise identical to the full run's last"
    print(f"  ✓ the last-snapshot run writes {created} created rows identical to the full run's")


def test_fixture_conflicting_configurations_are_rejected_at_startup():
    """
    Test that the module's phase contract is enforced before any snapshot is loaded.

    Expected: with hod_populate absent from post_timestep (the audit alone), and with a
              post_snapshot phase that does not contain it, the run fails at startup naming
              the module and the rule, loads no snapshot and writes no master file.
    """
    require_fixture_package()
    cases = {
        "absent_from_post_timestep": (
            {"post_snapshot": [{"hod_populate": "process_snapshot"}]},
            "must be configured exactly once in modules.post_timestep",
        ),
        "post_snapshot_without_module": (
            {
                "post_timestep": [{"hod_populate": "process_full_halo"}],
                "post_snapshot": [{"test_snapshot_fixture": "process_snapshot"}],
            },
            "modules.post_snapshot is configured but does not contain hod_populate",
        ),
    }
    for label, (modules, message) in cases.items():
        run_file, output_dir, stem = write_run(
            FIXTURE_RUN_FILE, f"conflict_{label}", modules=modules
        )
        returncode, stdout, stderr = run_mimic(run_file, cwd=REPO_ROOT)
        output = stdout + stderr
        assert returncode != 0, f"{label}: the configuration must be rejected:\n{output}"
        assert message in output, f"{label}: expected '{message}':\n{output}"
        assert "Loaded snapshot" not in output, f"{label}: the rejection must precede any snapshot"
        assert not (Path(output_dir) / f"{stem}.hdf5").exists(), f"{label}: no master file"
    print(f"  ✓ {len(cases)} conflicting configurations fail at startup")


# ---------------------------------------------------------------------------
# Vertical tests (hod x mini-millennium)
# ---------------------------------------------------------------------------


def test_vertical_run_creates_rows_that_follow_the_output_contract():
    """
    Test the output contract under the vertical driver on the eight mini-Millennium files.

    Expected: the run succeeds without leaks; every output snapshot of every file satisfies the
              contract (created rows are Type 2 with negative IDs, HODGhost 0 and a Type 0
              HODGhost 0 host; Type 1 rows are scaffold; no tree-born or inherited Type 2 row
              survives); created rows exist at the last snapshot, where the box also has Type 1
              rows and Type 0 hosts with HODGhost 1; there is no audit line (no post_snapshot).
    """
    require_vertical_package()
    run_file, output_dir, stem = write_run(VERTICAL_RUN_FILE, "vertical_contract")
    stdout = run_ok(run_file, "vertical run")
    assert not AUDIT_LINE.search(stdout), "the vertical run has no post_snapshot audit"

    galaxies = read_galaxies(output_dir, stem)
    assert len({name for name, _snap in galaxies}) == 8, "one output file per tree file"
    created_total = 0
    for (name, snap), rows in sorted(galaxies.items()):
        created_total += assert_output_contract(rows, f"{name} snapshot {snap}")
    assert created_total > 0, "the vertical run must create rows"
    final = snapshots_of(galaxies)[63]
    assert int((final["Type"] == 2).sum()) > 0, "the last snapshot must carry created rows"
    assert int((final["Type"] == 1).sum()) > 0, "the last snapshot must have Type 1 rows"
    assert int(((final["Type"] == 0) & (final["HODGhost"] == 1)).sum()) > 0, "no ghost hosts"
    print(
        f"  ✓ {created_total} created rows over {len(galaxies)} file-snapshots follow the contract"
    )


def test_vertical_repeated_runs_are_bitwise_identical():
    """
    Test that two vertical runs give identical Galaxies datasets.

    Expected: every file's every snapshot is byte-identical field by field.
    """
    require_vertical_package()
    outputs = []
    for label in ("vertical_repeat_a", "vertical_repeat_b"):
        run_file, output_dir, stem = write_run(VERTICAL_RUN_FILE, label)
        run_ok(run_file, label)
        outputs.append(all_field_bytes(read_galaxies(output_dir, stem)))
    assert outputs[0].keys() == outputs[1].keys(), "repeated runs must write the same datasets"
    assert outputs[0] == outputs[1], "repeated vertical runs must be bitwise identical"
    print(f"  ✓ two vertical runs are bitwise identical across {len(outputs[0])} datasets")


def main(argv=None):
    """Run every case, or with ``--cases fixture|vertical`` only the ``test_<cases>_*`` ones.

    The snapshot-global battery builds only the fixture pair and gates on a zero-skip count, so
    it passes ``--cases fixture``; the integration tier of a mini-Millennium build runs them all
    and reports the fixture cases as configuration skips.
    """
    global TEMP_DIR
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[1])
    parser.add_argument("--cases", choices=("fixture", "vertical"), default=None)
    args = parser.parse_args(argv)
    TEMP_DIR = Path(tempfile.mkdtemp(prefix="mimic_hod_populate_"))
    try:
        tests = [
            test_fixture_created_rows_follow_the_output_contract,
            test_fixture_shipped_run_file_is_valid,
            test_fixture_repeated_runs_are_bitwise_identical,
            test_fixture_changing_the_seed_changes_a_draw,
            test_fixture_last_snapshot_only_run_matches_the_full_run,
            test_fixture_conflicting_configurations_are_rejected_at_startup,
            test_vertical_run_creates_rows_that_follow_the_output_contract,
            test_vertical_repeated_runs_are_bitwise_identical,
        ]
        if args.cases:
            tests = [test for test in tests if test.__name__.startswith(f"test_{args.cases}_")]
        return run_test_suite(tests, "hod_populate (test_integration_hod_populate.py)")
    finally:
        shutil.rmtree(TEMP_DIR, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
