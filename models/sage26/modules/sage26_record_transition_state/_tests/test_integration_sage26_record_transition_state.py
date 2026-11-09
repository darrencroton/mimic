#!/usr/bin/env python3
"""
sage26_record_transition_state - Integration Test

Validates: the module's placement, and on a real run over consecutive snapshots the
progenitor-joining transitions it hands to sage26_initialise_merger_clock and the
infall outputs it writes.

SAGE26 decides a galaxy's infall values from its Type before progenitor joining: a Type 0
that becomes a Type 1 records its previous Mvir, Vvir and Vmax, its StellarMass and the
descendant's snapshot number as TimeOfInfall; a Type 1 that stays one keeps them. SAGE26's
writer stores infallMvir, infallVvir, infallVmax, infallStellarMass and TimeOfInfall for
Type != 0 and 0 for a Type 0.

Galaxies are followed by UniqueGalaxyID between consecutive written snapshots. The
package's run-file pipeline makes and consumes orphans (Type 2) within one snapshot,
so its written rows hold the transitions 0 -> 0, 0 -> 1, 1 -> 1 and 1 -> 0. The same
pipeline without its merger pass keeps orphans, so its written rows also hold the
orphan transitions 0 -> 2, 1 -> 2 and 2 -> 2. MergTime is not written; the clocks
are asserted by the merger clock's unit tests.

Test cases:
  - test_module_loads: the module initialises as the last post_timestep entry
  - test_placement_rejected: a run with the module in pre_timestep fails at startup
  - test_consecutive_snapshot_transitions: the package pipeline (snapshots 30 to 63);
    each written transition carries SAGE26's infall values, every Type 0 row 0
  - test_orphan_transitions_without_merger_pass: the pipeline without its merger pass
    (snapshots 0 to 63); the orphan transitions too, with TimeOfInfall's rule
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


def _check_transitions(snapshots, first, last):
    """
    Follow galaxies between consecutive written snapshots and check each transition

    Every Type 0 row is written with the five infall values 0. For a galaxy followed
    from snapshot s to s + 1 (by UniqueGalaxyID):
      0 -> 1: TimeOfInfall = s + 1; infallStellarMass, infallMvir and infallVmax are
              the previous StellarMass, Mvir and Vmax as written; infallVvir is the
              previous Vvir where the previous halo grew (deltaMvir > 0), since the
              core stores Vvir only when a halo grows, as
              SAGE26 does, while it writes the current halo's Vvir for a Type 0 or 1 row
      1 -> 1, 1 -> 2, 2 -> 2: all five kept
      0 -> 2: infallStellarMass, infallMvir and infallVmax as for 0 -> 1 (infallVvir
              where the halo grew), and TimeOfInfall kept from the galaxy's last
              earlier infall, or -1 if it never had one
              (SAGE26 writes no TimeOfInfall there); checked only when the run writes every
              snapshot from 0, so that each galaxy's whole history is known
      1 -> 0: all five written as 0

    Returns the count of each transition and of the rows behind each conditional check.
    """
    counts = {}
    checked = {"0->1 infallVvir": 0, "0->2 infallVvir": 0, "0->2 earlier TimeOfInfall": 0}
    full_history = first == 0
    last_infall = {}  # UniqueGalaxyID -> TimeOfInfall of its latest Type != 0 row

    def note_history(gals):
        sats = gals[gals["Type"] != 0]
        for gid, infall_time in zip(
            sats["UniqueGalaxyID"].tolist(), sats["TimeOfInfall"].tolist(), strict=True
        ):
            last_infall[gid] = infall_time

    for snap in range(first, last + 1):
        centrals = snapshots[snap][snapshots[snap]["Type"] == 0]
        for field in INFALL_FIELDS:
            assert np.all(centrals[field] == 0), f"Snap {snap}: Type 0 rows write {field} = 0"

    note_history(snapshots[first])
    for snap in range(first, last):
        before, after = snapshots[snap], snapshots[snap + 1]
        _, i0, i1 = np.intersect1d(
            before["UniqueGalaxyID"], after["UniqueGalaxyID"], return_indices=True
        )
        prev, new = before[i0], after[i1]
        at = f"at Snap {snap + 1}"

        for old_type in (0, 1, 2):
            for new_type in (0, 1, 2):
                case = f"{old_type}->{new_type}"
                counts[case] = counts.get(case, 0) + int(
                    np.sum((prev["Type"] == old_type) & (new["Type"] == new_type))
                )

        for new_type in (1, 2):
            infall = (prev["Type"] == 0) & (new["Type"] == new_type)
            p, n = prev[infall], new[infall]
            case = f"0->{new_type}"
            assert np.array_equal(
                n["infallStellarMass"], p["StellarMass"]
            ), f"{case} {at}: infallStellarMass should be the previous StellarMass"
            assert np.array_equal(
                n["infallMvir"], p["Mvir"]
            ), f"{case} {at}: infallMvir should be the previous Mvir"
            assert np.array_equal(
                n["infallVmax"], p["Vmax"].astype(n["infallVmax"].dtype)
            ), f"{case} {at}: infallVmax should be the previous Vmax"
            grew = p["deltaMvir"] > 0
            assert np.array_equal(
                n["infallVvir"][grew], p["Vvir"][grew]
            ), f"{case} {at}: infallVvir should be the previous Vvir where the halo grew"
            checked[f"{case} infallVvir"] += int(np.sum(grew))
            if new_type == 1:
                assert np.all(
                    n["TimeOfInfall"] == np.float32(snap + 1)
                ), f"0->1 {at}: TimeOfInfall should be the snapshot number"
            elif full_history:
                expected = np.array(
                    [last_infall.get(gid, -1.0) for gid in n["UniqueGalaxyID"].tolist()],
                    dtype=n["TimeOfInfall"].dtype,
                )
                assert np.array_equal(
                    n["TimeOfInfall"], expected
                ), f"0->2 {at}: TimeOfInfall should be kept from the last earlier infall"
                checked["0->2 earlier TimeOfInfall"] += int(np.sum(expected >= 0))

        for case, (old_type, new_type) in {"1->1": (1, 1), "1->2": (1, 2), "2->2": (2, 2)}.items():
            kept = (prev["Type"] == old_type) & (new["Type"] == new_type)
            for field in INFALL_FIELDS:
                assert np.array_equal(
                    new[kept][field], prev[kept][field]
                ), f"{case} {at}: {field} should be kept"

        promoted = (prev["Type"] == 1) & (new["Type"] == 0)
        for field in INFALL_FIELDS:
            assert np.all(new[promoted][field] == 0), f"1->0 {at}: {field} should be written 0"

        note_history(after)
    return counts, checked


def _run_and_load(output_name, phase_config, params, first):
    """Run the given pipeline writing every snapshot from first to 63, and load them."""
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name=output_name,
        phase_config=phase_config,
        model_params=params,
        output_format="hdf5",
    )
    try:
        _write_snapshots(param_file, range(first, LAST_SNAPSHOT + 1))
        returncode, stdout, stderr = run_mimic(param_file)
        assert returncode == 0, f"Mimic should run the pipeline\nStderr: {stderr}"
        snapshots = _load_snapshots(output_dir)
        assert sorted(snapshots) == list(
            range(first, LAST_SNAPSHOT + 1)
        ), f"every snapshot should be written, got {sorted(snapshots)}"
    except BaseException:
        shutil.rmtree(temp_dir)  # the caller removes it only on success
        raise
    return snapshots, temp_dir


def test_consecutive_snapshot_transitions():
    """
    The package pipeline: each written transition carries SAGE26's infall values

    With the merger pass, orphans are made and consumed within one snapshot, so the
    written rows hold 0 -> 0, 0 -> 1, 1 -> 1 and 1 -> 0 (checked as _check_transitions
    describes); every one must occur.
    """
    print("Testing infall values across consecutive snapshots (package pipeline)...")

    phase_config, params = _run_file_pipeline()
    snapshots, temp_dir = _run_and_load(
        "sage26_record_transition_state_snapshots", phase_config, params, FIRST_SNAPSHOT
    )
    try:
        counts, checked = _check_transitions(snapshots, FIRST_SNAPSHOT, LAST_SNAPSHOT)
        for case in ("0->0", "0->1", "1->1", "1->0"):
            assert counts[case] > 0, f"the run should contain the transition {case}"
        assert checked["0->1 infallVvir"] > 0, "some 0->1 halos should have grown"
        print(f"  transitions followed: {counts}")
        print(f"  conditional checks applied to: {checked}")
    finally:
        shutil.rmtree(temp_dir)
    print("  ✓ Every written transition carries SAGE26's infall values")


def test_orphan_transitions_without_merger_pass():
    """
    Without the merger pass, orphans persist and are written: each orphan transition

    The package pipeline without its satellite_mergers phase (the merger and
    disruption pass and its event consumers; no other module requires them) writes
    every snapshot from 0 to 63. Orphans are then never consumed, so the written rows
    hold 0 -> 2 (non-main branch), 1 -> 2 and 2 -> 2 as well, checked as
    _check_transitions describes, TimeOfInfall's rule included: kept on 0 -> 2 from
    an earlier infall, 0 on Type 0 rows.
    """
    print("Testing orphan transitions without the merger pass...")

    phase_config, params = _run_file_pipeline()
    phase_config = {
        name: mods for name, mods in phase_config.items() if name != "satellite_mergers"
    }
    for mods in phase_config.values():
        assert all(
            name != "sage26_resolve_mergers_and_disruption" for name, _ in mods
        ), "the merger pass should be left out"
    snapshots, temp_dir = _run_and_load(
        "sage26_record_transition_state_orphans", phase_config, params, 0
    )
    try:
        counts, checked = _check_transitions(snapshots, 0, LAST_SNAPSHOT)
        for case in ("0->1", "1->1", "1->0", "0->2", "1->2", "2->2"):
            assert counts[case] > 0, f"the run should contain the transition {case}"
        assert counts["2->0"] == 0 and counts["2->1"] == 0, "an orphan never regains a subhalo"
        assert (
            checked["0->2 earlier TimeOfInfall"] > 0
        ), "some 0->2 galaxy should carry an earlier infall's TimeOfInfall"
        written_type2 = sum(int(np.sum(g["Type"] == 2)) for g in snapshots.values())
        print(f"  transitions followed: {counts}; Type 2 rows written: {written_type2}")
        print(f"  conditional checks applied to: {checked}")
    finally:
        shutil.rmtree(temp_dir)
    print("  ✓ Every orphan transition carries SAGE26's infall values")


def main():
    """Run all integration test cases with structured result markers."""
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: sage26_record_transition_state Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    tests = [
        test_module_loads,
        test_placement_rejected,
        test_consecutive_snapshot_transitions,
        test_orphan_transitions_without_merger_pass,
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
