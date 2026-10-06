"""Conversion report emission for the ctrees -> horizontal-HDF5 converter.

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

A generic (format version 3) conversion has its own report,
:func:`run_report_v3`, in a separate section at the end of this module; it
reads the generic manifest and the v3 battery and shares only the identity
multiplier arithmetic with the legacy report above, which is unchanged.
"""

import json
import os
import sys
from pathlib import Path
from typing import Dict, List, Mapping, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
import runtime_routes  # noqa: E402
from conversion_manifest import ConversionManifest  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from scatter import Manifest, load_a_list  # noqa: E402
from validate import (  # noqa: E402
    DEFAULT_MULTIPLIER,
    DEFAULT_V3_BUDGET_BYTES,
    Outcome,
    battery_failed,
)
from validate_v3 import V3BatteryResult, run_battery_v3  # noqa: E402

_INT64_MAX = 2**63 - 1

REPORT_JSON = "conversion_report.json"
REPORT_TXT = "conversion_report.txt"


def identity_multiplier_window(max_rank: int, n_forests_total: int) -> Tuple[int, int]:
    """Inclusive (min, max) range of UniqueGalaxyID multipliers this dataset
    admits, exactly as horizontal_identity_bounds_valid() decides it at run time
    (src/io/horizontal/interface.c): ``multiplier > max_halo_rank_in_forest`` sets
    the floor and ``n_forests_total <= INT64_MAX / multiplier - 1`` sets the
    ceiling. min > max means no multiplier can encode the dataset.

    Nothing else narrows this. Preferring the reader default (TREE_MUL_FAC =
    1e9) belongs to the recommendation, not to the window: a dataset of very
    many small forests has a ceiling below 1e9, and clamping the floor up to it
    would report an empty window where the reader in fact admits a range.

    The floor is held at 1 because the reader also requires a positive
    multiplier: an all-empty dataset carries the documented sentinel pair
    (n_forests_total 0, max_halo_rank_in_forest -1, src/io/horizontal/reader.h),
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
        rows = entry.get("rows", 0)
        # flyby_demotions is a required, measured field for any snapshot
        # that actually has records — defaulting a missing field to 0 would
        # manufacture the very evidence this field exists to provide. An
        # unpopulated snapshot never went through fixups, so it legitimately
        # has no such field recorded.
        if rows > 0:
            if "flyby_demotions" not in entry:
                raise ConverterError(
                    "manifest snapshot {} entry has {} row(s) but no flyby_demotions field "
                    "recorded — it must be measured, not defaulted".format(snap, rows)
                )
            flyby_demotions = entry["flyby_demotions"]
        else:
            flyby_demotions = entry.get("flyby_demotions", 0)
        per_snapshot[str(snap)] = {
            "rows": rows,
            "flyby_demotions": flyby_demotions,
            "len_zero_count": entry.get("len_zero_count", 0),
        }

    sources = {}
    for path, entry in sorted(manifest.data.get("source_files", {}).items()):
        sources[path] = {
            "pre_count": entry["pre_count"],
            "parsed_count": entry["parsed_count"],
            "md5": entry["md5"],
        }

    # fix_flybys (which merged unrelated FoF groups) was removed from the converter;
    # flyby_demotions is retained as a required, always-zero field rather than
    # dropped, so a reappearance of the demotion fails loudly here rather than
    # silently reaching a dataset.
    total_demotions = sum(e["flyby_demotions"] for e in per_snapshot.values())
    if total_demotions != 0:
        raise ConverterError(
            "flyby_demotions is {} across the dataset, expected 0 — fix_flybys was removed and "
            "MostBoundID must always be positive; a non-zero count means a demotion was recorded "
            "by a stage that should no longer be able to produce one".format(total_demotions)
        )

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


# ==========================================================================
# Format version 3
# ==========================================================================
#
# The report of a generic (manifest version 3) conversion. Everything above is
# the legacy ASCII-to-v2 report and is unchanged.
#
# It states what was converted and what that output can and cannot do yet:
# source format, the ACTUAL format version the emitted files declare (read
# back by the battery, not assumed), the mapping and source layout, link-gap
# counts, index bounds, resource measurements, the validation outcomes, and
# its runtime status: that the current Mimic consumes format version 3, which
# routes a parity gate has actually validated, and what still limits it. It
# carries the consumer payload-metadata fragment, labelled insufficient for
# runtime execution. It writes into the conversion workdir only: no
# simulation package, halo_properties.yaml or run file is created or changed.

REPORT_V3_KIND = "mimic-generic-conversion-report"

#: ``INT32_MAX``: a slab above it is addressable only through int64 indices.
_C_INT_MAX = 2**31 - 1

#: The format version the current reader and driver consume through this
#: report's route; ``runnable_by_current_mimic`` is True exactly when the
#: emitted files all declare it.
_V3_CONSUMED_FORMAT_VERSIONS = [3]

#: Stated for every v3 dataset, whatever it contains. The route sentences come
#: from runtime_routes, the converter's one copy of the evidenced routes; the
#: payload sentence is about the schema rule, not about routes, so it lives here.
_V3_STANDING_LIMITATIONS = runtime_routes.standing_limitations() + (
    "Payload units and precision are the source's native ones as /schema declares them; "
    "every field a consuming simulation package's halo_properties.yaml declares must match "
    "/schema (undeclared /schema fields are validated and ignored), so there is one package "
    "per simulation and source format, and this converter neither writes nor edits one.",
)


def _v3_limitations(measured: Mapping, schema) -> List[str]:
    limitations = list(_V3_STANDING_LIMITATIONS)
    gapped = int(measured.get("gapped_descendants", 0))
    if gapped:
        limitations.append(
            "{} Descendant link(s) skip snapshots (longest span {}); the horizontal driver "
            "retains each snapshot generation until its descendants' snapshots are processed, "
            "so a run may retain more than two generations at once, at most the longest "
            "descendant span plus one.".format(gapped, measured.get("max_descendant_span"))
        )
    counts = measured.get("snapshot_counts") or []
    widest = max(counts) if counts else 0
    if widest > _C_INT_MAX:
        limitations.append(
            "The largest snapshot holds {} halos, above INT32_MAX ({}); the reader and driver "
            "index it with int64; the driver refuses it as an output snapshot above INT32_MAX "
            "and warns above 1e9 rows; otherwise, on forest-blocked output a chunk's memory "
            "(bounded below by the largest forest) decides whether it can run, and on "
            "consistent_trees_ascii output, which cannot be chunked, whole-slab memory "
            "does.".format(widest, _C_INT_MAX)
        )
    if schema.extra_fields:
        limitations.append(
            "{} declared extra field(s) ({}) are validated at open but materialised only "
            "where the consuming package declares them.".format(
                len(schema.extra_fields), ", ".join(extra.name for extra in schema.extra_fields)
            )
        )
    return limitations


def _v3_multiplier(max_rank: int, n_forests_total: int) -> Dict[str, object]:
    window = identity_multiplier_window(max_rank, n_forests_total)
    try:
        recommended = recommended_multiplier(max_rank, n_forests_total)
    except ConverterError as exc:
        return {"window": list(window), "recommended": None, "reason": str(exc)}
    return {"window": list(window), "recommended": recommended, "reason": None}


def build_report_v3(manifest, battery, dataset_dir) -> dict:
    """Assemble the v3 report from the generic manifest and a v3 battery
    result (:class:`validate_v3.V3BatteryResult`)."""
    schema = manifest.schema
    measured = dict(battery.measurements)
    configuration = manifest.configuration
    transpose = manifest.stage("transpose").get("result") or {}
    writer = (manifest.stage("write").get("result") or {}).get("writer")
    counts = [int(value) for value in measured.get("snapshot_counts") or []]
    total_halos = int(sum(counts))
    emitted_bytes = int(sum(measured.get("snapshot_bytes") or [])) + int(
        measured.get("sidecar_bytes") or 0
    )
    n_forests_total = int(measured.get("n_forests_total", 0))
    max_rank = int(measured.get("header_max_halo_rank_in_forest", -1))
    int32_max = int(np.iinfo(np.int32).max)
    widest = max(counts) if counts else 0
    return {
        "report_kind": REPORT_V3_KIND,
        "workdir": str(manifest.workdir),
        "dataset_dir": str(Path(dataset_dir).resolve()),
        "source": {
            "source_format": schema.source_format,
            "adapter_parameters": configuration["adapter"]["parameters"],
            "inventory": manifest.inventory,
            "dependencies": [
                {key: record.get(key) for key in ("path", "roles", "size_bytes", "sha256")}
                for record in manifest.dependencies
            ],
        },
        "format": {
            "declared_format_versions": measured.get("format_versions"),
            "writer": writer,
            "links_adjacent": measured.get("header_links_adjacent"),
            "links_adjacent_measured": measured.get("links_adjacent_measured"),
        },
        "mapping": {
            "column_mapping_sha256": schema.digest,
            "schema": schema.as_canonical(),
            "source_layout": (
                None if schema.source_layout is None else schema.source_layout.as_canonical()
            ),
        },
        "totals": {
            "halos": total_halos,
            "snapshots": len(counts),
            "snapshots_with_halos": sum(1 for count in counts if count),
            "n_forests_total": n_forests_total,
            "len_zero": measured.get("len_zero"),
        },
        "per_snapshot": [
            {"snapshot": snap, "halos": count, "file_bytes": size}
            for snap, (count, size) in enumerate(zip(counts, measured.get("snapshot_bytes") or []))
        ],
        "links": {
            "non_null": measured.get("non_null_links"),
            "gapped_descendants": measured.get("gapped_descendants"),
            "max_descendant_span": measured.get("max_descendant_span"),
            "gapped_first_progenitors": measured.get("gapped_first_progenitors"),
            "max_first_progenitor_span": measured.get("max_first_progenitor_span"),
            "next_progenitors_off_owner_snapshot": measured.get(
                "next_progenitors_off_owner_snapshot"
            ),
            "transpose_gapped_descendants": transpose.get("n_gapped_descendants"),
        },
        "index_bounds": {
            "source_halo_id_min": measured.get("min_source_halo_id"),
            "source_halo_id_max": measured.get("max_source_halo_id"),
            "max_forest_index": measured.get("max_forest_index"),
            "max_halo_rank_in_forest": max_rank,
            "max_snapshot_halos": widest,
            "max_link_row": measured.get("max_link_row"),
            "int32_max": int32_max,
            "any_index_above_int32": any(
                value is not None and int(value) > int32_max
                for value in (
                    widest,
                    measured.get("max_link_row"),
                    measured.get("max_source_halo_id"),
                )
            ),
        },
        "identity_multiplier": _v3_multiplier(max_rank, n_forests_total),
        "resources": {
            "emitted_bytes": emitted_bytes,
            "emitted_bytes_per_halo": (emitted_bytes / total_halos) if total_halos else None,
            "record_itemsize": {
                "ingest": configuration["record_dtypes"]["ingest"].get("itemsize"),
                "transposed": configuration["record_dtypes"]["transposed"].get("itemsize"),
            },
            "ingest_max_rows": configuration["ingest_max_rows"],
            "transpose": {
                "budget_bytes": configuration["transpose_budget_bytes"],
                "peak_resident_bytes": transpose.get("peak_resident_bytes"),
                "peak_spill_bytes": transpose.get("peak_spill_bytes"),
                "sort_runs": transpose.get("sort_runs"),
                "sort_merge_passes": transpose.get("sort_merge_passes"),
                "chain_rounds": transpose.get("chain_rounds"),
            },
            "validation": {
                "budget_bytes": measured.get("budget_bytes"),
                "peak_resident_bytes": measured.get("peak_resident_bytes"),
                "peak_spill_bytes": measured.get("peak_spill_bytes"),
            },
            "note": (
                "bytes per halo includes each dataset's final partially-filled 65536-row "
                "chunk, which dominates for small datasets"
            ),
        },
        "runtime_compatibility": {
            # Format capability ("format consumed"), not route validation: True when
            # every emitted file declares a version the current reader and driver
            # consume. Which routes are validated is in the limitations text.
            "runnable_by_current_mimic": measured.get("format_versions")
            == _V3_CONSUMED_FORMAT_VERSIONS,
            "limitations": _v3_limitations(measured, schema),
        },
        "consumer_metadata_fragment": {
            "sufficient_for_runtime_execution": False,
            "label": (
                "Payload types, native units and core-role bindings only. Insufficient to enable "
                "runtime execution: it does not describe the reader-owned target-snapshot and "
                "SourceHaloID arrays, gapped-link retention state, or the package's "
                "simulation_info.yaml and a_list, and no simulation package was written from it."
            ),
            "simulation_packages_written": False,
            "fragment": schema.consumer_metadata_fragment(),
        },
        "validation": [outcome.as_dict() for outcome in battery.outcomes],
        "validation_passed": not battery.failed,
    }


def _v3_runtime_status_header(report: dict) -> str:
    """The report's opening runtime-status line, rendered from the measured
    ``runnable_by_current_mimic`` flag rather than assumed: a failed or
    structurally rejected dataset may never have had its format measured."""
    if report["runtime_compatibility"]["runnable_by_current_mimic"]:
        return (
            "FORMAT CONSUMED BY THE CURRENT MIMIC; ROUTE NOT VALIDATED BY THIS CONVERSION -- "
            "see runtime compatibility below."
        )
    declared = report["format"]["declared_format_versions"]
    if not declared:
        return (
            "FORMAT VERSION UNMEASURED; NOT CONFIRMED AS CONSUMED BY THE CURRENT MIMIC -- "
            "see validation and runtime compatibility below."
        )
    return (
        "FORMAT NOT CONSUMED BY THE CURRENT MIMIC (declared format version(s) {}) -- see "
        "runtime compatibility below.".format(declared)
    )


def render_text_v3(report: dict) -> str:
    source = report["source"]
    totals = report["totals"]
    links = report["links"]
    bounds = report["index_bounds"]
    resources = report["resources"]
    multiplier = report["identity_multiplier"]
    lines = [
        "Conversion report (horizontal-HDF5 format version 3)",
        "====================================================",
        "",
        _v3_runtime_status_header(report),
        "",
        "workdir:        {}".format(report["workdir"]),
        "dataset dir:    {}".format(report["dataset_dir"]),
        "source format:  {}".format(source["source_format"]),
        "format version: {} (declared by the emitted files)".format(
            report["format"]["declared_format_versions"]
        ),
        "mapping:        column_mapping_sha256 {}".format(
            report["mapping"]["column_mapping_sha256"]
        ),
        "source layout:  {}".format(
            "none (not a fixed-record source)"
            if report["mapping"]["source_layout"] is None
            else "{} byte order, {}-byte records".format(
                report["mapping"]["source_layout"]["byte_order"],
                report["mapping"]["source_layout"]["itemsize"],
            )
        ),
        "",
        "totals: {} halo(s) in {} snapshot file(s) ({} populated); {} forest(s); {} "
        "Len==0 halo(s)".format(
            totals["halos"],
            totals["snapshots"],
            totals["snapshots_with_halos"],
            totals["n_forests_total"],
            totals["len_zero"],
        ),
        "links: links_adjacent={} (measured {}); {} gapped Descendant link(s), longest span {}; "
        "{} gapped FirstProgenitor link(s); {} NextProgenitor link(s) off their owner's "
        "snapshot".format(
            report["format"]["links_adjacent"],
            report["format"]["links_adjacent_measured"],
            links["gapped_descendants"],
            links["max_descendant_span"],
            links["gapped_first_progenitors"],
            links["next_progenitors_off_owner_snapshot"],
        ),
        "       non-null: {}".format(links["non_null"]),
        "index bounds: SourceHaloID [{}, {}]; max ForestIndex {}; max rank {}; largest snapshot "
        "{} halo(s); largest link row {}; any index above INT32_MAX: {}".format(
            bounds["source_halo_id_min"],
            bounds["source_halo_id_max"],
            bounds["max_forest_index"],
            bounds["max_halo_rank_in_forest"],
            bounds["max_snapshot_halos"],
            bounds["max_link_row"],
            bounds["any_index_above_int32"],
        ),
        "identity multiplier: window {}, recommended {}{}".format(
            multiplier["window"],
            multiplier["recommended"],
            "" if multiplier["reason"] is None else " ({})".format(multiplier["reason"]),
        ),
        "resources: {} byte(s) emitted ({} B/halo); transpose peak resident {} / spill {} "
        "byte(s); validation peak resident {} / spill {} byte(s)".format(
            resources["emitted_bytes"],
            (
                "n/a"
                if resources["emitted_bytes_per_halo"] is None
                else "{:.2f}".format(resources["emitted_bytes_per_halo"])
            ),
            resources["transpose"]["peak_resident_bytes"],
            resources["transpose"]["peak_spill_bytes"],
            resources["validation"]["peak_resident_bytes"],
            resources["validation"]["peak_spill_bytes"],
        ),
        "           ({})".format(resources["note"]),
        "",
        "per-snapshot halos / file bytes:",
    ]
    for entry in report["per_snapshot"]:
        lines.append(
            "  snapshot {:>3} -- {} / {}".format(
                entry["snapshot"], entry["halos"], entry["file_bytes"]
            )
        )
    lines += [
        "",
        "runtime compatibility: format consumed by the current Mimic: {} "
        "(format capability, not route validation)".format(
            "YES" if report["runtime_compatibility"]["runnable_by_current_mimic"] else "NO"
        ),
    ]
    lines += ["  - {}".format(text) for text in report["runtime_compatibility"]["limitations"]]
    fragment = report["consumer_metadata_fragment"]
    lines += ["", "consumer payload-metadata fragment (INSUFFICIENT for runtime execution):"]
    lines.append("  {}".format(fragment["label"]))
    for entry in fragment["fragment"]["halo_properties"]:
        lines.append(
            "  {:<16} {:<10} {:<22} h:{:<8} core role: {}".format(
                entry["name"],
                entry["type"],
                entry["units"],
                entry["h_convention"],
                entry["provides_core_role"] or "-",
            )
        )
    for entry in fragment["fragment"]["format_table_fields"]:
        lines.append(
            "  {:<16} {:<10} {:<33} core role: {}".format(
                entry["name"], entry["type"], "(format table)", entry["provides_core_role"] or "-"
            )
        )
    lines += ["", "validation outcomes:"]
    for outcome in report["validation"]:
        text = "  {}: {}".format(outcome["name"], outcome["status"])
        if outcome["detail"]:
            text += " -- {}".format(outcome["detail"])
        lines.append(text)
    lines += ["", "validation: {}".format("PASS" if report["validation_passed"] else "FAIL"), ""]
    return "\n".join(lines)


def _write_atomic(path: Path, text: str) -> None:
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "w") as handle:
        handle.write(text)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(tmp, path)


def write_report_v3(report: dict, workdir) -> Path:
    """Write the v3 JSON record and its text rendering into the workdir;
    returns the JSON path."""
    workdir = Path(workdir)
    json_path = workdir / REPORT_JSON
    _write_atomic(json_path, json.dumps(report, indent=2, sort_keys=True) + "\n")
    _write_atomic(workdir / REPORT_TXT, render_text_v3(report))
    return json_path


def run_report_v3(
    workdir,
    a_list_path,
    multiplier: int = DEFAULT_MULTIPLIER,
    *,
    budget_bytes: int = DEFAULT_V3_BUDGET_BYTES,
    spill_dir=None,
    battery: Optional[V3BatteryResult] = None,
) -> dict:
    """Validate a completed generic conversion's v3 dataset and write its
    report into the workdir. The battery outcome is in the report AND in its
    ``validation_passed``; a failing dataset still gets a report, never a
    quietly successful one.

    ``battery``, when given, is a :func:`run_battery_v3` result the caller
    already holds for this conversion's dataset and ``a_list_path``; it is
    reported as is and the battery is not run again, so ``multiplier``,
    ``budget_bytes`` and ``spill_dir`` are then unused. Binding that result to
    this dataset is the caller's obligation."""
    manifest = ConversionManifest.load(workdir)
    manifest.require_complete("write")
    dataset_dir = manifest.artifact_path(manifest.stage("write")["directory"])
    if battery is None:
        battery = run_battery_v3(
            dataset_dir,
            a_list_path,
            manifest_path=manifest.path,
            multiplier=multiplier,
            budget_bytes=budget_bytes,
            spill_dir=spill_dir,
        )
    report = build_report_v3(manifest, battery, dataset_dir)
    json_path = write_report_v3(report, manifest.workdir)
    print(
        "report: wrote {} and {} -- validation {}".format(
            json_path,
            Path(manifest.workdir) / REPORT_TXT,
            "PASS" if report["validation_passed"] else "FAIL",
        ),
        file=sys.stderr,
    )
    return report
