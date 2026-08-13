#!/usr/bin/env python3
"""
Output Format Integration Test

Validates: Binary and HDF5 output format correctness and baseline comparison

This test validates that Mimic's output system correctly:
- Produces binary output files with correct structure
- Produces HDF5 output files (when compiled with HDF5)
- Output data is loadable and has valid structure
- Output matches baseline reference data
- Both formats contain equivalent halo counts

"""

import json
import shutil
import sys
from pathlib import Path

import numpy as np
import yaml

# Add framework to path
REPO_ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    BASELINE_ATOL_DEFAULT,
    BASELINE_RTOL_DEFAULT,
    GREEN,
    MIMIC_EXE,
    NC,
    RED,
    TEST_DATA_DIR,
    TestSkipped,
    assert_hdf5_schema_layout,
    baseline_rtol,
    compare_halos_comprehensive,
    core_input_file,
    create_test_param_file,
    ensure_output_dirs,
    is_default_baseline_combo,
    load_binary_halos,
    load_hdf5_halos,
    load_hdf5_run_properties,
    resolve_sim_config_path,
    run_mimic,
    run_mimic_fresh,
    run_test_suite,
    skip_non_default_baseline,
)

VALIDATION_MANIFEST_PATH = REPO_ROOT / "tests" / "generated" / "property_ranges.json"

# Ensure output directories exist before any tests run
ensure_output_dirs()


# Cached result of the HDF5 capability probe: the probe is a full Mimic run,
# and five tests consult it, so it runs at most once per process.
_hdf5_support = None


def check_hdf5_support():
    """
    Check if Mimic was compiled with HDF5 support (memoized).

    Returns:
        bool: True if HDF5 support is available
    """
    global _hdf5_support
    if _hdf5_support is None:
        _hdf5_support = _probe_hdf5_support()
    return _hdf5_support


def _probe_hdf5_support():
    # The probe is an actual HDF5-output run: it succeeds only when Mimic was
    # compiled with HDF5 support.
    param_file = core_input_file("test_hdf5.yaml")
    if not param_file.exists():
        return False

    returncode, stdout, stderr = run_mimic(param_file)

    # If it fails with "unknown output format" or similar, HDF5 not supported
    if returncode != 0:
        output = (stdout + stderr).lower()
        if "hdf5" in output and (
            "unknown" in output or "not supported" in output or "not compiled" in output
        ):
            return False

    return returncode == 0


def selected_package_writes_binary():
    """
    Whether the selected simulation package can produce binary output.

    A snapshot-ordered package cannot: the driver rejects any output format but
    HDF5 at configuration time, so the generated ``test_binary.yaml`` is invalid
    by construction for it and every check below would fail on a run that never
    happened. The effective processing order is read exactly as the parser
    resolves it -- an explicit ``input.processing_order`` in the run file wins,
    otherwise the simulation config the run file points at -- so a package is
    identified by its own declaration rather than by name.
    """
    param_file = core_input_file("test_binary.yaml")
    with open(param_file) as handle:
        config = yaml.safe_load(handle)

    order = (config.get("input") or {}).get("processing_order")
    if order is None:
        sim_config_path = resolve_sim_config_path(config["simulation"]["config"], param_file)
        with open(sim_config_path) as handle:
            sim_config = yaml.safe_load(handle)
        order = ((sim_config or {}).get("input") or {}).get("processing_order")

    return order != "snapshot_ordered"


def first_requested_output_snapshot(param_file):
    """The first snapshot ``output.snapshot_list`` in ``param_file`` asks for.

    A snapshot-ordered run writes one partition file per requested output
    snapshot, named by that snapshot's number, so the filename cannot be derived
    from a fixed partition index. Reading the request from the run file keeps this
    correct for any list configuration validation admits, including an unsorted
    one.
    """
    with open(param_file) as handle:
        config = yaml.safe_load(handle)

    requested = (config.get("output") or {}).get("snapshot_list")
    if not requested:
        raise TestSkipped(f"{param_file} declares no output.snapshot_list to read output from")
    return int(requested[0])


def baseline_halo_properties():
    """
    Return current run halo-output fields for baseline comparison.

    The committed baseline intentionally covers deterministic halo tracking and
    selected simulation/catalog fields. Model-owned galaxy physics fields are
    excluded so SAGE and SHAM can share the same physics-free baseline.
    """
    with open(VALIDATION_MANIFEST_PATH) as f:
        manifest = json.load(f)
    return {
        name
        for name, spec in manifest.get("properties", {}).items()
        if spec.get("category") == "halo"
    }


def test_binary_format_execution():
    """
    Test that Mimic runs successfully with binary output format

    What: Executes Mimic with test_binary.yaml (binary output format)
    Expected: Zero exit code, no crashes
    Validates: Binary format end-to-end execution
    """
    print("Testing binary format execution...")

    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    param_file = core_input_file("test_binary.yaml")
    assert param_file.exists(), f"Parameter file not found: {param_file}"

    # Run Mimic
    returncode, _stdout, stderr = run_mimic(param_file)

    # Check execution success
    assert returncode == 0, f"Mimic failed with code {returncode}\nSTDERR: {stderr}"

    print(f"  ✓ Binary format execution successful")


def test_binary_format_loading():
    """
    Test that binary output file can be loaded and parsed

    What: Loads model_z0.000_0 using load_binary_halos() framework function
    Expected: File exists, halos array is populated, metadata is valid
    Validates: Binary format structure is readable by analysis tools
    """
    print("Testing binary format data loading...")

    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    # Check output file exists
    output_dir = TEST_DATA_DIR / "output" / "binary"
    output_file = output_dir / "model_z0.000_0"  # snapshot 63 is z=0

    # Always regenerate output for the selected model so a stale file cannot
    # satisfy this assertion.
    param_file = core_input_file("test_binary.yaml")
    run_mimic_fresh(param_file, output_file)

    assert output_file.exists(), f"Binary output file not created: {output_file}"

    # Load halos
    print(f"  Loading: {output_file.relative_to(REPO_ROOT)}")
    halos, metadata = load_binary_halos(output_file)

    # Validate loaded data
    assert metadata["TotHalos"] > 0, "No halos loaded from binary file"
    assert len(halos) == metadata["TotHalos"], "Halo count mismatch"
    assert hasattr(halos, "Mvir"), "Binary data missing expected property (Mvir)"
    assert hasattr(halos, "Rvir"), "Binary data missing expected property (Rvir)"

    print(f"  ✓ Loaded {metadata['TotHalos']} halos from CURRENT binary output")
    print(
        f"    Trees: {metadata.get('Ntrees', 'N/A')}, File size: {output_file.stat().st_size:,} bytes"
    )


def test_binary_baseline_comparison():
    """
    Test that current binary output matches committed halo-property baseline

    What: Compares tests/data/output/binary/model_z0.000_0 (current test run)
          against tests/data/output/baseline/binary/model_z0.000_0 (committed baseline)

    Comparison: All generated halo-output properties for ALL halos
                (core tracking plus selected simulation/catalog properties).
                Model galaxy properties (ColdGas, StellarMass, etc.) are NOT compared.

    Tolerance: 1e-6 relative for floats, exact for integers

    Expected: All core properties match exactly (within tolerance)

    Validates: Core halo tracking is deterministic and hasn't regressed

    Schema: Each binary output carries metadata/output_schema.json written by the
            Mimic run that produced it, so the loader always uses the correct dtype
            regardless of whether the property set has evolved since the baseline was
            committed.

    Note: If this test fails after a deliberate core change, regenerate baseline:
          cp tests/data/output/binary/model_z0.000_0 tests/data/output/baseline/binary/
          cp tests/data/output/binary/metadata/output_schema.json \\
             tests/data/output/baseline/binary/metadata/
    """
    print("Testing binary baseline comparison...")

    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    if not is_default_baseline_combo():
        skip_non_default_baseline()

    # Load current test output
    output_dir = TEST_DATA_DIR / "output" / "binary"
    output_file = output_dir / "model_z0.000_0"

    # Always regenerate output for the selected model so a stale file -- possibly
    # from a different MODEL writing the same path -- cannot be compared against
    # the baseline as if it were this run.
    param_file = core_input_file("test_binary.yaml")
    run_mimic_fresh(param_file, output_file)

    print(f"  Loading CURRENT: {output_file.relative_to(REPO_ROOT)}")
    halos_test, metadata_test = load_binary_halos(output_file)
    print(f"    → {metadata_test['TotHalos']} halos, {metadata_test.get('Ntrees', 'N/A')} trees")

    # Load committed baseline
    baseline_dir = TEST_DATA_DIR / "output" / "baseline" / "binary"
    baseline_file = baseline_dir / "model_z0.000_0"

    assert baseline_file.exists(), (
        f"{RED}Baseline file not found: {baseline_file}\n"
        f"Run Mimic once to establish baseline, then commit the baseline file.{NC}"
    )

    print(f"  Loading BASELINE: {baseline_file.relative_to(REPO_ROOT)}")
    halos_baseline, metadata_baseline = load_binary_halos(baseline_file)
    print(
        f"    → {metadata_baseline['TotHalos']} halos, {metadata_baseline.get('Ntrees', 'N/A')} trees"
    )

    # Compare halo counts
    assert metadata_test["TotHalos"] == metadata_baseline["TotHalos"], (
        f"{RED}Halo count mismatch: test={metadata_test['TotHalos']}, "
        f"baseline={metadata_baseline['TotHalos']}{NC}"
    )

    # Compare tree counts
    if "Ntrees" in metadata_test and "Ntrees" in metadata_baseline:
        assert metadata_test["Ntrees"] == metadata_baseline["Ntrees"], (
            f"{RED}Tree count mismatch: test={metadata_test['Ntrees']}, "
            f"baseline={metadata_baseline['Ntrees']}{NC}"
        )

    compare_properties = baseline_halo_properties()

    # Comprehensive comparison of generated halo properties for all halos.
    # Model-owned galaxy properties are deliberately excluded.
    print(f"  Comparing {len(compare_properties)} generated halo properties for all halos...")
    passed, report = compare_halos_comprehensive(
        halos_test,
        halos_baseline,
        label1="test",
        label2="baseline",
        rtol=baseline_rtol(),
        properties_to_compare=compare_properties,
        warn_rtol=BASELINE_RTOL_DEFAULT,
        atol=BASELINE_ATOL_DEFAULT,
    )

    # Print report
    print(report, end="")  # Remove blank line after report

    # Assert that comparison passed
    assert passed, (
        f"{RED}Binary output does not match baseline.\n"
        f"In physics-free mode, all generated halo properties should be identical.\n"
        f"See detailed comparison report above.{NC}"
    )

    print(f"{GREEN}  ✓ Binary output matches baseline - all core properties validated{NC}")


def test_hdf5_format_execution():
    """
    Test that Mimic runs successfully with HDF5 output format

    What: Executes Mimic with test_hdf5.yaml (HDF5 output format)
    Expected: Zero exit code, no crashes (or skips if HDF5 not compiled)
    Validates: HDF5 format end-to-end execution
    """
    print("Testing HDF5 format execution...")

    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    # Check if HDF5 parameter file exists
    param_file = core_input_file("test_hdf5.yaml")
    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Check if HDF5 support is available
    if returncode != 0:
        output = stdout + stderr
        if "requires HDF5" in output or "HDF5 support" in output or "Recompile with" in output:
            raise TestSkipped("HDF5 not compiled")
        else:
            # Execution failed for a different reason
            assert False, f"Mimic failed with code {returncode}\nSTDERR: {stderr}"

    print(f"  ✓ HDF5 format execution successful")


def test_hdf5_format_loading():
    """
    Test that HDF5 output file can be loaded and parsed

    What: Loads the selected package's HDF5 output partition file (filenr 0 on
          a tree-ordered package, the first requested output snapshot on a
          snapshot-ordered one) using load_hdf5_halos() function
    Expected: File exists, halos array is populated, metadata is valid
    Validates: HDF5 format structure is readable by analysis tools
    Requires: h5py library (skips if not available)
    """
    print("Testing HDF5 format data loading...")

    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    # Check if HDF5 is supported
    if not check_hdf5_support():
        raise TestSkipped("HDF5 not compiled")

    # Check output file exists
    param_file = core_input_file("test_hdf5.yaml")
    output_dir = TEST_DATA_DIR / "output" / "hdf5"
    if selected_package_writes_binary():
        # Tree-ordered partition files are named by filenr (forests_per_file
        # chunking), independent of which output snapshots are requested;
        # filenr 0 is always the first partition.
        output_file = output_dir / "model_000.hdf5"
    else:
        # A snapshot-ordered run names each partition file after the output
        # snapshot it holds, so the file to read comes from the run file's own
        # request rather than from a fixed partition index.
        snapnum = first_requested_output_snapshot(param_file)
        output_file = output_dir / f"model_{snapnum:03d}.hdf5"

    # Always regenerate output for the selected model so a stale file cannot
    # satisfy this assertion.
    run_mimic_fresh(param_file, output_file)

    assert output_file.exists(), f"HDF5 output file not created: {output_file}"

    # Check if h5py is available
    try:
        import h5py  # noqa: F401 - import used only for availability check
    except ImportError:
        raise TestSkipped("h5py not available")

    # Load halos
    print(f"  Loading: {output_file.relative_to(REPO_ROOT)}")
    halos, metadata = load_hdf5_halos(output_file)

    # Validate loaded data
    assert metadata["TotHalos"] > 0, "No halos loaded from HDF5 file"
    assert len(halos) == metadata["TotHalos"], "Halo count mismatch"
    assert_hdf5_schema_layout(output_file, expected_format_version="1.2")

    master_file = output_dir / "model.hdf5"
    assert master_file.exists(), f"HDF5 master file not created: {master_file}"
    assert_hdf5_schema_layout(master_file, expected_format_version="1.2")

    print(f"  ✓ Loaded {metadata['TotHalos']} halos from CURRENT HDF5 output")
    print(f"    File size: {output_file.stat().st_size:,} bytes")


def test_hdf5_timestep_run_properties():
    """Check master-file timestep provenance for fixed and dynamic schemes."""
    print("Testing HDF5 timestep run properties...")

    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    if not check_hdf5_support():
        raise TestSkipped("HDF5 not compiled")

    try:
        import h5py  # noqa: F401 - import used only for availability check
    except ImportError:
        raise TestSkipped("h5py not available")

    for scheme, substeps, max_dynamic_substeps in (
        ("fixed", 7, 123),
        ("dynamic", 11, 321),
    ):
        param_file, output_dir, temp_dir = create_test_param_file(
            f"hdf5_timestep_{scheme}",
            output_format="hdf5",
            substeps=substeps,
            timestep_scheme=scheme,
            max_dynamic_substeps=max_dynamic_substeps,
        )
        try:
            master_file = output_dir / "model.hdf5"

            run_mimic_fresh(param_file, master_file)
            assert master_file.exists(), f"HDF5 master file not created: {master_file}"

            run_properties = load_hdf5_run_properties(master_file)
            assert run_properties["SubSteps"] == substeps, (
                f"RunProperties/SubSteps mismatch for {scheme}: "
                f"expected {substeps}, got {run_properties['SubSteps']}"
            )
            assert run_properties["TimestepScheme"] == scheme, (
                f"RunProperties/TimestepScheme mismatch: expected {scheme}, "
                f"got {run_properties['TimestepScheme']}"
            )
            assert run_properties["MaxDynamicSubsteps"] == max_dynamic_substeps, (
                f"RunProperties/MaxDynamicSubsteps mismatch for {scheme}: "
                f"expected {max_dynamic_substeps}, got {run_properties['MaxDynamicSubsteps']}"
            )
        finally:
            shutil.rmtree(temp_dir)

    print("  ✓ HDF5 timestep run properties match fixed and dynamic run configs")


def test_hdf5_compression_equivalence():
    """
    Test that --compress produces readable gzip HDF5 output with unchanged values.
    """
    print("Testing HDF5 compression equivalence...")

    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    if not check_hdf5_support():
        raise TestSkipped("HDF5 not compiled")

    try:
        import h5py
    except ImportError:
        raise TestSkipped("h5py not available")

    param_file = core_input_file("test_hdf5.yaml")
    output_dir = TEST_DATA_DIR / "output" / "hdf5"
    if selected_package_writes_binary():
        # Tree-ordered partition files are named by filenr (forests_per_file
        # chunking), independent of which output snapshots are requested;
        # filenr 0 is always the first partition.
        output_file = output_dir / "model_000.hdf5"
    else:
        # A snapshot-ordered run names each partition file after the output
        # snapshot it holds, so the file to read comes from the run file's own
        # request rather than from a fixed partition index.
        snapnum = first_requested_output_snapshot(param_file)
        output_file = output_dir / f"model_{snapnum:03d}.hdf5"

    run_mimic_fresh(param_file, output_file)
    halos_uncompressed, metadata_uncompressed = load_hdf5_halos(output_file)
    with h5py.File(output_file, "r") as f:
        snap_name = metadata_uncompressed["SnapshotName"]
        assert f[snap_name]["Galaxies"].compression is None, "Default HDF5 output is compressed"

    run_mimic_fresh(param_file, output_file, extra_args=["--compress"])
    assert_hdf5_schema_layout(output_file, expected_format_version="1.2")
    halos_compressed, metadata_compressed = load_hdf5_halos(output_file)
    with h5py.File(output_file, "r") as f:
        snap_name = metadata_compressed["SnapshotName"]
        assert (
            f[snap_name]["Galaxies"].compression == "gzip"
        ), "--compress did not gzip-compress the Galaxies table"

    assert (
        metadata_uncompressed["TotHalos"] == metadata_compressed["TotHalos"]
    ), "Compressed and uncompressed HDF5 halo counts differ"

    passed, report = compare_halos_comprehensive(
        halos_uncompressed, halos_compressed, label1="uncompressed", label2="compressed", rtol=1e-6
    )
    print(report, end="")
    assert passed, (
        f"{RED}Compressed HDF5 output differs from uncompressed output.\n"
        f"Compression should only change on-disk bytes, not stored values.{NC}"
    )

    print(f"{GREEN}  ✓ Compressed HDF5 output is value-equivalent and gzip-filtered{NC}")


def test_hdf5_baseline_comparison():
    """
    Test that current HDF5 output matches committed halo-property baseline

    What: Compares tests/data/output/hdf5/model_000.hdf5 (current test run)
          against tests/data/output/baseline/hdf5/model_000.hdf5 (committed baseline)

    Comparison: All generated halo-output properties for ALL halos
                (core tracking plus selected simulation/catalog properties).
                Model galaxy properties (ColdGas, StellarMass, etc.) are NOT compared.

    Tolerance: 1e-6 relative for floats, exact for integers

    Expected: All core properties match exactly (within tolerance)

    Validates: Core halo tracking is deterministic and hasn't regressed

    Requires: h5py library (skips if not available)

    Note: If this test fails after a deliberate core change, regenerate baseline with:
          cp tests/data/output/hdf5/model_000.hdf5 tests/data/output/baseline/hdf5/
    """
    print("Testing HDF5 baseline comparison...")

    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    if not is_default_baseline_combo():
        skip_non_default_baseline()

    # Check if HDF5 is supported
    if not check_hdf5_support():
        raise TestSkipped("HDF5 not compiled")

    # Check if h5py is available
    try:
        import h5py  # noqa: F401 - import used only for availability check
    except ImportError:
        raise TestSkipped("h5py not available")

    # Load current test output
    output_dir = TEST_DATA_DIR / "output" / "hdf5"
    output_file = output_dir / "model_000.hdf5"

    # Always regenerate output for the selected model so a stale file -- possibly
    # from a different MODEL writing the same path -- cannot be compared against
    # the baseline as if it were this run.
    param_file = core_input_file("test_hdf5.yaml")
    run_mimic_fresh(param_file, output_file)

    print(f"  Loading CURRENT: {output_file.relative_to(REPO_ROOT)}")
    halos_test, metadata_test = load_hdf5_halos(output_file)
    print(f"    → {metadata_test['TotHalos']} halos")

    # Load committed baseline
    baseline_dir = TEST_DATA_DIR / "output" / "baseline" / "hdf5"
    baseline_file = baseline_dir / "model_000.hdf5"
    baseline_master_file = baseline_dir / "model.hdf5"

    assert baseline_file.exists(), (
        f"{RED}Baseline file not found: {baseline_file}\n"
        f"Run Mimic once with HDF5 to establish baseline, then commit the baseline file.{NC}"
    )
    assert baseline_master_file.exists(), (
        f"{RED}Baseline master file not found: {baseline_master_file}\n"
        f"Run Mimic once with HDF5 to establish baseline, then commit the baseline files.{NC}"
    )

    print(f"  Loading BASELINE: {baseline_file.relative_to(REPO_ROOT)}")
    halos_baseline, metadata_baseline = load_hdf5_halos(baseline_file)
    # Tracked baselines under tests/data/ were regenerated by the Spin
    # units-label change, and now carry format version 1.2 like fresh
    # output. Each call site must state the version its own input
    # actually carries, not assume it tracks the current build.
    assert_hdf5_schema_layout(baseline_file, expected_format_version="1.2")
    assert_hdf5_schema_layout(baseline_master_file, expected_format_version="1.2")
    print(f"    → {metadata_baseline['TotHalos']} halos")

    # Compare halo counts
    assert metadata_test["TotHalos"] == metadata_baseline["TotHalos"], (
        f"{RED}Halo count mismatch: test={metadata_test['TotHalos']}, "
        f"baseline={metadata_baseline['TotHalos']}{NC}"
    )

    compare_properties = baseline_halo_properties()

    # Comprehensive comparison of generated halo properties for all halos.
    # Model-owned galaxy properties are deliberately excluded.
    print(f"  Comparing {len(compare_properties)} generated halo properties for all halos...")
    passed, report = compare_halos_comprehensive(
        halos_test,
        halos_baseline,
        label1="test",
        label2="baseline",
        rtol=baseline_rtol(),
        properties_to_compare=compare_properties,
        warn_rtol=BASELINE_RTOL_DEFAULT,
        atol=BASELINE_ATOL_DEFAULT,
    )

    # Print report
    print(report, end="")  # Remove blank line after report

    # Assert that comparison passed
    assert passed, (
        f"{RED}HDF5 output does not match baseline.\n"
        f"In physics-free mode, all generated halo properties should be identical.\n"
        f"See detailed comparison report above.{NC}"
    )

    print(f"{GREEN}  ✓ HDF5 output matches baseline - all core properties validated{NC}")


def test_unique_id_contract():
    """
    Test UniqueGalaxyID/UniqueCentralGalaxyID contract on current HDF5 output.

    Contract:
      - UniqueGalaxyID is unique per snapshot
      - Type 0: UniqueCentralGalaxyID == UniqueGalaxyID
      - Type 1/2: UniqueCentralGalaxyID points to an existing Type 0 central
    """
    print("Testing Unique ID contract...")

    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    if not check_hdf5_support():
        raise TestSkipped("HDF5 not compiled")

    try:
        import h5py  # noqa: F401 - import used only for availability check
    except ImportError:
        raise TestSkipped("h5py not available")

    param_file = core_input_file("test_hdf5.yaml")
    output_dir = TEST_DATA_DIR / "output" / "hdf5"
    if selected_package_writes_binary():
        # Tree-ordered partition files are named by filenr (forests_per_file
        # chunking), independent of which output snapshots are requested;
        # filenr 0 is always the first partition.
        output_file = output_dir / "model_000.hdf5"
    else:
        # A snapshot-ordered run names each partition file after the output
        # snapshot it holds, so the file to read comes from the run file's own
        # request rather than from a fixed partition index.
        snapnum = first_requested_output_snapshot(param_file)
        output_file = output_dir / f"model_{snapnum:03d}.hdf5"

    # Always regenerate output for the selected model so a stale file cannot
    # satisfy this assertion.
    run_mimic_fresh(param_file, output_file)

    halos, metadata = load_hdf5_halos(output_file)
    print(f"  Loaded {metadata['TotHalos']} halos")

    unique_ids = halos.UniqueGalaxyID
    central_ids = halos.UniqueCentralGalaxyID
    types = halos.Type

    # UniqueGalaxyID must be unique within each snapshot.
    n_unique = len(np.unique(unique_ids))
    assert n_unique == len(
        halos
    ), f"UniqueGalaxyID not unique: {n_unique} unique values for {len(halos)} halos"

    id_to_type = {int(uid): int(t) for uid, t in zip(unique_ids, types)}

    type0_mask = types == 0
    assert np.all(
        central_ids[type0_mask] == unique_ids[type0_mask]
    ), "Type 0 halos must satisfy UniqueCentralGalaxyID == UniqueGalaxyID"

    sat_mask = np.isin(types, [1, 2])
    sat_central_ids = central_ids[sat_mask]

    missing = [int(cid) for cid in sat_central_ids if int(cid) not in id_to_type]
    assert not missing, f"{len(missing)} satellites reference missing UniqueCentralGalaxyID targets"

    bad_type = [int(cid) for cid in sat_central_ids if id_to_type[int(cid)] != 0]
    assert (
        not bad_type
    ), f"{len(bad_type)} satellites reference non-Type0 UniqueCentralGalaxyID targets"

    self_refs = np.sum(unique_ids[sat_mask] == sat_central_ids)
    assert self_refs == 0, f"{self_refs} satellites self-reference UniqueCentralGalaxyID"

    print("  ✓ UniqueGalaxyID uniqueness and UniqueCentralGalaxyID host-central mapping validated")


def test_format_equivalence():
    """
    Test that binary and HDF5 formats produce identical output (all properties)

    What: Compares tests/data/output/binary/model_z0.000_0 (binary format)
          against tests/data/output/hdf5/model_000.hdf5 (HDF5 format)
          Both generated in same test run with same modules enabled

    Comparison: ALL properties for ALL halos (core + modules)
                Including: ColdGas, StellarMass, and any other module properties

    Tolerance: 1e-6 relative for floats, exact for integers

    Expected: Perfect agreement - both formats write identical values

    Validates: Format consistency - output format doesn't affect results

    Requires: h5py library (skips if not available)

    Note: This test compares ALL properties because both files are generated
          in the same run, so they should have identical property sets

    Note: Skips on a snapshot-ordered package. Such a package cannot produce
          binary output at all -- the run is rejected at configuration time --
          so there is no binary output for this test to compare against HDF5.
    """
    print("Testing binary vs HDF5 format equivalence...")

    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    if not selected_package_writes_binary():
        raise TestSkipped(
            "Snapshot-ordered packages reject binary output at configuration "
            "time, so binary vs HDF5 format equivalence cannot be compared"
        )

    # Check if HDF5 is supported
    if not check_hdf5_support():
        raise TestSkipped("HDF5 not compiled")

    # Check if h5py is available
    try:
        import h5py  # noqa: F401 - import used only for availability check
    except ImportError:
        raise TestSkipped("h5py not available")

    # Load current binary output
    binary_dir = TEST_DATA_DIR / "output" / "binary"
    binary_file = binary_dir / "model_z0.000_0"

    # Always regenerate output for the selected model so stale files cannot
    # satisfy this comparison.
    binary_param_file = core_input_file("test_binary.yaml")
    run_mimic_fresh(binary_param_file, binary_file)

    print(f"  Loading BINARY: {binary_file.relative_to(REPO_ROOT)}")
    halos_binary, metadata_binary = load_binary_halos(binary_file)
    print(f"    → {metadata_binary['TotHalos']} halos")

    # Load current HDF5 output
    hdf5_dir = TEST_DATA_DIR / "output" / "hdf5"
    hdf5_file = hdf5_dir / "model_000.hdf5"

    # Always regenerate output for the selected model so stale files cannot
    # satisfy this comparison.
    hdf5_param_file = core_input_file("test_hdf5.yaml")
    run_mimic_fresh(hdf5_param_file, hdf5_file)

    print(f"  Loading HDF5: {hdf5_file.relative_to(REPO_ROOT)}")
    halos_hdf5, metadata_hdf5 = load_hdf5_halos(hdf5_file)
    print(f"    → {metadata_hdf5['TotHalos']} halos")

    # Compare halo counts
    assert metadata_binary["TotHalos"] == metadata_hdf5["TotHalos"], (
        f"{RED}Halo count mismatch: binary={metadata_binary['TotHalos']}, "
        f"hdf5={metadata_hdf5['TotHalos']}{NC}"
    )

    print(f"  ✓ Halo count matches: {metadata_binary['TotHalos']} halos in both formats")

    # Comprehensive comparison of all properties for all halos
    print(f"  Comparing all properties for all halos between formats...")
    passed, report = compare_halos_comprehensive(
        halos_binary, halos_hdf5, label1="binary", label2="HDF5", rtol=1e-6
    )

    # Print report
    print(report, end="")  # Remove blank line after report

    # Assert that comparison passed
    assert passed, (
        f"{RED}Binary and HDF5 formats do not produce identical output.\n"
        f"Both formats should write exactly the same property values.\n"
        f"See detailed comparison report above.{NC}"
    )

    print(
        f"{GREEN}  ✓ Binary and HDF5 formats produce identical output - all properties validated{NC}"
    )

    # Print file size comparison (informational)
    print(f"  Binary file size: {binary_file.stat().st_size:,} bytes")
    print(f"  HDF5 file size:   {hdf5_file.stat().st_size:,} bytes")
    size_ratio = hdf5_file.stat().st_size / binary_file.stat().st_size
    print(f"  Size ratio (HDF5/binary): {size_ratio:.2f}x")


def main():
    """Run this file's tests via the shared framework runner."""
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")

    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    return run_test_suite(
        [
            test_binary_format_execution,
            test_binary_format_loading,
            test_binary_baseline_comparison,
            test_hdf5_format_execution,
            test_hdf5_format_loading,
            test_hdf5_timestep_run_properties,
            test_hdf5_compression_equivalence,
            test_hdf5_baseline_comparison,
            test_unique_id_contract,
            test_format_equivalence,
        ],
        "Output Formats (test_output_formats.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
