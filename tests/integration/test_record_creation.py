#!/usr/bin/env python3
"""
Record Creation Integration Tests

Validates: the record-creation contract (module_create_record()) through a real run of the
selected package's committed fixture, reading HDF5 output at two consecutive output snapshots

The framework fixture module test_fixture runs as process_full_halo in pre_timestep with
TestFixtureCreateRecords set, so it creates that many records on every Type 0 host at every
processed snapshot. Under a vertical package (the default pair) this exercises the vertical
driver; under a horizontal one (MODEL=halos-only SIMULATION=mini-millennium-horizontal) the
horizontal driver. A package without a committed fixture skips with the generator's stated
reason.

The two output snapshots are the latest consecutive pair at which the fixture has Type 0
hosts on both sides and a host galaxy that continues from the first to the second, found
by a probe run without creation (a committed fixture may hold an empty snapshot). A host's
current halo is identified by matching its output MostBoundID against the fixture's input,
which gives the (unit, row) the encoder must have used: the global forest number and the
in-forest index under the vertical driver, the snapshot and the slab row under the
horizontal driver.

Test cases:
  - test_tree_rows_unchanged_and_created_ids_negative: every positive-ID row is bitwise
    identical to the same run without creation; every negative ID is a unique Type 2 row
  - test_created_rows_follow_their_host: each host's records of a snapshot decode to its
    current (unit, row) with ordinals 0..n-1, form one contiguous block inside the host's
    output segment, and carry the host's UniqueCentralGalaxyID
  - test_created_rows_are_inherited: records created at the first output snapshot reappear
    at the next as Type 2 rows with the same ID, inside their host's descendant segment
  - test_repeat_runs_are_bitwise_identical: two runs with creation write identical galaxies
  - test_memory_profile_and_leaks: every run is leak-free; creation raises G and P, never
    lowers C, and leaves R's generation count unchanged
"""

import re
import shutil
import struct
import sys
from pathlib import Path

import h5py
import numpy as np
import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    MIMIC_EXE,
    TestSkipped,
    check_no_memory_leaks,
    core_input_file,
    create_test_param_file,
    resolve_sim_config_path,
    run_mimic,
    run_test_suite,
    selected_package_is_horizontal,
)

CREATE_PER_HOST = 2
RADIX = 1024  # MAX_CREATED_RECORDS_PER_HOST (src/include/constants.h)

# The classic 104-byte L-Halo binary record (simulations/mini-millennium/halo_properties.yaml).
LHALO_RECORD = np.dtype(
    [
        ("Descendant", "<i4"),
        ("FirstProgenitor", "<i4"),
        ("NextProgenitor", "<i4"),
        ("FirstHaloInFOFgroup", "<i4"),
        ("NextHaloInFOFgroup", "<i4"),
        ("Len", "<i4"),
        ("M_Mean200", "<f4"),
        ("M_Crit200", "<f4"),
        ("M_TopHat", "<f4"),
        ("Pos", "<f4", 3),
        ("Vel", "<f4", 3),
        ("VelDisp", "<f4"),
        ("Vmax", "<f4"),
        ("Spin", "<f4", 3),
        ("MostBoundID", "<i8"),
        ("SnapNum", "<i4"),
        ("FileNr", "<i4"),
        ("SubhaloIndex", "<i4"),
        ("SubHalfMass", "<f4"),
    ]
)

_IDENTITY_LINE = re.compile(
    r"Created-record identity space \((vertical|horizontal), reader '[^']+'\): "
    r"units=(\d+), rows_per_unit=(\d+)"
)
_PROFILE_TERMS = {
    "C": re.compile(r"Output buffer capacity C: (\d+) records"),
    "P": re.compile(r"Output population P: (\d+) records"),
    "G": re.compile(r"Galaxy pool high-water G: (\d+) galaxies"),
    "R": re.compile(r"Retained generations R: (.*)"),
}

# Runs shared by the cases: name -> (stdout+stderr, output_dir, param_file)
_runs = {}
_temp_dirs = []
_pair = []


def _launch(name, create_records, snapshot_list):
    """Run the fixture with test_fixture in pre_timestep and the given output snapshots."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    ref = core_input_file("test_hdf5.yaml")  # skips for a package with no committed fixture
    params = {"TestFixtureDummyParameter": 0.25, "TestFixtureEnableLogging": 0}
    if create_records is not None:
        params["TestFixtureCreateRecords"] = create_records
    param_file, output_dir, temp_dir = create_test_param_file(
        f"record_creation_{name}",
        phase_config={"pre_timestep": [("test_fixture", "process_full_halo")]},
        model_params=params,
        ref_param_file=ref,
        output_format="hdf5",
    )
    _temp_dirs.append(temp_dir)

    with open(param_file) as handle:
        config = yaml.safe_load(handle)
    if snapshot_list is None:
        last = max(int(s) for s in config["output"]["snapshot_list"])
        snapshot_list = list(range(last + 1))
    config["output"]["snapshot_list"] = snapshot_list
    with open(param_file, "w") as handle:
        yaml.safe_dump(config, handle, sort_keys=False)

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic failed (rc={returncode})\nSTDOUT:\n{stdout}\nSTDERR:\n{stderr}"
    return stdout + "\n" + stderr, Path(output_dir), Path(param_file)


def _output_files(output_dir):
    """Per-partition HDF5 files when the writer makes them, else the single file."""
    files = sorted(output_dir.glob("model_*.hdf5"))
    return files if files else sorted(output_dir.glob("model.hdf5"))


def _read_snapshots(output_dir):
    """{snapnum: [Galaxies array per output file, in file order]}"""
    result = {}
    for path in _output_files(output_dir):
        with h5py.File(path, "r") as handle:
            for key in handle.keys():
                if key.startswith("Snap"):
                    result.setdefault(int(key[4:]), []).append(handle[key]["Galaxies"][:])
    return result


def _output_pair():
    """The latest consecutive (earlier, later) snapshots with a continuing Type 0 host."""
    if not _pair:
        _, probe_dir, _ = _launch("probe", None, None)
        snapshots = {s: np.concatenate(g) for s, g in _read_snapshots(probe_dir).items()}
        for later in sorted(snapshots, reverse=True):
            earlier = later - 1
            if earlier not in snapshots:
                continue
            hosts = snapshots[earlier]["UniqueGalaxyID"][snapshots[earlier]["Type"] == 0]
            if len(hosts) and np.isin(hosts, snapshots[later]["UniqueGalaxyID"]).any():
                _pair.extend([earlier, later])
                break
        assert _pair, "the fixture has two consecutive snapshots with a continuing host"
    return list(_pair)


def _run(name, create_records):
    """A run at the chosen output pair (cached by name)."""
    if name not in _runs:
        _runs[name] = _launch(name, create_records, _output_pair())
    return _runs[name]


def _snapshots(output_dir):
    result = _read_snapshots(output_dir)
    assert sorted(result) == _output_pair(), f"output snapshots {sorted(result)}"
    return result


def _same_bytes(a, b):
    """Field-by-field bitwise equality of two structured arrays.

    Whole-record tobytes() would also compare the dtype's padding, which h5py leaves
    uninitialised, so each field's values are compared as raw bytes instead.
    """
    if a.dtype != b.dtype or a.shape != b.shape:
        return False
    return all(
        np.ascontiguousarray(a[n]).tobytes() == np.ascontiguousarray(b[n]).tobytes()
        for n in a.dtype.names
    )


def _identity_space(log):
    match = _IDENTITY_LINE.search(log)
    assert match, "the run logs its created-record identity space"
    return match.group(1), int(match.group(2)), int(match.group(3))


def _input_config(param_file):
    with open(param_file) as handle:
        config = yaml.safe_load(handle)
    sim_path = resolve_sim_config_path(config["simulation"]["config"], param_file)
    with open(sim_path) as handle:
        sim_input = yaml.safe_load(handle)["input"]
    sim_dir = Path(sim_input["simulation_dir"])
    return sim_input, sim_dir if sim_dir.is_absolute() else REPO_ROOT / sim_dir


def _host_keys(param_file, driver, snapnum):
    """{MostBoundID: (unit, row)} of every halo of the fixture input at `snapnum`."""
    sim_input, sim_dir = _input_config(param_file)
    keys = {}
    if driver == "horizontal":
        path = sim_dir / (sim_input["tree_name"] % snapnum)
        with h5py.File(path, "r") as handle:
            for row, mbid in enumerate(handle["halos"]["MostBoundID"][:]):
                keys[int(mbid)] = (snapnum, row)
        return keys

    assert sim_input["tree_type"] == "lhalo_binary", "the vertical leg reads an L-Halo fixture"
    assert int(sim_input.get("first_file", 0)) == 0 and int(sim_input.get("last_file", 0)) == 0
    data = (sim_dir / f"{sim_input['tree_name']}.0").read_bytes()
    ntrees, total = struct.unpack_from("<ii", data, 0)
    counts = np.frombuffer(data, "<i4", ntrees, 8)
    offset = 8 + 4 * ntrees
    assert len(data) - offset == total * LHALO_RECORD.itemsize, "L-Halo records are 104 bytes"
    halos = np.frombuffer(data, LHALO_RECORD, total, offset)
    first = 0
    for forest, count in enumerate(counts):
        tree = halos[first : first + count]
        for row in np.flatnonzero(tree["SnapNum"] == snapnum):
            keys[int(tree["MostBoundID"][row])] = (forest, int(row))
        first += count
    return keys


def _host_rows(galaxies):
    return np.flatnonzero(galaxies["Type"] == 0)


def _records_by_host_key(galaxies, rows_per_unit):
    """{(unit, row): [(index, ordinal), ...] in output order} over the negative IDs.

    Inverts mimic_encode_created_galaxy_id(), -(1 + ordinal + RADIX * (row + rows_per_unit *
    unit)), vectorised so hundreds of thousands of created rows are indexed in one pass.
    """
    ids = galaxies["UniqueGalaxyID"].astype(np.int64)
    negative = np.flatnonzero(ids < 0)
    host_key, ordinal = np.divmod(-ids[negative] - 1, RADIX)
    unit, row = np.divmod(host_key, rows_per_unit)
    table = {}
    for index, u, r, o in zip(negative.tolist(), unit.tolist(), row.tolist(), ordinal.tolist()):
        table.setdefault((u, r), []).append((index, o))
    return table


def test_tree_rows_unchanged_and_created_ids_negative():
    """Positive-ID rows are bitwise identical to a run without creation; negatives are created."""
    _, created_dir, _ = _run("create", CREATE_PER_HOST)
    _, plain_dir, _ = _run("plain", None)
    created = _snapshots(created_dir)
    plain = _snapshots(plain_dir)

    for snapnum in sorted(created):
        for with_creation, without in zip(created[snapnum], plain[snapnum]):
            ids = with_creation["UniqueGalaxyID"]
            assert np.all(without["UniqueGalaxyID"] > 0), "a run without creation is all positive"
            unchanged = _same_bytes(with_creation[ids > 0], without)
            assert unchanged, f"snapshot {snapnum}: tree rows differ from the run without creation"
            created_rows = with_creation[ids < 0]
            expected_minimum = CREATE_PER_HOST * len(_host_rows(without))
            assert len(created_rows) >= expected_minimum, f"snapshot {snapnum}: too few records"
            assert np.all(created_rows["Type"] == 2), "every created row is Type 2"
            assert np.all(created_rows["Mvir"] == 0) and np.all(created_rows["Len"] == 0)
            assert len(np.unique(ids)) == len(ids), f"snapshot {snapnum}: IDs are unique"


def test_created_rows_follow_their_host():
    """Each host's records decode to its current (unit, row) and sit after it in its segment."""
    log, output_dir, param_file = _run("create", CREATE_PER_HOST)
    driver, _units, rows_per_unit = _identity_space(log)
    assert driver == ("horizontal" if selected_package_is_horizontal() else "vertical")

    hosts = 0
    for snapnum, arrays in _snapshots(output_dir).items():
        keys = _host_keys(param_file, driver, snapnum)
        for galaxies in arrays:
            ids = galaxies["UniqueGalaxyID"]
            types = galaxies["Type"]
            centrals = galaxies["UniqueCentralGalaxyID"]
            records = _records_by_host_key(galaxies, rows_per_unit)
            for host in _host_rows(galaxies):
                host_id = int(ids[host])
                key = keys[int(galaxies["MostBoundID"][host])]
                found = records.get(key, [])
                assert len(found) == CREATE_PER_HOST, (
                    f"snapshot {snapnum}: host {host_id} has {len(found)} records decoding to "
                    f"its (unit, row) = {key}, expected {CREATE_PER_HOST}"
                )
                mine = [index for index, _ in found]
                ordinals = [ordinal for _, ordinal in found]
                message = f"host {host_id}: ordinals {ordinals} are not 0..n-1 in output order"
                assert ordinals == list(range(CREATE_PER_HOST)), message
                start, stop = mine[0], mine[-1] + 1
                assert stop - start == CREATE_PER_HOST, f"host {host_id}: records not contiguous"
                assert start > host, f"host {host_id}: records precede their host"
                message = f"host {host_id}: a Type 0/1 row (another segment) precedes its records"
                assert np.all(types[host + 1 : start] == 2), message
                message = f"host {host_id}: records carry the host's UniqueCentralGalaxyID"
                assert np.all(centrals[host:stop] == host_id), message
                hosts += 1
    assert hosts > 0, "the output snapshots have Type 0 hosts"
    assert log.count("TEST_FIXTURE_CREATE: host=") >= hosts, "one log line per host and call"


def test_created_rows_are_inherited():
    """Records created at the first output snapshot are Type 2 rows of the next one.

    For every Type 0 host of the first snapshot whose galaxy continues at the next one, the
    records created on it must reappear with the same ID, as Type 2 orphans, inside the
    host's segment (after the host row, before the next Type 0/1 row).
    """
    log, output_dir, param_file = _run("create", CREATE_PER_HOST)
    driver, _units, rows_per_unit = _identity_space(log)
    snapshots = _snapshots(output_dir)
    first, last = _output_pair()
    earlier = np.concatenate(snapshots[first])
    later = np.concatenate(snapshots[last])
    keys = _host_keys(param_file, driver, first)
    later_index = {int(i): k for k, i in enumerate(later["UniqueGalaxyID"])}
    segment_starts = np.flatnonzero(later["Type"] <= 1)
    earlier_records = _records_by_host_key(earlier, rows_per_unit)

    inherited = 0
    for host in _host_rows(earlier):
        host_id = int(earlier["UniqueGalaxyID"][host])
        if host_id not in later_index:
            continue  # the host's halo has no descendant at the next snapshot
        key = keys[int(earlier["MostBoundID"][host])]
        records = [int(earlier["UniqueGalaxyID"][i]) for i, _ in earlier_records.get(key, [])]
        assert len(records) == CREATE_PER_HOST, f"host {host_id} created its records at {first}"
        host_at_last = later_index[host_id]
        following = segment_starts[segment_starts > host_at_last]
        segment_end = int(following[0]) if len(following) else len(later)
        for created_id in records:
            assert created_id in later_index, f"record {created_id} was not inherited"
            position = later_index[created_id]
            successor = later[position]
            assert successor["Type"] == 2, f"record {created_id} is inherited as Type 2"
            assert successor["Mvir"] == 0 and successor["Len"] == 0
            message = f"record {created_id} is not inside its host's descendant segment"
            assert host_at_last < position < segment_end, message
            inherited += 1
    assert inherited > 0, "some records were inherited at the next output snapshot"


def test_repeat_runs_are_bitwise_identical():
    """Two runs with creation write bitwise-identical galaxies at every output snapshot."""
    _, first_dir, _ = _run("create", CREATE_PER_HOST)
    _, repeat_dir, _ = _run("create_repeat", CREATE_PER_HOST)
    first = _snapshots(first_dir)
    repeat = _snapshots(repeat_dir)
    for snapnum in first:
        assert len(first[snapnum]) == len(repeat[snapnum])
        for a, b in zip(first[snapnum], repeat[snapnum]):
            assert _same_bytes(a, b), f"snapshot {snapnum} differs between repeats"


def _profile(log):
    terms = {}
    for name, pattern in _PROFILE_TERMS.items():
        match = pattern.search(log)
        assert match, f"run memory profile term {name} is reported"
        terms[name] = match.group(1)
    return terms


def test_memory_profile_and_leaks():
    """No run leaks; creation raises G and P, never lowers C, and keeps R's generation count."""
    runs = [_run("create", CREATE_PER_HOST), _run("plain", None)]
    for log, _, _ in runs:
        assert check_no_memory_leaks(log), "the run reports no memory leak"
        assert "No memory leaks detected" in log

    with_creation = _profile(runs[0][0])
    without = _profile(runs[1][0])
    assert int(with_creation["G"]) > int(without["G"]), "created galaxies raise G"
    assert int(with_creation["P"]) > int(without["P"]), "created rows raise P"
    assert int(with_creation["C"]) >= int(without["C"]), "created rows can only raise C"
    # R's resident bytes include the generations' output buffers and pools (C and G above), so
    # only its generation count is creation-independent; it is unmeasured under the vertical
    # driver.
    r_with = with_creation["R"].split(",")[0]
    r_without = without["R"].split(",")[0]
    assert r_with == r_without, f"R's generation count is unchanged ({r_with} vs {r_without})"


def _cleanup():
    for temp_dir in _temp_dirs:
        shutil.rmtree(temp_dir, ignore_errors=True)


def main():
    try:
        return run_test_suite(
            [
                test_tree_rows_unchanged_and_created_ids_negative,
                test_created_rows_follow_their_host,
                test_created_rows_are_inherited,
                test_repeat_runs_are_bitwise_identical,
                test_memory_profile_and_leaks,
            ],
            "Record Creation (test_record_creation.py)",
        )
    finally:
        _cleanup()


if __name__ == "__main__":
    sys.exit(main())
