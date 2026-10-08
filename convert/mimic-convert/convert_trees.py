"""Generic CLI for the generalised merger-tree converter.

Converts any of the three supported vertical source formats into lossless
horizontal-HDF5 **format version 3** (convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md, section "Version 3")
through six explicit subcommands over a user-supplied ``--workdir``:

  inspect    read-only: resolve the profile against the named source inventory
             and report counts, layout, record widths and estimated output size
  ingest     freeze the configuration into the workdir (first call) and stream
             the source into verified canonical chunks; resumable
  transpose  per-snapshot sort and 64-bit link remapping; resumable
  write      emit snapshot_NNN.h5 + forests.h5 in format version 3
  validate   run the version 3 producer battery over the written dataset
  report     validate and write conversion_report.json/.txt into the workdir

**Explicit inputs, no inference.** ``--source-format`` is always named, never
guessed from a file; the profile's own ``source_format`` and the simulation
metadata's ``input.tree_type`` must both agree with it. The inventory is always
named: an explicit file range for L-Halo binary and forests-HDF5, explicit tree
files for Consistent-Trees ASCII. ``simulation_info.yaml``'s declared
``first_file``/``last_file`` is the package's whole catalog, not a request -- a
requested range must lie inside it, and every requested file must exist.
``--column-map`` is optional; without it the format's shipped default profile
(``convert/mimic-convert/profiles/<source_format>.yaml``) is used and the output says
so. A named profile that fails to load or validate is fatal: there is no
fallback to the default.

**Package routes.** Every requested simulation package carries its own profile,
``simulations/<package>/converter_columns.yaml``:

  mini-millennium, millennium, micro-uchuu, mini-uchuu    lhalo_binary
  uchuu, micro-uchuu-hdf5                                  consistent_trees_hdf5
  micro-uchuu-ascii                                        consistent_trees_ascii

Profiles select source fields and describe the source layout. They never
change a run YAML, a ``halo_properties.yaml`` or a ``snapshots`` symlink, and
this CLI writes nothing outside ``--workdir``.

**Format versions.** This CLI emits version 3 only. The existing
Consistent-Trees ASCII to version 2 workflow is unchanged and lives in
``convert_ctrees.py``, whose commands keep their version 2 defaults.

**Conversion is not route validation.** Mimic's horizontal_hdf5 reader and
horizontal driver consume format version 3, but a converted dataset is an
evidenced runtime route only where a recorded parity gate passed against the
same source format's vertical reader over the same files. The evidenced
routes are the five in runtime_routes.ROUTES, which mirrors the table under
convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md#v3-runtime-support; full Uchuu is not
runnable. Every stage says so, and says whether the dataset has non-adjacent
(gapped) Descendant links or a snapshot wider than int32.

**Restart.** Each stage records its state in ``<workdir>/manifest.json``
(``conversion_manifest.py``, version 3). ``ingest`` resumes at chunk
granularity and proves that every already-ingested chunk is still what the
source yields before appending; ``transpose`` and ``write`` are all-or-nothing
per attempt, an interrupted attempt being discarded before the next begins.
Resume reads the configuration embedded in the manifest, so a resumed
``ingest`` needs only ``--workdir`` (the profile file may since have been edited
or removed); an ``ingest`` that names a configuration must name the recorded
one exactly, or it is refused before anything is touched. Re-running a
completed stage re-verifies that stage's own artifact checksums only -- it is
not a revalidation of the stages after it; ``validate`` is.

**Memory.** ``--memory-budget-mb`` is a ceiling on the converter's budgeted
working buffers: adapter read and validation buffers, the per-unit inventory,
the ASCII route's rank pass, and the transpose's external-sort buffers. Each
budgeted term is checked against it before that term is allocated, and a
conversion whose terms cannot fit is refused rather than run. It does not bound
the Python interpreter's total resident size, and concurrently resident terms
are not summed against it. ``validate``/``report`` take their own budget for
the battery's bounded sorts.

**Sources are read-only.** Source files are opened for reading only and pinned
in the manifest. There is no source release, transfer or deletion for these
routes; the only opt-in deletions are ``--consume-ingest`` and
``--consume-transposed``, which remove manifest-owned intermediates inside the
workdir after their successor has been verified.

Usage (mini-Millennium, files 0-7):

    mimic_venv/bin/python convert/mimic-convert/convert_trees.py ingest \\
        --workdir output/convert/mini-millennium \\
        --source-format lhalo_binary \\
        --simulation-info simulations/mini-millennium/simulation_info.yaml \\
        --a-list simulations/mini-millennium/mini-millennium.a_list \\
        --column-map simulations/mini-millennium/converter_columns.yaml \\
        --halo-properties simulations/mini-millennium/halo_properties.yaml \\
        --source-dir simulations/mini-millennium/snapshots --tree-name trees_063 \\
        --first-file 0 --last-file 7
    convert_trees.py transpose --workdir output/convert/mini-millennium
    convert_trees.py write --workdir output/convert/mini-millennium \\
        --simulation-info simulations/mini-millennium/simulation_info.yaml
    convert_trees.py report --workdir output/convert/mini-millennium

Full Uchuu (forests-HDF5) names ``--info-file`` instead of a source directory;
micro-Uchuu ASCII names ``--forests-list`` and one ``--tree-file`` per file.
"""

import argparse
import json
import os
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import runtime_routes  # noqa: E402
from column_schema import SOURCE_FORMATS, ConverterError  # noqa: E402

CONVERT_DIR = Path(os.path.dirname(os.path.abspath(__file__)))
PROFILE_DIR = CONVERT_DIR / "profiles"

#: The only format version this CLI writes. Version 2 is convert_ctrees.py's.
FORMAT_VERSION = 3

DEFAULT_MEMORY_BUDGET_MB = 2048

_INT32_MAX = 2**31 - 1

#: Printed by every stage that reports on a conversion, so a successful
#: conversion can never be read as a validated route. The wording and the
#: route list come from runtime_routes, the converter's one copy of the
#: evidenced routes (the spec's V3 Runtime Support table is the authority).
RUNTIME_NOTICE = runtime_routes.runtime_notice()

#: Per-format inventory options: every one is required for its format and
#: rejected for the other two, so an option meant for another route can never
#: be silently ignored.
_INVENTORY_OPTIONS = {
    "lhalo_binary": ("source_dir", "tree_name", "first_file", "last_file", "halo_properties"),
    "consistent_trees_hdf5": ("info_file", "first_file", "last_file"),
    "consistent_trees_ascii": ("forests_list", "tree_file"),
}
#: Per-format tuning options: optional for their format, rejected for others.
_TUNING_OPTIONS = {
    "lhalo_binary": (),
    "consistent_trees_hdf5": (),
    "consistent_trees_ascii": ("pool_size", "chunksize"),
}
_ALL_FORMAT_OPTIONS = sorted(
    {name for names in _INVENTORY_OPTIONS.values() for name in names}
    | {name for names in _TUNING_OPTIONS.values() for name in names}
)
#: Everything that defines a conversion, for the ingest resume check.
_CONFIGURATION_OPTIONS = (
    "simulation_info",
    "a_list",
    "column_map",
    "memory_budget_mb",
    "ingest_max_rows",
) + tuple(_ALL_FORMAT_OPTIONS)


def _flag(dest: str) -> str:
    return "--" + dest.replace("_", "-")


# ==========================================================================
# Configuration from arguments
# ==========================================================================


def _check_format_options(args) -> None:
    """Every inventory option of the named format is present; no option of
    another format is."""
    source_format = args.source_format
    required = _INVENTORY_OPTIONS[source_format]
    allowed = set(required) | set(_TUNING_OPTIONS[source_format])
    missing = [_flag(name) for name in required if getattr(args, name, None) is None]
    foreign = [
        _flag(name)
        for name in _ALL_FORMAT_OPTIONS
        if name not in allowed and getattr(args, name, None) is not None
    ]
    if foreign:
        raise ConverterError(
            "{} do not apply to --source-format {}; refusing rather than ignoring them".format(
                ", ".join(foreign), source_format
            )
        )
    if missing:
        raise ConverterError(
            "--source-format {} needs an explicit inventory: missing {}".format(
                source_format, ", ".join(missing)
            )
        )


def _load_simulation_info(path, source_format: str):
    """The package metadata, required to declare the named format."""
    from adapters.source_inventory import load_simulation_info

    info = load_simulation_info(path)
    if info.tree_type != source_format:
        raise ConverterError(
            "{}: input.tree_type is {!r}, not --source-format {!r}; the source format is never "
            "inferred or overridden".format(path, info.tree_type, source_format)
        )
    return info


def _check_file_range(args, info) -> None:
    first, last = args.first_file, args.last_file
    if first < 0 or last < first:
        raise ConverterError(
            "--first-file {} --last-file {} is not a nonempty ascending range".format(first, last)
        )
    if first < info.first_file or last > info.last_file:
        raise ConverterError(
            "requested files {}-{} lie outside the {}-{} that {} declares for this package".format(
                first, last, info.first_file, info.last_file, info.path
            )
        )


def _reference_particle_mass(path) -> float:
    """``simulation.particle_mass`` in 1e10 Msun/h, exactly as written, after
    the writer's own unit validation: the forests-HDF5 route derives ``Len``
    from it and the v3 writer refuses a header that disagrees."""
    import yaml
    from hdf5_writer import load_header_metadata

    load_header_metadata(path)
    with open(path) as handle:
        return float(yaml.safe_load(handle)["simulation"]["particle_mass"]["value"])


def _profile_path(args) -> Path:
    if args.column_map is not None:
        return Path(args.column_map)
    return PROFILE_DIR / "{}.yaml".format(args.source_format)


def _build_schema(args):
    from column_schema import build_schema, load_column_map, load_source_properties

    path = _profile_path(args)
    column_map = load_column_map(path)
    if column_map.source_format != args.source_format:
        raise ConverterError(
            "{}: profile declares source_format {!r}, not --source-format {!r}".format(
                path, column_map.source_format, args.source_format
            )
        )
    properties = None
    if args.source_format == "lhalo_binary":
        properties = load_source_properties(args.halo_properties)
    return build_schema(column_map, properties)


def _megabytes(value) -> int:
    """``--memory-budget-mb`` in bytes; ``None`` (not given) is the default."""
    megabytes = DEFAULT_MEMORY_BUDGET_MB if value is None else value
    if megabytes <= 0:
        raise ConverterError("--memory-budget-mb must be positive, got {}".format(megabytes))
    return megabytes << 20


def _adapter_parameters(args, info, budget_bytes: int) -> dict:
    source_format = args.source_format
    if source_format == "lhalo_binary":
        _check_file_range(args, info)
        source_dir = Path(args.source_dir)
        return {
            "sources": [
                [ordinal, str(source_dir / "{}.{}".format(args.tree_name, ordinal))]
                for ordinal in range(args.first_file, args.last_file + 1)
            ],
            "memory_budget_bytes": budget_bytes,
            "simulation_info": args.simulation_info,
        }
    if source_format == "consistent_trees_hdf5":
        _check_file_range(args, info)
        return {
            "info_path": args.info_file,
            "first_file": args.first_file,
            "last_file": args.last_file,
            "particle_mass": _reference_particle_mass(args.simulation_info),
            "memory_budget_bytes": budget_bytes,
            "simulation_info": args.simulation_info,
        }
    parameters = {
        "tree_files": list(args.tree_file),
        "forests_list": args.forests_list,
        "simulation_info": args.simulation_info,
        "memory_budget_bytes": budget_bytes,
        "rank_budget_bytes": budget_bytes,
    }
    if args.pool_size is not None:
        parameters["pool_size"] = args.pool_size
    if args.chunksize is not None:
        parameters["chunksize"] = args.chunksize
    return parameters


def _resolve_configuration(args):
    """Profile schema, canonical adapter parameters and the snapshot count
    for one explicitly named conversion. Read-only."""
    import pipeline
    from scatter import load_a_list

    _check_format_options(args)
    info = _load_simulation_info(args.simulation_info, args.source_format)
    budget_bytes = _megabytes(args.memory_budget_mb)
    schema = _build_schema(args)
    parameters = pipeline.canonical_parameters(
        args.source_format, _adapter_parameters(args, info, budget_bytes)
    )
    scale_factors, _md5 = load_a_list(args.a_list)
    if len(scale_factors) == 0:
        raise ConverterError("{}: the a_list declares no snapshots".format(args.a_list))
    return schema, parameters, len(scale_factors), budget_bytes


def _announce_profile(args, schema, *, file=sys.stdout) -> None:
    origin = "shipped default" if args.column_map is None else "named"
    print(
        "profile: {} {} ({} extra field(s); column_mapping_sha256 {})".format(
            origin, _profile_path(args), len(schema.extra_fields), schema.digest
        ),
        file=file,
    )


# ==========================================================================
# Stage summaries
# ==========================================================================


def _print_topology(n_gapped: int, max_span: int, adjacent: bool, counts) -> None:
    if adjacent:
        print("topology: links_adjacent=1 -- every non-null Descendant link is adjacent")
    else:
        print(
            "topology: links_adjacent=0 -- NONADJACENT output: {} Descendant link(s) skip "
            "snapshots (longest span {}); the horizontal driver retains each snapshot "
            "generation until its descendants' snapshots have been processed, so a gapped "
            "run may hold more than two generations at once, at most the longest "
            "descendant span plus one".format(n_gapped, max_span)
        )
    widest = max(counts) if counts else 0
    if widest > _INT32_MAX:
        print(
            "width: WIDE output -- the largest snapshot holds {} halos, above INT32_MAX ({}); "
            "version 3 indices are int64; the horizontal driver refuses such a slab before "
            "loading it only when that snapshot is a requested output snapshot (the output "
            "path counts emitted records in int), warns above 1e9 rows where the output "
            "marshaller cannot grow, and otherwise at forest_chunks: 1 whole-slab retention "
            "must hold it in memory (optionally bounded by input.retention_memory_ceiling_mb); "
            "output from every route (lhalo_binary, consistent_trees_hdf5 and "
            "consistent_trees_ascii sources) is forest-blocked, so chunked sweeps "
            "(input.forest_chunks) bound a run's memory by a chunk, down to the largest "
            "forest's share of the widest slab, which no setting splits".format(widest, _INT32_MAX)
        )
    else:
        print(
            "width: the largest snapshot holds {} halos, within INT32_MAX ({})".format(
                widest, _INT32_MAX
            )
        )


def _print_transpose_topology(manifest) -> None:
    result = manifest.stage("transpose")["result"]
    counts = manifest.stage("ingest")["result"]["snapshot_counts"]
    _print_topology(
        int(result["n_gapped_descendants"]),
        int(result["max_descendant_span"]),
        bool(result["links_adjacent"]),
        [int(count) for count in counts],
    )


def _dataset_dir(manifest) -> Path:
    return manifest.artifact_path(manifest.stage("write")["directory"])


# ==========================================================================
# Subcommands
# ==========================================================================


def cmd_inspect(args) -> int:
    """Read-only report of what ``ingest`` would convert from these inputs."""
    import pipeline
    import transpose as transpose_module
    from hdf5_writer_v3 import v3_halo_datasets

    schema, parameters, n_snapshots, _budget = _resolve_configuration(args)
    _announce_profile(args, schema, file=sys.stderr)
    report = {
        "source_format": args.source_format,
        "simulation_info": str(Path(args.simulation_info).resolve()),
        "a_list": {"path": str(Path(args.a_list).resolve()), "n_snapshots": n_snapshots},
        "profile": {
            "path": str(_profile_path(args).resolve()),
            "shipped_default": args.column_map is None,
            "column_mapping_sha256": schema.digest,
            "required_columns": {role: list(aliases) for role, aliases in schema.required_columns},
            "extra_fields": [extra.as_canonical() for extra in schema.extra_fields],
            "source_layout": (
                None if schema.source_layout is None else schema.source_layout.as_canonical()
            ),
        },
        "adapter_parameters": parameters,
    }
    if args.source_format == "consistent_trees_ascii":
        report["inventory"] = _inspect_ascii(schema, parameters)
        total_halos = report["inventory"]["total_rows"]
    else:
        adapter = pipeline.build_adapter(args.source_format, schema, parameters, n_snapshots - 1)
        report["inventory"] = _inspect_prelinked(args.source_format, adapter)
        total_halos = report["inventory"]["selected_halos"]

    table = v3_halo_datasets(schema)
    logical = sum(
        np.dtype(dtype).itemsize * (3 if is_vec else 1) for dtype, is_vec in table.values()
    )
    report["output"] = {
        "format_version": FORMAT_VERSION,
        "logical_bytes_per_halo": logical,
        "estimated_payload_bytes": logical * total_halos,
        "estimate_note": (
            "logical dataset bytes only; HDF5 allocates whole 65536-row chunks, so small "
            "snapshots occupy more on disk"
        ),
        "ingest_record_bytes": pipeline.ingest_dtype(schema).itemsize,
        "transposed_record_bytes": transpose_module.output_dtype(schema).itemsize,
        "runtime_support": RUNTIME_NOTICE,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    print(RUNTIME_NOTICE, file=sys.stderr)
    return 0


def _inspect_prelinked(source_format: str, adapter) -> dict:
    inventory = adapter.inventory()
    files = {}
    for unit in inventory.units:
        entry = files.setdefault(unit.source_file_ordinal, {"units": 0, "halos": 0})
        entry["units"] += 1
        entry["halos"] += unit.n_halos
    selected = set(inventory.selected)
    record = {
        "unit_kind": "tree" if source_format == "lhalo_binary" else "forest",
        "total_units": len(inventory.units),
        "total_halos": inventory.total_halos,
        "selected_units": len(selected),
        "selected_halos": sum(
            unit.n_halos
            for unit in inventory.units
            if (unit.source_file_ordinal, unit.unit_ordinal) in selected
        ),
        "files": [
            {"source_file_ordinal": ordinal, "units": entry["units"], "halos": entry["halos"]}
            for ordinal, entry in sorted(files.items())
        ],
    }
    if source_format == "consistent_trees_hdf5":
        record["dependencies"] = sorted(
            dependency.identity.path for dependency in adapter.dependencies()
        )
    return record


def _inspect_ascii(schema, parameters) -> dict:
    """Row and tree-marker counts from the parser's independent pre-scan, and
    the profile's resolution against every file's header. The ASCII route's
    forest inventory exists only after its topology preparation, which
    ``ingest`` runs inside the workdir."""
    from ctrees_parser import parse_header_line, prescan_file, resolve_selection

    if not Path(parameters["forests_list"]).is_file():
        raise ConverterError("{}: forests list not found".format(parameters["forests_list"]))
    files = []
    for path in parameters["tree_files"]:
        scan = prescan_file(path)
        resolve_selection(parse_header_line(scan.header_line), schema)
        files.append(
            {
                "path": path,
                "rows": scan.n_rows,
                "tree_markers": int(len(scan.tree_start_rows)),
                "bytes": scan.size,
            }
        )
    return {
        "unit_kind": "forest (enumerated during ingest's topology preparation)",
        "total_rows": sum(entry["rows"] for entry in files),
        "files": files,
    }


def cmd_ingest(args) -> int:
    import pipeline
    from conversion_manifest import MANIFEST_GENERIC, classify_manifest

    if args.source_format is None:
        given = [
            _flag(name) for name in _CONFIGURATION_OPTIONS if getattr(args, name, None) is not None
        ]
        if given:
            raise ConverterError(
                "{} given without --source-format: name the whole configuration to start (or "
                "check) a conversion, or only --workdir to resume the recorded one".format(
                    ", ".join(given)
                )
            )
        if classify_manifest(args.workdir) != MANIFEST_GENERIC:
            raise ConverterError(
                "{}: no generic conversion to resume; name --source-format, --simulation-info, "
                "--a-list and the source inventory to start one".format(args.workdir)
            )
        print("ingest: resuming the conversion recorded in {}".format(args.workdir))
    else:
        for name in ("simulation_info", "a_list"):
            if getattr(args, name) is None:
                raise ConverterError("--source-format needs {} as well".format(_flag(name)))
        schema, parameters, _n, budget_bytes = _resolve_configuration(args)
        _announce_profile(args, schema)
        pipeline.initialize(
            args.workdir,
            schema,
            parameters,
            args.a_list,
            ingest_max_rows=(
                args.ingest_max_rows
                if args.ingest_max_rows is not None
                else pipeline.DEFAULT_INGEST_MAX_ROWS
            ),
            transpose_budget_bytes=budget_bytes,
        )
    manifest = pipeline.run_ingest(args.workdir)
    result = manifest.stage("ingest")["result"]
    print(
        "ingest: COMPLETE -- {} halo(s) in {} chunk(s) under {}".format(
            result["n_rows"], result["n_chunks"], manifest.workdir
        )
    )
    print(RUNTIME_NOTICE)
    return 0


def cmd_transpose(args) -> int:
    import pipeline

    manifest = pipeline.run_transpose(args.workdir, consume_ingest=args.consume_ingest)
    result = manifest.stage("transpose")["result"]
    print(
        "transpose: COMPLETE -- {} halo(s) remapped over {} snapshot(s)".format(
            result["total_halos"], len(manifest.configuration["snapshots"]["numbers"])
        )
    )
    _print_transpose_topology(manifest)
    print(RUNTIME_NOTICE)
    return 0


def _require_recorded_simulation_info(workdir, path) -> None:
    """``write --simulation-info`` must be the very file the conversion was
    ingested with: every route this CLI starts pins it by content. The header's
    box size and cosmology come from it, and nothing in the source would catch
    another package's metadata. Read-only."""
    from conversion_manifest import ConversionManifest, sha256_file

    manifest = ConversionManifest.load(workdir)
    pinned = [record for record in manifest.dependencies if "simulation_info" in record["roles"]]
    if not pinned:
        return
    given = sha256_file(path)
    for record in pinned:
        if record.get("sha256") != given:
            raise ConverterError(
                "{}: this conversion was ingested with a different simulation_info ({}, sha256 "
                "{}); refusing to write a header from other metadata".format(
                    path, record["path"], record.get("sha256")
                )
            )


def cmd_write(args) -> int:
    import pipeline
    from hdf5_writer_v3 import HorizontalV3Writer

    _require_recorded_simulation_info(args.workdir, args.simulation_info)
    manifest = pipeline.run_write(
        args.workdir,
        HorizontalV3Writer(args.simulation_info),
        consume_transposed=args.consume_transposed,
    )
    print(
        "write: COMPLETE -- format version {} dataset of {} file(s) in {}".format(
            FORMAT_VERSION, manifest.stage("write")["result"]["n_files"], _dataset_dir(manifest)
        )
    )
    _print_transpose_topology(manifest)
    print(RUNTIME_NOTICE)
    return 0


def _complete_manifest(workdir):
    from conversion_manifest import ConversionManifest

    manifest = ConversionManifest.load(workdir)
    manifest.require_complete("write")
    return manifest


def cmd_validate(args) -> int:
    from validate_v3 import run_battery_v3

    manifest = _complete_manifest(args.workdir)
    budget_bytes = _megabytes(args.memory_budget_mb)
    battery = run_battery_v3(
        _dataset_dir(manifest),
        manifest.configuration["snapshots"]["a_list_path"],
        manifest_path=manifest.path,
        multiplier=args.multiplier,
        budget_bytes=budget_bytes,
        spill_dir=args.spill_dir,
    )
    for outcome in battery.outcomes:
        print(outcome.line())
    measured = battery.measurements
    if "gapped_descendants" in measured and "snapshot_counts" in measured:
        _print_topology(
            int(measured["gapped_descendants"]),
            int(measured.get("max_descendant_span") or 0),
            bool(measured["links_adjacent_measured"]),
            [int(count) for count in measured["snapshot_counts"]],
        )
    print("validation: {}".format("FAIL" if battery.failed else "PASS"))
    print(RUNTIME_NOTICE)
    return 1 if battery.failed else 0


def cmd_report(args) -> int:
    from report import REPORT_JSON, REPORT_TXT, run_report_v3

    manifest = _complete_manifest(args.workdir)
    report = run_report_v3(
        manifest.workdir,
        manifest.configuration["snapshots"]["a_list_path"],
        multiplier=args.multiplier,
        budget_bytes=_megabytes(args.memory_budget_mb),
        spill_dir=args.spill_dir,
    )
    links = report["links"]
    counts = [entry["halos"] for entry in report["per_snapshot"]]
    if links.get("gapped_descendants") is not None:
        _print_topology(
            int(links["gapped_descendants"]),
            int(links.get("max_descendant_span") or 0),
            bool(report["format"]["links_adjacent_measured"]),
            counts,
        )
    print(
        "report: {} and {} -- validation {}".format(
            Path(manifest.workdir) / REPORT_JSON,
            Path(manifest.workdir) / REPORT_TXT,
            "PASS" if report["validation_passed"] else "FAIL",
        )
    )
    print(RUNTIME_NOTICE)
    return 0 if report["validation_passed"] else 1


# ==========================================================================
# Argument parsing
# ==========================================================================


def _add_workdir(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--workdir", required=True, help="conversion workdir (manifest, chunks, dataset)"
    )


def _add_configuration(parser: argparse.ArgumentParser, *, required: bool) -> None:
    """The options that define a conversion. ``required`` marks the three
    every configuration names; format-specific requirements are checked after
    parsing, so the error names the format that needs them."""
    parser.add_argument(
        "--source-format",
        required=required,
        choices=SOURCE_FORMATS,
        help="source format; never inferred from the files",
    )
    parser.add_argument(
        "--simulation-info",
        required=required,
        default=None,
        help="the package's simulation_info.yaml; its input.tree_type must equal --source-format",
    )
    parser.add_argument(
        "--a-list", required=required, default=None, help="canonical a_list (one scale per line)"
    )
    parser.add_argument(
        "--column-map",
        default=None,
        help="mapping profile (e.g. simulations/<package>/converter_columns.yaml); default: the "
        "shipped convert/mimic-convert/profiles/<source-format>.yaml. A named profile that fails is "
        "fatal, never replaced by the default",
    )
    parser.add_argument(
        "--memory-budget-mb",
        type=int,
        default=None,
        help="ceiling, in MiB, on each budgeted working-buffer term (adapter reads/validation, "
        "inventory, ASCII rank pass, transpose sort buffers), each checked before it is "
        "allocated; not a bound on total interpreter RSS (default {})".format(
            DEFAULT_MEMORY_BUDGET_MB
        ),
    )

    binary = parser.add_argument_group("lhalo_binary inventory (all required)")
    binary.add_argument("--source-dir", default=None, help="directory holding the tree files")
    binary.add_argument(
        "--tree-name", default=None, help="tree file stem; files are <tree-name>.<N>"
    )
    binary.add_argument(
        "--halo-properties",
        default=None,
        help="the package's ordered halo_properties.yaml, validating the binary layout",
    )

    ranged = parser.add_argument_group(
        "file range (lhalo_binary and consistent_trees_hdf5; required)"
    )
    ranged.add_argument("--first-file", type=int, default=None, help="first source file number")
    ranged.add_argument("--last-file", type=int, default=None, help="last source file number")

    hdf5 = parser.add_argument_group("consistent_trees_hdf5 inventory (all required)")
    hdf5.add_argument(
        "--info-file",
        default=None,
        help="forests-HDF5 info file (e.g. mergertree_info.h5); particle_mass comes from "
        "--simulation-info",
    )

    ascii_group = parser.add_argument_group("consistent_trees_ascii inventory (all required)")
    ascii_group.add_argument("--forests-list", default=None, help="path to forests.list")
    ascii_group.add_argument(
        "--tree-file",
        action="append",
        default=None,
        help="ctrees ASCII tree file (repeat once per file, in inventory order)",
    )
    ascii_group.add_argument(
        "--pool-size", type=int, default=None, help="ASCII preparation worker processes"
    )
    ascii_group.add_argument(
        "--chunksize", type=int, default=None, help="ASCII parser rows per chunk"
    )


def _add_validation_options(parser: argparse.ArgumentParser) -> None:
    from validate import DEFAULT_MULTIPLIER, DEFAULT_V3_BUDGET_BYTES

    parser.add_argument(
        "--memory-budget-mb",
        type=int,
        default=DEFAULT_V3_BUDGET_BYTES >> 20,
        help="working-buffer budget of the battery's bounded sorts, in MiB (default {})".format(
            DEFAULT_V3_BUDGET_BYTES >> 20
        ),
    )
    parser.add_argument(
        "--multiplier",
        type=int,
        default=DEFAULT_MULTIPLIER,
        help="UniqueGalaxyID multiplier for the header bound checks (default {})".format(
            DEFAULT_MULTIPLIER
        ),
    )
    parser.add_argument(
        "--spill-dir",
        default=None,
        help="directory for the battery's temporary sort files (default: system temp dir)",
    )


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="convert_trees",
        description="Convert L-Halo binary, Consistent-Trees forests-HDF5 or Consistent-Trees "
        "ASCII merger trees to Mimic horizontal HDF5 format version 3. "
        + runtime_routes.cli_description()
        + " The ASCII-to-version-2 workflow is convert_ctrees.py.",
        epilog="Package profiles: simulations/<package>/converter_columns.yaml. Restart: every "
        "stage resumes from <workdir>/manifest.json; see the module docstring.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    inspect = sub.add_parser(
        "inspect",
        help="read-only: resolve the profile against the named inventory and report counts, "
        "layout, record widths and estimated output size (writes nothing)",
    )
    _add_configuration(inspect, required=True)

    ingest = sub.add_parser(
        "ingest",
        help="freeze the configuration into --workdir and stream the source into verified "
        "canonical chunks; with only --workdir, resume the recorded conversion",
    )
    _add_workdir(ingest)
    _add_configuration(ingest, required=False)
    ingest.add_argument(
        "--ingest-max-rows",
        type=int,
        default=None,
        help="halos per ingest chunk (default {})".format(1 << 20),
    )

    transpose = sub.add_parser(
        "transpose", help="per-snapshot sort and 64-bit link remapping of the ingested chunks"
    )
    _add_workdir(transpose)
    transpose.add_argument(
        "--consume-ingest",
        action="store_true",
        help="delete the ingest chunks once the transpose is complete and verified "
        "(IRREVERSIBLE for this workdir; source files are never touched)",
    )

    write = sub.add_parser(
        "write",
        help="emit snapshot_NNN.h5 + forests.h5 in horizontal-HDF5 format version 3",
        description="emit snapshot_NNN.h5 + forests.h5 in horizontal-HDF5 format version 3 "
        "(the only version this CLI writes; version 2 is the ASCII-only convert_ctrees.py "
        "workflow)",
    )
    _add_workdir(write)
    write.add_argument(
        "--simulation-info",
        required=True,
        help="simulation_info.yaml supplying the header's physical values",
    )
    write.add_argument(
        "--consume-transposed",
        action="store_true",
        help="delete the transposed snapshots once the dataset is written and verified "
        "(IRREVERSIBLE for this workdir; source files are never touched)",
    )

    validate = sub.add_parser(
        "validate",
        help="run the version 3 producer battery over the written dataset (exit 1 on FAIL)",
    )
    _add_workdir(validate)
    _add_validation_options(validate)

    report = sub.add_parser(
        "report",
        help="validate and write conversion_report.json/.txt into --workdir (exit 1 on FAIL)",
    )
    _add_workdir(report)
    _add_validation_options(report)
    return parser


_COMMANDS = {
    "inspect": cmd_inspect,
    "ingest": cmd_ingest,
    "transpose": cmd_transpose,
    "write": cmd_write,
    "validate": cmd_validate,
    "report": cmd_report,
}


def main(argv=None) -> int:
    args = build_arg_parser().parse_args(argv)
    from adapters.source_inventory import MissingDependencyError

    try:
        return _COMMANDS[args.command](args)
    except (ConverterError, MissingDependencyError, OSError) as exc:
        print("ERROR: {}".format(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
