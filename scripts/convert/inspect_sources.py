#!/usr/bin/env python3
"""Read-only source inspection CLI (Slice 1 of the converter generalisation
plan, docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md).

Two subcommands:

  inspect  -- explicit --source-format/--simulation-info/--a-list inspection
              of one source: per-file/per-snapshot counts, link-span summary,
              identity bounds, available fields/types/units.
  survey   -- reachability sweep of all five named simulation packages
              (mini-Millennium, Millennium, micro-Uchuu, mini-Uchuu, full
              Uchuu), each against its declared adapter route, plus a full
              inspection of whichever ones are reachable.

Never writes into a source directory; every open is read-only. This is the
generic `inspect` command named in the plan's C4 command list (`inspect,
ingest, transpose, write, validate, report`) -- the other five remain future
work.
"""

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ctrees_parser  # noqa: E402
from adapters import source_inventory as si  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from scatter import load_a_list  # noqa: E402

#: The five simulation packages named by the plan, and their simulation_info.yaml
#: paths relative to the repository root. Declared here rather than discovered,
#: so "all five requested packages have a declared adapter route" is a fact
#: about this file, not a side effect of what happens to exist under
#: simulations/ on a given checkout.
NAMED_PACKAGES = {
    "mini-millennium": "simulations/mini-millennium/simulation_info.yaml",
    "millennium": "simulations/millennium/simulation_info.yaml",
    "micro-uchuu": "simulations/micro-uchuu/simulation_info.yaml",
    "mini-uchuu": "simulations/mini-uchuu/simulation_info.yaml",
    "uchuu": "simulations/uchuu/simulation_info.yaml",
}

#: tree_type -> adapter route name (C1: three adapters -- ASCII retained,
#: L-Halo binary added, Consistent-Trees forests-HDF5 added).
ADAPTER_ROUTES = {
    "lhalo_binary": "lhalo_binary",
    "consistent_trees_hdf5": "ctrees_hdf5",
    "consistent_trees_ascii": "ctrees_ascii",
}


def _repo_root() -> Path:
    return Path(os.path.dirname(os.path.abspath(__file__))).parent.parent


def _link_summary_to_dict(summary):
    if summary is None:
        return None
    return {
        "non_null_descendant_links": summary.non_null_descendant_links,
        "forward_adjacent_links": summary.forward_adjacent_links,
        "forward_gap_links": summary.forward_gap_links,
        "max_span": summary.max_span,
        "non_forward_or_zero_span": summary.non_forward_or_zero_span,
        "snapshot_halo_counts": dict(sorted(summary.snapshot_halo_counts.items())),
        "mostboundid_min": summary.mostboundid_min,
        "mostboundid_max": summary.mostboundid_max,
    }


def inspect_lhalo_source(sim_info: si.SimulationInfo, byte_order: str, scan_links: bool):
    reach = si.check_lhalo_reachability(sim_info)
    files_report = []
    total_trees = 0
    total_halos = 0
    max_tree_halo_count = 0
    combined = si.LinkSpanSummary()
    for path_str in reach.present_files:
        path = Path(path_str)
        header = si.read_lhalo_header(path, byte_order=byte_order)
        file_max_tree = int(header.tree_halo_counts.max()) if header.ntrees else 0
        entry = {
            "file": str(path),
            "byte_order": byte_order,
            "record_bytes": si.LHALO_RECORD_BYTES,
            "ntrees": header.ntrees,
            "total_halos": header.total_halos,
            "file_size": header.file_size,
            "max_tree_halo_count": file_max_tree,
        }
        total_trees += header.ntrees
        total_halos += header.total_halos
        max_tree_halo_count = max(max_tree_halo_count, file_max_tree)
        if scan_links:
            summary = si.scan_lhalo_file(header)
            entry["link_summary"] = _link_summary_to_dict(summary)
            combined.non_null_descendant_links += summary.non_null_descendant_links
            combined.forward_adjacent_links += summary.forward_adjacent_links
            combined.forward_gap_links += summary.forward_gap_links
            combined.non_forward_or_zero_span += summary.non_forward_or_zero_span
            combined.max_span = max(combined.max_span, summary.max_span)
            if summary.mostboundid_min is not None:
                combined.mostboundid_min = (
                    summary.mostboundid_min
                    if combined.mostboundid_min is None
                    else min(combined.mostboundid_min, summary.mostboundid_min)
                )
            if summary.mostboundid_max is not None:
                combined.mostboundid_max = (
                    summary.mostboundid_max
                    if combined.mostboundid_max is None
                    else max(combined.mostboundid_max, summary.mostboundid_max)
                )
            for snap, count in summary.snapshot_halo_counts.items():
                combined.snapshot_halo_counts[snap] = (
                    combined.snapshot_halo_counts.get(snap, 0) + count
                )
        files_report.append(entry)
    return {
        "adapter": "lhalo_binary",
        "reachability": reach.__dict__,
        "files": files_report,
        "total_trees": total_trees,
        "total_halos": total_halos,
        "max_tree_halo_count": max_tree_halo_count,
        "combined_link_summary": _link_summary_to_dict(combined) if scan_links else None,
        "fields": {
            name: {"kind": kind, "shape": list(shape)} for name, kind, shape in si.LHALO_FIELDS
        },
    }


def inspect_hdf5_source(sim_info: si.SimulationInfo, scan_links: bool):
    """Raises MissingDependencyError (uncaught) if h5py is not installed --
    the acceptance criterion is that a missing HDF5 dependency fails, so this
    propagates rather than being swallowed into an 'error' field with a
    process exit of 0 (see inspect_one's callers)."""
    reach = si.check_hdf5_reachability(sim_info)
    result = {
        "adapter": "ctrees_hdf5",
        "reachability": reach.__dict__,
        "files": None,
        "root_attrs": None,
    }
    info_path = Path(sim_info.simulation_dir) / sim_info.tree_name
    if not reach.exists or not info_path.exists():
        return result
    root_attrs, files = si.inspect_ctrees_hdf5_source(info_path, scan_links=scan_links)
    result["root_attrs"] = root_attrs
    result["files"] = [
        {
            "name": f.name,
            "link_type": f.link_type,
            "reachable": f.reachable,
            "error": f.error,
            "n_forests": f.n_forests,
            "n_halos": f.n_halos,
            "max_forest_nhalos": f.max_forest_nhalos,
            "fields": f.fields,
            "link_summary": _link_summary_to_dict(f.link_summary),
        }
        for f in files
    ]
    return result


def inspect_ascii_source(sim_info: si.SimulationInfo):
    """Lightweight ASCII reachability/field report. Full inspection (counts,
    forest identity, snapshot population) reuses the existing converter
    pipeline directly -- see docs/dev/MIMIC-CONVERTER-BASELINE-REFERENCE.md,
    which captures it as this slice's required pre-change ASCII reference."""
    sim_dir = Path(sim_info.simulation_dir)
    forests_list = sim_dir / "forests.list"
    locations = sim_dir / "locations.dat"
    present = {}
    for name, path in (("forests.list", forests_list), ("locations.dat", locations)):
        present[name] = {
            "exists": path.exists(),
            "bytes": path.stat().st_size if path.exists() else None,
        }
    tree_files = sorted(str(p) for p in sim_dir.glob("tree_*.dat"))
    return {
        "adapter": "ctrees_ascii",
        "simulation_dir": str(sim_dir),
        "exists": sim_dir.exists(),
        "host": si.host_identity(),
        "free_bytes_on_volume": si.free_space_bytes(sim_dir),
        "index_files": present,
        "tree_files_present": tree_files,
        "required_int_columns": list(ctrees_parser._INT_COLUMNS),
        "required_float_columns": list(ctrees_parser._FLOAT_COLUMNS),
        "snapshot_column_spellings": list(ctrees_parser.SNAPSHOT_SPELLINGS),
    }


def inspect_one(
    source_format: str, sim_info_path: str, a_list_path: str, byte_order: str, scan_links: bool
):
    sim_info = si.load_simulation_info(sim_info_path)
    if sim_info.tree_type != source_format:
        raise ConverterError(
            "{}: input.tree_type={!r} does not match --source-format {!r}".format(
                sim_info_path, sim_info.tree_type, source_format
            )
        )
    a_list, a_list_md5 = load_a_list(a_list_path)
    order = "<" if byte_order == "little" else ">"
    if source_format == "lhalo_binary":
        report = inspect_lhalo_source(sim_info, order, scan_links)
    elif source_format == "consistent_trees_hdf5":
        report = inspect_hdf5_source(sim_info, scan_links)
    elif source_format == "consistent_trees_ascii":
        report = inspect_ascii_source(sim_info)
    else:
        raise ConverterError("unknown --source-format {!r}".format(source_format))
    report["source_format"] = source_format
    report["simulation_info"] = str(sim_info_path)
    report["a_list"] = {"path": str(a_list_path), "n_snapshots": len(a_list), "md5": a_list_md5}
    return report


def cmd_inspect(args):
    report = inspect_one(
        args.source_format,
        args.simulation_info,
        args.a_list,
        args.byte_order,
        not args.no_link_scan,
    )
    text = json.dumps(report, indent=2, sort_keys=True, default=str)
    if args.json:
        Path(args.json).write_text(text + "\n")
    print(text)
    return 0


#: Exceptions a single package's inspection may realistically raise without
#: aborting the whole survey and discarding every other package's
#: already-gathered results: the tool's own declared failure types, plus the
#: realistic corrupted-data exceptions numpy/h5py raise directly (untrusted
#: binary/HDF5 input is this slice's own declared risky surface).
PER_PACKAGE_EXCEPTIONS = (
    ConverterError,
    si.MissingDependencyError,
    FileNotFoundError,
    ValueError,
    IndexError,
    KeyError,
    OSError,
)


def cmd_survey(args):
    root = _repo_root()
    results = {}
    any_error = False
    for name, rel_path in NAMED_PACKAGES.items():
        sim_info_path = root / rel_path
        entry = {"simulation_info": str(sim_info_path)}
        try:
            sim_info = si.load_simulation_info(sim_info_path)
        except PER_PACKAGE_EXCEPTIONS as exc:
            entry["error"] = str(exc)
            any_error = True
            results[name] = entry
            continue
        adapter = ADAPTER_ROUTES.get(sim_info.tree_type)
        entry["tree_type"] = sim_info.tree_type
        entry["adapter_route"] = adapter
        if adapter is None:
            entry["error"] = "no declared adapter route for tree_type {!r}".format(
                sim_info.tree_type
            )
            any_error = True
            results[name] = entry
            continue
        if adapter == "lhalo_binary":
            reach = si.check_lhalo_reachability(sim_info)
        elif adapter == "ctrees_hdf5":
            reach = si.check_hdf5_reachability(sim_info)
        else:
            sim_dir = Path(sim_info.simulation_dir)
            reach = si.SourceReachability(
                simulation_dir=str(sim_dir),
                exists=sim_dir.exists(),
                host=si.host_identity(),
                declared_first_file=sim_info.first_file,
                declared_last_file=sim_info.last_file,
                declared_file_count=0,
                present_files=[],
                present_file_count=0,
                total_bytes=0,
                free_bytes_on_volume=si.free_space_bytes(sim_dir),
                notes=[
                    "ASCII reachability uses forests.list/locations.dat, not first_file/last_file"
                ],
            )
        entry["reachability"] = reach.__dict__
        if reach.exists and reach.present_file_count > 0 and not args.reachability_only:
            try:
                entry["inspection"] = inspect_one(
                    sim_info.tree_type,
                    sim_info_path,
                    root / sim_info.snapshot_list_file,
                    args.byte_order,
                    not args.no_link_scan,
                )
            except PER_PACKAGE_EXCEPTIONS as exc:
                entry["inspection_error"] = str(exc)
                any_error = True
        results[name] = entry

    text = json.dumps(results, indent=2, sort_keys=True, default=str)
    if args.json:
        Path(args.json).write_text(text + "\n")
    print(text)
    return 1 if any_error else 0


def build_parser():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="command", required=True)

    inspect_p = sub.add_parser("inspect", help="inspect one explicit source")
    inspect_p.add_argument(
        "--source-format",
        required=True,
        choices=sorted(ADAPTER_ROUTES.keys()),
    )
    inspect_p.add_argument("--simulation-info", required=True)
    inspect_p.add_argument("--a-list", required=True)
    inspect_p.add_argument("--byte-order", choices=("little", "big"), default="little")
    inspect_p.add_argument(
        "--no-link-scan", action="store_true", help="skip the full link-span scan"
    )
    inspect_p.add_argument("--json", help="also write the report to this path")
    inspect_p.set_defaults(func=cmd_inspect)

    survey_p = sub.add_parser("survey", help="reachability sweep of all five named packages")
    survey_p.add_argument("--byte-order", choices=("little", "big"), default="little")
    survey_p.add_argument("--no-link-scan", action="store_true", help="skip full link-span scans")
    survey_p.add_argument(
        "--reachability-only", action="store_true", help="report reachability only, skip inspection"
    )
    survey_p.add_argument("--json", help="also write the report to this path")
    survey_p.set_defaults(func=cmd_survey)

    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except (ConverterError, si.MissingDependencyError, OSError, ValueError) as exc:
        print("error: {}".format(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
