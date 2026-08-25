"""Conversion report emission for the ctrees -> snapshot-HDF5 converter
(plan Slice 7).

Builds the durable conversion report from the manifest, the emitted dataset,
and the producer validation battery outcomes: source-file provenance, halo
totals, per-snapshot counts, forest count, measured max_halo_rank_in_forest,
flyby demotion counts, Len==0 counts, the observed (SnapNum, scale) pair
table (the input from which a future production a_list is drafted — drafting
itself is out of scope), the validation outcomes, and the recommended
UniqueGalaxyID multiplier for the consuming simulation package together with
the full window of multipliers the dataset admits.

Emitted as ``conversion_report.json`` (machine-readable record) plus
``conversion_report.txt`` (human-readable rendering) under the workdir.
"""

import json
import os
import sys
from pathlib import Path
from typing import List, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import ConverterError  # noqa: E402
from scatter import Manifest, load_a_list  # noqa: E402
from validate import DEFAULT_MULTIPLIER, Outcome, battery_failed  # noqa: E402

_INT64_MAX = 2**63 - 1

REPORT_JSON = "conversion_report.json"
REPORT_TXT = "conversion_report.txt"


def identity_multiplier_window(max_rank: int, n_forests_total: int) -> Tuple[int, int]:
    """Inclusive (min, max) range of UniqueGalaxyID multipliers this dataset
    admits, exactly as snapshot_identity_bounds_valid() decides it at run time
    (src/io/snapshot/interface.c): ``multiplier > max_halo_rank_in_forest`` sets
    the floor and ``n_forests_total <= INT64_MAX / multiplier - 1`` sets the
    ceiling. min > max means no multiplier can encode the dataset.

    Nothing else narrows this. Preferring the reader default (TREE_MUL_FAC =
    1e9) belongs to the recommendation, not to the window: a dataset of very
    many small forests has a ceiling below 1e9, and clamping the floor up to it
    would report an empty window where the reader in fact admits a range.

    The floor is held at 1 because the reader also requires a positive
    multiplier: an all-empty dataset carries the documented sentinel pair
    (n_forests_total 0, max_halo_rank_in_forest -1, src/io/snapshot/reader.h),
    for which max_rank + 1 alone would report that 0 encodes the dataset."""
    return max(max_rank + 1, 1), _INT64_MAX // (n_forests_total + 1)


def _smallest_round_value(lower: int, upper: int, steps: Tuple[int, ...]) -> int:
    """Smallest ``step * 10**k`` for a step in `steps` lying within
    [lower, upper], or 0 when the window admits none. Steps must ascend."""
    decade = DEFAULT_MULTIPLIER
    while decade <= upper:
        for step in steps:
            if lower <= step * decade <= upper:
                return step * decade
        decade *= 10
    return 0


def recommended_multiplier(max_rank: int, n_forests_total: int) -> int:
    """Smallest round multiplier within the window identity_multiplier_window()
    reports, or that window's floor when it holds no round value; the int64
    headroom bound is asserted, never silently degraded.

    Powers of ten are preferred because they are the roundest value an operator
    copies into simulation_info.yaml, but they are not sufficient on their own:
    at Shin-Uchuu production scale the window is 1.28e10 .. 5.54e10 and the
    decade ladder steps from 1e10 straight over it to 1e11, so a search
    restricted to powers of ten reports no valid multiplier where a 4.3e10-wide
    one exists. The 1/2/5 ladder fills those gaps, and a window that holds none
    of those -- including one lying entirely below the reader default -- falls
    back to its own floor rather than reporting no multiplier at all."""
    lower, upper = identity_multiplier_window(max_rank, n_forests_total)
    if lower > upper:
        raise ConverterError(
            "no valid identity multiplier: max_halo_rank_in_forest {} needs at least {}, "
            "but n_forests_total {} caps it at {} without overflowing int64".format(
                max_rank, lower, n_forests_total, upper
            )
        )
    return (
        _smallest_round_value(lower, upper, (1,))
        or _smallest_round_value(lower, upper, (1, 2, 5))
        or lower
    )


def build_report(manifest: Manifest, outcomes: List[Outcome], n_snapshots: int) -> dict:
    """Assemble the report dict from the manifest and battery outcomes.

    ``n_snapshots`` is the a_list length: every a_list snapshot appears in the
    per-snapshot table, with explicit zero counts for snapshots that have no
    halos (the emitted dataset contains an empty file for each of them)."""
    snapshots = manifest.data.get("snapshots", {})
    links_values = manifest.data.get("links")
    if links_values is None:
        raise ConverterError("manifest records no run-scoped links values; run links first")
    n_forests_total = int(links_values["n_forests_total"])
    max_rank = int(links_values["max_halo_rank_in_forest"])

    per_snapshot = {}
    for snap in range(n_snapshots):
        entry = snapshots.get(str(snap), {})
        per_snapshot[str(snap)] = {
            "rows": entry.get("rows", 0),
            "flyby_demotions": entry.get("flyby_demotions", 0),
            "len_zero_count": entry.get("len_zero_count", 0),
        }

    sources = {}
    for path, entry in sorted(manifest.data.get("source_files", {}).items()):
        sources[path] = {
            "pre_count": entry["pre_count"],
            "parsed_count": entry["parsed_count"],
            "md5": entry["md5"],
        }

    return {
        "workdir": str(manifest.workdir),
        "outputs_dir": manifest.data.get("outputs_dir"),
        "provenance": manifest.data.get("provenance", {}),
        "source_files": sources,
        "totals": {
            "halos": sum(entry["rows"] for entry in per_snapshot.values()),
            "snapshots_with_halos": sum(1 for e in per_snapshot.values() if e["rows"] > 0),
            "flyby_demotions": sum(e["flyby_demotions"] for e in per_snapshot.values()),
            "len_zero": sum(e["len_zero_count"] for e in per_snapshot.values()),
        },
        "per_snapshot": per_snapshot,
        "n_forests_total": n_forests_total,
        "max_halo_rank_in_forest": max_rank,
        "identity_multiplier_window": list(identity_multiplier_window(max_rank, n_forests_total)),
        "recommended_identity_multiplier": recommended_multiplier(max_rank, n_forests_total),
        "observed_pairs": manifest.data.get("observed_pairs", []),
        "validation": [outcome.as_dict() for outcome in outcomes],
        "validation_passed": not battery_failed(outcomes),
    }


def render_text(report: dict) -> str:
    lines = [
        "Conversion report",
        "=================",
        "",
        "workdir:     {}".format(report["workdir"]),
        "output dir:  {}".format(report["outputs_dir"]),
        "",
        "totals: {} halo(s) in {} populated snapshot(s); {} flyby demotion(s); "
        "{} Len==0 halo(s)".format(
            report["totals"]["halos"],
            report["totals"]["snapshots_with_halos"],
            report["totals"]["flyby_demotions"],
            report["totals"]["len_zero"],
        ),
        "forests: n_forests_total={}, max_halo_rank_in_forest={}, "
        "recommended identity multiplier={}".format(
            report["n_forests_total"],
            report["max_halo_rank_in_forest"],
            report["recommended_identity_multiplier"],
        ),
        "         any multiplier in [{}, {}] encodes this dataset".format(
            *report["identity_multiplier_window"]
        ),
        "",
        "source files:",
    ]
    for path, entry in report["source_files"].items():
        lines.append(
            "  {} — pre-count {}, parsed {}, md5 {}".format(
                path, entry["pre_count"], entry["parsed_count"], entry["md5"]
            )
        )
    lines += ["", "per-snapshot counts (rows / flyby demotions / Len==0):"]
    for snap_str, entry in report["per_snapshot"].items():
        lines.append(
            "  snapshot {:>3} — {} / {} / {}".format(
                snap_str, entry["rows"], entry["flyby_demotions"], entry["len_zero_count"]
            )
        )
    lines += ["", "observed (SnapNum, scale) pairs:"]
    for snap, scale in report["observed_pairs"]:
        lines.append("  {:>3}  {}".format(snap, scale))
    lines += ["", "validation outcomes:"]
    for outcome in report["validation"]:
        text = "  {}: {}".format(outcome["name"], outcome["status"])
        if outcome["detail"]:
            text += " — {}".format(outcome["detail"])
        lines.append(text)
    lines += ["", "validation: {}".format("PASS" if report["validation_passed"] else "FAIL"), ""]
    return "\n".join(lines)


def write_report(report: dict, workdir) -> Path:
    """Write the JSON record and the text rendering; returns the JSON path."""
    workdir = Path(workdir)
    json_path = workdir / REPORT_JSON
    txt_path = workdir / REPORT_TXT
    with open(json_path, "w") as handle:
        json.dump(report, handle, indent=2, sort_keys=True)
        handle.write("\n")
    txt_path.write_text(render_text(report))
    return json_path


def run_report(workdir, a_list_path, multiplier: int = DEFAULT_MULTIPLIER) -> dict:
    """Run the validation battery over the emitted dataset and write the
    conversion report. The battery outcome is recorded in the report AND
    reflected in the caller's exit status — a failing dataset never yields a
    quietly successful report run."""
    from validate import run_battery  # deferred: keeps CLI import cost low

    manifest = Manifest.load_or_create(workdir)
    if not manifest.path.exists():
        raise ConverterError("{}: no manifest found; run scatter first".format(workdir))
    outputs_dir = manifest.data.get("outputs_dir")
    if outputs_dir is None:
        raise ConverterError("{}: no emitted dataset recorded; run write first".format(workdir))
    a_list, _ = load_a_list(a_list_path)
    outcomes = run_battery(
        outputs_dir, a_list_path, manifest_path=manifest.path, multiplier=multiplier
    )
    report = build_report(manifest, outcomes, n_snapshots=len(a_list))
    json_path = write_report(report, manifest.workdir)
    print(
        "report: wrote {} and {} — validation {}".format(
            json_path,
            Path(manifest.workdir) / REPORT_TXT,
            "PASS" if report["validation_passed"] else "FAIL",
        ),
        file=sys.stderr,
    )
    return report
