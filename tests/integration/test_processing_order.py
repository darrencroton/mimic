#!/usr/bin/env python3
"""
Integration tests for startup validation of the reader/processing-order seam.

Covers input.processing_order, the two-registry input.tree_type resolution, the
snapshot reader's exact tree_name contract, and the
simulation.unique_galaxy_id_multiplier key (parse, default, precedence across
both parser passes, and a tree-ordered run honouring a non-default value).
"""

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
    create_test_param_file,
    load_binary_halos,
    run_mimic,
    run_test_suite,
)

TEMP_DIR = None

#: Default forest multiplier (TREE_MUL_FAC in src/include/constants.h).
DEFAULT_MULTIPLIER = 1000000000

#: The only input.tree_name the snapshot_hdf5 reader accepts.
SNAPSHOT_TREE_NAME = "snapshot_%03d.h5"

#: Committed snapshot-package fixture (small, deterministic, always present in a
#: full checkout) -- used instead of the machine-local production dataset so the
#: driver test below is reproducible on any checkout and reads kilobytes, not
#: the multi-gigabyte real conversion.
SNAPSHOT_FIXTURE_DIR = REPO_ROOT / "simulations" / "micro-uchuu-snapshot" / "_tests" / "data"
SNAPSHOT_FIXTURE_A_LIST = SNAPSHOT_FIXTURE_DIR / "micro-uchuu-fixture.a_list"


def snapshot_fixture_snapshot_count():
    """Number of snapshots the fixture's scale-factor list declares, or 0 if absent."""
    if not SNAPSHOT_FIXTURE_A_LIST.is_file():
        return 0
    with open(SNAPSHOT_FIXTURE_A_LIST, "r") as handle:
        return sum(1 for line in handle if line.strip())


def snapshot_fixture_snapshot_files():
    """Every snapshot payload file the fixture's a_list implies, in load order."""
    return [
        SNAPSHOT_FIXTURE_DIR / f"snapshot_{snap:03d}.h5"
        for snap in range(snapshot_fixture_snapshot_count())
    ]


def snapshot_fixture_present():
    """Is the committed snapshot-package fixture's full payload present?

    The guard is derived from the a_list, because the a_list is what bounds the
    run: the driver loads every snapshot the scale-factor list declares, and
    open_run validates every one of those files. output.snapshot_list selects
    which of them are *written*, and bounds nothing that is *read*. Checking the
    a_list alone would pass a partial checkout that has the 48-byte list but no
    payload, so every implied file is checked; a resized fixture changes the set
    checked here rather than letting the guard drift out of sync. forests.h5 is
    deliberately not checked: it is converter provenance the C reader never
    opens.
    """
    return snapshot_fixture_snapshot_count() > 0 and all(
        path.is_file() for path in snapshot_fixture_snapshot_files()
    )


def snapshot_fixture_input_overrides():
    """input.* overrides that repoint a run at the committed fixture dataset."""
    return {
        "simulation_dir": str(SNAPSHOT_FIXTURE_DIR),
        "snapshot_list_file": str(SNAPSHOT_FIXTURE_A_LIST),
    }


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

    Expected: Non-zero exit; output includes the bad value name and "Valid values are tree_ordered, snapshot_ordered".
    Validates: startup validation rejects unknown ordering strings with an actionable message.
    """
    returncode, output = run_config(
        "not_a_real_ordering", input_overrides={"processing_order": "not_a_real_ordering"}
    )

    assert returncode != 0, "Unknown processing_order should fail startup validation"
    assert "Unknown input.processing_order 'not_a_real_ordering'" in output
    assert "Valid values are tree_ordered, snapshot_ordered" in output


def test_snapshot_run_completes_and_writes_output_over_the_fixture():
    """
    Test that a valid snapshot-ordered configuration runs end to end and writes output.

    Expected: exit 0; output does NOT include "Parameter validation failed" or either of
              the two messages earlier slices retired; the per-snapshot lifecycle lines
              show every snapshot loaded and released in ascending order under the
              two-generation rotation, with two slabs live from snapshot 1 onward; and
              the run leaves exactly one numbered partition file plus a master, whose
              TotHalosPerSnap totals equal the rows actually written, with no Ntrees
              attribute, no TreeHalosPerSnap dataset or link, TreeType "snapshot_hdf5",
              and UniqueGalaxyIDMultiplier in both per-file and master RunProperties.
    Validates: the snapshot-ordered driver produces output through the driver-neutral
               output partition seam, and does so under the state rotation the phase
               specifies rather than by holding every slab live.

    Runs against the committed snapshot-package fixture (simulations/micro-uchuu-snapshot/
    _tests/data/), not the machine-local production dataset: the latter is multi-gigabyte,
    gitignored, and absent on a fresh checkout, which would make this proof unreproducible
    outside one workstation. input.simulation_dir and input.snapshot_list_file are
    overridden to point at the fixture; output.snapshot_list is overridden to indices the
    fixture's own a_list actually contains, since the generated core run file's default
    (49) is only valid for the real package's 50-snapshot production list.
    simulations/micro-uchuu-snapshot/_tests/unit/test_unit_snapshot_reader_open.c already
    proves open_run succeeds against exactly this fixture with these same two fields set.

    The test still only runs when the selected package is itself snapshot-ordered (its own
    configuration is the only source of input.tree_type/tree_name/processing_order here);
    forcing tree_type: snapshot_hdf5 onto a tree-ordered package would abort for an
    unrelated config-mismatch reason. Guarded separately against the fixture being absent,
    so a sparse or partial checkout skips rather than fails.

    output_format is forced to hdf5 because the generated core test input this run file is
    based on is output_format: binary, which a snapshot-ordered configuration rejects at
    config time (see test_snapshot_binary_output_rejected_at_config_time).

    -v is passed so the driver's per-snapshot lifecycle lines (silent at the default log
    level) are captured. They are VERBOSE_LOG rather than DEBUG_LOG deliberately: the
    driver enables debug-log rate limiting for the physics phase, which caps each
    DEBUG_LOG site at five calls and would truncate the ordered sequence asserted below.
    """
    import h5py

    if effective_input_setting("valid_snapshot_probe", "processing_order") != "snapshot_ordered":
        raise TestSkipped(
            "selected package is not snapshot-ordered; its own configuration is the only "
            "source of input.tree_type/tree_name/processing_order this test relies on"
        )
    if not snapshot_fixture_present():
        raise TestSkipped(f"committed snapshot fixture not found at {SNAPSHOT_FIXTURE_DIR}")

    nsnapshots = snapshot_fixture_snapshot_count()
    requested = [nsnapshots - 1, 1]
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

    assert returncode == 0, f"a valid snapshot-ordered run should complete:\n{output}"
    assert (
        "Parameter validation failed" not in output
    ), "a valid snapshot-ordered configuration must pass config validation"
    assert (
        "The snapshot-ordered driver is not implemented yet" not in output
    ), "the dispatch-time FATAL an earlier slice retired must not reappear"
    assert (
        "cannot yet produce output" not in output
    ), "the skeleton driver's abort must not survive into a producing driver"

    # The FULL ordered lifecycle, not just "a line mentioning two slabs somewhere":
    # every snapshot must be loaded in ascending order, each load after the first
    # with two generations live, and every snapshot released. Asserting the ordered
    # subsequence is what makes a shortened or reordered loop fail here.
    expected_sequence = []
    for snap in range(nsnapshots):
        live = 1 if snap == 0 else 2
        expected_sequence.append(f"Loaded snapshot {snap} (")
        expected_sequence.append(f"; {live} slab{'' if live == 1 else 's'} live")
        if snap > 0:
            expected_sequence.append(f"Released snapshot {snap - 1} ")
    expected_sequence.append(f"Released snapshot {nsnapshots - 1} ")

    cursor = 0
    for needle in expected_sequence:
        found = output.find(needle, cursor)
        assert found >= 0, (
            f"expected {needle!r} after position {cursor} in the driver's lifecycle log; "
            f"the rotation sequence is incomplete or out of order:\n{output}"
        )
        cursor = found + len(needle)

    partitions = sorted(output_dir.glob("*_[0-9][0-9][0-9].hdf5"))
    assert [p.name for p in partitions] == [
        "model_000.hdf5"
    ], f"a snapshot-ordered run writes exactly one partition, found {[p.name for p in partitions]}"
    master = output_dir / "model.hdf5"
    assert master.is_file(), f"the master file is missing from {output_dir}"

    with h5py.File(partitions[0], "r") as handle:
        for snap in requested:
            group = handle[f"Snap{snap:03d}"]
            dataset = group["Galaxies"]
            total = int(dataset.attrs["TotHalosPerSnap"].ravel()[0])
            assert total == dataset.shape[0], (
                f"Snap{snap:03d}: TotHalosPerSnap {total} should equal the "
                f"{dataset.shape[0]} marshalled rows"
            )
            assert (
                "Ntrees" not in dataset.attrs
            ), f"Snap{snap:03d}: a snapshot-ordered run has no trees to count"
            assert (
                "TreeHalosPerSnap" not in group
            ), f"Snap{snap:03d}: a snapshot-ordered run has no per-tree counts"
        assert (
            "UniqueGalaxyIDMultiplier" in handle["RunProperties"].attrs
        ), "per-file RunProperties should record the identity multiplier"

    with h5py.File(master, "r") as handle:
        for snap in requested:
            members = sorted(handle[f"Snap{snap:03d}"])
            assert members == [
                "File000"
            ], f"master Snap{snap:03d} should hold exactly File000, found {members}"
            file_group = handle[f"Snap{snap:03d}/File000"]
            assert sorted(file_group) == [
                "Galaxies"
            ], f"master Snap{snap:03d}/File000 should link only Galaxies, found {sorted(file_group)}"
            assert isinstance(
                file_group.get("Galaxies", getlink=True), h5py.ExternalLink
            ), f"master Snap{snap:03d}/File000/Galaxies should be an external link"
        properties = handle["RunProperties"].attrs
        tree_type = properties["TreeType"].ravel()[0]
        if isinstance(tree_type, bytes):
            tree_type = tree_type.decode()
        assert tree_type == "snapshot_hdf5", f"master TreeType is {tree_type!r}"
        assert (
            "UniqueGalaxyIDMultiplier" in properties
        ), "master RunProperties should record the identity multiplier"


def test_snapshot_binary_output_rejected_at_config_time():
    """
    Test that a snapshot-ordered configuration with output_format binary is rejected.

    Expected: Non-zero exit; output includes the HDF5-only message and
              "Parameter validation failed". The rejection fires purely from parsed
              configuration, before any reader is opened, so it applies regardless of
              which package is selected.
    Validates: acceptance criterion (a) -- output_format: binary is HDF5-only for a
               snapshot-ordered configuration.
    """
    returncode, output = run_config(
        "snapshot_binary_output",
        input_overrides={
            "tree_type": "snapshot_hdf5",
            "processing_order": "snapshot_ordered",
            "tree_name": SNAPSHOT_TREE_NAME,
        },
        output_overrides={"output_format": "binary"},
    )

    assert returncode != 0, "binary output_format must be rejected for a snapshot-ordered config"
    assert "output_format is 'binary', but snapshot-ordered runs are HDF5-only" in output
    assert "Parameter validation failed" in output


def test_snapshot_skip_rejected_at_config_time():
    """
    Test that --skip is rejected for a snapshot-ordered configuration.

    Expected: Non-zero exit; output includes the no-resume message and
              "Parameter validation failed". The rejection fires purely from parsed
              configuration, before any reader is opened, so it applies regardless of
              which package is selected.
    Validates: acceptance criterion (b) -- resume is not supported for snapshot-ordered
               runs.
    """
    returncode, output = run_config(
        "snapshot_skip",
        input_overrides={
            "tree_type": "snapshot_hdf5",
            "processing_order": "snapshot_ordered",
            "tree_name": SNAPSHOT_TREE_NAME,
        },
        extra_args=["--skip"],
    )

    assert returncode != 0, "--skip must be rejected for a snapshot-ordered config"
    assert "--skip was given, but resume is not supported for snapshot-ordered runs" in output
    assert "Parameter validation failed" in output


def test_snapshot_reader_rejects_tree_ordered():
    """
    Test that a snapshot reader with processing_order tree_ordered is rejected.

    Expected: Non-zero exit; output includes the reader/order compatibility message.
    Validates: the compatibility check now covers snapshot readers too.
    """
    returncode, output = run_config(
        "snapshot_tree_ordered",
        input_overrides={
            "tree_type": "snapshot_hdf5",
            "processing_order": "tree_ordered",
            "tree_name": SNAPSHOT_TREE_NAME,
        },
    )

    assert returncode != 0, "snapshot_hdf5 with tree_ordered should fail config validation"
    assert (
        "Reader 'snapshot_hdf5' is compatible with processing_order 'snapshot_ordered', "
        "but input.processing_order is 'tree_ordered'" in output
    )
    assert "Parameter validation failed" in output


def test_snapshot_reader_unset_processing_order_names_the_default():
    """
    Test that a snapshot reader with processing_order entirely unset blames the default.

    Expected: Non-zero exit; output includes the reader/order compatibility message with
              the "(the default; input.processing_order was not set)" fragment, and
              "Parameter validation failed" is present (config-time rejection, not the
              driver message).
    Validates: Part 1 finding 3 — when input.processing_order appears in neither the run
               file nor the simulation config it points at, the mismatch message names the
               internal tree_ordered seed as a default rather than attributing it to the
               user, since the user never wrote it.

    The unset-default case only exists when neither the run file nor the simulation config
    it points at declares input.processing_order; a package whose own configuration declares
    the key (e.g. micro-uchuu-snapshot's snapshot_ordered) makes it configured, so the test
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
            "tree_type": "snapshot_hdf5",
            "tree_name": SNAPSHOT_TREE_NAME,
        },
    )

    assert returncode != 0, "an unset processing_order should still fail the compatibility check"
    assert (
        "Reader 'snapshot_hdf5' is compatible with processing_order 'snapshot_ordered', "
        "but input.processing_order is 'tree_ordered' "
        "(the default; input.processing_order was not set)" in output
    )
    assert "Parameter validation failed" in output


def test_tree_reader_rejects_snapshot_ordered():
    """
    Test that a tree reader with processing_order snapshot_ordered is rejected.

    Expected: Non-zero exit; output includes the reader/order compatibility message.
    Validates: the compatibility check is reached for tree readers under
               snapshot_ordered, which the removed blanket rejection used to mask.
    """
    returncode, output = run_config(
        "ascii_snapshot_ordered",
        input_overrides={
            "tree_type": "consistent_trees_ascii",
            "processing_order": "snapshot_ordered",
        },
    )

    assert returncode != 0, "consistent_trees_ascii with snapshot_ordered should fail"
    assert (
        "Reader 'consistent_trees_ascii' is compatible with processing_order 'tree_ordered', "
        "but input.processing_order is 'snapshot_ordered'" in output
    )
    assert "Parameter validation failed" in output


def test_unknown_tree_type_names_both_registries():
    """
    Test that an unknown input.tree_type fails with one message naming both registries.

    Expected: Non-zero exit; exactly one "Unknown tree_type" message, naming both
              src/io/tree/registry.c and src/io/snapshot/registry.c.
    Validates: the two-registry lookup reports a single actionable failure rather
               than one per registry.
    """
    returncode, output = run_config(
        "unknown_tree_type", input_overrides={"tree_type": "not_a_real_reader"}
    )

    assert returncode != 0, "an unknown tree_type should fail at startup"
    assert output.count("Unknown tree_type") == 1, "the failure should be reported exactly once"
    assert "Unknown tree_type 'not_a_real_reader'" in output
    assert "src/io/tree/registry.c" in output
    assert "src/io/snapshot/registry.c" in output


def test_snapshot_tree_name_must_be_exact_literal():
    """
    Test that a snapshot configuration accepts only the exact tree_name literal.

    Expected: Non-zero exit for every other value, with a message naming the accepted literal.
              The accepted-literal control additionally asserts "Unknown tree_type" is absent
              (see the comment above it) -- reaching and exercising the real driver is a
              separate concern, owned by test_snapshot_run_completes_and_writes_output_over_the_fixture.
    Validates: configured text never becomes a printf format or a silent filename mismatch.
    """
    rejected = ["snapshot_%d.h5", "snapshot_%s.h5", "", "trees_063"]
    for index, tree_name in enumerate(rejected):
        returncode, output = run_config(
            f"tree_name_{index}",
            input_overrides={
                "tree_type": "snapshot_hdf5",
                "processing_order": "snapshot_ordered",
                "tree_name": tree_name,
            },
        )
        assert returncode != 0, f"tree_name '{tree_name}' should be rejected"
        assert "input.tree_name" in output, f"the failure should name input.tree_name ({tree_name})"
        assert "Parameter validation failed" in output
        if tree_name:
            assert (
                f"accepts input.tree_name only as the exact literal '{SNAPSHOT_TREE_NAME}'"
                in output
            )

    # The accepted literal is the control. An absence-only assertion on "Parameter
    # validation failed" alone cannot distinguish "config accepted" from "config never
    # got that far", so this also asserts "Unknown tree_type" is absent -- ruling out
    # the specific alternative explanation that the literal silently failed reader
    # lookup instead of being genuinely accepted. output_format is forced to hdf5 for
    # the same reason the completing-run test does: the generated reference run file is
    # output_format: binary, which a snapshot-ordered configuration rejects at config
    # time, independent of tree_name.
    #
    # simulation_dir/snapshot_list_file are repointed at the committed fixture so this
    # control cannot start a full production run: now that the driver produces output,
    # leaving them at a snapshot-ordered package's own machine-local dataset would make
    # this config-time control read gigabytes and write a complete run. Whether the
    # driver then aborts (any package whose catalog does not match the fixture) or
    # completes is outside this control's contract -- it asserts config-time acceptance
    # only, and the run is cheap either way.
    returncode, output = run_config(
        "tree_name_accepted",
        input_overrides={
            "tree_type": "snapshot_hdf5",
            "processing_order": "snapshot_ordered",
            "tree_name": SNAPSHOT_TREE_NAME,
            **snapshot_fixture_input_overrides(),
        },
        # snapshot_list must name an index the fixture's own 6-entry scale-factor
        # list contains: the generated reference run file requests a snapshot valid
        # only for the selected package's production list, and an out-of-range
        # request is itself a config-time rejection, which would mask the one this
        # control is looking for.
        output_overrides={"output_format": "hdf5", "snapshot_list": [1]},
    )
    assert "Parameter validation failed" not in output
    assert "Unknown tree_type" not in output, "the accepted literal must resolve the reader"


def test_multiplier_default_and_non_positive_rejection():
    """
    Test the identity multiplier's default and its non-positive rejection.

    Expected: the default value runs a tree-ordered configuration to completion;
              0 and a negative value fail at config time with a "must be positive" message.
    Validates: simulation.unique_galaxy_id_multiplier parses, defaults to TREE_MUL_FAC,
               and rejects non-positive values.

    The returncode == 0 assertions below run the selected package's own committed
    configuration to completion, and the multiplier is then read back out of BINARY
    galaxy output (_run_and_read_unique_ids). A snapshot-ordered package rejects
    output_format: binary at config time, and its own dataset is the machine-local
    production conversion rather than the committed fixture, so neither the run nor
    the read-back applies there and the test skips. The snapshot-ordered driver's own
    end-to-end behaviour is covered by
    test_snapshot_run_completes_and_writes_output_over_the_fixture.
    """
    if effective_input_setting("multiplier_probe", "processing_order") == "snapshot_ordered":
        raise TestSkipped(
            "selected package is snapshot-ordered; these assertions read binary galaxy "
            "output, which a snapshot-ordered configuration rejects at config time"
        )
    # Absent key: the seeded default is TREE_MUL_FAC, so a tree-ordered run is
    # accepted by the non-default guard and completes normally.
    returncode, output = run_config("multiplier_absent")
    assert returncode == 0, f"a default tree-ordered run should succeed:\n{output}"
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


def _skip_unless_selected_package_is_tree_ordered(probe_name):
    """Skip tests that read binary galaxy output when the selected package forbids it."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    if effective_input_setting(probe_name, "processing_order") == "snapshot_ordered":
        raise TestSkipped(
            "selected package is snapshot-ordered; these assertions read binary galaxy "
            "output, which a snapshot-ordered configuration rejects at config time"
        )


def _run_and_read_unique_ids(name, **kwargs):
    """
    Run a tree-ordered configuration to completion and return its UniqueGalaxyID list.

    The effective identity multiplier is only observable in what the encoder actually
    wrote, so the multiplier tests below read the ids back out of the binary galaxy
    output rather than trusting a log line or a config-time message.
    """
    param_file = make_param_file(name, output_overrides={"output_format": "binary"}, **kwargs)
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, (
        f"tree-ordered run '{name}' should succeed (rc={returncode})\n"
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


def test_tree_ordered_accepts_non_default_multiplier():
    """
    Test that a tree-ordered run honours a non-default identity multiplier end to end.

    Expected: exit 0, and the run's ids decompose under 10^10 into exactly the
              (halonr, forestnr_global) component pairs the same run produces under the
              default 10^9 multiplier.
    Validates: a tree-ordered configuration declaring 10^10 passes config validation, runs,
               and produces ids encoded with 10^10. Comparing decomposed COMPONENTS rather
               than raw ids is what makes this falsifiable: an encoder still hard-coded to
               TREE_MUL_FAC would emit the default run's ids, which decompose under 10^10
               to forest index -1 and cannot match. The min-id assertion catches the same
               failure independently.
    """
    _skip_unless_selected_package_is_tree_ordered("multiplier_accept_probe")

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
    _skip_unless_selected_package_is_tree_ordered("multiplier_precedence_probe")

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
            test_snapshot_run_completes_and_writes_output_over_the_fixture,
            test_snapshot_binary_output_rejected_at_config_time,
            test_snapshot_skip_rejected_at_config_time,
            test_snapshot_reader_rejects_tree_ordered,
            test_snapshot_reader_unset_processing_order_names_the_default,
            test_tree_reader_rejects_snapshot_ordered,
            test_unknown_tree_type_names_both_registries,
            test_snapshot_tree_name_must_be_exact_literal,
            test_multiplier_default_and_non_positive_rejection,
            test_tree_ordered_accepts_non_default_multiplier,
            test_multiplier_precedence_across_both_parser_passes,
        ]
        return run_test_suite(tests, "Processing Order Validation")
    finally:
        shutil.rmtree(TEMP_DIR)


if __name__ == "__main__":
    sys.exit(main())
