#!/usr/bin/env python3
"""Component attribution and aggregation over a set of parsed `sample` reports.

    attribute.py <sample_dir> <out.json> [--wall-mean-s S] [--nm-index PATH]
                 [--skip-first] [--max-unattributed-pct P]

Every self-sample is assigned to exactly one component along the ownership boundaries in
`docs/VISION.md`:

  source-annotated frames        -> component from the repository-relative source path
  libsystem_m frames             -> "Math library", plus a secondary "who pays for libm"
                                    table keyed on the nearest source-annotated ancestor
  libhdf5 frames                 -> the I/O component of the nearest sourced ancestor,
                                    falling back to "Output I/O" when no I/O ancestor is found
  kernel syscalls                -> the same I/O component when reached through libhdf5,
                                    "Runtime/system" when reached through the allocator,
                                    otherwise the component of the nearest sourced ancestor
  libsystem_platform/malloc/c,   -> "Runtime/system"; memmove/bzero/malloc also get a
  dyld, other kernel                secondary "who pays" table
  mimic DYLD-STUB$$<libm fn>     -> "Math library" (the PLT thunk for that call)
  anything else                  -> "Unattributed", never silently folded into a component

Frames in ./mimic with no source annotation and no rule are resolved to their defining
translation unit through the index built by nm_index.py, when one is available.

Two health checks decide whether the attribution itself can be trusted, because the way
this harness fails is not a crash but a plausible-looking profile that is quietly wrong.
Both exit non-zero rather than adding a footnote:

  - The reports must carry self-samples at all, and some of them must carry file:line
    annotations.  Without annotations the whole method rests on the coarser symbol index,
    which is exactly the state the next check exists to detect.
  - Source-annotated samples must overwhelmingly map to a component.  When they do not,
    the report's paths do not sit under REPO, and every annotated frame has silently
    fallen through to the symbol-index fallback.
  - The Unattributed share must stay small, or whole binaries matched no rule at all.
"""

import argparse
import collections
import json
import math
import os
import sys
from functools import lru_cache

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from parse_sample import parse_file, walk  # noqa: E402

REPO = os.path.realpath(
    os.environ.get(
        "MIMIC_REPO",
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    )
)
DEFAULT_NM_INDEX = os.path.join(os.path.dirname(os.path.abspath(__file__)), "nm_index.json")
DEFAULT_MAX_UNATTRIBUTED_PCT = 1.0
# Below this share of source-annotated samples mapping to a component, REPO is wrong.
MIN_SOURCE_MAPPED_FRACTION = 0.9
HOT_LINE_LIMIT = 60
INCLUSIVE_LIMIT = 60
UNMAPPED_EXAMPLE_LIMIT = 5

CORE_SUBPART = {
    "tree_driver.c": "core: tree_driver",
    "snapshot_driver.c": "core: snapshot_driver",
    "build_model.c": "core: build_model",
    "inheritance.c": "core: inheritance",
    "galaxy_pool.c": "core: galaxy_pool",
    "output_buffer.c": "core: output_buffer/marshalling",
    "copy_to_output.inc": "core: output_buffer/marshalling",
    "populate_halo_payload.inc": "core: output_buffer/marshalling",
    "read_parameter_file.c": "core: init + config parsing",
    "init.c": "core: init + config parsing",
    "allvars.c": "core: init + config parsing",
    "main.c": "core: main",
    "module_registry.c": "core: module dispatch (module_registry.c)",
    "halo_evolution.c": "core: halo_evolution",
    "timestep.c": "core: timestep",
    "virial.c": "core: virial",
}
UTIL_SUBPART = {
    "error.c": "util: logging/error",
    "run_log.c": "util: logging/error",
    "memory.c": "util: memory",
    "numeric.c": "util: numeric",
    "integration.c": "util: numeric",
    "progress.c": "util: progress",
    "io.c": "util: io",
    "run_profile.c": "util: run_profile",
    "version.c": "util: version",
}
LIBM_FNS = frozenset(
    {
        "log10",
        "__exp10",
        "exp",
        "pow",
        "cbrt",
        "log",
        "sqrt",
        "exp2",
        "log2",
        "sin",
        "cos",
        "atan",
        "pow_precise",
    }
)
RUNTIME_BINARIES = frozenset(
    {
        "libsystem_platform.dylib",
        "libsystem_malloc.dylib",
        "libsystem_c.dylib",
        "libsystem_pthread.dylib",
        "libc++.1.dylib",
        "libz.1.dylib",
        "libcompiler_rt.dylib",
        "dyld",
    }
)
# Runtime details worth a secondary "who pays" table: bulk memory work and allocation.
RUNTIME_PAYER_MARKERS = ("platform", "bzero", "memmove", "memset", "malloc")
# Components that own file I/O, and so keep ownership of libhdf5 work done for them.
IO_COMPONENTS = frozenset({"Output I/O", "Tree input I/O", "Snapshot input I/O"})


@lru_cache(maxsize=None)
def repo_relative(src):
    """Repository-relative form of a source annotation, or None if it is not ours.

    `sample -fullPaths` emits absolute paths, while annotations for headers reached through
    relative includes arrive as `src/core/../include/x.h`.  Both must reduce to the same
    repository-relative path, because attribution dispatches on a path prefix: a stray
    leading separator or an unresolved `..` sends every frame to Unattributed instead.
    """
    if not src:
        return None
    path = os.path.normpath(src)
    if not os.path.isabs(path):
        return None if path.startswith(os.pardir) else path
    relative = os.path.relpath(os.path.realpath(path), REPO)
    return None if relative.startswith(os.pardir) else relative


@lru_cache(maxsize=None)
def component_from_source(src):
    """(component, detail) for a source-annotated frame, or None if not repository source."""
    path = repo_relative(src)
    if path is None:
        return None
    base = os.path.basename(path)
    if path.startswith("models/") and "/modules/" in path:
        module = path.split("/modules/", 1)[1].split("/")[0]
        if module.endswith(".c"):
            module = module[:-2]  # a standalone prototype module, not a module directory
        return ("Physics modules", module)
    if path.startswith("models/") and "/shared/" in path:
        return ("Model shared helpers", base)
    if path.startswith("src/module_system/"):
        return ("Module system framework", base)
    if path.startswith("src/io/tree/"):
        return ("Tree input I/O", base)
    if path.startswith("src/io/snapshot/"):
        return ("Snapshot input I/O", base)
    if path.startswith("src/io/output/"):
        return ("Output I/O", base)
    if path.startswith("src/util/"):
        return ("Utilities", UTIL_SUBPART.get(base, f"util: {base}"))
    # src/include/generated holds the generated marshalling includes, which are core work.
    if path.startswith("src/core/") or path.startswith("src/include/"):
        return ("Core execution", CORE_SUBPART.get(base, f"core: {base}"))
    return None


def nearest_sourced(node):
    """(component, detail) of the closest source-annotated ancestor, or None."""
    parent = node.parent
    while parent is not None:
        component = component_from_source(parent.srcfile)
        if component:
            return component
        parent = parent.parent
    return None


def classify(node, nm_index):
    """Assign one frame to a component.

    Args:
        node: a parsed frame.
        nm_index: symbol -> source path map from nm_index.py; may be empty.

    Returns:
        (component, detail, payer, reason).  `payer` names the component that caused a
        library or kernel frame, for the secondary tables; `reason` is set only for
        Unattributed frames and records why no rule matched.
    """
    component = component_from_source(node.srcfile)
    if component:
        return component[0], component[1], None, None

    binary = node.binary
    symbol = node.symbol
    ancestor = nearest_sourced(node)
    payer = f"{ancestor[0]} / {ancestor[1]}" if ancestor else "unknown caller"

    if binary == "libsystem_m.dylib" or (
        binary == "mimic"
        and symbol.startswith("DYLD-STUB$$")
        and symbol.split("$$")[-1] in LIBM_FNS
    ):
        return "Math library", symbol, payer, None
    if binary.startswith("libhdf5"):
        if ancestor and ancestor[0] in IO_COMPONENTS:
            return ancestor[0], f"libhdf5: {symbol}", None, None
        return "Output I/O", f"libhdf5 (caller {payer}): {symbol}", None, None
    if binary == "libsystem_kernel.dylib":
        # Walk the library frames between this syscall and the nearest source-annotated
        # ancestor: an intervening libhdf5 frame means the syscall is HDF5 output traffic;
        # an intervening malloc-zone frame means allocator internals; otherwise the syscall
        # belongs to the calling component.
        intervening = []
        parent = node.parent
        while parent is not None and not component_from_source(parent.srcfile):
            intervening.append(parent.binary)
            parent = parent.parent
        if any(b.startswith("libhdf5") for b in intervening):
            # An HDF5 read on behalf of a tree or snapshot reader is input, not output.
            owner = ancestor[0] if ancestor and ancestor[0] in IO_COMPONENTS else "Output I/O"
            return owner, f"kernel via libhdf5: {symbol}", None, None
        if "libsystem_malloc.dylib" in intervening:
            return "Runtime/system", f"malloc zone: {symbol}", payer, None
        if ancestor:
            return ancestor[0], f"kernel: {symbol}", None, None
        return "Runtime/system", f"kernel: {symbol}", payer, None
    if binary in RUNTIME_BINARIES:
        return "Runtime/system", f"{binary}: {symbol}", payer, None
    if binary == "mimic":
        source = nm_index.get(symbol)
        if source:
            resolved = component_from_source(source)
            if resolved:
                return resolved[0], resolved[1], None, None
            return (
                "Unattributed",
                f"mimic symbol in unmapped source: {symbol}",
                payer,
                f"nm hit {source}, no component rule",
            )
        return "Unattributed", f"mimic symbol unresolved: {symbol}", payer, "no source, no nm hit"
    if binary == "<thread>":
        return "Unattributed", "sampler thread-header residual", None, "thread header frame"
    return "Unattributed", f"{binary}: {symbol}", payer, "unknown binary"


class Aggregation:
    """Self-sample and inclusive-sample tallies accumulated across sample reports."""

    def __init__(self):
        self.components = collections.Counter()
        self.details = collections.Counter()
        self.lines = collections.Counter()
        self.libm_payers = collections.Counter()
        self.runtime_payers = collections.Counter()
        self.unattributed = collections.Counter()
        self.inclusive = collections.Counter()
        self.per_run_total_samples = []
        # Health check: every frame carrying a source annotation should map to a component.
        self.annotated_self_samples = 0
        self.mapped_self_samples = 0
        self.unmapped_sources = collections.Counter()


def _is_outermost(node, keys):
    """True when no ancestor carries the same component/detail key.

    Inclusive time is only meaningful for the outermost entry into a component; counting
    every occurrence would multiply-count recursion such as the depth-first tree walk.
    """
    key = keys[id(node)][:2]
    parent = node.parent
    while parent is not None:
        if keys[id(parent)][:2] == key:
            return False
        parent = parent.parent
    return True


def aggregate(sample_files, nm_index):
    """Tally every frame in every report into one Aggregation.

    Raises:
        ValueError: a frame has negative self-time, which means the call tree was
            reconstructed wrongly; the totals below it cannot be trusted.
    """
    agg = Aggregation()
    for path in sample_files:
        roots, total = parse_file(path)
        agg.per_run_total_samples.append(total)
        nodes = list(walk(roots))
        keys = {id(node): classify(node, nm_index) for node in nodes}
        for node in nodes:
            component, detail, payer, reason = keys[id(node)]
            if _is_outermost(node, keys):
                agg.inclusive[f"{component} / {detail}"] += node.count
            if node.self_ < 0:
                raise ValueError(
                    f"{path}: frame {node.symbol!r} has self-time {node.self_}; "
                    "the call tree was mis-nested and the aggregate would be wrong"
                )
            if not node.self_:
                continue
            if node.srcfile:
                agg.annotated_self_samples += node.self_
                if component_from_source(node.srcfile):
                    agg.mapped_self_samples += node.self_
                else:
                    # Named so a failed health check says which paths it could not place.
                    agg.unmapped_sources[node.srcfile] += node.self_
            agg.components[component] += node.self_
            agg.details[(component, detail)] += node.self_
            if node.srcfile:
                # Fall back to the raw path so a source outside the repo still names itself.
                named = repo_relative(node.srcfile) or node.srcfile
                agg.lines[(named, node.line, node.symbol)] += node.self_
            else:
                agg.lines[(f"<{node.binary}>", None, node.symbol)] += node.self_
            if component == "Math library" and payer:
                agg.libm_payers[payer] += node.self_
            if (
                component == "Runtime/system"
                and payer
                and any(marker in detail for marker in RUNTIME_PAYER_MARKERS)
            ):
                agg.runtime_payers[payer] += node.self_
            if component == "Unattributed":
                agg.unattributed[(detail, reason, payer)] += node.self_
    return agg


def _share(value, grand, n_runs, wall_mean_s):
    """Percentage, Poisson sigma, per-run mean, and wall-time equivalent for one tally."""
    pct = 100.0 * value / grand
    return {
        "self_samples_total": value,
        "mean_per_run": value / n_runs,
        "pct": pct,
        "pct_sigma_poisson": 100.0 * math.sqrt(value) / grand,
        "wall_ms_equiv": (wall_mean_s * 1000.0 * pct / 100.0) if wall_mean_s else None,
    }


def build_report(agg, n_runs, wall_mean_s):
    """Assemble the JSON report from an Aggregation."""
    grand = sum(agg.components.values())
    report = {
        "n_runs": n_runs,
        "per_run_total_samples": agg.per_run_total_samples,
        "grand_total_self_samples": grand,
        "wall_mean_s": wall_mean_s,
        "source_attribution": {
            "annotated_self_samples": agg.annotated_self_samples,
            "mapped_self_samples": agg.mapped_self_samples,
            "mapped_fraction": (
                agg.mapped_self_samples / agg.annotated_self_samples
                if agg.annotated_self_samples
                else None
            ),
            "unmapped_sources": [
                {"file": path, "self_samples": value}
                for path, value in agg.unmapped_sources.most_common(UNMAPPED_EXAMPLE_LIMIT)
            ],
        },
        "components": {},
        "details": {},
        "hot_lines": [],
        "libm_payers": {},
        "platform_malloc_payers": {},
        "unattributed": [],
        "inclusive": {},
    }
    for name, value in agg.components.most_common():
        report["components"][name] = _share(value, grand, n_runs, wall_mean_s)
    for (component, detail), value in agg.details.most_common():
        report["details"][f"{component} :: {detail}"] = _share(value, grand, n_runs, wall_mean_s)
    for (path, line, symbol), value in agg.lines.most_common(HOT_LINE_LIMIT):
        report["hot_lines"].append(
            {
                "file": path,
                "line": line,
                "symbol": symbol,
                "self_samples": value,
                "pct": 100.0 * value / grand,
                "wall_ms_equiv": (wall_mean_s * 1000.0 * value / grand) if wall_mean_s else None,
            }
        )
    for payer, value in agg.libm_payers.most_common():
        report["libm_payers"][payer] = {
            "self_samples": value,
            "pct_of_total": 100.0 * value / grand,
        }
    for payer, value in agg.runtime_payers.most_common():
        report["platform_malloc_payers"][payer] = {
            "self_samples": value,
            "pct_of_total": 100.0 * value / grand,
        }
    for (detail, reason, payer), value in agg.unattributed.most_common():
        report["unattributed"].append(
            {
                "detail": detail,
                "reason": reason,
                "caller": payer,
                "self_samples": value,
                "pct": 100.0 * value / grand,
            }
        )
    for name, value in agg.inclusive.most_common(INCLUSIVE_LIMIT):
        report["inclusive"][name] = {
            "inclusive_samples": value,
            "pct": 100.0 * value / grand,
        }
    return report


def load_nm_index(path, explicit):
    """Load the symbol index, warning when an implicit default is simply absent."""
    if os.path.exists(path):
        try:
            with open(path) as fh:
                return json.load(fh)
        except (OSError, ValueError) as exc:
            raise SystemExit(f"attribute.py: cannot read symbol index {path}: {exc}") from exc
    message = f"no symbol index at {path}; unannotated mimic frames stay Unattributed"
    if explicit:
        raise SystemExit(f"attribute.py: {message} (run nm_index.py -o {path})")
    print(f"attribute.py: warning: {message} (run nm_index.py)", file=sys.stderr)
    return {}


def find_sample_files(sample_dir, skip_first):
    """Sorted sample reports in a directory, optionally dropping the warm-up repeat."""
    if not os.path.isdir(sample_dir):
        raise SystemExit(f"attribute.py: no such sample directory: {sample_dir}")
    files = sorted(
        os.path.join(sample_dir, f)
        for f in os.listdir(sample_dir)
        if f.startswith("sample_") and f.endswith(".txt")
    )
    if skip_first:
        files = files[1:]
    if not files:
        raise SystemExit(
            f"attribute.py: no sample_*.txt reports to aggregate in {sample_dir}"
            f"{' after --skip-first' if skip_first else ''}"
        )
    return files


def main():
    parser = argparse.ArgumentParser(
        description="Attribute sampled CPU time to Mimic components along VISION boundaries."
    )
    parser.add_argument("sample_dir", help="directory of sample_NN.txt reports")
    parser.add_argument("out_json", help="where to write the aggregate report")
    parser.add_argument(
        "--wall-mean-s",
        type=float,
        help="sampler-free mean wall time, used to convert percentages to milliseconds",
    )
    parser.add_argument(
        "--nm-index",
        default=DEFAULT_NM_INDEX,
        help="symbol index from nm_index.py (default: %(default)s)",
    )
    parser.add_argument(
        "--skip-first",
        action="store_true",
        help="drop the first report as a cold-cache warm-up",
    )
    parser.add_argument(
        "--max-unattributed-pct",
        type=float,
        default=DEFAULT_MAX_UNATTRIBUTED_PCT,
        help="fail if Unattributed exceeds this share (default: %(default)s)",
    )
    args = parser.parse_args()

    nm_index = load_nm_index(args.nm_index, explicit=args.nm_index != DEFAULT_NM_INDEX)
    files = find_sample_files(args.sample_dir, args.skip_first)
    agg = aggregate(files, nm_index)
    report = build_report(agg, len(files), args.wall_mean_s)

    with open(args.out_json, "w") as fh:
        json.dump(report, fh, indent=1)

    print(
        f"runs={len(files)} grand_total_self={report['grand_total_self_samples']} "
        f"components={len(report['components'])}"
    )
    for name, share in report["components"].items():
        print(f"  {share['pct']:6.2f}% +-{share['pct_sigma_poisson']:.2f}  {name}")

    return report_health(report, args.max_unattributed_pct)


def report_health(report, max_unattributed_pct):
    """Exit status for one report: 0 when the attribution itself can be trusted.

    Two independent failures produce a plausible-looking but wrong profile, so both are
    hard failures rather than footnotes.  A low mapped fraction means the source paths in
    the reports do not sit under REPO, which silently pushes every annotated frame onto the
    symbol-index fallback -- coarser, and wrong wherever the compiler inlined across files.
    A large Unattributed share means whole binaries matched no rule at all.
    """
    if not report["grand_total_self_samples"]:
        print(
            "\nattribute.py: the reports carry no self-samples; there is nothing to attribute.",
            file=sys.stderr,
        )
        return 1

    source = report["source_attribution"]
    if not source["annotated_self_samples"]:
        print(
            "\nattribute.py: no sampled frame carries a file:line annotation, so every frame "
            "would rest on the coarser symbol index.\nBuild with symbols (-g) before profiling; "
            "see precondition 2 in README.md.",
            file=sys.stderr,
        )
        return 1

    mapped = source["mapped_fraction"]
    if mapped < MIN_SOURCE_MAPPED_FRACTION:
        print(
            f"\nattribute.py: only {100.0 * mapped:.1f}% of source-annotated samples mapped to a "
            f"component (need {100.0 * MIN_SOURCE_MAPPED_FRACTION:.0f}%).\n"
            f"The reports' source paths do not sit under MIMIC_REPO ({REPO}); "
            "attribution has silently fallen back to the symbol index.\nUnplaced paths:",
            file=sys.stderr,
        )
        for entry in source["unmapped_sources"]:
            print(f"  {entry['self_samples']:6d} samples  {entry['file']}", file=sys.stderr)
        return 1

    unattributed_pct = report["components"].get("Unattributed", {}).get("pct", 0.0)
    if unattributed_pct > max_unattributed_pct:
        print(
            f"\nattribute.py: {unattributed_pct:.2f}% Unattributed exceeds "
            f"{max_unattributed_pct:.2f}%; attribution is broken, not merely noisy.\n"
            "Largest unattributed buckets:",
            file=sys.stderr,
        )
        for entry in report["unattributed"][:5]:
            print(f"  {entry['pct']:6.2f}%  {entry['reason']}: {entry['detail']}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
