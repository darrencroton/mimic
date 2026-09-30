#!/usr/bin/env python3
"""
Integration tests for startup validation of the reader/processing-order seam.

Covers input.processing_order, the two-registry input.tree_type resolution, the
horizontal reader's exact tree_name contract, and the
simulation.unique_galaxy_id_multiplier key (parse, default, precedence across
both parser passes, and a vertical run honouring a non-default value).
"""

import os
import shutil
import sys
import tempfile
from pathlib import Path

import yaml

# Add framework to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import (
    MIMIC_EXE,
    REPO_ROOT,
    TestSkipped,
    compiled_simulation,
    create_test_param_file,
    load_binary_halos,
    run_mimic,
    run_test_suite,
    selected_package_is_horizontal,
    selected_package_test_config,
    skip_if_selected_package_is_horizontal,
)

TEMP_DIR = None

#: Default forest multiplier (TREE_MUL_FAC in src/include/constants.h).
DEFAULT_MULTIPLIER = 1000000000

#: The only input.tree_name the horizontal_hdf5 reader accepts.
HORIZONTAL_TREE_NAME = "snapshot_%03d.h5"


def package_fixture():
    """The selected package's committed horizontal fixture as (dataset dir, a_list), or None.

    Read from the package's own _tests/input/test_simulation.yaml -- the file the
    generated run files point at -- so each horizontal package is exercised on the
    fixture whose headers match its own simulation_info.yaml (the reader aborts on
    any disagreement), and a kilobyte-sized committed dataset is read instead of the
    machine-local production conversion. None when the selected package is vertical or
    ships no fixture.
    """
    config_path = selected_package_test_config()
    if config_path is None or not selected_package_is_horizontal():
        return None
    with open(config_path, "r") as handle:
        input_config = (yaml.safe_load(handle) or {}).get("input") or {}
    dataset_dir = REPO_ROOT / input_config["simulation_dir"]
    a_list = REPO_ROOT / input_config["snapshot_list_file"]
    return dataset_dir, a_list


def snapshot_fixture_dir():
    """The directory holding the selected package's committed snapshot fixture."""
    return package_fixture()[0]


def snapshot_fixture_a_list():
    """The selected package's committed fixture's own scale-factor list."""
    return package_fixture()[1]


def snapshot_fixture_snapshot_count():
    """Number of snapshots the fixture's scale-factor list declares, or 0 if absent."""
    if package_fixture() is None or not snapshot_fixture_a_list().is_file():
        return 0
    with open(snapshot_fixture_a_list(), "r") as handle:
        return sum(1 for line in handle if line.strip())


def snapshot_fixture_snapshot_files():
    """Every snapshot payload file the fixture's a_list implies, in load order."""
    return [
        snapshot_fixture_dir() / f"snapshot_{snap:03d}.h5"
        for snap in range(snapshot_fixture_snapshot_count())
    ]


def snapshot_fixture_halo_counts():
    """Number of halos in every fixture snapshot, from each file's own halo table."""
    import h5py

    counts = []
    for path in snapshot_fixture_snapshot_files():
        with h5py.File(path, "r") as handle:
            counts.append(int(handle["halos/Descendant"].shape[0]))
    return counts


def snapshot_fixture_links_adjacent():
    """Whether the fixture declares every Descendant link adjacent (header links_adjacent)."""
    import h5py

    with h5py.File(snapshot_fixture_snapshot_files()[0], "r") as handle:
        return bool(int(handle["header"].attrs["links_adjacent"]))


def snapshot_fixture_horizons():
    """Retention horizon of every fixture snapshot, from its own descendant columns.

    The horizon of snapshot k is the latest snapshot any of its halos names as its
    descendant's, or k itself when none has a descendant -- the driver's rule
    (horizontal_generation_horizon() in src/core/horizontal_driver.c). A version 3
    fixture carries each link's target in DescendantSnapshot; a version 2 fixture has
    no such column because every descendant lives at snapshot k + 1.
    """
    import h5py

    horizons = []
    for snap, path in enumerate(snapshot_fixture_snapshot_files()):
        with h5py.File(path, "r") as handle:
            linked = handle["halos/Descendant"][()] >= 0
            if "DescendantSnapshot" in handle["halos"]:
                targets = handle["halos/DescendantSnapshot"][()][linked]
            else:
                targets = [snap + 1] * int(linked.sum())
        horizons.append(max([snap, *(int(target) for target in targets)]))
    return horizons


def snapshot_fixture_present():
    """Is the selected package's committed horizontal fixture's full payload present?

    The guard is derived from the a_list, because the a_list is what bounds the
    run: the driver loads every snapshot the scale-factor list declares, and
    open_run validates every one of those files. output.snapshot_list selects
    which of them are *written*, and bounds nothing that is *read*. Checking the
    a_list alone would pass a partial checkout that has the list but no
    payload, so every implied file is checked; a resized fixture changes the set
    checked here rather than letting the guard drift out of sync. forests.h5 is
    deliberately not checked: it is converter provenance the C reader never
    opens.
    """
    return snapshot_fixture_snapshot_count() > 0 and all(
        path.is_file() for path in snapshot_fixture_snapshot_files()
    )


def skip_unless_snapshot_fixture_present():
    """Skip, naming what is missing, unless the selected package's fixture is usable."""
    if package_fixture() is None:
        raise TestSkipped(
            f"selected package {compiled_simulation()} ships no committed horizontal fixture "
            f"(_tests/input/test_simulation.yaml); this test runs the driver over one"
        )
    if not snapshot_fixture_present():
        raise TestSkipped(f"committed snapshot fixture not found at {snapshot_fixture_dir()}")


def snapshot_fixture_input_overrides(simulation_dir=None):
    """input.* overrides that repoint a run at the committed fixture dataset.

    A caller that has built a modified copy of the fixture (the failure-injection
    cases below) passes its directory, and the copy's own scale-factor list is
    used with it so the run reads one self-consistent dataset.
    """
    if simulation_dir is None:
        return {
            "simulation_dir": str(snapshot_fixture_dir()),
            "snapshot_list_file": str(snapshot_fixture_a_list()),
        }
    return {
        "simulation_dir": str(simulation_dir),
        "snapshot_list_file": str(Path(simulation_dir) / snapshot_fixture_a_list().name),
    }


def snapshot_partition_files(output_dir):
    """The numbered partition files a horizontal run left in output_dir.

    Named by glob rather than by expectation so a run that wrote a file nobody
    asked for shows up as an extra entry instead of going unnoticed.
    """
    return sorted(Path(output_dir).glob("*_[0-9][0-9][0-9].hdf5"))


def snapshot_partition_path(output_dir, snapnum):
    """Where a horizontal run puts snapshot `snapnum`'s partition file."""
    return Path(output_dir) / f"model_{snapnum:03d}.hdf5"


def fixture_copy_with_broken_fof_link(destination, snapnum):
    """Copy the fixture, and its scale-factor list, to `destination` and break one FoF link.

    FirstHaloInFOFgroup is bounded by its OWN snapshot's halo count
    (src/io/horizontal/read_horizontal_hdf5.c:849, :884-886), so setting it to that
    count is guaranteed out of range rather than accidentally valid. The
    validator that rejects it runs only from load_slab_horizontal_hdf5() (:1297),
    never from open_run, so the abort lands mid-sweep — after earlier requested
    snapshots have already been written and closed — rather than at startup.
    """
    import h5py

    destination = Path(destination)
    shutil.copytree(snapshot_fixture_dir(), destination)
    if not (destination / snapshot_fixture_a_list().name).is_file():
        shutil.copy2(snapshot_fixture_a_list(), destination)

    with h5py.File(destination / f"snapshot_{snapnum:03d}.h5", "r+") as handle:
        dataset = handle["halos/FirstHaloInFOFgroup"]
        n_halos = dataset.shape[0]
        assert n_halos > 0, (
            f"snapshot {snapnum} of the fixture is empty, so it cannot carry a link to "
            f"corrupt; pick a populated snapshot"
        )
        dataset[0] = n_halos

    return destination


def skip_unless_mode_bits_deny_access():
    """Skip a permission-based injection where mode bits do not deny access."""
    if os.geteuid() == 0:
        raise TestSkipped("running as root: mode bits do not deny access, so this cannot be forced")


def make_param_file(
    name,
    input_overrides=None,
    simulation_overrides=None,
    output_overrides=None,
    package_multiplier=None,
):
    """
    Return a run file path with the given input/simulation/output overrides applied.

    Generates a base test run file via create_test_param_file and rewrites it.
    When package_multiplier is given, a scratch copy of the simulation config the
    generated run file already points at — create_test_param_file() has already
    resolved and materialized it under TEMP_DIR, so it is the SELECTED package's
    own config, not a hard-coded reference — is written with
    simulation.unique_galaxy_id_multiplier added, and simulation.config is
    repointed at it by absolute path, so the value arrives through the
    simulation-package parser pass rather than the run file.
    """
    param_file, _output_dir, _ = create_test_param_file(
        output_name=f"processing_order_{name}",
        first_file=0,
        last_file=0,
        temp_dir=TEMP_DIR,
    )
    with open(param_file, "r") as handle:
        config = yaml.safe_load(handle)

    if package_multiplier is not None:
        ref_simulation_config = Path(config["simulation"]["config"])
        with open(ref_simulation_config, "r") as handle:
            sim_config = yaml.safe_load(handle)
        sim_config.setdefault("simulation", {})["unique_galaxy_id_multiplier"] = package_multiplier
        sim_config_path = Path(TEMP_DIR) / f"{name}_simulation.yaml"
        with open(sim_config_path, "w") as handle:
            yaml.safe_dump(sim_config, handle, default_flow_style=False, sort_keys=False)
        config.setdefault("simulation", {})["config"] = str(sim_config_path.resolve())

    if input_overrides:
        config.setdefault("input", {}).update(input_overrides)
    if simulation_overrides:
        config.setdefault("simulation", {}).update(simulation_overrides)
    if output_overrides:
        config.setdefault("output", {}).update(output_overrides)

    rewritten = Path(TEMP_DIR) / f"{name}.yaml"
    with open(rewritten, "w") as handle:
        yaml.safe_dump(config, handle, default_flow_style=False, sort_keys=False)
    return rewritten


def run_config(name, extra_args=None, **kwargs):
    """Run Mimic on a rewritten run file and return (returncode, combined output)."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    param_file = make_param_file(name, **kwargs)
    returncode, stdout, stderr = run_mimic(param_file, extra_args=extra_args)
    return returncode, stdout + stderr


def effective_input_setting(name, key):
    """
    Return the effective value of input.<key> for a freshly generated run file.

    Mirrors the parser's precedence for a key the run file may inherit: an
    explicit value in the run file wins, else the simulation config the run file
    points at, else None. Lets package-dependent tests skip rather than assert a
    condition the selected package's own configuration contradicts.
    """
    param_file = make_param_file(name)
    with open(param_file, "r") as handle:
        config = yaml.safe_load(handle)

    value = (config.get("input") or {}).get(key)
    if value is not None:
        return value

    sim_config_path = Path(config["simulation"]["config"])
    with open(sim_config_path, "r") as handle:
        sim_config = yaml.safe_load(handle)
    return ((sim_config or {}).get("input") or {}).get(key)


def test_unknown_processing_order_fails_fast():
    """
    Test that an unrecognised input.processing_order value fails at startup.

    Expected: Non-zero exit; output includes the bad value name and "Valid values are vertical, horizontal".
    Validates: startup validation rejects unknown ordering strings with an actionable message.
    """
    returncode, output = run_config(
        "not_a_real_ordering", input_overrides={"processing_order": "not_a_real_ordering"}
    )

    assert returncode != 0, "Unknown processing_order should fail startup validation"
    assert "Unknown input.processing_order 'not_a_real_ordering'" in output
    assert "Valid values are vertical, horizontal" in output


def test_horizontal_run_completes_and_writes_output_over_the_fixture():
    """
    Test that a valid horizontal configuration runs end to end and writes output.

    Expected: exit 0; output does NOT include "Parameter validation failed" or either of
              the two messages earlier slices retired; the per-snapshot lifecycle lines
              show every snapshot loaded in ascending order with the live-slab count the
              retention horizons imply, and every snapshot released exactly once, at its
              horizon, never more than two slabs live when every link is adjacent; and
              the run leaves exactly one numbered partition file per requested output
              snapshot, each named for and holding only that snapshot, plus a master
              linking each snapshot to its own file, with TotHalosPerSnap totals equal
              to the rows actually written, no Ntrees attribute, no TreeHalosPerSnap
              dataset or link, TreeType "horizontal_hdf5", and UniqueGalaxyIDMultiplier in
              both per-file and master RunProperties.
    Validates: the horizontal driver produces output through the driver-neutral
               output partition seam, and does so under its retention schedule (a
               generation lives until its retention horizon has been processed) rather
               than by holding every slab live.

    The retention horizon of snapshot k is the latest snapshot any of its halos names as
    its descendant's, or k itself when none has a descendant. The expected lifecycle is
    derived from the fixture's own descendant columns (snapshot_fixture_horizons()), not
    hard-coded, so it holds for a version 2 fixture (every descendant at k + 1) and a
    gapped version 3 one alike. The two-live-slab bound is asserted only for a fixture
    whose header declares links_adjacent: a gapped link legitimately keeps a generation
    live across the gap.

    Runs against the selected package's own committed fixture (package_fixture(): for
    micro-uchuu-horizontal its _tests/data/generic/, for mini-millennium-horizontal
    _tests/data/worked_graph/), not the machine-local production dataset: the latter is
    multi-gigabyte, gitignored, and absent on a fresh checkout, which would make this
    proof unreproducible outside one workstation. input.simulation_dir and
    input.snapshot_list_file are overridden to point at the fixture, and
    output.snapshot_list to indices the fixture's own a_list contains.

    The test still only runs when the selected package is itself horizontal (its own
    configuration is the only source of input.tree_type/tree_name/processing_order here);
    forcing tree_type: horizontal_hdf5 onto a vertical package would abort for an
    unrelated config-mismatch reason. Guarded separately against the fixture being absent,
    so a sparse or partial checkout skips rather than fails.

    output_format is forced to hdf5 explicitly, because output_format: binary is rejected
    for a horizontal configuration at config time (see
    test_horizontal_binary_output_rejected_at_config_time).

    -v is passed so the driver's per-snapshot lifecycle lines (silent at the default log
    level) are captured. They are VERBOSE_LOG rather than DEBUG_LOG deliberately: the
    driver enables debug-log rate limiting for the physics phase, which caps each
    DEBUG_LOG site at five calls and would truncate the ordered sequence asserted below.
    """
    import h5py

    if effective_input_setting("valid_horizontal_probe", "processing_order") != "horizontal":
        raise TestSkipped(
            "selected package is not horizontal; its own configuration is the only "
            "source of input.tree_type/tree_name/processing_order this test relies on"
        )
    skip_unless_snapshot_fixture_present()

    nsnapshots = snapshot_fixture_snapshot_count()
    # Every package fixture this runs over must hold an empty snapshot (the
    # micro-Uchuu fixture's is 0, create_snapshot_fixture.py; the mini-Millennium
    # worked_graph fixture's is 3, generate_sources.py), so the zero-galaxy
    # partition contract below is always exercised; a fixture without one fails
    # here rather than quietly leaving that contract untested.
    empty_snapshots = [
        snap for snap, count in enumerate(snapshot_fixture_halo_counts()) if count == 0
    ]
    assert empty_snapshots, (
        f"the fixture at {snapshot_fixture_dir()} must hold an empty snapshot so the "
        f"zero-galaxy partition is exercised"
    )
    empty_snapshot = empty_snapshots[0]
    # Deliberately unsorted, and deliberately including the empty snapshot. One
    # list therefore exercises both the unsorted-naming contract (each file must be
    # named for the snapshot it holds, not for its position in this list) and the
    # zero-galaxy partition, which must still be written.
    requested = [nsnapshots - 1, 1, empty_snapshot]
    assert len(set(requested)) == len(requested), (
        f"the fixture's empty snapshot {empty_snapshot} must differ from snapshots 1 and "
        f"{nsnapshots - 1}, or the request {requested} names a snapshot twice"
    )
    output_dir = Path(TEMP_DIR) / "valid_snapshot_output"

    returncode, output = run_config(
        "valid_snapshot",
        input_overrides=snapshot_fixture_input_overrides(),
        output_overrides={
            "output_format": "hdf5",
            "output_directory": str(output_dir),
            "snapshot_list": requested,
        },
        extra_args=["-v"],
    )

    assert returncode == 0, f"a valid horizontal run should complete:\n{output}"
    assert (
        "Parameter validation failed" not in output
    ), "a valid horizontal configuration must pass config validation"
    assert (
        "The horizontal driver is not implemented yet" not in output
    ), "the dispatch-time FATAL an earlier slice retired must not reappear"
    assert (
        "cannot yet produce output" not in output
    ), "the skeleton driver's abort must not survive into a producing driver"

    # The FULL ordered lifecycle, not just "a line mentioning two slabs somewhere":
    # every snapshot must be loaded in ascending order with the live-slab count its
    # predecessors' horizons imply, and every snapshot released exactly at its
    # horizon: after the sweep of that snapshot for an earlier generation, and after
    # its own output for a generation nothing later links into. Asserting the ordered
    # subsequence is what makes a shortened, reordered, early or late release fail here.
    horizons = snapshot_fixture_horizons()
    expected_sequence = []
    retained = []
    max_live = 0
    for snap in range(nsnapshots):
        retained.append(snap)
        live = len(retained)
        max_live = max(max_live, live)
        expected_sequence.append(f"Loaded snapshot {snap} (")
        expected_sequence.append(f"; {live} slab{'' if live == 1 else 's'} live")
        for earlier in [k for k in retained if k < snap and horizons[k] <= snap]:
            expected_sequence.append(f"Released snapshot {earlier} ")
            retained.remove(earlier)
        if horizons[snap] <= snap:
            expected_sequence.append(f"Released snapshot {snap} ")
            retained.remove(snap)
    assert not retained, f"every horizon of the fixture lies inside the run: {horizons}"
    if snapshot_fixture_links_adjacent():
        assert max_live <= 2, (
            f"an all-adjacent fixture must never need more than two live generations, "
            f"derived {max_live} from horizons {horizons}"
        )

    cursor = 0
    for needle in expected_sequence:
        found = output.find(needle, cursor)
        assert found >= 0, (
            f"expected {needle!r} after position {cursor} in the driver's lifecycle log; "
            f"the rotation sequence is incomplete or out of order:\n{output}"
        )
        cursor = found + len(needle)

    # Exactly once each: the ordered subsequence above cannot see a second release.
    released_lines = output.count("Released snapshot ")
    assert released_lines == nsnapshots, (
        f"each of the {nsnapshots} snapshots must be released exactly once, "
        f"found {released_lines} release lines:\n{output}"
    )
    assert (
        f"Retained at most {max_live} generation" in output
    ), f"the driver should report the {max_live}-generation peak the horizons imply:\n{output}"

    partitions = snapshot_partition_files(output_dir)
    assert [p.name for p in partitions] == sorted(f"model_{snap:03d}.hdf5" for snap in requested), (
        f"a horizontal run writes one partition per requested output snapshot named by "
        f"that snapshot's number, found {[p.name for p in partitions]}"
    )
    master = output_dir / "model.hdf5"
    assert master.is_file(), f"the master file is missing from {output_dir}"

    # Every requested snapshot is checked, so the assertions below cover
    # partitions whose requested-snapshot index is NOT 0 (with this unsorted list
    # only snapshot nsnapshots-1 sits at index 0) -- which is what proves the
    # per-file metadata is written at file open rather than for one index.
    for snap in requested:
        with h5py.File(snapshot_partition_path(output_dir, snap), "r") as handle:
            snapshot_groups = sorted(name for name in handle if name.startswith("Snap"))
            assert snapshot_groups == [f"Snap{snap:03d}"], (
                f"model_{snap:03d}.hdf5 should hold only its own snapshot group, "
                f"found {snapshot_groups}"
            )
            group = handle[f"Snap{snap:03d}"]
            dataset = group["Galaxies"]
            total = int(dataset.attrs["TotHalosPerSnap"].ravel()[0])
            assert total == dataset.shape[0], (
                f"Snap{snap:03d}: TotHalosPerSnap {total} should equal the "
                f"{dataset.shape[0]} marshalled rows"
            )
            assert (
                "Ntrees" not in dataset.attrs
            ), f"Snap{snap:03d}: a horizontal run has no trees to count"
            assert (
                "TreeHalosPerSnap" not in group
            ), f"Snap{snap:03d}: a horizontal run has no per-tree counts"
            assert "UniqueGalaxyIDMultiplier" in handle["RunProperties"].attrs, (
                f"model_{snap:03d}.hdf5: per-file RunProperties should record the identity "
                f"multiplier"
            )

    # The empty snapshot's partition is asserted empty as well as present.
    with h5py.File(snapshot_partition_path(output_dir, empty_snapshot), "r") as handle:
        rows = handle[f"Snap{empty_snapshot:03d}/Galaxies"].shape[0]
        assert rows == 0, (
            f"snapshot {empty_snapshot} is an empty fixture snapshot, so its partition "
            f"should carry an empty Galaxies table, found {rows} rows"
        )

    with h5py.File(master, "r") as handle:
        master_groups = sorted(name for name in handle if name.startswith("Snap"))
        assert master_groups == sorted(f"Snap{snap:03d}" for snap in requested), (
            f"the master should hold one group per requested output snapshot, "
            f"found {master_groups}"
        )
        for snap in requested:
            members = sorted(handle[f"Snap{snap:03d}"])
            assert members == [
                f"File{snap:03d}"
            ], f"master Snap{snap:03d} should hold exactly File{snap:03d}, found {members}"
            file_group = handle[f"Snap{snap:03d}/File{snap:03d}"]
            assert sorted(file_group) == ["Galaxies"], (
                f"master Snap{snap:03d}/File{snap:03d} should link only Galaxies, "
                f"found {sorted(file_group)}"
            )
            link = file_group.get("Galaxies", getlink=True)
            assert isinstance(
                link, h5py.ExternalLink
            ), f"master Snap{snap:03d}/File{snap:03d}/Galaxies should be an external link"
            # The link must resolve to the file named for THIS snapshot: a master
            # that pointed every snapshot at one partition would still satisfy an
            # is-a-link assertion.
            assert link.filename == f"model_{snap:03d}.hdf5", (
                f"master Snap{snap:03d} should link into model_{snap:03d}.hdf5, "
                f"found {link.filename}"
            )
            assert (
                link.path == f"Snap{snap:03d}/Galaxies"
            ), f"master Snap{snap:03d} should link that file's own group, found {link.path}"
            linked_total = int(file_group.attrs["TotHalosPerSnap"].ravel()[0])
            assert linked_total == file_group["Galaxies"].shape[0], (
                f"master Snap{snap:03d}/File{snap:03d}: TotHalosPerSnap {linked_total} should "
                f"equal the {file_group['Galaxies'].shape[0]} rows it links to"
            )
        properties = handle["RunProperties"].attrs
        tree_type = properties["TreeType"].ravel()[0]
        if isinstance(tree_type, bytes):
            tree_type = tree_type.decode()
        assert tree_type == "horizontal_hdf5", f"master TreeType is {tree_type!r}"
        assert (
            "UniqueGalaxyIDMultiplier" in properties
        ), "master RunProperties should record the identity multiplier"


def skip_unless_horizontal_driver_is_runnable(probe_name):
    """Skip unless the selected package is horizontal and the fixture is present.

    Same two guards the completing-run test above carries, for the same reasons:
    the package's own configuration is the only source of
    input.tree_type/tree_name/processing_order here, and a sparse checkout has no
    fixture payload to run against.
    """
    if effective_input_setting(probe_name, "processing_order") != "horizontal":
        raise TestSkipped(
            "selected package is not horizontal; its own configuration is the only "
            "source of input.tree_type/tree_name/processing_order this test relies on"
        )
    skip_unless_snapshot_fixture_present()


def test_horizontal_failure_keeps_partition_files_that_already_closed():
    """
    Test that a mid-run abort leaves completed partition files alone and writes no master.

    Expected: non-zero exit naming the invalid link; the partition file of the requested
              snapshot that completed BEFORE the corrupted snapshot still exists; the
              partition file of the requested snapshot AFTER it does not; and no master
              file exists.
    Validates: the per-partition (not all-or-nothing) cleanup contract. A closed
               partition file is final output and must survive a later failure,
               because destroying weeks of finished output on a late abort is the
               larger hazard; the master, which never got written, must
               not be left behind.

    The fault is a deterministic link corruption in a temporary copy of the fixture, not a
    committed corrupt fixture: FirstHaloInFOFgroup is range-checked only when its slab is
    loaded, so the abort lands mid-sweep with one requested snapshot already written and
    closed. Nothing here can exercise the in-flight half of the registry -- the failing
    snapshot's own output file does not exist yet -- which is why the next test exists.
    """
    skip_unless_horizontal_driver_is_runnable("retention_probe")

    requested = [1, snapshot_fixture_snapshot_count() - 1]
    # The first populated snapshot strictly between the two requested ones, so the
    # abort lands after the first request is written and before the second exists.
    counts = snapshot_fixture_halo_counts()
    candidates = [snap for snap in range(requested[0] + 1, requested[1]) if counts[snap] > 0]
    assert candidates, (
        f"the fixture needs a populated snapshot strictly between {requested[0]} and "
        f"{requested[1]} to corrupt, found halo counts {counts}"
    )
    broken_snapshot = candidates[0]
    dataset_dir = fixture_copy_with_broken_fof_link(
        Path(TEMP_DIR) / "retention_dataset", broken_snapshot
    )
    output_dir = Path(TEMP_DIR) / "retention_output"

    returncode, output = run_config(
        "retention",
        input_overrides=snapshot_fixture_input_overrides(dataset_dir),
        output_overrides={
            "output_format": "hdf5",
            "output_directory": str(output_dir),
            "snapshot_list": requested,
        },
        extra_args=["-v"],
    )

    assert returncode != 0, f"an out-of-range FoF link should abort the run:\n{output}"
    assert "invalid link field(s)" in output, f"the abort should name the invalid link:\n{output}"

    completed = snapshot_partition_path(output_dir, requested[0])
    assert completed.is_file(), (
        f"{completed.name} closed before the failure and must survive it; "
        f"{output_dir} holds {[p.name for p in snapshot_partition_files(output_dir)]}"
    )
    later = snapshot_partition_path(output_dir, requested[1])
    assert (
        not later.exists()
    ), f"{later.name} is after the corrupted snapshot and should never have been created"
    assert not (
        output_dir / "model.hdf5"
    ).exists(), "a failed run must not leave a master file claiming complete output"


def test_horizontal_failure_removes_the_in_flight_partition_file():
    """
    Test that a failure while a partition file is in flight removes that file.

    Expected: non-zero exit naming the file it could not create; the pre-created marker
              file at the later snapshot's partition path is GONE; the earlier requested
              snapshot's partition file survives; and no master exists.
    Validates: the removal half of the cleanup registry -- the in-flight partition slot is
               armed before the file is created and acted on by bye(). This is the only
               injection that reaches it, since a slab-loading failure aborts before the
               output file exists.

    The fault is a read-only regular file pre-created at the target partition path, which
    makes H5Fcreate fail on a path the driver has already armed. unlink() needs write
    permission on the directory rather than on the file, so cleanup can still remove it --
    and the marker byte is what proves the file that disappeared was this one.
    """
    skip_unless_horizontal_driver_is_runnable("inflight_probe")
    skip_unless_mode_bits_deny_access()

    requested = [1, snapshot_fixture_snapshot_count() - 1]
    output_dir = Path(TEMP_DIR) / "inflight_output"
    output_dir.mkdir(parents=True, exist_ok=True)

    blocked = snapshot_partition_path(output_dir, requested[1])
    blocked.write_bytes(b"marker")
    blocked.chmod(0o444)

    returncode, output = run_config(
        "inflight",
        input_overrides=snapshot_fixture_input_overrides(),
        output_overrides={
            "output_format": "hdf5",
            "output_directory": str(output_dir),
            "snapshot_list": requested,
        },
        extra_args=["-v"],
    )

    assert returncode != 0, f"an uncreatable partition file should abort the run:\n{output}"
    assert (
        f"Failed to create HDF5 file '{blocked}'" in output
    ), f"the abort should name the partition file it could not create:\n{output}"
    assert not blocked.exists(), (
        f"{blocked.name} was armed as the in-flight partition and must be removed by "
        f"cleanup, marker byte and all"
    )
    completed = snapshot_partition_path(output_dir, requested[0])
    assert completed.is_file(), f"{completed.name} closed before the failure and must survive it"
    assert not (
        output_dir / "model.hdf5"
    ).exists(), "a failed run must not leave a master file claiming complete output"


def test_horizontal_unwritable_output_directory_fails_before_the_dataset_opens():
    """
    Test that an unwritable output directory aborts the run before the dataset is opened.

    Expected: non-zero exit; output names the output directory as not writable; and the
              driver's "Opened horizontal run" line -- emitted once the reader has
              validated the whole dataset -- is absent, so the failure preceded it.
    Validates: the up-front writability probe. main.c proves the output directory can be
               created, not written to, and now that a partition file appears only when its
               snapshot completes, an unwritable directory would otherwise surface at the
               first requested output snapshot -- the end of a multi-week run for a z=0-only
               request.
    """
    skip_unless_horizontal_driver_is_runnable("writability_probe")
    skip_unless_mode_bits_deny_access()

    output_dir = Path(TEMP_DIR) / "unwritable_output"
    output_dir.mkdir(parents=True, exist_ok=True)
    output_dir.chmod(0o555)
    try:
        returncode, output = run_config(
            "unwritable",
            input_overrides=snapshot_fixture_input_overrides(),
            output_overrides={
                "output_format": "hdf5",
                "output_directory": str(output_dir),
                "snapshot_list": [1],
            },
            extra_args=["-v"],
        )
    finally:
        output_dir.chmod(0o755)

    assert returncode != 0, f"an unwritable output directory should abort the run:\n{output}"
    assert (
        f"Output directory '{output_dir}' is not writable" in output
    ), f"the abort should name the unwritable output directory:\n{output}"
    assert "Opened horizontal run" not in output, (
        "the writability probe must fail before the dataset is opened and validated, "
        f"which is not instant at production scale:\n{output}"
    )


def test_horizontal_binary_output_rejected_at_config_time():
    """
    Test that a horizontal configuration with output_format binary is rejected.

    Expected: Non-zero exit; output includes the HDF5-only message and
              "Parameter validation failed". The rejection fires purely from parsed
              configuration, before any reader is opened, so it applies regardless of
              which package is selected.
    Validates: acceptance criterion (a) -- output_format: binary is HDF5-only for a
               horizontal configuration.
    """
    returncode, output = run_config(
        "snapshot_binary_output",
        input_overrides={
            "tree_type": "horizontal_hdf5",
            "processing_order": "horizontal",
            "tree_name": HORIZONTAL_TREE_NAME,
        },
        output_overrides={"output_format": "binary"},
    )

    assert returncode != 0, "binary output_format must be rejected for a horizontal config"
    assert "output_format is 'binary', but horizontal runs are HDF5-only" in output
    assert "Parameter validation failed" in output


def test_horizontal_skip_rejected_at_config_time():
    """
    Test that --skip is rejected for a horizontal configuration.

    Expected: Non-zero exit; output includes the no-resume message and
              "Parameter validation failed". The rejection fires purely from parsed
              configuration, before any reader is opened, so it applies regardless of
              which package is selected.
    Validates: acceptance criterion (b) -- resume is not supported for horizontal
               runs.
    """
    returncode, output = run_config(
        "snapshot_skip",
        input_overrides={
            "tree_type": "horizontal_hdf5",
            "processing_order": "horizontal",
            "tree_name": HORIZONTAL_TREE_NAME,
        },
        extra_args=["--skip"],
    )

    assert returncode != 0, "--skip must be rejected for a horizontal config"
    assert "--skip was given, but resume is not supported for horizontal runs" in output
    assert "Parameter validation failed" in output


def test_horizontal_reader_rejects_vertical_order():
    """
    Test that a horizontal reader with processing_order vertical is rejected.

    Expected: Non-zero exit; output includes the reader/order compatibility message.
    Validates: the compatibility check now covers horizontal readers too.
    """
    returncode, output = run_config(
        "horizontal_reader_vertical_order",
        input_overrides={
            "tree_type": "horizontal_hdf5",
            "processing_order": "vertical",
            "tree_name": HORIZONTAL_TREE_NAME,
        },
    )

    assert returncode != 0, "horizontal_hdf5 with vertical should fail config validation"
    assert (
        "Reader 'horizontal_hdf5' is compatible with processing_order 'horizontal', "
        "but input.processing_order is 'vertical'" in output
    )
    assert "Parameter validation failed" in output


def test_horizontal_reader_unset_processing_order_names_the_default():
    """
    Test that a horizontal reader with processing_order entirely unset blames the default.

    Expected: Non-zero exit; output includes the reader/order compatibility message with
              the "(the default; input.processing_order was not set)" fragment, and
              "Parameter validation failed" is present (config-time rejection, not the
              driver message).
    Validates: Part 1 finding 3 — when input.processing_order appears in neither the run
               file nor the simulation config it points at, the mismatch message names the
               internal vertical seed as a default rather than attributing it to the
               user, since the user never wrote it.

    The unset-default case only exists when neither the run file nor the simulation config
    it points at declares input.processing_order; a package whose own configuration declares
    the key (e.g. micro-uchuu-horizontal's horizontal) makes it configured, so the test
    skips there rather than asserting a condition the package contradicts.
    """
    if effective_input_setting("unset_order_probe", "processing_order") is not None:
        raise TestSkipped(
            "selected package's configuration declares input.processing_order; "
            "the unset-default case does not apply"
        )

    returncode, output = run_config(
        "snapshot_processing_order_unset",
        input_overrides={
            "tree_type": "horizontal_hdf5",
            "tree_name": HORIZONTAL_TREE_NAME,
        },
    )

    assert returncode != 0, "an unset processing_order should still fail the compatibility check"
    assert (
        "Reader 'horizontal_hdf5' is compatible with processing_order 'horizontal', "
        "but input.processing_order is 'vertical' "
        "(the default; input.processing_order was not set)" in output
    )
    assert "Parameter validation failed" in output


def test_vertical_reader_rejects_horizontal_order():
    """
    Test that a vertical reader with processing_order horizontal is rejected.

    Expected: Non-zero exit; output includes the reader/order compatibility message.
    Validates: the compatibility check is reached for vertical readers under
               horizontal, which the removed blanket rejection used to mask.
    """
    returncode, output = run_config(
        "ascii_horizontal_order",
        input_overrides={
            "tree_type": "consistent_trees_ascii",
            "processing_order": "horizontal",
        },
    )

    assert returncode != 0, "consistent_trees_ascii with horizontal should fail"
    assert (
        "Reader 'consistent_trees_ascii' is compatible with processing_order 'vertical', "
        "but input.processing_order is 'horizontal'" in output
    )
    assert "Parameter validation failed" in output


def test_unknown_tree_type_names_both_registries():
    """
    Test that an unknown input.tree_type fails with one message naming both registries.

    Expected: Non-zero exit; exactly one "Unknown tree_type" message, naming both
              src/io/vertical/registry.c and src/io/horizontal/registry.c.
    Validates: the two-registry lookup reports a single actionable failure rather
               than one per registry.
    """
    returncode, output = run_config(
        "unknown_tree_type", input_overrides={"tree_type": "not_a_real_reader"}
    )

    assert returncode != 0, "an unknown tree_type should fail at startup"
    assert output.count("Unknown tree_type") == 1, "the failure should be reported exactly once"
    assert "Unknown tree_type 'not_a_real_reader'" in output
    assert "src/io/vertical/registry.c" in output
    assert "src/io/horizontal/registry.c" in output


def test_horizontal_tree_name_must_be_exact_literal():
    """
    Test that a horizontal configuration accepts only the exact tree_name literal.

    Expected: Non-zero exit for every other value, with a message naming the accepted literal.
              The accepted-literal control additionally asserts "Unknown tree_type" is absent
              (see the comment above it) -- reaching and exercising the real driver is a
              separate concern, owned by test_horizontal_run_completes_and_writes_output_over_the_fixture.
    Validates: configured text never becomes a printf format or a silent filename mismatch.
    """
    rejected = ["snapshot_%d.h5", "snapshot_%s.h5", "", "trees_063"]
    for index, tree_name in enumerate(rejected):
        returncode, output = run_config(
            f"tree_name_{index}",
            input_overrides={
                "tree_type": "horizontal_hdf5",
                "processing_order": "horizontal",
                "tree_name": tree_name,
            },
        )
        assert returncode != 0, f"tree_name '{tree_name}' should be rejected"
        assert "input.tree_name" in output, f"the failure should name input.tree_name ({tree_name})"
        assert "Parameter validation failed" in output
        if tree_name:
            assert (
                f"accepts input.tree_name only as the exact literal '{HORIZONTAL_TREE_NAME}'"
                in output
            )

    # The accepted literal is the control. An absence-only assertion on "Parameter
    # validation failed" alone cannot distinguish "config accepted" from "config never
    # got that far", so this also asserts "Unknown tree_type" is absent -- ruling out
    # the specific alternative explanation that the literal silently failed reader
    # lookup instead of being genuinely accepted. output_format is forced to hdf5 for
    # the same reason the completing-run test does: the generated reference run file is
    # output_format: binary, which a horizontal configuration rejects at config
    # time, independent of tree_name.
    #
    # The dataset is repointed so this config-time control can never start a full
    # production run: at the selected package's committed fixture when it has one (the
    # run is then cheap and completes), otherwise at an empty scratch directory, so
    # nothing is readable once configuration has been accepted. Whether the driver
    # then aborts or completes is outside this control's contract -- it asserts
    # config-time acceptance only.
    if package_fixture() is not None:
        dataset_overrides = snapshot_fixture_input_overrides()
    else:
        no_dataset = Path(TEMP_DIR) / "tree_name_no_dataset"
        no_dataset.mkdir(parents=True, exist_ok=True)
        dataset_overrides = {"simulation_dir": str(no_dataset)}
    returncode, output = run_config(
        "tree_name_accepted",
        input_overrides={
            "tree_type": "horizontal_hdf5",
            "processing_order": "horizontal",
            "tree_name": HORIZONTAL_TREE_NAME,
            **dataset_overrides,
        },
        # snapshot_list must name an index the run's scale-factor list contains,
        # and every committed fixture and package list holds at least two: an
        # out-of-range request is itself a config-time rejection, which would mask
        # the one this control is looking for.
        output_overrides={"output_format": "hdf5", "snapshot_list": [1]},
    )
    assert "Parameter validation failed" not in output
    assert "Unknown tree_type" not in output, "the accepted literal must resolve the reader"


def test_multiplier_default_and_non_positive_rejection():
    """
    Test the identity multiplier's default and its non-positive rejection.

    Expected: the default value runs the selected package's configuration to completion;
              0 and a negative value fail at config time with a "must be positive" message.
    Validates: simulation.unique_galaxy_id_multiplier parses, defaults to TREE_MUL_FAC,
               and rejects non-positive values.

    The returncode == 0 assertions below run the selected package's own generated run
    file, on its committed test data, to completion; nothing is read back from the
    output, so the test applies to vertical and horizontal packages alike. Reading the
    effective multiplier back out of the ids is the job of the two tests after it.
    """
    # Absent key: the seeded default is TREE_MUL_FAC, so the run is accepted by the
    # non-default guard and completes normally.
    returncode, output = run_config("multiplier_absent")
    assert returncode == 0, f"a default run should succeed:\n{output}"
    assert "unique_galaxy_id_multiplier" not in output

    # Explicitly declaring the default is equally accepted.
    returncode, output = run_config(
        "multiplier_default",
        simulation_overrides={"unique_galaxy_id_multiplier": DEFAULT_MULTIPLIER},
    )
    assert returncode == 0, f"declaring the default multiplier should succeed:\n{output}"

    for name, value in (("multiplier_zero", 0), ("multiplier_negative", -5)):
        returncode, output = run_config(
            name, simulation_overrides={"unique_galaxy_id_multiplier": value}
        )
        assert returncode != 0, f"multiplier {value} should be rejected"
        assert f"simulation.unique_galaxy_id_multiplier is {value}" in output
        assert "must be positive" in output


def _skip_unless_selected_package_is_vertical():
    """Skip tests that read binary galaxy output when the selected package forbids it."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    skip_if_selected_package_is_horizontal("binary galaxy output to read the multiplier back from")


def _run_and_read_unique_ids(name, **kwargs):
    """
    Run a vertical configuration to completion and return its UniqueGalaxyID list.

    The effective identity multiplier is only observable in what the encoder actually
    wrote, so the multiplier tests below read the ids back out of the binary galaxy
    output rather than trusting a log line or a config-time message.
    """
    param_file = make_param_file(name, output_overrides={"output_format": "binary"}, **kwargs)
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, (
        f"vertical run '{name}' should succeed (rc={returncode})\n"
        f"STDOUT:\n{stdout}\nSTDERR:\n{stderr}"
    )

    with open(param_file, "r") as handle:
        output_dir = Path(yaml.safe_load(handle)["output"]["output_directory"])
    output_files = sorted(output_dir.glob("model_z*_*"))
    assert output_files, f"no binary output partitions found in {output_dir}"

    ids = []
    for path in output_files:
        halos, _metadata = load_binary_halos(path)
        ids.extend(int(value) for value in halos["UniqueGalaxyID"])
    assert ids, f"run '{name}' produced no galaxies to read ids from"
    return ids


def test_vertical_accepts_non_default_multiplier():
    """
    Test that a vertical run honours a non-default identity multiplier end to end.

    Expected: exit 0, and the run's ids decompose under 10^10 into exactly the
              (halonr, forestnr_global) component pairs the same run produces under the
              default 10^9 multiplier.
    Validates: a vertical configuration declaring 10^10 passes config validation, runs,
               and produces ids encoded with 10^10. Comparing decomposed COMPONENTS rather
               than raw ids is what makes this falsifiable: an encoder still hard-coded to
               TREE_MUL_FAC would emit the default run's ids, which decompose under 10^10
               to forest index -1 and cannot match. The min-id assertion catches the same
               failure independently.
    """
    _skip_unless_selected_package_is_vertical()

    ten_billion = 10 * DEFAULT_MULTIPLIER

    default_ids = _run_and_read_unique_ids("multiplier_default_reference")
    scaled_ids = _run_and_read_unique_ids(
        "multiplier_non_default",
        simulation_overrides={"unique_galaxy_id_multiplier": ten_billion},
    )

    def components(ids, multiplier):
        return sorted((value % multiplier, value // multiplier - 1) for value in ids)

    assert components(scaled_ids, ten_billion) == components(default_ids, DEFAULT_MULTIPLIER), (
        "a 10^10 multiplier must encode the same (halonr, forestnr_global) components "
        "as the default run, only scaled"
    )
    assert (
        min(scaled_ids) >= ten_billion
    ), "under a 10^10 multiplier every id must sit at or above one multiplier block"
    assert set(scaled_ids) != set(
        default_ids
    ), "the configured multiplier must actually change the encoding"


def test_multiplier_precedence_across_both_parser_passes():
    """
    Test both precedence directions for simulation.unique_galaxy_id_multiplier.

    Expected: a value set only in the simulation config survives a run file that omits the
              key (ids encoded with 2x10^9); a run-file value overrides the package value
              (ids encoded with 9x10^9, neither 2x10^9 nor the default).
    Validates: the default is seeded once before either parse_simulation_section pass and
               assigned only when the key is present, so the second pass cannot clobber a
               package value. The observable is the encoding the run actually used -- the
               smallest id in a run belongs to forest 0, so it lies in [M, 2M) and
               min(ids) // M == 1 identifies the effective multiplier M. The three candidate
               values are spread more than two-fold apart precisely so the test cannot pass
               under the wrong one.
    """
    _skip_unless_selected_package_is_vertical()

    package_value = 2 * DEFAULT_MULTIPLIER
    run_file_value = 9 * DEFAULT_MULTIPLIER

    ids = _run_and_read_unique_ids("multiplier_package_only", package_multiplier=package_value)
    assert (
        min(ids) // package_value == 1
    ), "a simulation_info.yaml value must survive a run file that omits the key"
    assert (
        min(ids) // DEFAULT_MULTIPLIER != 1
    ), "the seeded default must not win over a package value"

    ids = _run_and_read_unique_ids(
        "multiplier_run_file_wins",
        package_multiplier=package_value,
        simulation_overrides={"unique_galaxy_id_multiplier": run_file_value},
    )
    assert (
        min(ids) // run_file_value == 1
    ), "an explicit run-file value must override the package value"
    assert min(ids) // package_value != 1, "the package value must not survive a run-file value"
    assert min(ids) // DEFAULT_MULTIPLIER != 1, "the seeded default must not win either"


def main():
    global TEMP_DIR
    TEMP_DIR = Path(tempfile.mkdtemp(prefix="mimic_processing_order_"))
    try:
        tests = [
            test_unknown_processing_order_fails_fast,
            test_horizontal_run_completes_and_writes_output_over_the_fixture,
            test_horizontal_failure_keeps_partition_files_that_already_closed,
            test_horizontal_failure_removes_the_in_flight_partition_file,
            test_horizontal_unwritable_output_directory_fails_before_the_dataset_opens,
            test_horizontal_binary_output_rejected_at_config_time,
            test_horizontal_skip_rejected_at_config_time,
            test_horizontal_reader_rejects_vertical_order,
            test_horizontal_reader_unset_processing_order_names_the_default,
            test_vertical_reader_rejects_horizontal_order,
            test_unknown_tree_type_names_both_registries,
            test_horizontal_tree_name_must_be_exact_literal,
            test_multiplier_default_and_non_positive_rejection,
            test_vertical_accepts_non_default_multiplier,
            test_multiplier_precedence_across_both_parser_passes,
        ]
        return run_test_suite(tests, "Processing Order Validation")
    finally:
        shutil.rmtree(TEMP_DIR)


if __name__ == "__main__":
    sys.exit(main())
