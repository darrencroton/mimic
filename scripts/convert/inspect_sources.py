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


def _anchor_simulation_dir(sim_info: si.SimulationInfo) -> None:
    """simulation_info.yaml's `input.simulation_dir` is written repo-relative
    (every existing converter command, and every symlink under simulations/,
    assumes CWD == repo root); anchor it against _repo_root() explicitly
    rather than leaving it to resolve against whatever the process's actual
    CWD happens to be. Without this, running from any directory other than
    the repo root makes every package silently report exists=False with
    exit 0 -- honest-looking but wrong evidence."""
    p = Path(sim_info.simulation_dir)
    if not p.is_absolute():
        sim_info.simulation_dir = str(_repo_root() / p)


def _is_within_or_equal(candidate: Path, container: Path) -> bool:
    if candidate == container:
        return True
    try:
        candidate.relative_to(container)
        return True
    except ValueError:
        return False


def _protected_paths_for_hdf5(sim_info: si.SimulationInfo):
    """External-link targets a forests-HDF5 info file resolves to. HDF5
    ExternalLinks permit arbitrary relative/absolute targets, so a link can
    legally point outside `simulation_dir` -- `simulation_dir` alone is not
    a complete protected set for this route. `check_hdf5_reachability`
    already enumerates every resolvable ExternalLink target into
    `present_files` (alongside the info file itself), so this reuses that
    rather than re-walking the HDF5 file. Any failure while checking is
    itself a per-package-shaped failure, not a reason to under-protect."""
    if sim_info.tree_type != "consistent_trees_hdf5":
        return []
    try:
        return si.check_hdf5_reachability(sim_info).present_files
    except Exception:
        return []


def _check_json_output_safe(json_path, protected_paths) -> None:
    """Refuse to let --json overwrite a source file or write inside a source
    directory. `protected_paths` is an iterable of file/directory paths
    (source paths this run read from) that must not equal, or contain, the
    resolved --json path. Both sides are resolved (symlinks followed) before
    comparing, so a symlinked source directory is still protected.

    Without this, --json is a normal documented flag that can silently
    destroy a real source file: pointing it at a source path overwrites that
    path in place with the JSON report, which is exactly what "inspection
    never writes into source directories" (this slice's own acceptance
    criterion) forbids."""
    resolved = Path(json_path).resolve()
    for protected in protected_paths:
        if protected is None:
            continue
        try:
            protected_resolved = Path(protected).resolve()
        except OSError:
            continue
        if _is_within_or_equal(resolved, protected_resolved):
            raise ConverterError(
                "--json {} resolves to {}, which is or is inside a source path ({}) -- "
                "refusing to overwrite source data".format(json_path, resolved, protected_resolved)
            )


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


def inspect_lhalo_source(
    sim_info: si.SimulationInfo, byte_order: str, scan_links: bool, max_snapshot=None
):
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
            summary = si.scan_lhalo_file(header, max_snapshot=max_snapshot)
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


def inspect_hdf5_source(sim_info: si.SimulationInfo, scan_links: bool, max_snapshot=None):
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
    root_attrs, files = si.inspect_ctrees_hdf5_source(
        info_path, scan_links=scan_links, max_snapshot=max_snapshot
    )
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


def inspect_ascii_source(sim_info: si.SimulationInfo, scan_links: bool = True):
    """ASCII reachability/field report, plus (unless scan_links=False) cheap
    real counts via the parser's own existing independent pre-count helper
    (ctrees_parser.prescan_file -- a single stream pass, no pandas, no
    topology reconstruction). Full link-span/topology/forest identity for
    ASCII remains out of this slice's scope (that duplicates Slice 5's job);
    see docs/dev/MIMIC-CONVERTER-BASELINE-REFERENCE.md for the full pipeline
    counts on the one dataset this slice captured end to end."""
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

    prescan = None
    if scan_links and tree_files:
        per_file = []
        total_rows = 0
        total_tree_markers = 0
        for tf in tree_files:
            scan = ctrees_parser.prescan_file(tf)
            per_file.append(
                {
                    "file": tf,
                    "n_rows": scan.n_rows,
                    "n_tree_markers": len(scan.tree_start_rows),
                    "md5": scan.md5,
                }
            )
            total_rows += scan.n_rows
            total_tree_markers += len(scan.tree_start_rows)
        prescan = {
            "total_rows": total_rows,
            "total_tree_markers": total_tree_markers,
            "files": per_file,
        }

    return {
        "adapter": "ctrees_ascii",
        "simulation_dir": str(sim_dir),
        "exists": sim_dir.exists(),
        "host": si.host_identity(),
        "free_bytes_on_volume": si.free_space_bytes(sim_dir),
        "index_files": present,
        "tree_files_present": tree_files,
        "prescan": prescan,
        "required_int_columns": list(ctrees_parser._INT_COLUMNS),
        "required_float_columns": list(ctrees_parser._FLOAT_COLUMNS),
        "snapshot_column_spellings": list(ctrees_parser.SNAPSHOT_SPELLINGS),
    }


def inspect_one(
    source_format: str, sim_info_path: str, a_list_path: str, byte_order: str, scan_links: bool
):
    sim_info = si.load_simulation_info(sim_info_path)
    _anchor_simulation_dir(sim_info)
    if sim_info.tree_type != source_format:
        raise ConverterError(
            "{}: input.tree_type={!r} does not match --source-format {!r}".format(
                sim_info_path, sim_info.tree_type, source_format
            )
        )
    a_list, a_list_md5 = load_a_list(a_list_path)
    max_snapshot = len(a_list) - 1 if len(a_list) else None
    order = "<" if byte_order == "little" else ">"
    if source_format == "lhalo_binary":
        report = inspect_lhalo_source(sim_info, order, scan_links, max_snapshot)
    elif source_format == "consistent_trees_hdf5":
        report = inspect_hdf5_source(sim_info, scan_links, max_snapshot)
    elif source_format == "consistent_trees_ascii":
        report = inspect_ascii_source(sim_info, scan_links)
    else:
        raise ConverterError("unknown --source-format {!r}".format(source_format))
    report["source_format"] = source_format
    report["simulation_info"] = str(sim_info_path)
    report["a_list"] = {"path": str(a_list_path), "n_snapshots": len(a_list), "md5": a_list_md5}
    return report


def cmd_inspect(args):
    if args.json:
        sim_info = si.load_simulation_info(args.simulation_info)
        _anchor_simulation_dir(sim_info)
        protected = [args.simulation_info, args.a_list, sim_info.simulation_dir]
        protected.extend(_protected_paths_for_hdf5(sim_info))
        _check_json_output_safe(args.json, protected)
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


# Per-package isolation boundaries (cmd_survey's three try blocks below, and
# _named_package_protected_paths above) each catch bare `Exception`, not an
# enumerated tuple. Rounds 2-5 each separately discovered one more specific
# exception type escaping a fixed tuple here (KeyError/IndexError from a
# malformed dataset, a transient OSError during reachability, yaml.YAMLError
# from broken YAML, ValueError/TypeError from a malformed first_file) --
# every one of those library calls sits on untrusted per-package binary/HDF5/
# YAML input, this tool's own declared risky surface, and there is no
# exception type for which "crash and discard every other package's results"
# is the right behavior here. A fifth distinct exception type reaching one of
# these boundaries from a later slice's descendant code should not need its
# own future correction round.


def _named_package_protected_paths(root: Path):
    """Every path `survey` might read from, across all five named packages:
    each simulation_info.yaml, and (best-effort, since a package's YAML may
    itself be malformed) its simulation_dir, snapshot_list_file, and (for the
    forests-HDF5 route) every external-link target its info file resolves
    to -- those can legally point outside simulation_dir. Catches bare
    `Exception`, not an enumerated tuple: this helper runs before any per-
    package try/except loop even starts, on the same untrusted per-package
    YAML/HDF5 input as the rest of this tool, so a malformed package here
    must degrade to "that package contributes no protected paths", never
    abort the safety check (and therefore the whole --json write) for every
    other package."""
    protected = []
    for rel_path in NAMED_PACKAGES.values():
        sim_info_path = root / rel_path
        protected.append(sim_info_path)
        try:
            sim_info = si.load_simulation_info(sim_info_path)
            _anchor_simulation_dir(sim_info)
            protected.append(sim_info.simulation_dir)
            protected.append(root / sim_info.snapshot_list_file)
            protected.extend(_protected_paths_for_hdf5(sim_info))
        except Exception:
            continue
    return protected


def cmd_survey(args):
    root = _repo_root()
    if args.json:
        _check_json_output_safe(args.json, _named_package_protected_paths(root))
    results = {}
    any_error = False
    for name, rel_path in NAMED_PACKAGES.items():
        sim_info_path = root / rel_path
        entry = {"simulation_info": str(sim_info_path)}
        try:
            sim_info = si.load_simulation_info(sim_info_path)
            _anchor_simulation_dir(sim_info)
        except Exception as exc:
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
        try:
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
                        "ASCII reachability uses forests.list/locations.dat, "
                        "not first_file/last_file"
                    ],
                )
        except Exception as exc:
            # A transient filesystem error here (e.g. a path vanishing
            # between exists() and stat()) must not abort the whole survey
            # and discard every other package's already-gathered results.
            entry["reachability_error"] = str(exc)
            any_error = True
            results[name] = entry
            continue
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
            except Exception as exc:
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
    except (
        ConverterError,
        si.MissingDependencyError,
        OSError,
        ValueError,
        TypeError,
        IndexError,
    ) as exc:
        print("error: {}".format(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
