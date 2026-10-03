#!/usr/bin/env python3
"""
Integration tests for the modules.post_snapshot phase.

Validates, through the real executable:

  - configuration: an absent, null or empty post_snapshot means no snapshot modules and
    leaves the pipeline and its provenance exactly as before; every malformed or illegal
    entry (non-sequence phase, non-mapping entry, an entry naming two modules in any
    phase, unknown mode, FoF mode in post_snapshot, process_snapshot in a FoF phase,
    duplicate entry, unknown module, a module without the snapshot mode, post_snapshot as
    a substep phase name) fails at startup, naming the phase or module, before any module
    init() and before the dataset is opened;
  - the driver seam: a non-empty post_snapshot is rejected under the vertical driver and
    keeps the horizontal driver's HDF5-only and no-resume restrictions;
  - horizontal execution, on the selected package's committed fixture: every configured
    callback runs exactly once per input snapshot, in YAML order (proven by the two
    fixtures' log markers, also when a FoF placement of test_fixture puts it first in the
    pipeline), after the snapshot's FoF sweep and before any generation is released or the
    snapshot is written, for empty, non-output and final snapshots alike and whatever the
    substep scheme; its population is exactly the snapshot's own output (Types 0/1/2,
    never an older retained generation); its writes to
    TestDummyProperty reach the next callback and the galaxies descendants inherit,
    across adjacent links and across an empty-snapshot gap; FoF event emission from a
    callback is rejected; EnabledModules records the phase; a failing callback aborts the
    run with module, snapshot and return code and leaves no master file behind.

Type 3 exclusion is not proven here: MODEL=halos-only creates no Type 3 galaxy, so the
"no Type 3" checks below are sanity checks only. The proof is the unit test
test_post_snapshot_population_excludes_type3 (tests/unit/test_snapshot_module_contract.c),
which runs the real marshaller over a workspace that contains a Type 3 entry.

Parser and vertical-rejection cases run on every package. The horizontal-execution cases
need a horizontal package with a committed fixture and report a configuration SKIP
otherwise; the required evidence is a run under each of

  MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal TEST_BUILD=yes   (v2, adjacent)
  MODEL=halos-only SIMULATION=mini-millennium-horizontal TEST_BUILD=yes     (v3, gapped)

with no MIMIC_RESULT: SKIP. The test never invokes make; build the selected TEST_BUILD
executable first. It contains no multi-rank case.
"""

import re
import shutil
import sys
import tempfile
from pathlib import Path

import yaml

# Add framework to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import (  # noqa: E402
    MIMIC_EXE,
    REPO_ROOT,
    TestSkipped,
    check_no_memory_leaks,
    compiled_simulation,
    create_test_param_file,
    run_mimic,
    run_test_suite,
    selected_package_is_horizontal,
    selected_package_test_config,
)

TEMP_DIR = None

#: Value test_snapshot_fixture writes (TEST_SNAPSHOT_FIXTURE_VALUE in its source).
SNAPSHOT_FIXTURE_VALUE = 0.5

#: TestFixtureDummyParameter for runs where test_fixture writes in range.
DUAL_FIXTURE_VALUE = 0.25

#: Return code test_fixture's snapshot callback gives for an out-of-range parameter.
DUAL_FIXTURE_RANGE_ERROR = 2

#: Vertical-reader input used to put a horizontal package's run on the vertical driver.
#: simulation_dir is replaced by an empty directory: the compiled horizontal package's
#: halo layout is not the L-Halo binary record, so no tree file may be read, and the
#: vertical driver skips a missing tree file and completes.
VERTICAL_INPUT = {
    "tree_type": "lhalo_binary",
    "processing_order": "vertical",
    "tree_name": "trees_063",
    "snapshot_list_file": "./tests/data/input/mini-millennium.a_list",
    "first_file": 0,
    "last_file": 0,
}

#: Committed v2 fixture, used to put a vertical package's run on the horizontal driver
#: for startup-only checks: each fails before the dataset is opened. Runs using it
#: write every snapshot (an empty output.snapshot_list), since the vertical reference
#: run file selects a snapshot the fixture's scale-factor list does not have.
HORIZONTAL_INPUT = {
    "tree_type": "horizontal_hdf5",
    "processing_order": "horizontal",
    "tree_name": "snapshot_%03d.h5",
    "simulation_dir": "simulations/micro-uchuu-ascii-horizontal/_tests/data/generic",
    "snapshot_list_file": (
        "simulations/micro-uchuu-ascii-horizontal/_tests/data/generic/micro-uchuu-fixture.a_list"
    ),
}

#: Log lines that show a module was initialised or the dataset was opened. The last three
#: are the vertical driver's: its "Processing N input file(s) (first_file=..." banner, a
#: completed input file, and a skipped missing tree file (src/core/vertical_driver.c).
INIT_OR_PROCESSING_MARKERS = (
    "Test snapshot fixture module initialized",
    "Test fixture module initialized",
    "Opened horizontal run",
    "Loaded snapshot",
    "(first_file=",
    "Completed input file",
    "Missing tree",
)

_SNAPSHOT_FIXTURE_PATTERN = re.compile(
    r"TEST_SNAPSHOT_FIXTURE_EXEC: call=(?P<call>\d+) snapshot=(?P<snapshot>\d+) "
    r"count=(?P<count>\d+) z=(?P<z>[\d.]+) time=(?P<time>[\d.e+-]+) "
    r"types=(?P<t0>\d+)/(?P<t1>\d+)/(?P<t2>\d+) other_types=(?P<other>\d+) "
    r"foreign_snapnum=(?P<foreign>\d+) seen_zero=(?P<seen_zero>\d+) "
    r"seen_max=(?P<seen_max>[\d.]+) id_sum=(?P<id_sum>\d+) emit=rejected"
)

_DUAL_FIXTURE_PATTERN = re.compile(
    r"TEST_FIXTURE_SNAPSHOT_EXEC: count=(?P<call>\d+) snapshot=(?P<snapshot>\d+) "
    r"n=(?P<count>\d+) z=(?P<z>[\d.]+) seen_min=(?P<seen_min>[\d.]+) "
    r"seen_max=(?P<seen_max>[\d.]+)"
)

_FOF_FIXTURE_PATTERN = re.compile(r"TEST_FIXTURE_EXEC: count=\d+ ngal=\d+ substep=\d+/(\d+) ")

_EMIT_REJECTION = "module_emit_event called during post_snapshot dispatch"


# ---------------------------------------------------------------------------
# Selection and fixture helpers
# ---------------------------------------------------------------------------


def package_fixture():
    """The selected package's committed horizontal fixture as (dataset dir, a_list), or None."""
    config_path = selected_package_test_config()
    if config_path is None or not selected_package_is_horizontal():
        return None
    with open(config_path, "r") as handle:
        input_config = (yaml.safe_load(handle) or {}).get("input") or {}
    return (
        REPO_ROOT / input_config["simulation_dir"],
        REPO_ROOT / input_config["snapshot_list_file"],
    )


def require_horizontal_fixture(case):
    """Raise the explicit configuration SKIP for a horizontal-execution case."""
    fixture = package_fixture()
    if fixture is None:
        raise TestSkipped(
            f"configuration SKIP: selected package {compiled_simulation()} is not a horizontal "
            f"package with a committed fixture; {case} needs the horizontal driver running "
            f"over one (run under SIMULATION=micro-uchuu-ascii-horizontal or "
            f"mini-millennium-horizontal)"
        )
    if not MIMIC_EXE.exists():
        raise FileNotFoundError(f"Mimic executable not found at {MIMIC_EXE}")
    return fixture


def fixture_scale_factors():
    """The fixture's scale factors, one per input snapshot."""
    _dataset_dir, a_list = package_fixture()
    with open(a_list, "r") as handle:
        return [float(line.split()[0]) for line in handle if line.strip()]


def fixture_halo_counts():
    """Halo count of every fixture snapshot slab."""
    import h5py

    dataset_dir, _a_list = package_fixture()
    counts = []
    for snap in range(len(fixture_scale_factors())):
        with h5py.File(dataset_dir / f"snapshot_{snap:03d}.h5", "r") as handle:
            counts.append(int(handle["header"].attrs["n_halos"]))
    return counts


def make_run(
    name,
    phase_config=None,
    model_params=None,
    snapshot_list=None,
    substeps=None,
    timestep_scheme=None,
    input_overrides=None,
    output_format="hdf5",
    modules_override=None,
):
    """Write a run file for this test and return (run file, output directory).

    phase_config goes through the harness writer (post_snapshot included). When
    modules_override is given it replaces modules.* except parameters verbatim, so
    malformed shapes can be written exactly.
    """
    param_file, output_dir, _ = create_test_param_file(
        output_name=f"snapshot_phase_{name}",
        phase_config=phase_config,
        model_params=model_params,
        first_file=0,
        last_file=0,
        temp_dir=TEMP_DIR,
        output_format=output_format,
        substeps=substeps,
        timestep_scheme=timestep_scheme,
    )
    with open(param_file, "r") as handle:
        config = yaml.safe_load(handle)

    config["output"]["output_filename"] = "model"
    if snapshot_list is not None:
        config["output"]["snapshot_list"] = list(snapshot_list)
    if input_overrides is not None:
        config.setdefault("input", {}).update(input_overrides)
    if modules_override is not None:
        parameters = config["modules"].get("parameters")
        config["modules"] = dict(modules_override)
        if parameters is not None:
            config["modules"]["parameters"] = parameters

    with open(param_file, "w") as handle:
        yaml.safe_dump(config, handle, default_flow_style=False, sort_keys=False)
    return param_file, output_dir


def run_tree_type(param_file):
    """The input.tree_type a run file resolves to: its own, else its simulation config's.

    The run file's input section wins over the simulation config it names, as in the
    executable's own two-pass parse, so this is the reader the run will select.
    """
    with open(param_file, "r") as handle:
        config = yaml.safe_load(handle)
    tree_type = (config.get("input") or {}).get("tree_type")
    if tree_type is not None:
        return tree_type
    sim_config = Path(config["simulation"]["config"])
    if not sim_config.is_absolute():
        sim_config = REPO_ROOT / sim_config
    with open(sim_config, "r") as handle:
        return (yaml.safe_load(handle).get("input") or {})["tree_type"]


def fixture_params(value=DUAL_FIXTURE_VALUE):
    return {"TestFixtureDummyParameter": value, "TestFixtureEnableLogging": 1}


def snapshot_markers(stdout):
    """Every TEST_SNAPSHOT_FIXTURE_EXEC marker, with its position in stdout."""
    markers = []
    for match in _SNAPSHOT_FIXTURE_PATTERN.finditer(stdout):
        record = {key: match.group(key) for key in _SNAPSHOT_FIXTURE_PATTERN.groupindex}
        for key in ("call", "snapshot", "count", "t0", "t1", "t2", "other", "foreign"):
            record[key] = int(record[key])
        record["seen_zero"] = int(record["seen_zero"])
        record["id_sum"] = int(record["id_sum"])
        for key in ("z", "time", "seen_max"):
            record[key] = float(record[key])
        record["pos"] = match.start()
        markers.append(record)
    return markers


def dual_markers(stdout):
    """Every TEST_FIXTURE_SNAPSHOT_EXEC marker, with its position in stdout."""
    markers = []
    for match in _DUAL_FIXTURE_PATTERN.finditer(stdout):
        markers.append(
            {
                "call": int(match.group("call")),
                "snapshot": int(match.group("snapshot")),
                "count": int(match.group("count")),
                "seen_min": float(match.group("seen_min")),
                "seen_max": float(match.group("seen_max")),
                "pos": match.start(),
            }
        )
    return markers


def output_rows(output_dir, snap):
    """The Galaxies rows the run wrote for snapshot `snap`."""
    import h5py

    with h5py.File(Path(output_dir) / f"model_{snap:03d}.hdf5", "r") as handle:
        return handle[f"Snap{snap:03d}"]["Galaxies"][()]


def galaxy_bytes(path):
    """Every Snap*/Galaxies dataset in one output file, as {(file, group): field bytes}.

    Read by what the file holds, since a vertical run names its files by output file
    number and a horizontal run by snapshot. Each field's values are compared as bytes
    (exact, NaN payloads included), field by field: a whole compound row also carries
    its padding bytes, which are not output data and can differ between two otherwise
    identical runs.
    """
    import h5py

    found = {}
    with h5py.File(path, "r") as handle:
        for group in sorted(name for name in handle if name.startswith("Snap")):
            if "Galaxies" in handle[group]:
                rows = handle[group]["Galaxies"][()]
                found[(Path(path).name, group)] = tuple(
                    (name, rows[name].tobytes()) for name in rows.dtype.names
                )
    return found


def enabled_modules(path):
    """RunProperties/EnabledModules of one output file as (module, phase, mode) tuples."""
    import h5py

    with h5py.File(path, "r") as handle:
        if "EnabledModules" not in handle["RunProperties"]:
            return []
        rows = handle["RunProperties"]["EnabledModules"][()]
    return [
        (row["module_name"].decode(), row["phase"].decode(), row["processing_mode"].decode())
        for row in rows
    ]


def id_sum(rows):
    """Sum of UniqueGalaxyID modulo 2**64, as test_snapshot_fixture computes it."""
    return sum(int(value) for value in rows["UniqueGalaxyID"]) % (1 << 64)


def assert_ok(returncode, stdout, stderr, what):
    output = stdout + stderr
    assert returncode == 0, f"{what}: the run should complete (rc={returncode}):\n{output}"
    assert check_no_memory_leaks(stdout, stderr), f"{what}: the run reported a memory leak"


def assert_startup_rejection(returncode, stdout, stderr, needles, what):
    """Non-zero exit with every needle in the output and nothing initialised or opened."""
    output = stdout + stderr
    assert returncode != 0, f"{what}: startup should fail:\n{output}"
    for needle in needles:
        assert needle in output, f"{what}: diagnostic {needle!r} missing:\n{output}"
    for marker in INIT_OR_PROCESSING_MARKERS:
        assert marker not in output, f"{what}: {marker!r} appeared before the rejection:\n{output}"


def snapshot_blocks(stdout, nsnapshots):
    """stdout split into one block per snapshot, from 'Loaded snapshot N (' to the next."""
    starts = []
    for snap in range(nsnapshots):
        position = stdout.find(f"Loaded snapshot {snap} (")
        assert position >= 0, f"snapshot {snap} was never loaded:\n{stdout}"
        starts.append(position)
    assert starts == sorted(starts), "snapshots must load in increasing order"
    ends = starts[1:] + [len(stdout)]
    return list(zip(starts, ends))


# ---------------------------------------------------------------------------
# Configuration (every package)
# ---------------------------------------------------------------------------


def test_absent_null_and_empty_phase_change_nothing():
    """
    Test that an absent, null or empty post_snapshot leaves the run exactly as before.

    Expected: all three runs (a FoF test_fixture pipeline on the selected package's own
              driver) succeed; none logs or records a post_snapshot phase; EnabledModules is
              the single FoF row in every file; every written Galaxies dataset is identical.
    """
    variants = {
        "absent": {"phases": {"galaxy_physics": [{"test_fixture": "process_by_galaxy"}]}},
        "null": {
            "phases": {"galaxy_physics": [{"test_fixture": "process_by_galaxy"}]},
            "post_snapshot": None,
        },
        "empty": {
            "phases": {"galaxy_physics": [{"test_fixture": "process_by_galaxy"}]},
            "post_snapshot": [],
        },
    }
    results = {}
    for label, modules in variants.items():
        param_file, output_dir = make_run(
            f"inactive_{label}",
            model_params=fixture_params(),
            modules_override=modules,
        )
        returncode, stdout, stderr = run_mimic(param_file)
        assert_ok(returncode, stdout, stderr, f"post_snapshot {label}")
        assert "post_snapshot" not in stdout + stderr, f"{label}: no post_snapshot may be logged"
        assert "TEST_SNAPSHOT_FIXTURE_EXEC" not in stdout, f"{label}: no snapshot callback runs"
        partitions = sorted(Path(output_dir).glob("model_[0-9][0-9][0-9].hdf5"))
        assert partitions, f"{label}: the run should write output partitions"
        rows = {}
        for path in partitions + [Path(output_dir) / "model.hdf5"]:
            assert enabled_modules(path) == [
                ("test_fixture", "galaxy_physics", "process_by_galaxy")
            ], f"{label}: {path.name} EnabledModules {enabled_modules(path)}"
        for path in partitions:
            rows.update(galaxy_bytes(path))
        assert rows, f"{label}: the run should write Galaxies datasets"
        results[label] = rows

    assert results["null"] == results["absent"], "a null phase must not change any output row"
    assert results["empty"] == results["absent"], "an empty phase must not change any output row"
    print("  ✓ absent, null and [] post_snapshot runs are identical and record no snapshot phase")


def _fof_phase_rejection(phase, module):
    """Diagnostics for process_snapshot configured in a FoF phase."""
    return [
        f"Configuration error in phase '{phase}'",
        f"Module '{module}' is configured with processing mode 'process_snapshot', which is not "
        f"a FoF mode",
        "Module system initialization failed",
    ]


def _malformed_cases():
    fof = [{"test_fixture": "process_by_galaxy"}]
    return [
        (
            "scalar phase",
            {"post_snapshot": "test_snapshot_fixture"},
            ["Phase 'post_snapshot' must be a sequence"],
        ),
        (
            "non-mapping entry",
            {"post_snapshot": ["test_snapshot_fixture"]},
            ["Phase 'post_snapshot': module entry must be 'name: mode'"],
        ),
        (
            "entry mapping with two modules",
            {
                "post_snapshot": [
                    {
                        "test_snapshot_fixture": "process_snapshot",
                        "test_fixture": "process_snapshot",
                    }
                ]
            },
            [
                "Phase 'post_snapshot': entry 1 lists 2 modules ('test_snapshot_fixture', "
                "'test_fixture'); each entry must be exactly one 'name: mode' pair",
                "Failed to parse post_snapshot phase",
            ],
        ),
        (
            "pre_timestep entry mapping with two modules",
            {
                "pre_timestep": [
                    {
                        "test_fixture": "process_full_halo",
                        "test_event_producer": "process_full_halo",
                    }
                ]
            },
            [
                "Phase 'pre_timestep': entry 1 lists 2 modules ('test_fixture', "
                "'test_event_producer'); each entry must be exactly one 'name: mode' pair",
                "Failed to parse pre_timestep phase",
            ],
        ),
        (
            "named substep phase entry mapping with two modules",
            {
                "phases": {
                    "galaxy_physics": [
                        {"test_fixture": "process_by_galaxy"},
                        {
                            "test_event_producer": "process_full_halo",
                            "test_fixture": "process_full_halo",
                        },
                    ]
                }
            },
            [
                "Phase 'galaxy_physics': entry 2 lists 2 modules ('test_event_producer', "
                "'test_fixture'); each entry must be exactly one 'name: mode' pair",
                "Failed to parse substep phase 'galaxy_physics'",
            ],
        ),
        (
            "unknown mode",
            {"post_snapshot": [{"test_snapshot_fixture": "process_global"}]},
            [
                "Phase 'post_snapshot': module 'test_snapshot_fixture' has invalid processing "
                "mode 'process_global'",
                "Failed to parse post_snapshot phase",
            ],
        ),
        (
            "FoF mode in post_snapshot",
            {"post_snapshot": [{"test_fixture": "process_full_halo"}]},
            [
                "Configuration error in phase 'post_snapshot'",
                "Module 'test_fixture' is configured with processing mode 'process_full_halo'; "
                "only process_snapshot is allowed in this phase",
            ],
        ),
        (
            "process_snapshot in pre_timestep",
            {"pre_timestep": [{"test_snapshot_fixture": "process_snapshot"}]},
            _fof_phase_rejection("pre_timestep", "test_snapshot_fixture"),
        ),
        (
            "process_snapshot in post_timestep",
            {"post_timestep": [{"test_fixture": "process_snapshot"}]},
            _fof_phase_rejection("post_timestep", "test_fixture"),
        ),
        (
            "process_snapshot in a named substep phase",
            {"phases": {"galaxy_physics": [{"test_fixture": "process_snapshot"}]}},
            _fof_phase_rejection("galaxy_physics", "test_fixture"),
        ),
        (
            "duplicate entry",
            {
                "post_snapshot": [
                    {"test_snapshot_fixture": "process_snapshot"},
                    {"test_fixture": "process_snapshot"},
                    {"test_snapshot_fixture": "process_snapshot"},
                ]
            },
            [
                "Configuration error in phase 'post_snapshot'",
                "Module 'test_snapshot_fixture' is listed more than once",
                "Module system initialization failed",
            ],
        ),
        (
            "reserved substep phase name",
            {"phases": {"post_snapshot": fof}},
            ["Substep phase name 'post_snapshot' is reserved"],
        ),
        (
            "unknown module",
            {"post_snapshot": [{"no_such_module": "process_snapshot"}]},
            ["Unknown module 'no_such_module' in phase 'post_snapshot'"],
        ),
        (
            "module without the snapshot mode",
            {"post_snapshot": [{"test_event_producer": "process_snapshot"}]},
            [
                "Configuration error in phase 'post_snapshot'",
                "Module 'test_event_producer' does not support processing mode 'process_snapshot'",
            ],
        ),
    ]


def test_malformed_entries_fail_at_startup():
    """
    Test every illegal phase shape and entry fails before module init and dataset open.

    Includes entry mappings that name two modules, in post_snapshot, pre_timestep and a
    named substep phase, which must be rejected rather than silently reduced to their
    first pair (that would drop the second module).

    Expected: non-zero exit with the named phase/module diagnostic; no module init() line,
              no dataset opened, no snapshot loaded. Shape and mode-name errors stop the
              parser (a two-module entry included); phase/mode legality, duplicate
              post_snapshot entries, unknown modules and unsupported modes stop
              module_system_init(), which runs before any init(). Every case runs under the
              horizontal driver's configuration, so a non-empty post_snapshot reaches those
              checks: a vertical package is pointed at the committed v2 fixture's input
              settings, and the dataset is never opened.
    """
    overrides = None if selected_package_is_horizontal() else HORIZONTAL_INPUT
    for label, modules, needles in _malformed_cases():
        param_file, _output_dir = make_run(
            f"malformed_{re.sub(r'[^a-z0-9]+', '_', label)}",
            model_params=fixture_params(),
            input_overrides=overrides,
            snapshot_list=[] if overrides else None,
            modules_override=modules,
        )
        returncode, stdout, stderr = run_mimic(param_file)
        assert_startup_rejection(returncode, stdout, stderr, needles, label)
        print(f"  ✓ {label}: rejected at startup")


def test_vertical_driver_rejects_non_empty_phase():
    """
    Test the vertical driver accepts an empty post_snapshot and rejects a non-empty one.

    Expected (non-empty): non-zero exit, the diagnostic naming modules.post_snapshot, its
              first module and the vertical reader the run file selects (read from it, so
              any vertical package's reader is named correctly); no module init() and no
              tree file read.
    Expected (empty, []): the run completes on the vertical driver. On a vertical package
              it processes the package's own test tree; on a horizontal package the vertical
              input points at an empty directory (the compiled halo layout is not L-Halo's),
              so the vertical driver runs, skips the missing tree file and completes.
    """
    vertical_package = not selected_package_is_horizontal()
    overrides = None
    if not vertical_package:
        empty_dir = Path(TEMP_DIR) / "no_tree_files"
        empty_dir.mkdir(exist_ok=True)
        overrides = dict(VERTICAL_INPUT, simulation_dir=str(empty_dir))

    param_file, _ = make_run(
        "vertical_non_empty",
        phase_config={"post_snapshot": [("test_snapshot_fixture", "process_snapshot")]},
        input_overrides=overrides,
    )
    reader = run_tree_type(param_file)
    returncode, stdout, stderr = run_mimic(param_file)
    assert_startup_rejection(
        returncode,
        stdout,
        stderr,
        [
            "modules.post_snapshot lists 1 module (first: 'test_snapshot_fixture'), but it runs "
            f"only under the horizontal driver and reader '{reader}' feeds the vertical driver",
            "Parameter validation failed",
        ],
        "non-empty post_snapshot under the vertical driver",
    )

    param_file, _ = make_run(
        "vertical_empty",
        model_params=fixture_params(),
        input_overrides=overrides,
        modules_override={
            "phases": {"galaxy_physics": [{"test_fixture": "process_by_galaxy"}]},
            "post_snapshot": [],
        },
    )
    returncode, stdout, stderr = run_mimic(param_file)
    output = stdout + stderr
    assert "modules.post_snapshot lists" not in output, f"[] must be accepted:\n{output}"
    assert_ok(returncode, stdout, stderr, "empty post_snapshot under the vertical driver")
    assert "Opened horizontal run" not in stdout, "the run must use the vertical driver"
    processing = re.search(r"Processing \d+ input files? \(first_file=", stdout)
    assert processing, f"the vertical driver must run:\n{output}"
    if not vertical_package:
        assert "Missing tree" in stdout, f"no tree file may be read here:\n{output}"
    print("  ✓ vertical driver: [] accepted, a configured snapshot module rejected at startup")


def test_horizontal_restrictions_still_apply():
    """
    Test the horizontal HDF5-only and no-resume restrictions hold with post_snapshot set.

    Expected: binary output and --skip are each rejected at configuration, alongside a
              configured snapshot module, before any module init() or dataset open. On a
              vertical package the run is pointed at the committed v2 fixture's input
              settings for this configuration-only check; the dataset is never opened.
    """
    overrides = None if selected_package_is_horizontal() else HORIZONTAL_INPUT
    phase = {"post_snapshot": [("test_snapshot_fixture", "process_snapshot")]}

    snapshot_list = [] if overrides else None
    param_file, _ = make_run(
        "restriction_binary",
        phase_config=phase,
        input_overrides=overrides,
        snapshot_list=snapshot_list,
        output_format="binary",
    )
    returncode, stdout, stderr = run_mimic(param_file)
    assert_startup_rejection(
        returncode,
        stdout,
        stderr,
        ["output_format is 'binary', but horizontal runs are HDF5-only"],
        "binary output",
    )

    param_file, _ = make_run(
        "restriction_skip",
        phase_config=phase,
        input_overrides=overrides,
        snapshot_list=snapshot_list,
    )
    returncode, stdout, stderr = run_mimic(param_file, extra_args=["--skip"])
    assert_startup_rejection(
        returncode,
        stdout,
        stderr,
        ["--skip was given, but resume is not supported for horizontal runs"],
        "--skip",
    )
    print("  ✓ HDF5-only and no-resume restrictions still reject with post_snapshot configured")


# ---------------------------------------------------------------------------
# Horizontal execution (horizontal packages with a committed fixture)
# ---------------------------------------------------------------------------


#: Runs shared by more than one test, keyed by run name (see shared_run()).
_SHARED_RUNS = {}


def shared_run(name, **make_run_kwargs):
    """Run one configuration through the executable once per suite and cache the result.

    Tests whose configurations are identical share the run and each makes its own
    assertions on the cached (returncode, stdout, stderr, output_dir, param_file). A name
    always maps to one configuration: only the fixed helpers below call this.
    """
    if name not in _SHARED_RUNS:
        param_file, output_dir = make_run(name, **make_run_kwargs)
        returncode, stdout, stderr = run_mimic(param_file)
        _SHARED_RUNS[name] = (returncode, stdout, stderr, output_dir, param_file)
    return _SHARED_RUNS[name]


def _two_callback_run(name, snapshot_list):
    """post_snapshot [test_snapshot_fixture, test_fixture], writing `snapshot_list`."""
    return shared_run(
        name,
        phase_config={
            "post_snapshot": [
                ("test_snapshot_fixture", "process_snapshot"),
                ("test_fixture", "process_snapshot"),
            ]
        },
        model_params=fixture_params(),
        snapshot_list=list(snapshot_list),
    )


def order_output_snapshots():
    """The output snapshots of the shared ordering run: the middle and the final one."""
    nsnap = len(fixture_scale_factors())
    return sorted({nsnap // 2, nsnap - 1})


def _order_run():
    """The two-callback run shared by the ordering and provenance tests."""
    return _two_callback_run("order", order_output_snapshots())


def _population_run():
    """post_snapshot [test_snapshot_fixture], every snapshot written.

    Shared by the population and gapped-inheritance tests.
    """
    nsnap = len(fixture_scale_factors())
    return shared_run(
        "population",
        phase_config={"post_snapshot": [("test_snapshot_fixture", "process_snapshot")]},
        snapshot_list=list(range(nsnap)),
    )


def test_callbacks_run_once_per_snapshot_in_yaml_order():
    """
    Test two callbacks run exactly once per input snapshot, in YAML order, at the seam.

    Setup: post_snapshot [test_snapshot_fixture, test_fixture], output only at the middle
           and final snapshots, so snapshot 0 and the others are non-output snapshots.
    Expected: one marker pair per snapshot 0..N-1, in snapshot order, each
              test_snapshot_fixture marker before its test_fixture marker; both inside that
              snapshot's block, after its load and before any generation release or output
              write in the block; empty snapshots called with count 0; the context's redshift
              is the snapshot's own and its lookback time decreases; each module initialised
              and cleaned up once; event emission rejected at every call.
    """
    require_horizontal_fixture("the once-per-snapshot ordering case")
    scale_factors = fixture_scale_factors()
    nsnap = len(scale_factors)
    halo_counts = fixture_halo_counts()
    selected = order_output_snapshots()
    returncode, stdout, stderr, _output_dir, _ = _order_run()
    assert_ok(returncode, stdout, stderr, "two-callback run")

    first = snapshot_markers(stdout)
    second = dual_markers(stdout)
    assert [m["snapshot"] for m in first] == list(range(nsnap)), [m["snapshot"] for m in first]
    assert [m["snapshot"] for m in second] == list(range(nsnap)), [m["snapshot"] for m in second]
    assert [m["call"] for m in first] == list(range(1, nsnap + 1)), "one call per snapshot"

    for snap, (start, end) in enumerate(snapshot_blocks(stdout, nsnap)):
        a, b = first[snap], second[snap]
        assert start < a["pos"] < b["pos"] < end, f"snapshot {snap}: callbacks out of order/block"
        block = stdout[start:end]
        callbacks_end = b["pos"] - start
        for needle in ("Released snapshot ", f"Wrote snapshot {snap} output"):
            hit = block.find(needle)
            assert hit < 0 or hit > callbacks_end, f"snapshot {snap}: {needle!r} before callbacks"
        assert a["count"] == b["count"], f"snapshot {snap}: both callbacks see one population"
        if halo_counts[snap] == 0:
            assert a["count"] == 0, f"snapshot {snap} is empty and is still called with count 0"
        assert abs(a["z"] - (1.0 / scale_factors[snap] - 1.0)) < 6e-5, f"snapshot {snap} z"
        if snap > 0:
            assert a["time"] < first[snap - 1]["time"], "lookback time decreases toward z=0"

    empty = [snap for snap, count in enumerate(halo_counts) if count == 0]
    assert empty, "the committed fixture has an empty snapshot"
    assert set(empty) - set(selected), "an empty non-output snapshot is called too"
    assert stdout.count("Wrote snapshot") == len(selected), "only selected snapshots written"
    assert stdout.count("Test snapshot fixture module initialized") == 1, "init once"
    assert stdout.count("Test fixture module initialized") == 1, "dual-mode init once"
    assert f"TEST_SNAPSHOT_FIXTURE_CLEANUP: total_calls={nsnap}" in stdout, "cleanup once"
    assert f"TEST_FIXTURE_SNAPSHOT_CLEANUP: total_snapshot_executions={nsnap}" in stdout
    assert (stdout + stderr).count(_EMIT_REJECTION) == nsnap, "emission rejected at every call"
    print(f"  ✓ {nsnap} snapshots × 2 callbacks in YAML order at the post-sweep seam")


def test_snapshot_phase_runs_yaml_order_not_pipeline_order():
    """
    Test post_snapshot runs its own YAML order when the pipeline order differs.

    Setup: test_fixture process_by_galaxy in galaxy_physics, so it joins the pipeline
           first, and post_snapshot [test_snapshot_fixture, test_fixture], which lists it
           second; only the final snapshot written.
    Expected: at every snapshot, test_snapshot_fixture's marker precedes test_fixture's
              TEST_FIXTURE_SNAPSHOT_EXEC marker inside that snapshot's block; test_fixture is
              initialised once; EnabledModules records the FoF row and then the two
              post_snapshot rows in YAML order.
    """
    require_horizontal_fixture("the YAML-versus-pipeline order case")
    nsnap = len(fixture_scale_factors())
    param_file, output_dir = make_run(
        "pipeline_order",
        phase_config={
            "galaxy_physics": [("test_fixture", "process_by_galaxy")],
            "post_snapshot": [
                ("test_snapshot_fixture", "process_snapshot"),
                ("test_fixture", "process_snapshot"),
            ],
        },
        model_params=fixture_params(),
        snapshot_list=[nsnap - 1],
    )
    returncode, stdout, stderr = run_mimic(param_file)
    assert_ok(returncode, stdout, stderr, "FoF-plus-snapshot run")

    first = snapshot_markers(stdout)
    second = dual_markers(stdout)
    assert [m["snapshot"] for m in first] == list(range(nsnap)), [m["snapshot"] for m in first]
    assert [m["snapshot"] for m in second] == list(range(nsnap)), [m["snapshot"] for m in second]
    for snap, (start, end) in enumerate(snapshot_blocks(stdout, nsnap)):
        message = f"snapshot {snap}: test_snapshot_fixture must run before test_fixture"
        assert start < first[snap]["pos"] < second[snap]["pos"] < end, message
    assert stdout.count("Test fixture module initialized") == 1, "dual-mode init once"
    master = Path(output_dir) / "model.hdf5"
    assert enabled_modules(master) == [
        ("test_fixture", "galaxy_physics", "process_by_galaxy"),
        ("test_snapshot_fixture", "post_snapshot", "process_snapshot"),
        ("test_fixture", "post_snapshot", "process_snapshot"),
    ], f"EnabledModules {enabled_modules(master)}"
    print(f"  ✓ {nsnap} snapshots run post_snapshot in YAML order, not pipeline order")


def test_writes_reach_the_next_callback_and_descendants():
    """
    Test callback writes reach the next callback and the galaxies descendants inherit.

    Setup: the two-callback phase; test_snapshot_fixture writes 0.5, then test_fixture 0.25.
    Expected: test_fixture always finds 0.5 everywhere (inter-callback visibility); at each
              later snapshot test_snapshot_fixture finds 0.25 on every galaxy inherited from
              an earlier snapshot and 0 on every new one, so seen_zero equals the galaxies
              whose UniqueGalaxyID no earlier snapshot wrote. On a gapped fixture that holds
              across the empty snapshot too.
    """
    require_horizontal_fixture("the write-visibility case")
    nsnap = len(fixture_scale_factors())
    returncode, stdout, stderr, output_dir, _ = _two_callback_run("visibility", range(nsnap))
    assert_ok(returncode, stdout, stderr, "visibility run")
    first = snapshot_markers(stdout)
    second = dual_markers(stdout)

    earlier_ids = set()
    inherited_total = 0
    for snap in range(nsnap):
        rows = output_rows(output_dir, snap)
        ids = [int(value) for value in rows["UniqueGalaxyID"]]
        fresh = sum(1 for value in ids if value not in earlier_ids)
        inherited = len(ids) - fresh
        inherited_total += inherited
        a, b = first[snap], second[snap]
        if a["count"] > 0:
            message = f"snapshot {snap}: test_fixture must see test_snapshot_fixture's write: {b}"
            assert b["seen_min"] == b["seen_max"] == SNAPSHOT_FIXTURE_VALUE, message
        assert a["seen_zero"] == fresh, f"snapshot {snap}: {a['seen_zero']} unset vs {fresh} new"
        expected_max = DUAL_FIXTURE_VALUE if inherited else 0.0
        assert a["seen_max"] == expected_max, f"snapshot {snap}: inherited value {a['seen_max']}"
        earlier_ids.update(ids)
    assert inherited_total > 0, "the fixture must carry galaxies across snapshots"
    print(f"  ✓ writes visible to the next callback and to {inherited_total} inherited galaxies")


def test_population_is_the_current_generation():
    """
    Test the callback population is exactly the snapshot's own output, nothing else.

    Setup: post_snapshot [test_snapshot_fixture]; every snapshot written.
    Expected: per snapshot, the marker's count, Type 0/1/2 counts and UniqueGalaxyID sum equal
              the written rows; no entry of another Type; every entry's SnapNum is the
              snapshot's own, including while an older generation is still retained (its
              release is logged after the callback); Type 2 galaxies are included somewhere.
              The "no Type 3" checks are a sanity check only: halos-only never creates a
              Type 3 galaxy, so they cannot fail here. Type 3 exclusion is proven by the unit
              test test_post_snapshot_population_excludes_type3 over the real marshaller.
    """
    require_horizontal_fixture("the current-generation population case")
    nsnap = len(fixture_scale_factors())
    returncode, stdout, stderr, output_dir, _ = _population_run()
    assert_ok(returncode, stdout, stderr, "population run")
    markers = snapshot_markers(stdout)
    assert [m["snapshot"] for m in markers] == list(range(nsnap))

    type2_total = 0
    retained_checks = 0
    blocks = snapshot_blocks(stdout, nsnap)
    for snap in range(nsnap):
        rows = output_rows(output_dir, snap)
        types = [int(value) for value in rows["Type"]]
        marker = markers[snap]
        # Sanity check only (halos-only creates no Type 3); see the docstring.
        assert 3 not in types, f"snapshot {snap}: a Type 3 galaxy was written"
        assert marker["count"] == len(rows), f"snapshot {snap}: count {marker['count']}"
        assert (marker["t0"], marker["t1"], marker["t2"]) == (
            types.count(0),
            types.count(1),
            types.count(2),
        ), f"snapshot {snap}: Type counts {marker} vs {types}"
        assert marker["other"] == 0, f"snapshot {snap}: an entry of another Type was handed over"
        assert marker["foreign"] == 0, f"snapshot {snap}: an entry of another snapshot"
        assert marker["id_sum"] == id_sum(rows), f"snapshot {snap}: population differs"
        type2_total += marker["t2"]

        # Older generations still retained when this snapshot's callback ran are
        # released later in its block; the population must not be theirs.
        _start, end = blocks[snap]
        block = stdout[marker["pos"] : end]
        for earlier in range(snap):
            if f"Released snapshot {earlier} " in block and len(output_rows(output_dir, earlier)):
                retained_checks += 1
    assert type2_total > 0, "the fixture must hand Type 2 orphans to the callback"
    assert retained_checks > 0, "an older populated generation must be retained during a call"
    print(
        f"  ✓ population == written rows at all {nsnap} snapshots; {type2_total} Type 2 entries; "
        f"{retained_checks} calls ran beside an older retained generation"
    )


def test_gapped_inheritance_skips_the_empty_snapshot():
    """
    Test inheritance across an empty-snapshot gap, from the right generation.

    Setup: the population test's shared run (post_snapshot [test_snapshot_fixture]; every
           snapshot written). Applies to a fixture with links_adjacent = 0 (the v3 worked
           graph). On an adjacent fixture there is no gap to cross, and inheritance from each
           predecessor is asserted at every snapshot by
           test_writes_reach_the_next_callback_and_descendants, so this case has nothing
           further to check and says so.
    Expected: the empty snapshot is called with count 0; the first populated snapshot after it
              finds every inherited galaxy carrying 0.5 (written at the snapshot before the
              gap, the only earlier write), with every entry's SnapNum its own while the
              pre-gap generation is still retained.
    """
    fixture = require_horizontal_fixture("the gapped inheritance case")
    import h5py

    with h5py.File(fixture[0] / "snapshot_000.h5", "r") as handle:
        adjacent = int(handle["header"].attrs["links_adjacent"])
    if adjacent:
        print(
            "  ✓ adjacent fixture: no gap to cross; per-predecessor inheritance is asserted by "
            "test_writes_reach_the_next_callback_and_descendants"
        )
        return

    nsnap = len(fixture_scale_factors())
    counts = fixture_halo_counts()
    returncode, stdout, stderr, output_dir, _ = _population_run()
    assert_ok(returncode, stdout, stderr, "gapped run")
    markers = snapshot_markers(stdout)

    gaps = [snap for snap in range(1, nsnap - 1) if counts[snap] == 0]
    assert gaps, "a gapped fixture must have an empty interior snapshot"
    for gap in gaps:
        assert markers[gap]["count"] == 0, f"empty snapshot {gap} is called with count 0"
        after = gap + 1
        before_ids = {int(v) for v in output_rows(output_dir, gap - 1)["UniqueGalaxyID"]}
        after_ids = [int(v) for v in output_rows(output_dir, after)["UniqueGalaxyID"]]
        crossed = [value for value in after_ids if value in before_ids]
        assert crossed, f"galaxies must cross the gap at snapshot {gap}"
        assert markers[after]["seen_max"] == SNAPSHOT_FIXTURE_VALUE, "the write crosses the gap"
        assert markers[after]["seen_zero"] == len(after_ids) - len(crossed)
        assert markers[after]["foreign"] == 0, "the population is the descendant snapshot's own"
        tail = stdout[markers[after]["pos"] :]
        message = (
            f"generation {gap - 1} must still be retained when snapshot {after}'s callback runs"
        )
        assert f"Released snapshot {gap - 1} " in tail, message
    print(f"  ✓ writes inherited across the empty snapshot(s) {gaps} from the pre-gap generation")


def test_substep_schemes_do_not_change_snapshot_calls():
    """
    Test the snapshot phase is independent of the substep scheme.

    Setup: test_fixture process_by_galaxy in a substep phase plus test_snapshot_fixture in
           post_snapshot, under fixed SubSteps 1, fixed SubSteps 4 and dynamic SubSteps 2.
    Expected: the FoF markers show the different substep counts; the snapshot markers
              (snapshot, count, Types, UniqueGalaxyID sum) are identical and once per
              snapshot in all three; EnabledModules is the FoF row then the post_snapshot row.
    """
    require_horizontal_fixture("the substep-scheme case")
    nsnap = len(fixture_scale_factors())
    phases = {
        "galaxy_physics": [("test_fixture", "process_by_galaxy")],
        "post_snapshot": [("test_snapshot_fixture", "process_snapshot")],
    }
    signatures = {}
    substep_counts = {}
    for label, substeps, scheme in (
        ("fixed1", 1, "fixed"),
        ("fixed4", 4, "fixed"),
        ("dyn", 2, "dynamic"),
    ):
        param_file, output_dir = make_run(
            f"substeps_{label}",
            phase_config=phases,
            model_params=fixture_params(),
            substeps=substeps,
            timestep_scheme=scheme,
        )
        returncode, stdout, stderr = run_mimic(param_file)
        assert_ok(returncode, stdout, stderr, f"{label} run")
        markers = snapshot_markers(stdout)
        assert [m["snapshot"] for m in markers] == list(range(nsnap)), f"{label}: once each"
        signatures[label] = [
            (m["snapshot"], m["count"], m["t0"], m["t1"], m["t2"], m["id_sum"]) for m in markers
        ]
        substep_counts[label] = {int(n) for n in _FOF_FIXTURE_PATTERN.findall(stdout)}
        master = Path(output_dir) / "model.hdf5"
        assert enabled_modules(master) == [
            ("test_fixture", "galaxy_physics", "process_by_galaxy"),
            ("test_snapshot_fixture", "post_snapshot", "process_snapshot"),
        ], f"{label}: EnabledModules {enabled_modules(master)}"
    assert substep_counts["fixed1"] == {1}, substep_counts
    assert substep_counts["fixed4"] == {4}, substep_counts
    assert substep_counts["dyn"], "the dynamic run must execute FoF substeps"
    assert signatures["fixed4"] == signatures["fixed1"], "fixed substeps change no snapshot call"
    assert signatures["dyn"] == signatures["fixed1"], "dynamic substeps change no snapshot call"
    print(
        f"  ✓ snapshot calls identical under substep counts {sorted(substep_counts['fixed1'])}, "
        f"{sorted(substep_counts['fixed4'])} and dynamic {sorted(substep_counts['dyn'])}"
    )


def test_provenance_records_the_phase():
    """
    Test EnabledModules and the copied run file record the configured snapshot modules.

    Setup: the ordering test's shared two-callback run (final snapshot among its outputs).
    Expected: the final partition and the master carry EnabledModules
              [(test_snapshot_fixture, post_snapshot, process_snapshot),
               (test_fixture, post_snapshot, process_snapshot)] in that order, and the copied
              run YAML under metadata/ carries the same post_snapshot list.
    """
    require_horizontal_fixture("the provenance case")
    nsnap = len(fixture_scale_factors())
    returncode, stdout, stderr, output_dir, param_file = _order_run()
    assert_ok(returncode, stdout, stderr, "provenance run")
    assert nsnap - 1 in order_output_snapshots(), "the shared run writes the final snapshot"
    expected = [
        ("test_snapshot_fixture", "post_snapshot", "process_snapshot"),
        ("test_fixture", "post_snapshot", "process_snapshot"),
    ]
    for path in [Path(output_dir) / f"model_{nsnap - 1:03d}.hdf5", Path(output_dir) / "model.hdf5"]:
        assert enabled_modules(path) == expected, f"{path.name}: {enabled_modules(path)}"
    copied = Path(output_dir) / "metadata" / Path(param_file).name
    assert copied.is_file(), f"the run file must be copied to {copied}"
    with open(copied, "r") as handle:
        modules = yaml.safe_load(handle)["modules"]
    assert modules["post_snapshot"] == [
        {"test_snapshot_fixture": "process_snapshot"},
        {"test_fixture": "process_snapshot"},
    ], modules
    assert modules["parameters"]["TestFixtureDummyParameter"] == DUAL_FIXTURE_VALUE
    print("  ✓ EnabledModules and the copied run file record post_snapshot in execution order")


def test_failing_callback_aborts_and_cleans_up():
    """
    Test a non-zero callback return fails the run and the driver's failure cleanup runs.

    Setup: the two-callback phase with TestFixtureDummyParameter = 2.5, outside
           TestDummyProperty's [0, 1] range, so test_fixture refuses the first non-empty
           population with return code 2; every snapshot selected for output.
    Expected: non-zero exit naming test_fixture, post_snapshot, that snapshot and code 2;
              test_snapshot_fixture ran there first, test_fixture logged no success there; the
              snapshot was never written and the next never loaded; every retained
              generation released and the reader's run closed with no slab loaded; no master
              file and no partition at or after the failing snapshot.
    """
    require_horizontal_fixture("the callback-failure case")
    nsnap = len(fixture_scale_factors())
    failing = next(snap for snap, count in enumerate(fixture_halo_counts()) if count > 0)
    param_file, output_dir = make_run(
        "failure",
        phase_config={
            "post_snapshot": [
                ("test_snapshot_fixture", "process_snapshot"),
                ("test_fixture", "process_snapshot"),
            ]
        },
        model_params=fixture_params(2.5),
        snapshot_list=range(nsnap),
    )
    returncode, stdout, stderr = run_mimic(param_file)
    output = stdout + stderr
    assert returncode != 0, f"a failing snapshot callback must fail the run:\n{output}"
    message = (
        f"Module 'test_fixture' failed in phase 'post_snapshot' at snapshot {failing} with "
        f"return code {DUAL_FIXTURE_RANGE_ERROR}"
    )
    assert message in output, f"missing {message!r}:\n{output}"
    assert [m["snapshot"] for m in snapshot_markers(stdout)] == list(range(failing + 1))
    assert all(m["snapshot"] < failing for m in dual_markers(stdout)), "no success at failure"
    assert f"Wrote snapshot {failing} output" not in stdout, "the failed snapshot is not written"
    assert f"Loaded snapshot {failing + 1} (" not in stdout, "the run stops at the failure"
    assert "Horizontal driver exiting early: releasing" in stdout, f"no failure cleanup:\n{output}"
    assert "Closed horizontal run 'horizontal_hdf5' with no slab loaded" in stdout
    # The two log lines above are the evidence that the failure cleanup ran. The output-file
    # checks below hold by construction (a snapshot's partition is written only after its
    # callbacks, and the master only at the end), so they are not cleanup proof.
    assert not (Path(output_dir) / "model.hdf5").exists(), "no master file after a failure"
    left = sorted(int(p.stem.split("_")[-1]) for p in Path(output_dir).glob("model_*.hdf5"))
    assert all(snap < failing for snap in left), f"partitions left at/after the failure: {left}"
    print(f"  ✓ failure at snapshot {failing} reported and cleaned up (partitions left: {left})")


def main():
    global TEMP_DIR
    TEMP_DIR = Path(tempfile.mkdtemp(prefix="mimic_snapshot_phase_"))
    try:
        tests = [
            test_absent_null_and_empty_phase_change_nothing,
            test_malformed_entries_fail_at_startup,
            test_vertical_driver_rejects_non_empty_phase,
            test_horizontal_restrictions_still_apply,
            test_callbacks_run_once_per_snapshot_in_yaml_order,
            test_snapshot_phase_runs_yaml_order_not_pipeline_order,
            test_writes_reach_the_next_callback_and_descendants,
            test_population_is_the_current_generation,
            test_gapped_inheritance_skips_the_empty_snapshot,
            test_substep_schemes_do_not_change_snapshot_calls,
            test_provenance_records_the_phase,
            test_failing_callback_aborts_and_cleans_up,
        ]
        return run_test_suite(tests, "Snapshot Phase (modules.post_snapshot)")
    finally:
        shutil.rmtree(TEMP_DIR)


if __name__ == "__main__":
    sys.exit(main())
