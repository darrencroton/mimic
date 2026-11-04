#!/usr/bin/env python3
"""
sage26_record_transition_state - Integration Test

Validates: the module's placement, and on a real run over consecutive snapshots the
progenitor-joining transitions it hands to sage26_initialise_merger_clock and the
infall outputs it writes.

SAGE26 decides a galaxy's infall values from its Type before progenitor joining
(core_build_model.c:240-338): a Type 0 that becomes a Type 1 records its previous
Mvir, Vvir and Vmax, its StellarMass and the descendant's snapshot number as
TimeOfInfall (:306-313); a Type 1 that stays one keeps them. SAGE26's writer stores
infallMvir, infallVvir, infallVmax, infallStellarMass and TimeOfInfall for Type != 0
and 0 for a Type 0 (io/save_gals_hdf5.c:1004-1016).

The run uses the package's run-file pipeline and writes every snapshot from 30 to 63,
and galaxies are followed by UniqueGalaxyID between consecutive snapshots. Orphans
(Type 2) are made and consumed by the merger pass within one snapshot, so the
written rows hold the transitions 0 -> 0, 0 -> 1, 1 -> 1 and 1 -> 0; the orphan
transitions are covered by the module's unit test through the core's inheritance.

Test cases:
  - test_module_loads: the module initialises as the last post_timestep entry
  - test_placement_rejected: a run with the module in pre_timestep fails at startup
  - test_consecutive_snapshot_transitions: each written transition case carries
    SAGE26's infall values, and every Type 0 row is written with 0
"""

import shutil
import sys
from pathlib import Path

import numpy as np
import yaml

# Add tests directory to path to import framework
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    BLUE,
    GREEN,
    MIMIC_EXE,
    NC,
    RED,
    TestSkipped,
    create_test_param_file,
    result_error,
    result_fail,
    result_pass,
    result_skip,
    run_mimic,
)

RUN_FILE = REPO_ROOT / "models" / "sage26" / "input" / "sage26_mini-millennium.yaml"
FIRST_SNAPSHOT = 30
LAST_SNAPSHOT = 63
INFALL_FIELDS = ("infallMvir", "infallVvir", "infallVmax", "infallStellarMass", "TimeOfInfall")


def _run_file_pipeline():
    """The package run file's phases and parameters, as create_test_param_file takes them."""
    with open(RUN_FILE, "r") as f:
        modules = yaml.safe_load(f)["modules"]

    def entries(items):
        return [(name, mode) for item in items or [] for name, mode in item.items()]

    phase_config = {"pre_timestep": entries(modules.get("pre_timestep"))}
    for name, items in (modules.get("phases") or {}).items():
        phase_config[name] = entries(items)
    phase_config["post_timestep"] = entries(modules.get("post_timestep"))
    return phase_config, dict(modules.get("parameters") or {})


def _write_snapshots(param_file, snapshots):
    """Set the generated run file's output snapshot list."""
    with open(param_file, "r") as f:
        config = yaml.safe_load(f)
    config["output"]["snapshot_list"] = list(snapshots)
    with open(param_file, "w") as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)


def _load_snapshots(output_dir):
    """Every written snapshot's galaxies, concatenated over the output files."""
    import h5py

    files = sorted(p for p in Path(output_dir).glob("model_*.hdf5"))
    if not files:
        files = sorted(Path(output_dir).glob("*.hdf5"))
    snapshots = {}
    for path in files:
        with h5py.File(path, "r") as f:
            for group in f:
                if not group.startswith("Snap") or "Galaxies" not in f[group]:
                    continue
                snap = int(group[len("Snap") :])
                snapshots.setdefault(snap, []).append(f[group]["Galaxies"][:])
    return {snap: np.concatenate(parts) for snap, parts in snapshots.items()}


def test_module_loads():
    """The module initialises as the last post_timestep entry of the package pipeline."""
    print("Testing module load and initialization...")

    phase_config, params = _run_file_pipeline()
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_record_transition_state_load",
        phase_config=phase_config,
        model_params=params,
        output_format="hdf5",
    )
    try:
        returncode, stdout, stderr = run_mimic(param_file)
        assert returncode == 0, f"Mimic should run the package pipeline\nStderr: {stderr}"
        assert (
            "SAGE record transition state module initialized" in stdout
        ), "sage26_record_transition_state should log its initialisation"
    finally:
        shutil.rmtree(temp_dir)
    print("  ✓ Module loads and initializes successfully")


def test_placement_rejected():
    """A run with the module in pre_timestep fails at startup with the placement error."""
    print("Testing the placement check...")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_record_transition_state_placement",
        phase_config={
            "pre_timestep": [("sage26_record_transition_state", "process_full_halo")],
            "post_timestep": [],
        },
        model_params={},
        output_format="hdf5",
    )
    try:
        returncode, stdout, stderr = run_mimic(param_file)
        combined = stdout + stderr
        assert returncode != 0, "a pre_timestep placement should fail at startup"
        assert "modules.pre_timestep" in combined, "the error should name the placement"
    finally:
        shutil.rmtree(temp_dir)
    print("  ✓ A pre_timestep placement is rejected")


def test_consecutive_snapshot_transitions():
    """
    Each written transition case carries SAGE26's infall values

    Expected, for galaxies followed between snapshots s and s + 1:
      0 -> 1: TimeOfInfall = s + 1, infallStellarMass = StellarMass(s), infallMvir =
              Mvir(s), infallVmax = Vmax(s), all as written
      1 -> 1: all five infall values unchanged
      1 -> 0: all five written as 0
    and every Type 0 row of every snapshot is written with all five 0.
    """
    print("Testing infall values across consecutive snapshots...")

    phase_config, params = _run_file_pipeline()
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_record_transition_state_snapshots",
        phase_config=phase_config,
        model_params=params,
        output_format="hdf5",
    )
    _write_snapshots(param_file, range(FIRST_SNAPSHOT, LAST_SNAPSHOT + 1))
    try:
        returncode, stdout, stderr = run_mimic(param_file)
        assert returncode == 0, f"Mimic should run the package pipeline\nStderr: {stderr}"
        snapshots = _load_snapshots(output_dir)
        assert sorted(snapshots) == list(
            range(FIRST_SNAPSHOT, LAST_SNAPSHOT + 1)
        ), f"every snapshot should be written, got {sorted(snapshots)}"

        counts = {"0->0": 0, "0->1": 0, "1->1": 0, "1->0": 0}
        type0_rows = 0
        for snap, gals in snapshots.items():
            centrals = gals[gals["Type"] == 0]
            type0_rows += len(centrals)
            for field in INFALL_FIELDS:
                assert np.all(centrals[field] == 0), f"Snap {snap}: Type 0 rows write {field} = 0"

        for snap in range(FIRST_SNAPSHOT, LAST_SNAPSHOT):
            before, after = snapshots[snap], snapshots[snap + 1]
            common, i0, i1 = np.intersect1d(
                before["UniqueGalaxyID"], after["UniqueGalaxyID"], return_indices=True
            )
            prev, new = before[i0], after[i1]

            stay0 = (prev["Type"] == 0) & (new["Type"] == 0)
            counts["0->0"] += int(np.sum(stay0))

            infall = (prev["Type"] == 0) & (new["Type"] == 1)
            counts["0->1"] += int(np.sum(infall))
            p, n = prev[infall], new[infall]
            assert np.all(
                n["TimeOfInfall"] == np.float32(snap + 1)
            ), f"0 -> 1 at Snap {snap + 1}: TimeOfInfall should be the snapshot number"
            assert np.array_equal(
                n["infallStellarMass"], p["StellarMass"]
            ), f"0 -> 1 at Snap {snap + 1}: infallStellarMass should be the previous StellarMass"
            assert np.array_equal(
                n["infallMvir"], p["Mvir"]
            ), f"0 -> 1 at Snap {snap + 1}: infallMvir should be the previous Mvir"
            assert np.array_equal(
                n["infallVmax"], p["Vmax"].astype(n["infallVmax"].dtype)
            ), f"0 -> 1 at Snap {snap + 1}: infallVmax should be the previous Vmax"

            kept = (prev["Type"] == 1) & (new["Type"] == 1)
            counts["1->1"] += int(np.sum(kept))
            for field in INFALL_FIELDS:
                assert np.array_equal(
                    new[kept][field], prev[kept][field]
                ), f"1 -> 1 at Snap {snap + 1}: {field} should be kept"

            promoted = (prev["Type"] == 1) & (new["Type"] == 0)
            counts["1->0"] += int(np.sum(promoted))
            for field in INFALL_FIELDS:
                assert np.all(
                    new[promoted][field] == 0
                ), f"1 -> 0 at Snap {snap + 1}: {field} should be written as 0"

        for case, count in counts.items():
            assert count > 0, f"the run should contain the transition {case}"
        written_type2 = sum(int(np.sum(g["Type"] == 2)) for g in snapshots.values())
        print(
            f"  transitions followed: {counts}; Type 0 rows written with 0: {type0_rows}; "
            f"Type 2 rows written: {written_type2}"
        )
    finally:
        shutil.rmtree(temp_dir)
    print("  ✓ Every written transition carries SAGE26's infall values")


def main():
    """Run all integration test cases with structured result markers."""
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: sage26_record_transition_state Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    tests = [
        test_module_loads,
        test_placement_rejected,
        test_consecutive_snapshot_transitions,
    ]

    passed = 0
    failed = 0
    skipped = 0

    if not MIMIC_EXE.exists():
        for test in tests:
            result_skip(test.__name__, "Mimic not built")
        return 0

    for test in tests:
        print()
        try:
            test()
            result_pass(test.__name__)
            passed += 1
        except TestSkipped as e:
            result_skip(test.__name__, str(e))
            skipped += 1
        except AssertionError as e:
            result_fail(test.__name__, str(e).splitlines()[0])
            failed += 1
        except Exception as e:
            result_error(test.__name__, str(e).splitlines()[0])
            failed += 1

    print()
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Summary{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"Passed:  {passed}")
    if skipped:
        print(f"Skipped: {skipped}")
    print(f"Failed:  {failed}")
    print(f"Total:   {passed + failed + skipped}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    if failed == 0:
        print(f"{GREEN}✓ All tests passed!{NC}")
        return 0
    else:
        print(f"{RED}✗ {failed} test(s) failed{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
