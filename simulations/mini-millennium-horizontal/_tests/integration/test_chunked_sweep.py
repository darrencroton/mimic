#!/usr/bin/env python3
"""
mini-Millennium Horizontal Chunked-Sweep Tests

Validates: a serial horizontal run at ``input.forest_chunks: G`` writes, galaxy for galaxy and
row for row, the output of the same run at ``forest_chunks: 1``. Each chunk is a contiguous
sub-range of the task's forests, swept through every snapshot in turn with its own retained
generations, and appends its rows to the same per-snapshot partition files; the first chunk
creates each partition and the last one finalises it.

The committed ``forest_blocks`` fixture (../data/forest_blocks/: six forests over seven gapped
snapshots, snapshot 3 empty, widest slab snapshot 5 with 17 halos, widest-slab forest weights
``[2, 1, 7, 2, 2, 3]``) is run with ``halos-only`` through
``../input/forest_blocks_halos-only.yaml`` (output snapshots 6, 5, 4 and 2), its output directory
redirected as the distributed identity gate does, at ``forest_chunks`` 1, 2, 3 and 8, plus 2 with
``--compress`` (whose deflated ``Galaxies`` table is appended across file sessions). The cuts
are ``[0, 3, 6]`` at ``G = 2`` and ``[0, 2, 3, 6, 6, ...]`` for every ``G >= 3``, so ``G = 8``
has five idle chunks. Each check is one ``MIMIC_RESULT:`` marker:

  - every run exits 0, leak-free, with the same four partition files and the master;
  - ``scripts/compare_cross_format_identity.py --compare-created`` reports every chunked run
    bitwise identical to the ``G = 1`` run over a positive galaxy count;
  - each partition's ``UniqueGalaxyID`` column, read in file order, equals the ``G = 1`` run's
    (the comparator matches by id, so row order is checked here);
  - the ``--compress`` run's partitions really are deflated;
  - the ``G = 8`` log shows exactly its five idle chunks, the ``G = 3`` log none;
  - the ``G = 2`` log carries the partition's chunk lines and each chunk's sweep line with the
    hand-counted forest ranges, weights and widest-slab rows, ``2 x 7`` "Loaded snapshot"
    lines, and every release of chunk 0 before chunk 1 starts; the ``G = 1`` log has none of
    the partition or chunk lines;
  - the multi-visit failure window: on a copy of the fixture whose snapshot 5 row 12 (forest 4,
    inside chunk 1's rows ``[10, 17)``) has an out-of-range ``FirstHaloInFOFgroup``, a
    ``G = 2`` run passes chunk 0 (which loads rows ``[0, 10)``, never the broken row), then
    fails in chunk 1 at snapshot 5; the partitions chunk 1 had already finalised (snapshots 2
    and 4) survive whole, and the two still only appended to (5 and 6) are removed with the
    master.

Skips automatically when:
  - SIMULATION != mini-millennium-horizontal (wrong compiled package)
  - MODEL != halos-only (the fixture run file names halos-only)
  - Mimic is not built

Run with:
  MODEL=halos-only SIMULATION=mini-millennium-horizontal \\
      python3 simulations/mini-millennium-horizontal/_tests/integration/test_chunked_sweep.py
or (registered):
  make tests-horizontal-v3
"""

import re
import shutil
import subprocess
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
PACKAGE_TESTS = REPO_ROOT / "simulations" / "mini-millennium-horizontal" / "_tests"
FIXTURE_DIR = PACKAGE_TESTS / "data" / "forest_blocks"
RUN_FILE = PACKAGE_TESTS / "input" / "forest_blocks_halos-only.yaml"
COMPARATOR = REPO_ROOT / "scripts" / "compare_cross_format_identity.py"

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

BASE = "halos"  # the run file's output_filename
OUTPUT_SNAPSHOTS = (6, 5, 4, 2)
NSNAPSHOTS = 7
EXPECTED_FILES = {f"{BASE}.hdf5"} | {f"{BASE}_{snap:03d}.hdf5" for snap in OUTPUT_SNAPSHOTS}

# The legs: name -> (forest_chunks, extra command-line arguments).
LEGS = {
    "g1": (1, []),
    "g2": (2, []),
    "g3": (3, []),
    "g8": (8, []),
    "g2_compress": (2, ["--compress"]),
}
CHUNKED_LEGS = [name for name in LEGS if name != "g1"]

IDLE_LINE = "(an idle chunk: no forest)"
PASSED_RE = re.compile(r"^PASSED: (\d+) galaxies", re.MULTILINE)

# The failure window's injected fault: snapshot 5, row 12 (forest 4, chunk 1 at G = 2).
BROKEN_SNAPSHOT = 5
BROKEN_ROW = 12

_RESULTS = {}


class RunResult:
    """One Mimic run: its exit status, its combined log and where it wrote."""

    def __init__(self, returncode, stdout, stderr, output_dir):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr
        self.output = stdout + stderr
        self.output_dir = output_dir


def _require_package():
    sim = compiled_simulation()
    if sim != "mini-millennium-horizontal":
        raise TestSkipped(f"compiled simulation is {sim!r}, not mini-millennium-horizontal")
    model = compiled_model()
    if model != "halos-only":
        raise TestSkipped(f"compiled model is {model!r}; the fixture run file names halos-only")
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")


def _run(name, forest_chunks, extra_args=(), dataset_dir=None):
    """Run the fixture's halos-only run file at `forest_chunks`, its output redirected."""
    with open(RUN_FILE, "r") as handle:
        config = yaml.safe_load(handle)

    output_dir = Path(TEMP_DIR) / name
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)

    config.setdefault("input", {})["forest_chunks"] = forest_chunks
    if dataset_dir is not None:
        config["input"]["simulation_dir"] = str(dataset_dir)
        config["input"]["snapshot_list_file"] = str(dataset_dir / "forest_blocks.a_list")
    config.setdefault("output", {})["output_directory"] = str(output_dir)

    run_file = Path(TEMP_DIR) / f"{name}.yaml"
    with open(run_file, "w") as handle:
        yaml.safe_dump(config, handle, default_flow_style=False, sort_keys=False)

    returncode, stdout, stderr = run_mimic(run_file, extra_args=list(extra_args))
    return RunResult(returncode, stdout, stderr, output_dir)


def _leg(name):
    """The leg's run, made once and shared by every check."""
    if name not in _RESULTS:
        forest_chunks, extra_args = LEGS[name]
        _RESULTS[name] = _run(name, forest_chunks, extra_args)
    return _RESULTS[name]


def _successful_leg(name):
    result = _leg(name)
    assert result.returncode == 0, f"leg {name} should complete:\n{result.output}"
    return result


def _output_files(output_dir):
    return {path.name for path in Path(output_dir).glob(f"{BASE}*.hdf5")}


def _unique_ids(output_dir, snap):
    """The UniqueGalaxyID column of snapshot `snap`'s partition, in file order."""
    import h5py

    with h5py.File(Path(output_dir) / f"{BASE}_{snap:03d}.hdf5", "r") as handle:
        return [int(value) for value in handle[f"Snap{snap:03d}"]["Galaxies"]["UniqueGalaxyID"]]


def _galaxies(output_dir, snap):
    """Snapshot `snap`'s partition: its Galaxies rows and its TotHalosPerSnap attribute."""
    import h5py

    with h5py.File(Path(output_dir) / f"{BASE}_{snap:03d}.hdf5", "r") as handle:
        table = handle[f"Snap{snap:03d}"]["Galaxies"]
        rows = table[()]
        total = int(table.attrs["TotHalosPerSnap"].ravel()[0])
    return rows, total


def test_every_chunk_count_writes_the_same_files():
    """
    Test that every leg exits 0, leak-free, with the same partition files and master.

    Expected: exit 0 at forest_chunks 1, 2, 3, 8 and 2 with --compress; each output directory
              holds exactly the master and the four partitions halos_<snap>.hdf5 of the
              requested snapshots, every generation released once per chunk, and no leak.
    Validates: chunking changes neither file names nor the per-(snapshot, task) partition layout.
    """
    _require_package()
    for name, (forest_chunks, _) in LEGS.items():
        result = _successful_leg(name)
        found = _output_files(result.output_dir)
        message = f"leg {name} wrote {sorted(found)}, expected {sorted(EXPECTED_FILES)}"
        assert found == EXPECTED_FILES, message
        released = result.stdout.count("Released snapshot ")
        assert released == forest_chunks * NSNAPSHOTS, (
            f"leg {name}: each of the {NSNAPSHOTS} snapshots should be released once per chunk "
            f"({forest_chunks * NSNAPSHOTS} in all), found {released}:\n{result.stdout}"
        )
        assert "No memory leaks detected" in result.stdout and check_no_memory_leaks(
            result.stdout, result.stderr
        ), f"leg {name} should report no leak:\n{result.output}"


def test_comparator_reports_identity_with_one_chunk():
    """
    Test that every chunked leg is bitwise identical to forest_chunks: 1, galaxy for galaxy.

    Expected: compare_cross_format_identity.py --compare-created exits 0 against the G = 1 run
              and reports PASSED over a positive galaxy count.
    Validates: byte-identity per UniqueGalaxyID to the forest_chunks: 1 run, created rows included.
    """
    _require_package()
    reference = _successful_leg("g1").output_dir / BASE
    for name in CHUNKED_LEGS:
        candidate = _successful_leg(name).output_dir / BASE
        completed = subprocess.run(
            [
                sys.executable,
                str(COMPARATOR),
                str(reference),
                str(candidate),
                "--compare-created",
            ],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
        )
        report = completed.stdout + completed.stderr
        assert completed.returncode == 0, f"leg {name} differs from forest_chunks 1:\n{report}"
        passed = PASSED_RE.search(completed.stdout)
        message = f"leg {name}: the comparison must cover a positive galaxy count:\n{report}"
        assert passed is not None and int(passed.group(1)) > 0, message


def test_partition_row_order_matches_one_chunk():
    """
    Test that each partition's rows are in the G = 1 run's order at every chunk count.

    Expected: the UniqueGalaxyID column of every partition, read in file order, equals the
              G = 1 run's, and each is non-empty for at least one snapshot.
    Validates: chunks append ascending row ranges to the partition, so the file order is the
               G = 1 order; the comparator matches by id and cannot see order.
    """
    _require_package()
    reference = _successful_leg("g1").output_dir
    expected = {snap: _unique_ids(reference, snap) for snap in OUTPUT_SNAPSHOTS}
    assert any(expected.values()), "the G = 1 run wrote no galaxy, so row order is vacuous"
    for name in CHUNKED_LEGS:
        output_dir = _successful_leg(name).output_dir
        for snap in OUTPUT_SNAPSHOTS:
            found = _unique_ids(output_dir, snap)
            assert found == expected[snap], (
                f"leg {name}, snapshot {snap}: UniqueGalaxyID order {found} differs from the "
                f"forest_chunks 1 order {expected[snap]}"
            )


def test_compressed_partitions_are_appended_deflated():
    """
    Test that the --compress leg's partition tables are deflated.

    Expected: every partition's Galaxies dataset of the --compress leg carries the gzip
              filter, and the uncompressed G = 2 leg's carries none.
    Validates: the identity check of the --compress leg exercised the compressed append across
               file sessions, not an uncompressed table.
    """
    _require_package()
    import h5py

    for name, expected in (("g2_compress", "gzip"), ("g2", None)):
        output_dir = _successful_leg(name).output_dir
        for snap in OUTPUT_SNAPSHOTS:
            with h5py.File(output_dir / f"{BASE}_{snap:03d}.hdf5", "r") as handle:
                compression = handle[f"Snap{snap:03d}"]["Galaxies"].compression
            assert compression == expected, (
                f"leg {name}, snapshot {snap}: Galaxies compression is {compression!r}, "
                f"expected {expected!r}"
            )


def test_idle_chunks_are_logged():
    """
    Test that the idle chunks of G = 8 are swept and logged as idle.

    Expected: the G = 8 log holds exactly five idle chunk sweep lines (chunks 3 to 7, forests
              [6, 6)) and every chunk's sweep line; the G = 3 log holds none.
    Validates: the task/chunk partition allows idle trailing chunks, which still take their turn.
    """
    _require_package()
    g8 = _successful_leg("g8").output
    idle = [line for line in g8.splitlines() if IDLE_LINE in line]
    assert len(idle) == 5, f"G = 8 should sweep five idle chunks, found {len(idle)}:\n{g8}"
    for chunk in range(3, 8):
        needle = f"Sweeping this task's chunk {chunk} of 8: forests [6, 6)"
        assert any(needle in line for line in idle), f"missing idle chunk line {needle!r}:\n{g8}"
    for chunk in range(8):
        assert f"Sweeping this task's chunk {chunk} of 8:" in g8, f"chunk {chunk} never swept"
    g3 = _successful_leg("g3").output
    assert IDLE_LINE not in g3, f"G = 3 has no idle chunk:\n{g3}"


def test_two_chunk_log_lines():
    """
    Test the G = 2 log's partition, chunk and lifecycle lines against a hand count.

    Expected: the partition's chunk lines and each chunk's sweep line name forests [0, 3) and
              [3, 6) with widest-slab weights 10 and 7 and snapshot 5 rows [0, 10) and
              [10, 17); there are 2 x 7 "Loaded snapshot" lines, chunk 1's carrying its row note;
              all seven releases of chunk 0 precede chunk 1's sweep line and seven follow it;
              the G = 1 log holds no partition or chunk line.
    Validates: each chunk is logged, and each chunk releases its slabs before the next loads.
    """
    _require_package()
    g2 = _successful_leg("g2").stdout
    for needle in (
        "Chunked horizontal partition: 6 forests over 1 task in 2 chunks each",
        "Partition task 0: forests [0, 6), widest-slab weight 17 (snapshot 5 rows [0, 17))",
        "Partition task 0 chunk 0: forests [0, 3), widest-slab weight 10 (snapshot 5 rows [0, 10))",
        "Partition task 0 chunk 1: forests [3, 6), widest-slab weight 7 (snapshot 5 rows [10, 17))",
        "Sweeping this task's chunk 0 of 2: forests [0, 3), widest slab snapshot 5 rows [0, 10)",
        "Sweeping this task's chunk 1 of 2: forests [3, 6), widest slab snapshot 5 rows [10, 17)",
        "Loaded snapshot 5 (7 halos (this task's chunk 1 of 2, rows [10, 17) of the snapshot's "
        "17)); ",
    ):
        assert needle in g2, f"the G = 2 log should carry {needle!r}:\n{g2}"

    loaded = g2.count("Loaded snapshot ")
    message = f"G = 2 should load each of {NSNAPSHOTS} snapshots once per chunk, found {loaded}"
    assert loaded == 2 * NSNAPSHOTS, message
    boundary = g2.index("Sweeping this task's chunk 1 of 2:")
    before = g2[:boundary].count("Released snapshot ")
    after = g2[boundary:].count("Released snapshot ")
    assert before == NSNAPSHOTS and after == NSNAPSHOTS, (
        f"chunk 0 should release all {NSNAPSHOTS} generations before chunk 1 loads any "
        f"(found {before} before and {after} after chunk 1's sweep line):\n{g2}"
    )
    first_after = g2[g2.index("Loaded snapshot ", boundary) :].splitlines()[0]
    assert "this task's chunk 1 of 2" in first_after, first_after

    g1 = _successful_leg("g1").stdout
    for needle in ("horizontal partition:", "Partition task", "Sweeping this task's chunk"):
        assert needle not in g1, f"the G = 1 log should hold no {needle!r} line:\n{g1}"


def fixture_copy_with_broken_chunk_link(destination):
    """Copy the forest_blocks fixture to `destination` and break one FoF link in chunk 1.

    Modelled on fixture_copy_with_broken_fof_link() in tests/integration/test_processing_order.py,
    which copies the package's default fixture and always breaks row 0, so it cannot place the
    fault inside a later chunk. Row BROKEN_ROW of snapshot BROKEN_SNAPSHOT is forest 4, inside
    chunk 1's rows [10, 17) at forest_chunks 2. Its FirstHaloInFOFgroup is set to the slab's halo
    count, which the reader's link validation (run only on the rows load_slab loads) rejects as
    out of range; chunk 0 loads rows [0, 10) of that snapshot and never sees it.
    """
    import h5py

    destination = Path(destination)
    shutil.copytree(FIXTURE_DIR, destination)
    with h5py.File(destination / f"snapshot_{BROKEN_SNAPSHOT:03d}.h5", "r+") as handle:
        forest = int(handle["halos/ForestIndex"][BROKEN_ROW])
        assert forest == 4, f"row {BROKEN_ROW} should be forest 4, found {forest}"
        dataset = handle["halos/FirstHaloInFOFgroup"]
        dataset[BROKEN_ROW] = dataset.shape[0]
    return destination


def test_failure_keeps_finalised_partitions_and_removes_the_rest():
    """
    Test the multi-visit failure window: a failure in chunk 1 keeps what chunk 1 finalised.

    Expected: forest_chunks 2 on the broken copy exits non-zero after chunk 0's whole sweep
              and chunk 1's sweep line, in snapshot 5's load; the partitions of snapshots 2 and
              4 (finalised by chunk 1 before the fault) survive, row for row and with the
              TotHalosPerSnap of the G = 1 run; snapshots 5 and 6 (created by chunk 0, not yet
              final) and the master are removed.
    Validates: the cleanup registry holds one slot per output snapshot under G > 1.
    """
    _require_package()
    dataset_dir = fixture_copy_with_broken_chunk_link(Path(TEMP_DIR) / "broken_forest_blocks")
    result = _run("broken_g2", 2, dataset_dir=dataset_dir)

    assert result.returncode != 0, f"the broken link should fail the run:\n{result.output}"
    message = f"the failure should name the broken link:\n{result.output}"
    assert "FirstHaloInFOFgroup" in result.output, message
    boundary = result.stdout.find("Sweeping this task's chunk 1 of 2:")
    assert boundary >= 0, f"chunk 0 should complete before the fault:\n{result.output}"
    message = f"chunk 0 should load every snapshot, the broken one included:\n{result.output}"
    assert result.stdout[:boundary].count("Loaded snapshot ") == NSNAPSHOTS, message
    message = f"chunk 1 should fail inside snapshot {BROKEN_SNAPSHOT}'s load:\n{result.output}"
    assert f"Loaded snapshot {BROKEN_SNAPSHOT} (" not in result.stdout[boundary:], message

    found = _output_files(result.output_dir)
    expected = {f"{BASE}_002.hdf5", f"{BASE}_004.hdf5"}
    message = (
        f"the finalised partitions {sorted(expected)} should survive alone, found {sorted(found)}"
    )
    assert found == expected, message

    reference = _successful_leg("g1").output_dir
    for snap in (2, 4):
        rows, total = _galaxies(result.output_dir, snap)
        reference_rows, reference_total = _galaxies(reference, snap)
        message = (
            f"snapshot {snap}: TotHalosPerSnap {total} should be the G = 1 run's {reference_total}"
        )
        assert total == reference_total and len(rows) > 0, message
        message = f"snapshot {snap}: the surviving partition should hold the G = 1 run's rows"
        assert rows.tobytes() == reference_rows.tobytes(), message


def main():
    global TEMP_DIR
    TEMP_DIR = Path(tempfile.mkdtemp(prefix="mimic_chunked_sweep_"))
    try:
        tests = [
            test_every_chunk_count_writes_the_same_files,
            test_comparator_reports_identity_with_one_chunk,
            test_partition_row_order_matches_one_chunk,
            test_compressed_partitions_are_appended_deflated,
            test_idle_chunks_are_logged,
            test_two_chunk_log_lines,
            test_failure_keeps_finalised_partitions_and_removes_the_rest,
        ]
        return run_test_suite(tests, "mini-Millennium Horizontal Chunked Sweep")
    finally:
        shutil.rmtree(TEMP_DIR, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
