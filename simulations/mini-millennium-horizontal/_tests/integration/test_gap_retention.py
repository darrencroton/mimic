#!/usr/bin/env python3
"""
mini-Millennium Horizontal v3 Gap-Retention Tests

Validates: the horizontal driver's retained-generation pool on version 3 input.
The committed fixtures (../data/, converted by ../data/regenerate.sh from the
L-Halo sources ../data/source/generate_sources.py writes) are run end to end
through the real reader and driver, and each test pins, by hand:

  - the worked five-halo mixed-gap graph (``trees_worked_graph.0``, defined in
    ../data/source/generate_sources.py): that the committed fixture is that
    graph, the retention horizon and release point of every generation, the
    expected inheritance and every output row;
  - a progenitor chain spanning three snapshots, with every progenitor at row 0
    of its own slab so a lookup naming a progenitor by row alone would mistake
    the main branch;
  - an adjacent version 3 dataset (links_adjacent = 1) with a same-snapshot
    NextProgenitor, which must never retain more than two generations;
  - release of every retained generation on failure, at an output write and at
    a slab load, with the reader's run closed and no slab left loaded.

A generation's retention horizon is the latest snapshot any of its halos names
as its descendant's, or its own snapshot when none has a descendant. The driver
releases an earlier generation right after the sweep of its horizon, and a
generation whose horizon is its own snapshot right after that snapshot's
output.

The expected output rows are what the vertical path emits for the same source
trees: at a snapshot between a halo and its gapped descendant, only the galaxies
of halos that exist there (the galaxy of a halo whose descendant skips the
snapshot is emitted at its own snapshot and next at the descendant's). That is
what this test pins; the measured vertical/horizontal comparison lives in the
real-data parity gate (../scientific/test_cross_format_identity.py), not here.

Row expectations hold for the physics-free halos-only model only; under another
model those tests report NA and the lifecycle tests still run.

Skips automatically when:
  - SIMULATION != mini-millennium-horizontal (wrong compiled package)
  - Mimic is not built
Reports NA when:
  - MODEL != halos-only (output-row tests only)

Run with:
  MODEL=halos-only SIMULATION=mini-millennium-horizontal \\
      python3 simulations/mini-millennium-horizontal/_tests/integration/test_gap_retention.py
or (registered):
  make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-integration
"""

import math
import os
import shutil
import sys
import tempfile
from pathlib import Path

import yaml


def find_repo_root(start):
    for parent in [start, *start.parents]:
        if (parent / "tests").is_dir() and (parent / "src").is_dir():
            return parent
    raise RuntimeError(f"Could not find repository root from {start}")


REPO_ROOT = find_repo_root(Path(__file__).resolve())
DATA_DIR = REPO_ROOT / "simulations" / "mini-millennium-horizontal" / "_tests" / "data"

sys.path.insert(0, str(REPO_ROOT / "tests"))

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

TEMP_DIR = None

# Worked-graph halos by their HaloRankInForest (their row in the one source tree),
# which with ForestIndex 0 fixes each galaxy's UniqueGalaxyID.
RANK = {"E": 0, "D": 1, "A": 2, "B": 3, "C": 4}


def _require_package():
    sim = compiled_simulation()
    if sim != "mini-millennium-horizontal":
        raise TestSkipped(f"compiled simulation is {sim!r}, not mini-millennium-horizontal")
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")


def _require_halos_only():
    if compiled_model() != "halos-only":
        raise TestNotApplicable(
            f"compiled model is {compiled_model()!r}; the expected rows are physics-free "
            f"halos-only inheritance"
        )


def _run(name, fixture, snapshot_list, dataset_dir=None, prepare_output=None):
    """Run the fixture through the compiled model's shipped run file for this package.

    Returns (returncode, stdout, stderr, output_dir).
    """
    model = compiled_model()
    base = REPO_ROOT / "models" / model / "input" / f"{model}_mini-millennium-horizontal.yaml"
    with open(base, "r") as handle:
        config = yaml.safe_load(handle)

    dataset_dir = Path(dataset_dir) if dataset_dir is not None else DATA_DIR / fixture
    output_dir = Path(TEMP_DIR) / f"{name}_output"
    output_dir.mkdir(parents=True, exist_ok=True)
    if prepare_output is not None:
        prepare_output(output_dir)

    config.setdefault("input", {}).update(
        {
            "simulation_dir": str(dataset_dir),
            "snapshot_list_file": str(dataset_dir / f"{fixture}.a_list"),
        }
    )
    config.setdefault("output", {}).update(
        {
            "output_filename": "model",
            "output_format": "hdf5",
            "output_directory": str(output_dir),
            "snapshot_list": list(snapshot_list),
        }
    )

    run_file = Path(TEMP_DIR) / f"{name}.yaml"
    with open(run_file, "w") as handle:
        yaml.safe_dump(config, handle, default_flow_style=False, sort_keys=False)

    returncode, stdout, stderr = run_mimic(run_file)
    return returncode, stdout, stderr, output_dir


def _assert_in_order(text, needles, what):
    """Assert every needle occurs in `text`, in the given order."""
    cursor = 0
    for needle in needles:
        found = text.find(needle, cursor)
        assert found >= 0, (
            f"{what}: expected {needle!r} after position {cursor}; the lifecycle is "
            f"incomplete or out of order:\n{text}"
        )
        cursor = found + len(needle)


def _lifecycle(steps):
    """Expected stdout needles for a run whose snapshots are each
    (snap, live, horizon, earlier releases, written, released at own snapshot)."""
    needles = []
    for snap, live, horizon, released_earlier, written, released_self in steps:
        needles.append(f"Loaded snapshot {snap} (")
        needles.append(f"; {live} slab{'' if live == 1 else 's'} live")
        needles.append(f"Snapshot {snap} retention horizon is snapshot {horizon}")
        for earlier in released_earlier:
            needles.append(f"Released snapshot {earlier} ")
        if written:
            needles.append(f"Wrote snapshot {snap} output")
        if released_self:
            needles.append(f"Released snapshot {snap} ")
    return needles


def _assert_clean_success(returncode, stdout, stderr, nsnapshots, max_live):
    output = stdout + stderr
    assert returncode == 0, f"the run should complete:\n{output}"
    released = stdout.count("Released snapshot ")
    message = f"each of the {nsnapshots} snapshots must be released exactly once:\n{stdout}"
    assert released == nsnapshots, message
    peak = f"Retained at most {max_live} generations concurrently"
    assert peak in stdout, f"the driver should report {peak!r}:\n{stdout}"
    closed = "Closed horizontal run 'horizontal_hdf5' with no slab loaded"
    assert closed in stdout, f"the reader's run must close with no slab loaded:\n{output}"
    assert "No memory leaks detected" in stdout, f"the leak report should be clean:\n{output}"
    assert check_no_memory_leaks(stdout, stderr), "the run reported a memory leak"


def _rows(output_dir, snap):
    """Every galaxy row the run wrote for `snap`, and the run's identity multiplier."""
    import h5py

    with h5py.File(output_dir / f"model_{snap:03d}.hdf5", "r") as handle:
        multiplier = int(handle["RunProperties"].attrs["UniqueGalaxyIDMultiplier"].ravel()[0])
        rows = handle[f"Snap{snap:03d}"]["Galaxies"][()]
    return rows, multiplier


def _identity(rows):
    return [(int(row["UniqueGalaxyID"]), int(row["Type"]), int(row["SnapNum"])) for row in rows]


def _close(a, b):
    return math.isclose(a, b, rel_tol=1e-12, abs_tol=0.0)


# ---------------------------------------------------------------------------
# The worked five-halo mixed-gap graph
# ---------------------------------------------------------------------------

# The design review's emitted dataset, verbatim: per snapshot, one tuple per row of
# (SourceHaloID, HaloRankInForest, Descendant, DescendantSnapshot, FirstProgenitor,
#  FirstProgenitorSnapshot, NextProgenitor, NextProgenitorSnapshot,
#  FirstHaloInFOFgroup, NextHaloInFOFgroup).
WORKED_GRAPH_TABLES = {
    0: [(3, 2, 0, 2, -1, -1, 0, 1, 0, -1)],  # A
    1: [(4, 3, 0, 2, -1, -1, 1, 1, 0, 1), (5, 4, 0, 2, -1, -1, -1, -1, 0, -1)],  # B, C
    2: [(2, 1, 0, 4, 0, 0, -1, -1, 0, -1)],  # D
    3: [],
    4: [(1, 0, -1, -1, 0, 2, -1, -1, 0, -1)],  # E
}

WORKED_GRAPH_COLUMNS = (
    "SourceHaloID",
    "HaloRankInForest",
    "Descendant",
    "DescendantSnapshot",
    "FirstProgenitor",
    "FirstProgenitorSnapshot",
    "NextProgenitor",
    "NextProgenitorSnapshot",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
)


def test_worked_graph_fixture_is_the_design_review_graph():
    """
    Test that the committed worked-graph fixture is the design review's graph, row for row.

    Expected: five files; n_halos 1, 2, 1, 0, 1; links_adjacent 0 in every file (the
              dataset has gaps, so every file says so); every link, target snapshot,
              SourceHaloID and HaloRankInForest equal to the review's tables; ForestIndex 0.
    Validates: every expectation below is about the graph the review worked through,
               not about whatever the converter happened to emit.
    """
    import h5py

    for snap, expected_rows in WORKED_GRAPH_TABLES.items():
        path = DATA_DIR / "worked_graph" / f"snapshot_{snap:03d}.h5"
        with h5py.File(path, "r") as handle:
            header = handle["header"].attrs
            assert int(header["format_version"]) == 3, f"{path.name} is not version 3"
            assert int(header["links_adjacent"]) == 0, f"{path.name} must say links_adjacent 0"
            assert int(header["n_halos"]) == len(expected_rows), f"{path.name} n_halos"
            columns = [handle["halos"][name][()] for name in WORKED_GRAPH_COLUMNS]
            rows = [tuple(int(column[i]) for column in columns) for i in range(len(expected_rows))]
            forest = [int(v) for v in handle["halos"]["ForestIndex"][()]]
        assert rows == expected_rows, f"snapshot {snap}: {rows} != review table {expected_rows}"
        assert forest == [0] * len(expected_rows), f"snapshot {snap}: one forest, ForestIndex 0"


def test_worked_graph_retention_schedule():
    """
    Test the worked graph's horizons, live-generation counts and release points.

    Expected (the design review's "Retained gap-state ownership" section):
      horizons 2, 2, 4, 3, 4;
      snapshot 0 loads with 1 slab live, 1 with 2, 2 with 3 (0, 1 and 2 held together);
      0 and 1 are released right after snapshot 2's sweep, before its output;
      the empty snapshot 3 loads with 2 live (2 and 3) and is released right after its
      own (empty) output, because nothing points past it;
      snapshot 4 loads with 2 live (2 and 4); 2 is released after its sweep, 4 after its
      output; peak 3 retained; the run closes with no slab loaded and no leak.
    """
    _require_package()
    returncode, stdout, stderr, _ = _run("worked_schedule", "worked_graph", [0, 1, 2, 3, 4])
    _assert_clean_success(returncode, stdout, stderr, nsnapshots=5, max_live=3)
    _assert_in_order(
        stdout,
        _lifecycle(
            [
                (0, 1, 2, [], True, False),
                (1, 2, 2, [], True, False),
                (2, 3, 4, [0, 1], True, False),
                (3, 2, 3, [], True, True),
                (4, 2, 4, [2], True, True),
            ]
        ),
        "worked graph",
    )
    gapped = "links_adjacent 0" in stdout
    assert gapped, f"the run should report the dataset as gapped (links_adjacent 0):\n{stdout}"


def test_worked_graph_inheritance_and_output_rows():
    """
    Test every output row of the worked graph, hand-derived from the vertical path's rules.

    Expected rows (UniqueGalaxyID = HaloRankInForest + multiplier x (ForestIndex + 1)):
      snapshot 0: A's new central galaxy                         [(A, Type 0)]
      snapshot 1: B's new central galaxy; C is a satellite with
                  no progenitor and gets none; A's galaxy is not
                  emitted here, because A's descendant is D(2)   [(B, Type 0)]
      snapshot 2: D inherits A (FirstProgenitor, occupied, so
                  the main branch) and B (orphaned); C has none  [(A, 0), (B, 2)]
      snapshot 3: empty, and nothing crosses it is emitted       []
      snapshot 4: E inherits both, B's orphan staying Type 2     [(A, 0), (B, 2)]
    Inheritance spans the real gap: A's galaxy evolves over Age[0] -> Age[2], so
    dT(A@2) = dT(B@1) + dT(B@2), where B's galaxy was born over Age[0] -> Age[1] and
    evolved over Age[1] -> Age[2]; every galaxy at snapshot 4 evolved over Age[2] ->
    Age[4], across the empty snapshot 3. The main branch takes D's and E's catalog
    virial mass; the orphan's is zero.
    """
    _require_package()
    _require_halos_only()
    returncode, stdout, stderr, output_dir = _run("worked_rows", "worked_graph", [0, 1, 2, 3, 4])
    assert returncode == 0, f"the run should complete:\n{stdout}{stderr}"

    rows = {}
    for snap in range(5):
        rows[snap], multiplier = _rows(output_dir, snap)
    uid = {halo: rank + multiplier for halo, rank in RANK.items()}

    expected = {
        0: [(uid["A"], 0, 0)],
        1: [(uid["B"], 0, 1)],
        2: [(uid["A"], 0, 2), (uid["B"], 2, 2)],
        3: [],
        4: [(uid["A"], 0, 4), (uid["B"], 2, 4)],
    }
    for snap in range(5):
        found = _identity(rows[snap])
        assert found == expected[snap], f"snapshot {snap}: rows {found} != {expected[snap]}"

    dt_a0 = float(rows[0]["dT"][0])
    dt_b1 = float(rows[1]["dT"][0])
    dt_a2, dt_b2 = (float(v) for v in rows[2]["dT"])
    dt_a4, dt_b4 = (float(v) for v in rows[4]["dT"])
    assert dt_a0 == -1.0, "a galaxy born at snapshot 0 carries the no-interval sentinel"
    assert dt_b1 > 0 and dt_b2 > 0, "every later interval is positive"
    assert _close(dt_a2, dt_b1 + dt_b2), (
        f"A's galaxy must evolve over the whole gap Age[0] -> Age[2]: dT {dt_a2} != "
        f"{dt_b1} + {dt_b2}"
    )
    assert dt_a2 > dt_b2, "A's interval spans the gap, B's only the last snapshot"
    # Both galaxies at snapshot 4 evolve over Age[2] -> Age[4], across the empty snapshot 3.
    assert dt_a4 == dt_b4 > dt_b2, "snapshot 4 intervals must span Age[2] -> Age[4]"

    mvir = {snap: [float(v) for v in rows[snap]["Mvir"]] for snap in (2, 4)}
    assert mvir[2] == [50.0, 0.0], f"snapshot 2 Mvir: main branch takes D's, orphan 0: {mvir[2]}"
    assert mvir[4] == [60.0, 0.0], f"snapshot 4 Mvir: main branch takes E's, orphan 0: {mvir[4]}"


# ---------------------------------------------------------------------------
# A chain spanning three snapshots
# ---------------------------------------------------------------------------


def test_three_snapshot_chain():
    """
    Test one progenitor chain spanning snapshots 0, 1 and 2 into D at snapshot 3.

    Expected lifecycle: every horizon is 3, so the loads see 1, 2, 3 and 4 slabs live;
    0, 1 and 2 are released after snapshot 3's sweep and 3 after its output; peak 4.
    Expected rows (halos-only): snapshot 3 holds P0's galaxy as the main branch (the
    occupied head is pinned although P1 is heavier) and P1's and P2's as orphans --
    P1 and P2 sit at row 0 of their own slabs, P0's row, so naming the main branch by row
    alone would make all three centrals and abort. Intervals span the chain:
    dT(P0@3) = dT(P1@1) + dT(P2@2) + dT(P2@3) and dT(P1@3) = dT(P2@2) + dT(P2@3).
    """
    _require_package()
    returncode, stdout, stderr, output_dir = _run("chain", "three_snapshot_chain", [0, 1, 2, 3])
    _assert_clean_success(returncode, stdout, stderr, nsnapshots=4, max_live=4)
    _assert_in_order(
        stdout,
        _lifecycle(
            [
                (0, 1, 3, [], True, False),
                (1, 2, 3, [], True, False),
                (2, 3, 3, [], True, False),
                (3, 4, 3, [0, 1, 2], True, True),
            ]
        ),
        "three-snapshot chain",
    )

    if compiled_model() != "halos-only":
        return None

    rows = {}
    for snap in range(4):
        rows[snap], multiplier = _rows(output_dir, snap)
    p0, p1, p2 = (multiplier + rank for rank in (1, 2, 3))
    assert _identity(rows[3]) == [(p0, 0, 3), (p1, 2, 3), (p2, 2, 3)], _identity(rows[3])
    for snap, galaxy in ((0, p0), (1, p1), (2, p2)):
        assert _identity(rows[snap]) == [(galaxy, 0, snap)], _identity(rows[snap])

    dt_p1_born = float(rows[1]["dT"][0])
    dt_p2_born = float(rows[2]["dT"][0])
    dt_p0, dt_p1, dt_p2 = (float(v) for v in rows[3]["dT"])
    assert dt_p2 > 0 and dt_p1_born > 0 and dt_p2_born > 0, "every interval is positive"
    assert _close(dt_p1, dt_p2_born + dt_p2), "P1's galaxy evolves over Age[1] -> Age[3]"
    assert _close(dt_p0, dt_p1_born + dt_p2_born + dt_p2), "P0's over Age[0] -> Age[3]"
    return None


# ---------------------------------------------------------------------------
# An adjacent version 3 dataset
# ---------------------------------------------------------------------------


def test_adjacent_version3_retains_two_generations():
    """
    Test an all-adjacent version 3 dataset (links_adjacent = 1).

    Expected: the run reports links_adjacent 1; horizons 1, 2, 2; the loads see 1, 2 and
    2 slabs live, never more than two; each earlier generation goes after the next
    snapshot's sweep and the last after its own output. Rows (halos-only): Y's
    same-snapshot NextProgenitor W is gathered with it, so X at snapshot 2 holds Z's
    continuing galaxy (via Y, the main branch) and W's orphan.
    """
    _require_package()
    returncode, stdout, stderr, output_dir = _run("adjacent", "adjacent", [0, 1, 2])
    _assert_clean_success(returncode, stdout, stderr, nsnapshots=3, max_live=2)
    assert "links_adjacent 1" in stdout, f"the dataset should be reported adjacent:\n{stdout}"
    _assert_in_order(
        stdout,
        _lifecycle(
            [
                (0, 1, 1, [], True, False),
                (1, 2, 2, [0], True, False),
                (2, 2, 2, [1], True, True),
            ]
        ),
        "adjacent version 3",
    )
    assert "; 3 slabs live" not in stdout, "an adjacent dataset never holds three generations"

    if compiled_model() != "halos-only":
        return None

    rows = {}
    for snap in range(3):
        rows[snap], multiplier = _rows(output_dir, snap)
    z, w = multiplier + 2, multiplier + 3
    assert _identity(rows[0]) == [(z, 0, 0)], _identity(rows[0])
    assert _identity(rows[1]) == [(z, 0, 1), (w, 0, 1)], _identity(rows[1])
    assert _identity(rows[2]) == [(z, 0, 2), (w, 2, 2)], _identity(rows[2])
    return None


# ---------------------------------------------------------------------------
# Release on failure
# ---------------------------------------------------------------------------


def _assert_failure_cleanup(stdout, stderr, last_loaded, released):
    output = stdout + stderr
    tail = stdout[stdout.rfind(f"Loaded snapshot {last_loaded} (") :]
    count = len(released)
    _assert_in_order(
        tail,
        [
            f"Horizontal driver exiting early: releasing {count} retained generation"
            f"{'' if count == 1 else 's'}",
            *[f"Released snapshot {snap} " for snap in released],
            "Closed horizontal run 'horizontal_hdf5' with no slab loaded",
        ],
        "failure cleanup",
    )
    message = f"only the {count} generations retained at the failure are released, once each"
    assert stdout.count("Released snapshot ") == count, f"{message}:\n{stdout}"
    assert "still loaded" not in output, f"close_run must find no slab loaded:\n{output}"
    assert f"Loaded snapshot {last_loaded + 1} (" not in stdout, "the run must stop at the failure"


def test_failure_at_output_releases_retained_generations():
    """
    Test that an abort while writing output releases every retained generation.

    The fault is a read-only file pre-created at snapshot 1's partition path, so the
    abort lands in snapshot 1's output write, while generations 0 (horizon 2) and 1
    (horizon 2) are both retained. Expected: non-zero exit naming the file; the driver's
    exit handler then releases 0 and 1 and closes the reader's run, whose own check
    finds no slab loaded.
    """
    _require_package()
    if os.geteuid() == 0:
        raise TestSkipped("running as root: mode bits do not deny access, so this cannot be forced")

    def block_partition(output_dir):
        blocked = output_dir / "model_001.hdf5"
        blocked.write_bytes(b"marker")
        blocked.chmod(0o444)

    returncode, stdout, stderr, _ = _run(
        "fail_output", "worked_graph", [1, 4], prepare_output=block_partition
    )
    assert returncode != 0, f"an uncreatable partition should abort:\n{stdout}{stderr}"
    assert "Failed to create HDF5 file" in stdout + stderr, f"unexpected abort:\n{stderr}"
    _assert_failure_cleanup(stdout, stderr, last_loaded=1, released=[0, 1])


def test_failure_at_load_releases_retained_generations():
    """
    Test that an abort while loading a slab releases the generations already retained.

    The fault is an out-of-range FirstHaloInFOFgroup written into snapshot 2 of a
    temporary copy of the worked graph; load_slab validates it and aborts before the
    slab is handed over. Expected: non-zero exit; generations 0 and 1 are released and
    the run closed with no slab loaded; the half-loaded snapshot 2 never counts as
    retained.
    """
    _require_package()
    import h5py

    dataset_dir = Path(TEMP_DIR) / "broken_worked_graph"
    shutil.copytree(DATA_DIR / "worked_graph", dataset_dir)
    with h5py.File(dataset_dir / "snapshot_002.h5", "r+") as handle:
        links = handle["halos/FirstHaloInFOFgroup"]
        links[0] = links.shape[0]

    returncode, stdout, stderr, _ = _run("fail_load", "worked_graph", [4], dataset_dir=dataset_dir)
    assert returncode != 0, f"an out-of-range FoF link should abort:\n{stdout}{stderr}"
    assert "Loaded snapshot 2 (" not in stdout, "snapshot 2 must fail inside load_slab"
    tail_start = stdout.rfind("Loaded snapshot 1 (")
    assert tail_start >= 0, f"snapshot 1 should have loaded before the failure:\n{stdout}"
    _assert_failure_cleanup(stdout, stderr, last_loaded=1, released=[0, 1])


def main():
    global TEMP_DIR
    TEMP_DIR = Path(tempfile.mkdtemp(prefix="mimic_gap_retention_"))
    try:
        tests = [
            test_worked_graph_fixture_is_the_design_review_graph,
            test_worked_graph_retention_schedule,
            test_worked_graph_inheritance_and_output_rows,
            test_three_snapshot_chain,
            test_adjacent_version3_retains_two_generations,
            test_failure_at_output_releases_retained_generations,
            test_failure_at_load_releases_retained_generations,
        ]
        return run_test_suite(tests, "mini-Millennium Horizontal Gap Retention")
    finally:
        shutil.rmtree(TEMP_DIR, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
