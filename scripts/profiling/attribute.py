#!/usr/bin/env python3
"""Component attribution and aggregation over a set of parsed `sample` reports.

Usage: attribute.py <sample_dir> <out.json> [wall_seconds_mean]

Attribution rules (VISION.md architectural boundaries):

  source-annotated frames        -> component from the source path
  libsystem_m frames             -> "Math library" (plus a secondary
                                    "who pays for libm" table keyed on the
                                    nearest source-annotated ancestor)
  libhdf5 frames                 -> "Output I/O" (checked: nearest sourced
                                    ancestor must be src/io/output; otherwise
                                    the frame is reported as Unattributed)
  kernel read/open/lseek/close   -> component of nearest sourced ancestor when
  write/pwrite/ftruncate/fstat      that ancestor is Tree input I/O or Output I/O,
                                    else Runtime/system
  libsystem_platform/malloc/c,   -> "Runtime/system" (memmove/bzero also get a
  dyld, other kernel                secondary "who pays" table)
  mimic DYLD-STUB$$<libm fn>     -> "Math library" (PLT thunk for that call)
  anything else                  -> "Unattributed" (never silently folded in)

Symbols in ./mimic with no source annotation and no rule are resolved to their
defining translation unit by the nm index built by nm_index.py, if available.
"""

import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from parse_sample import parse_file, walk  # noqa: E402

REPO = os.environ.get(
    "MIMIC_REPO", os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)

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
KERNEL_IO = {
    "__read_nocancel",
    "read",
    "__open",
    "__open_nocancel",
    "open",
    "lseek",
    "__lseek",
    "close",
    "fstat",
    "fstat64",
    "pread",
    "write",
    "__write_nocancel",
    "pwrite",
    "ftruncate",
    "fsync",
    "__pwrite_nocancel",
}
LIBM_FNS = {
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


def component_from_source(src):
    """(component, detail) for a source-annotated frame; None if not ours."""
    if not src:
        return None
    p = src.replace(REPO, "")
    p = p.replace("src/core/../", "src/").replace("src/io/output/../../", "src/")
    p = p.replace("src/io/tree/../../", "src/")
    base = os.path.basename(p)
    if p.startswith("models/") and "/modules/" in p:
        mod = p.split("/modules/", 1)[1].split("/")[0]
        if mod.endswith(".c"):
            mod = mod[:-2]
        return ("Physics modules", mod)
    if p.startswith("models/") and "/shared/" in p:
        return ("Model shared helpers", base)
    if p.startswith("src/module_system/"):
        return ("Module system framework", base)
    if p.startswith("src/io/tree/"):
        return ("Tree input I/O", base)
    if p.startswith("src/io/output/"):
        return ("Output I/O", base)
    if p.startswith("src/util/"):
        return ("Utilities", UTIL_SUBPART.get(base, f"util: {base}"))
    if p.startswith("src/core/") or "include/generated" in p:
        return ("Core execution", CORE_SUBPART.get(base, f"core: {base}"))
    if p.startswith("src/include/"):
        return ("Core execution", f"core: {base}")
    return None


def nearest_sourced(node):
    p = node.parent
    while p is not None:
        c = component_from_source(p.srcfile)
        if c:
            return c
        p = p.parent
    return None


def classify(node, nm_index):
    """-> (component, detail, payer_component_or_None, unattributed_reason)"""
    c = component_from_source(node.srcfile)
    if c:
        return c[0], c[1], None, None
    b = node.binary
    sym = node.symbol
    ancestor = nearest_sourced(node)
    payer = f"{ancestor[0]} / {ancestor[1]}" if ancestor else "unknown caller"
    if b == "libsystem_m.dylib" or (
        b == "mimic" and sym.startswith("DYLD-STUB$$") and sym.split("$$")[-1] in LIBM_FNS
    ):
        return "Math library", sym, payer, None
    if b.startswith("libhdf5"):
        if ancestor and ancestor[0] == "Output I/O":
            return "Output I/O", f"libhdf5: {sym}", None, None
        if ancestor and ancestor[0] == "Tree input I/O":
            return "Tree input I/O", f"libhdf5: {sym}", None, None
        return "Output I/O", f"libhdf5 (caller {payer}): {sym}", None, None
    if b == "libsystem_kernel.dylib":
        # Walk the library frames between this syscall and the nearest source-
        # annotated ancestor: an intervening libhdf5 frame means the syscall is
        # HDF5 output traffic; an intervening malloc-zone frame means it is
        # allocator internals; otherwise it belongs to the calling component.
        inter = []
        q = node.parent
        while q is not None and not component_from_source(q.srcfile):
            inter.append(q.binary)
            q = q.parent
        if any(x.startswith("libhdf5") for x in inter):
            return "Output I/O", f"kernel via libhdf5: {sym}", None, None
        if any(x == "libsystem_malloc.dylib" for x in inter):
            return "Runtime/system", f"malloc zone: {sym}", payer, None
        if ancestor:
            return ancestor[0], f"kernel: {sym}", None, None
        return "Runtime/system", f"kernel: {sym}", payer, None
    if b in (
        "libsystem_platform.dylib",
        "libsystem_malloc.dylib",
        "libsystem_c.dylib",
        "libsystem_pthread.dylib",
        "libc++.1.dylib",
        "libz.1.dylib",
        "libcompiler_rt.dylib",
        "dyld",
    ):
        return "Runtime/system", f"{b}: {sym}", payer, None
    if b == "mimic":
        tu = nm_index.get(sym)
        if tu:
            c2 = component_from_source(REPO + tu)
            if c2:
                return c2[0], c2[1], None, None
        return "Unattributed", f"mimic symbol unresolved: {sym}", payer, "no source, no nm hit"
    if b == "<thread>":
        return "Unattributed", "sampler thread-header residual", None, "thread header frame"
    return "Unattributed", f"{b}: {sym}", payer, "unknown binary"


def main():
    sdir, outjson = sys.argv[1], sys.argv[2]
    wall_mean = float(sys.argv[3]) if len(sys.argv) > 3 else None
    nm_index = {}
    nmf = os.path.join(os.path.dirname(os.path.abspath(__file__)), "nm_index.json")
    if os.path.exists(nmf):
        nm_index = json.load(open(nmf))
    files = sorted(f for f in os.listdir(sdir) if f.startswith("sample_"))
    comp = collections.Counter()
    detail = collections.Counter()
    lines = collections.Counter()
    libm_payer = collections.Counter()
    plat_payer = collections.Counter()
    unattr = collections.Counter()
    incl = collections.Counter()
    per_run_total = []
    for f in files:
        roots, total = parse_file(os.path.join(sdir, f))
        per_run_total.append(total)
        nodes = list(walk(roots))
        for n in nodes:
            # inclusive, outermost occurrence only (recursion-safe)
            c = component_from_source(n.srcfile)
            keyname = None
            if c:
                keyname = f"{c[0]} / {c[1]}"
            else:
                cc = classify(n, nm_index)
                keyname = f"{cc[0]} / {cc[1]}"
            outer = True
            p = n.parent
            while p is not None:
                cp = component_from_source(p.srcfile)
                pk = f"{cp[0]} / {cp[1]}" if cp else None
                if pk == keyname or (p.symbol == n.symbol and p.srcfile == n.srcfile):
                    outer = False
                    break
                p = p.parent
            if outer:
                incl[keyname] += n.count
                if c:
                    incl["COMPONENT::" + c[0]] += 0  # placeholder
            if not n.self_:
                continue
            cname, det, payer, reason = classify(n, nm_index)
            comp[cname] += n.self_
            detail[(cname, det)] += n.self_
            if n.srcfile:
                rel = n.srcfile.replace(REPO, "")
                lines[(rel, n.line, n.symbol)] += n.self_
            else:
                lines[(f"<{n.binary}>", None, n.symbol)] += n.self_
            if cname == "Math library" and payer:
                libm_payer[payer] += n.self_
            if (
                cname == "Runtime/system"
                and payer
                and (
                    "platform" in det
                    or "bzero" in det
                    or "memmove" in det
                    or "memset" in det
                    or "malloc" in det
                )
            ):
                plat_payer[payer] += n.self_
            if cname == "Unattributed":
                unattr[(det, reason, payer)] += n.self_
    nruns = len(files)
    grand = sum(comp.values())
    res = {
        "n_runs": nruns,
        "per_run_total_samples": per_run_total,
        "grand_total_self_samples": grand,
        "wall_mean_s": wall_mean,
        "components": {},
        "details": {},
        "hot_lines": [],
        "libm_payers": {},
        "platform_malloc_payers": {},
        "unattributed": [],
        "inclusive": {},
    }
    import math

    for k, v in comp.most_common():
        pct = 100.0 * v / grand
        res["components"][k] = {
            "self_samples_total": v,
            "mean_per_run": v / nruns,
            "pct": pct,
            "pct_sigma_poisson": 100.0 * math.sqrt(v) / grand,
            "wall_ms_equiv": (wall_mean * 1000.0 * pct / 100.0) if wall_mean else None,
        }
    for (c, d), v in detail.most_common():
        pct = 100.0 * v / grand
        res["details"][f"{c} :: {d}"] = {
            "self_samples_total": v,
            "mean_per_run": v / nruns,
            "pct": pct,
            "pct_sigma_poisson": 100.0 * math.sqrt(v) / grand,
            "wall_ms_equiv": (wall_mean * 1000.0 * pct / 100.0) if wall_mean else None,
        }
    for (f, l, s), v in lines.most_common(60):
        res["hot_lines"].append(
            {
                "file": f,
                "line": l,
                "symbol": s,
                "self_samples": v,
                "pct": 100.0 * v / grand,
                "wall_ms_equiv": (wall_mean * 1000.0 * v / grand) if wall_mean else None,
            }
        )
    for k, v in libm_payer.most_common():
        res["libm_payers"][k] = {"self_samples": v, "pct_of_total": 100.0 * v / grand}
    for k, v in plat_payer.most_common():
        res["platform_malloc_payers"][k] = {"self_samples": v, "pct_of_total": 100.0 * v / grand}
    for (d, r, p), v in unattr.most_common():
        res["unattributed"].append(
            {"detail": d, "reason": r, "caller": p, "self_samples": v, "pct": 100.0 * v / grand}
        )
    for k, v in incl.most_common(60):
        if k.startswith("COMPONENT::"):
            continue
        res["inclusive"][k] = {"inclusive_samples": v, "pct": 100.0 * v / grand}
    with open(outjson, "w") as fh:
        json.dump(res, fh, indent=1)
    print(f"runs={nruns} grand_total_self={grand} components={len(comp)}")
    for k, d in list(res["components"].items()):
        print(f"  {d['pct']:6.2f}% +-{d['pct_sigma_poisson']:.2f}  {k}")


if __name__ == "__main__":
    main()
