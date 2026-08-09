#!/usr/bin/env python3
"""
Integration tests for startup validation of the reader/processing-order seam.

Covers input.processing_order, the two-registry input.tree_type resolution, the
snapshot reader's exact tree_name contract, and the
simulation.unique_galaxy_id_multiplier key (parse, default, precedence across
both parser passes, and the tree-ordered rejection of a non-default value).
"""

import shutil
import sys
import tempfile
from pathlib import Path

import yaml

# Add framework to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import MIMIC_EXE, TestSkipped, create_test_param_file, run_mimic, run_test_suite

TEMP_DIR = None

#: Default forest multiplier (TREE_MUL_FAC in src/include/constants.h).
DEFAULT_MULTIPLIER = 1000000000

#: The only input.tree_name the snapshot_hdf5 reader accepts.
SNAPSHOT_TREE_NAME = "snapshot_%03d.h5"

#: Explicit tree-ordered input configuration for tests whose observable is a
#: tree-reader-only config-time check. lhalo_binary is registered in every
#: build, and the config-time rejections under test fire before any input file
#: is opened, so the tree_name value never has to exist on disk.
TREE_ORDERED_OVERRIDES = {
    "tree_type": "lhalo_binary",
    "processing_order": "tree_ordered",
    "tree_name": "trees_063",
}


def make_param_file(name, input_overrides=None, simulation_overrides=None, package_multiplier=None):
    """
    Return a run file path with the given input/simulation overrides applied.

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

    rewritten = Path(TEMP_DIR) / f"{name}.yaml"
    with open(rewritten, "w") as handle:
        yaml.safe_dump(config, handle, default_flow_style=False, sort_keys=False)
    return rewritten


def run_config(name, **kwargs):
    """Run Mimic on a rewritten run file and return (returncode, combined output)."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    param_file = make_param_file(name, **kwargs)
    returncode, stdout, stderr = run_mimic(param_file)
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


def test_snapshot_config_reaches_unimplemented_driver():
    """
    Test that a snapshot reader with snapshot_ordered passes config and stops at the driver.

    Expected: Non-zero exit; output includes "The snapshot-ordered driver is not implemented yet"
              and does NOT include "Parameter validation failed".
    Validates: configuration accepts a snapshot-ordered configuration, leaving
               run_processing_driver() as the single not-implemented point. The absent
               "Parameter validation failed" is what distinguishes the driver rejection
               from the old config-time rejection, whose text was identical.
    """
    returncode, output = run_config(
        "snapshot_driver",
        input_overrides={
            "tree_type": "snapshot_hdf5",
            "processing_order": "snapshot_ordered",
            "tree_name": SNAPSHOT_TREE_NAME,
        },
    )

    assert returncode != 0, "snapshot_ordered should fail until the snapshot driver exists"
    assert "The snapshot-ordered driver is not implemented yet" in output
    assert (
        "Parameter validation failed" not in output
    ), "the snapshot-ordered configuration must pass config validation and fail at the driver"


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

    # The accepted literal is the control: it gets past configuration to the driver.
    returncode, output = run_config(
        "tree_name_accepted",
        input_overrides={
            "tree_type": "snapshot_hdf5",
            "processing_order": "snapshot_ordered",
            "tree_name": SNAPSHOT_TREE_NAME,
        },
    )
    assert "Parameter validation failed" not in output
    assert "The snapshot-ordered driver is not implemented yet" in output


def test_multiplier_default_and_non_positive_rejection():
    """
    Test the identity multiplier's default and its non-positive rejection.

    Expected: the default value runs a tree-ordered configuration to completion;
              0 and a negative value fail at config time with a "must be positive" message.
    Validates: simulation.unique_galaxy_id_multiplier parses, defaults to TREE_MUL_FAC,
               and rejects non-positive values.

    The returncode == 0 assertions below require the selected package's own reader
    to reach a working, implemented driver — true for every tree-ordered package
    today. A snapshot-ordered package (e.g. micro-uchuu-snapshot) would pass config
    validation but fail at run_processing_driver() ("not implemented yet"), a driver
    limitation unrelated to this test's assertions, so the test skips there.
    """
    if effective_input_setting("multiplier_probe", "processing_order") == "snapshot_ordered":
        raise TestSkipped(
            "selected package is snapshot-ordered; the success-path assertions "
            "require an implemented driver"
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


def test_tree_ordered_rejects_non_default_multiplier():
    """
    Test that a tree-ordered configuration rejects a non-default multiplier.

    Expected: Non-zero exit; the message names the configured value and states that the
              tree-ordered identity encoder does not yet honour a configurable multiplier.
    Validates: a value the hard-coded encoder in src/include/galaxy_id.h would ignore is
               refused rather than silently producing ids from TREE_MUL_FAC. The tree-ordered
               configuration is forced explicitly so the rejection (a tree-reader-only check)
               fires regardless of the selected package's own reader family.
    """
    returncode, output = run_config(
        "multiplier_non_default",
        input_overrides=dict(TREE_ORDERED_OVERRIDES),
        simulation_overrides={"unique_galaxy_id_multiplier": 12345},
    )

    assert returncode != 0, "a non-default multiplier must be rejected for tree-ordered runs"
    assert "simulation.unique_galaxy_id_multiplier is 12345" in output
    assert "tree-ordered identity encoder does not yet honour a configurable multiplier" in output
    assert f"TREE_MUL_FAC ({DEFAULT_MULTIPLIER})" in output
    assert "Parameter validation failed" in output


def test_multiplier_precedence_across_both_parser_passes():
    """
    Test both precedence directions for simulation.unique_galaxy_id_multiplier.

    Expected: a value set only in the simulation config survives a run file that omits the
              key (the rejection names 777); a run-file value overrides the package value
              (the rejection names 888 and never 777).
    Validates: the default is seeded once before either parse_simulation_section pass and
               assigned only when the key is present, so the second pass cannot clobber a
               package value. Both directions use the tree-ordered non-default rejection as
               the observable, so the tree-ordered configuration is forced explicitly and the
               test is independent of the selected package's own reader family.
    """
    returncode, output = run_config(
        "multiplier_package_only",
        input_overrides=dict(TREE_ORDERED_OVERRIDES),
        package_multiplier=777,
    )
    assert returncode != 0, "the package multiplier should survive and be rejected"
    assert (
        "simulation.unique_galaxy_id_multiplier is 777" in output
    ), "a simulation_info.yaml value must survive a run file that omits the key"

    returncode, output = run_config(
        "multiplier_run_file_wins",
        input_overrides=dict(TREE_ORDERED_OVERRIDES),
        package_multiplier=777,
        simulation_overrides={"unique_galaxy_id_multiplier": 888},
    )
    assert returncode != 0, "the run-file multiplier should be rejected"
    assert (
        "simulation.unique_galaxy_id_multiplier is 888" in output
    ), "an explicit run-file value must override the package value"
    assert "is 777" not in output, "the package value must not survive an explicit run-file value"


def main():
    global TEMP_DIR
    TEMP_DIR = Path(tempfile.mkdtemp(prefix="mimic_processing_order_"))
    try:
        tests = [
            test_unknown_processing_order_fails_fast,
            test_snapshot_config_reaches_unimplemented_driver,
            test_snapshot_reader_rejects_tree_ordered,
            test_snapshot_reader_unset_processing_order_names_the_default,
            test_tree_reader_rejects_snapshot_ordered,
            test_unknown_tree_type_names_both_registries,
            test_snapshot_tree_name_must_be_exact_literal,
            test_multiplier_default_and_non_positive_rejection,
            test_tree_ordered_rejects_non_default_multiplier,
            test_multiplier_precedence_across_both_parser_passes,
        ]
        return run_test_suite(tests, "Processing Order Validation")
    finally:
        shutil.rmtree(TEMP_DIR)


if __name__ == "__main__":
    sys.exit(main())
