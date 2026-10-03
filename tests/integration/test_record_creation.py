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
by a probe run without creation (a committed fixture may hold an empty snapshot); a fixture
with no such pair skips with that configuration reason.

The identity checks are format-neutral, so they run on every committed fixture: a host's
records of a snapshot end its output segment, decode (with rows_per_unit from the run's
INFO line) to one (unit, row) with ordinals 0..n-1, carry the unit the driver publishes for
the host (its forest under the vertical driver, the snapshot under the horizontal driver),
and no two hosts share a key. Where the input is L-Halo binary or horizontal HDF5 the key is
also checked against the host's current halo, found by matching its output MostBoundID in
the input (in-forest index or slab row).

Test cases:
  - test_tree_rows_unchanged_and_created_ids_negative: every positive-ID row is bitwise
    identical to the same run without creation; every negative ID is a unique Type 2 row
  - test_created_rows_follow_their_host: each host's records of a snapshot are the last rows
    of its output segment, decode to its own (unit, row) with ordinals 0..n-1, and carry the
    host's UniqueCentralGalaxyID; keys are distinct per snapshot
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
        if not _pair:
            raise TestSkipped(
                "configuration SKIP: the selected package's committed fixture has no two "
                "consecutive output snapshots with a Type 0 host whose galaxy continues from the "
                "first to the second, so creation and inheritance cannot both be observed"
            )
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
    """The run's effective input section (simulation config, then run-level overrides)."""
    with open(param_file) as handle:
        config = yaml.safe_load(handle)
    sim_path = resolve_sim_config_path(config["simulation"]["config"], param_file)
    with open(sim_path) as handle:
        sim_input = dict(yaml.safe_load(handle)["input"])
    sim_input.update(config.get("input") or {})
    sim_dir = Path(sim_input["simulation_dir"])
    return sim_input, sim_dir if sim_dir.is_absolute() else REPO_ROOT / sim_dir


def _input_keys(param_file, driver, snapnum):
    """{MostBoundID: (unit, row)} of the fixture input's halos at `snapnum`, or None.

    An extra cross-check of the decoded keys against the input, available where the input
    is cheap to read here: L-Halo binary under the vertical driver (unit = global forest
    number across the run's file range, row = in-forest index) and horizontal HDF5 (unit =
    snapshot, row = slab row). Any other format returns None and is checked by the
    format-neutral assertions alone.
    """
    sim_input, sim_dir = _input_config(param_file)
    keys = {}
    if driver == "horizontal":
        if sim_input.get("tree_type") != "horizontal_hdf5":
            return None
        with h5py.File(sim_dir / (sim_input["tree_name"] % snapnum), "r") as handle:
            for row, mbid in enumerate(handle["halos"]["MostBoundID"][:]):
                keys[int(mbid)] = (snapnum, row)
        return keys

    if sim_input.get("tree_type") != "lhalo_binary":
        return None
    forest_offset = 0
    first_file = int(sim_input.get("first_file", 0))
    last_file = int(sim_input.get("last_file", first_file))
    for file_number in range(first_file, last_file + 1):
        data = (sim_dir / f"{sim_input['tree_name']}.{file_number}").read_bytes()
        ntrees, total = struct.unpack_from("<ii", data, 0)
        counts = np.frombuffer(data, "<i4", ntrees, 8)
        offset = 8 + 4 * ntrees
        if len(data) - offset != total * LHALO_RECORD.itemsize:
            return None  # not the classic 104-byte record; the extra check does not apply
        halos = np.frombuffer(data, LHALO_RECORD, total, offset)
        first = 0
        for forest, count in enumerate(counts):
            tree = halos[first : first + count]
            for row in np.flatnonzero(tree["SnapNum"] == snapnum):
                keys[int(tree["MostBoundID"][row])] = (forest_offset + forest, int(row))
            first += count
        forest_offset += ntrees
    return keys


def _decode(ids, rows_per_unit):
    """Vectorised inverse of mimic_encode_created_galaxy_id(): (unit, row, ordinal) arrays.

    Inverts -(1 + ordinal + RADIX * (row + rows_per_unit * unit)).
    """
    host_key, ordinal = np.divmod(-np.asarray(ids, dtype=np.int64) - 1, RADIX)
    unit, row = np.divmod(host_key, rows_per_unit)
    return unit, row, ordinal


def _host_rows(galaxies):
    return np.flatnonzero(galaxies["Type"] == 0)


def _host_blocks(galaxies, snapnum, driver, rows_per_unit, multiplier):
    """{host index: ((unit, row), [record indices])} for every Type 0 host of one output array.

    Format-neutral. The marshaller appends a host's records of this snapshot after its
    subhalo slice, so they are the last CREATE_PER_HOST rows of the host's segment (the host
    row up to the next Type 0/1 row). They must be negative IDs decoding to one shared
    (unit, row) with ordinals 0..n-1 in output order; the unit must be the one the driver
    publishes for the host (its forest, read from its tree ID, under the vertical driver; the
    snapshot under the horizontal driver); the rows between the host and them must be Type 2;
    and the whole block must carry the host's UniqueCentralGalaxyID.
    """
    ids = galaxies["UniqueGalaxyID"].astype(np.int64)
    types = galaxies["Type"]
    centrals = galaxies["UniqueCentralGalaxyID"]
    segment_starts = np.flatnonzero(types <= 1)
    blocks = {}
    for host in _host_rows(galaxies):
        host_id = int(ids[host])
        following = segment_starts[segment_starts > host]
        end = int(following[0]) if len(following) else len(ids)
        start = end - CREATE_PER_HOST
        where = f"snapshot {snapnum}: host {host_id}"
        assert start > host, f"{where}: its segment has no room for its records"
        assert np.all(ids[start:end] < 0), f"{where}: its segment does not end in records"
        unit, row, ordinal = _decode(ids[start:end], rows_per_unit)
        keys = set(zip(unit.tolist(), row.tolist()))
        assert len(keys) == 1, f"{where}: its records decode to several hosts {sorted(keys)}"
        message = f"{where}: ordinals {ordinal.tolist()} are not 0..n-1 in output order"
        assert ordinal.tolist() == list(range(CREATE_PER_HOST)), message
        expected_unit = snapnum if driver == "horizontal" else host_id // multiplier - 1
        message = f"{where}: records decode to unit {int(unit[0])}, expected {expected_unit}"
        assert int(unit[0]) == expected_unit, message
        message = f"{where}: a Type 0/1 row (another segment) precedes its records"
        assert np.all(types[host + 1 : start] == 2), message
        message = f"{where}: records carry the host's UniqueCentralGalaxyID"
        assert np.all(centrals[host:end] == host_id), message
        blocks[int(host)] = (keys.pop(), list(range(start, end)))
    return blocks


def _id_multiplier(output_dir):
    with h5py.File(_output_files(output_dir)[0], "r") as handle:
        value = handle["RunProperties"].attrs["UniqueGalaxyIDMultiplier"]
    return int(np.asarray(value).ravel()[0])


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
    """Each host's records end its segment and decode to its own (unit, row), ordinals 0..n-1.

    The format-neutral checks of _host_blocks() run on every fixture; hosts' decoded keys must
    also be distinct within a snapshot. Where the input is L-Halo binary or horizontal HDF5,
    each key is further checked against the host's current halo, found by MostBoundID.
    """
    log, output_dir, param_file = _run("create", CREATE_PER_HOST)
    driver, _units, rows_per_unit = _identity_space(log)
    assert driver == ("horizontal" if selected_package_is_horizontal() else "vertical")
    multiplier = _id_multiplier(output_dir)

    hosts = 0
    for snapnum, arrays in _snapshots(output_dir).items():
        input_keys = _input_keys(param_file, driver, snapnum)
        snapshot_keys = []
        for galaxies in arrays:
            blocks = _host_blocks(galaxies, snapnum, driver, rows_per_unit, multiplier)
            for host, (key, _records) in blocks.items():
                snapshot_keys.append(key)
                if input_keys is not None:
                    expected = input_keys[int(galaxies["MostBoundID"][host])]
                    message = f"snapshot {snapnum}: host row {host} key {key} is not {expected}"
                    assert key == expected, message
            hosts += len(blocks)
        message = f"snapshot {snapnum}: two hosts' records decode to the same (unit, row)"
        assert len(set(snapshot_keys)) == len(snapshot_keys), message
    assert hosts > 0, "the output snapshots have Type 0 hosts"


def test_created_rows_are_inherited():
    """Records created at the first output snapshot are Type 2 rows of the next one.

    For every Type 0 host of the first snapshot whose galaxy continues at the next one, the
    records created on it there (found format-neutrally by _host_blocks()) must reappear with
    the same ID, as Type 2 orphans, inside the host's segment at the next snapshot (after the
    host row, before the next Type 0/1 row).
    """
    log, output_dir, _ = _run("create", CREATE_PER_HOST)
    driver, _units, rows_per_unit = _identity_space(log)
    multiplier = _id_multiplier(output_dir)
    snapshots = _snapshots(output_dir)
    first, last = _output_pair()
    later = np.concatenate(snapshots[last])
    later_index = {int(i): k for k, i in enumerate(later["UniqueGalaxyID"])}
    segment_starts = np.flatnonzero(later["Type"] <= 1)

    inherited = 0
    for earlier in snapshots[first]:
        blocks = _host_blocks(earlier, first, driver, rows_per_unit, multiplier)
        for host, (_key, records) in blocks.items():
            host_id = int(earlier["UniqueGalaxyID"][host])
            if host_id not in later_index:
                continue  # the host's halo has no descendant at the next snapshot
            host_at_last = later_index[host_id]
            following = segment_starts[segment_starts > host_at_last]
            segment_end = int(following[0]) if len(following) else len(later)
            for index in records:
                created_id = int(earlier["UniqueGalaxyID"][index])
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
    """No run leaks; creation raises G and P, never lowers C, and keeps R's generation count.

    P (records emitted) and G (galaxies allocated) must rise strictly. C, the output buffer's
    realised capacity, is only required not to fall: the buffer is seeded at a capacity that
    overshoots the population, so a run whose created rows still fit the seeded capacity
    leaves C unchanged, and strict growth is not provable.
    """
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
