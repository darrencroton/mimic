#!/usr/bin/env python3
"""Disposable C compile probes for the proposed snapshot callback's const view.

Slice 1 of the plan specifies::

    int (*process_snapshot)(const struct SnapshotContext *ctx,
                            const struct Halo *halos, int64_t count)

and requires that a callback "writes only through halos[i].galaxy" and may not
"reorder/resize the population, change halo fields or pointers". These probes
compile small translation units against the repository's *real* headers
(``module_interface.h`` and the generated ``property_defs.h`` present in the
working tree) with ``-fsyntax-only -Werror`` and assert which writes the type
system accepts and which it rejects.

``SnapshotContext`` does not exist in the repository yet; each probe defines it
locally with exactly the four fields the plan lists. A positive probe must
compile; a negative probe must fail. A probe that compiles although the plan
forbids the behaviour is reported as a *limitation* of the const view (a rule the
compiler cannot enforce), not as a failure of the plan.

Usage::

    python3 docs/dev/snapshot-global-checks/c_const_view_probes.py [--cc CC] [--json OUT]

This is a planning experiment against headers only; it does not link, run, or
prove any driver integration.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from checklib import REPO_ROOT, Checker  # noqa: E402

HEADER = """#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "module_interface.h"
#include "types.h"

/* Local stand-in for the Slice 1 context: exactly the four fields the plan lists. */
struct SnapshotContext {
  int snapshot_number;
  double redshift;
  double time;
  const struct MimicConfig *params;
};
"""

INCLUDE_DIRS = ("src/core", "src/include", "src/util", "src", "src/module_system")
BASE_FLAGS = (
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wshadow",
    "-Wformat-security",
    "-Wundef",
    "-Werror",
    "-fsyntax-only",
)

# (name, expect_compiles, description, body)
PROBES: tuple[tuple[str, bool, str, str], ...] = (
    (
        "positive_galaxy_write_through_const_view",
        True,
        "writing halos[i].galaxy->StellarMass through const struct Halo * is legal",
        """int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)ctx;
  for (int64_t i = 0; i < count; i++) halos[i].galaxy->StellarMass = 1.0f;
  return 0;
}
""",
    ),
    (
        "positive_zero_count_null_population",
        True,
        "count zero with a NULL population pointer is expressible and needs no dereference",
        """int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)ctx;
  if (count == 0) return 0;
  if (halos == NULL) return 1;
  return 0;
}
int call(const struct SnapshotContext *ctx) { return probe(ctx, NULL, 0); }
""",
    ),
    (
        "positive_read_halo_fields_and_context",
        True,
        "reading Type, Vmax, Mvir, UniqueGalaxyID and the typed context is legal",
        """int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  double acc = ctx->redshift + ctx->time + (double)ctx->snapshot_number + ctx->params->BoxSize;
  for (int64_t i = 0; i < count; i++) {
    if (halos[i].Type == 0 || halos[i].Type == 1) acc += halos[i].Vmax + halos[i].Mvir;
    acc += (double)halos[i].UniqueGalaxyID;
  }
  return acc > 0.0 ? 0 : 0;
}
""",
    ),
    (
        "positive_sort_private_scratch_not_population",
        True,
        "sorting a private scratch array of (proxy, id, index) records is legal; the borrowed view is untouched",
        """struct RankRecord { float vpeak; long long id; int64_t index; };
static int cmp(const void *a, const void *b) {
  const struct RankRecord *x = a, *y = b;
  if (x->vpeak != y->vpeak) return (x->vpeak < y->vpeak) ? 1 : -1;
  return (x->id > y->id) - (x->id < y->id);
}
int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)ctx;
  if (count <= 0) return 0;
  if ((uint64_t)count > SIZE_MAX / sizeof(struct RankRecord)) return 2;
  struct RankRecord *scratch = malloc((size_t)count * sizeof(struct RankRecord));
  if (scratch == NULL) return 3;
  for (int64_t i = 0; i < count; i++) {
    scratch[i].vpeak = halos[i].galaxy->ShamVpeakPlaceholder;
    scratch[i].id = halos[i].UniqueGalaxyID;
    scratch[i].index = i;
  }
  qsort(scratch, (size_t)count, sizeof(struct RankRecord), cmp);
  free(scratch);
  return 0;
}
""",
    ),
    (
        "positive_int64_count_loop_no_narrowing",
        True,
        "an int64_t count indexes the population without an int narrowing",
        """int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)ctx;
  int64_t eligible = 0;
  for (int64_t i = 0; i < count; i++) eligible += (halos[i].galaxy != NULL);
  return eligible >= 0 ? 0 : 1;
}
""",
    ),
    (
        "negative_write_halo_type",
        False,
        "halos[i].Type = 3 must be rejected (callback may not change halo fields)",
        """int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)ctx; (void)count;
  halos[0].Type = 3;
  return 0;
}
""",
    ),
    (
        "negative_write_central_halo",
        False,
        "halos[i].CentralHalo = 0 must be rejected",
        """int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)ctx; (void)count;
  halos[0].CentralHalo = 0;
  return 0;
}
""",
    ),
    (
        "negative_write_galaxy_pointer",
        False,
        "halos[i].galaxy = NULL must be rejected (pointer swap)",
        """int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)ctx; (void)count;
  halos[0].galaxy = NULL;
  return 0;
}
""",
    ),
    (
        "negative_reorder_population_by_assignment",
        False,
        "halos[0] = halos[1] (reordering the borrowed population) must be rejected",
        """int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)ctx;
  if (count < 2) return 0;
  struct Halo tmp = halos[0];
  halos[0] = halos[1];
  halos[1] = tmp;
  return 0;
}
""",
    ),
    (
        "negative_qsort_borrowed_population",
        False,
        "qsort(halos, ...) on the const view must be rejected (discards qualifiers)",
        """static int cmp(const void *a, const void *b) {
  const struct Halo *x = a, *y = b;
  return (x->Vmax < y->Vmax) - (x->Vmax > y->Vmax);
}
int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)ctx;
  qsort(halos, (size_t)count, sizeof(struct Halo), cmp);
  return 0;
}
""",
    ),
    (
        "negative_memcpy_into_population",
        False,
        "memcpy into the const view must be rejected",
        """int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)ctx;
  if (count < 2) return 0;
  memcpy(halos, halos + 1, sizeof(struct Halo));
  return 0;
}
""",
    ),
    (
        "negative_write_context_field",
        False,
        "ctx->snapshot_number = 0 must be rejected (context is read-only)",
        """int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)halos; (void)count;
  ctx->snapshot_number = 0;
  return 0;
}
""",
    ),
    (
        "negative_emit_event_with_snapshot_context",
        False,
        "module_emit_event(ctx, ...) with a SnapshotContext must not type-check (no FoF event contract)",
        """int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)halos; (void)count;
  return module_emit_event(ctx, 0, 0, 0, 0.0, 0.0);
}
""",
    ),
    (
        "negative_pass_const_view_to_fof_process",
        False,
        "handing the const view to the existing FoF process signature must not type-check",
        """extern int some_fof_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)ctx;
  return some_fof_process(NULL, halos, (int)count);
}
""",
    ),
    (
        "negative_cast_away_const",
        False,
        "an explicit cast that strips const must be rejected under -Wcast-qual -Werror",
        """int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)ctx; (void)count;
  struct Halo *mutable_view = (struct Halo *)halos;
  mutable_view[0].Type = 3;
  return 0;
}
""",
    ),
    (
        "limitation_retaining_pointer_compiles",
        True,
        "LIMITATION: storing the borrowed pointer in a static compiles; the no-retention rule is not compiler-enforced",
        """static const struct Halo *retained;
int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)ctx; (void)count;
  retained = halos;
  return 0;
}
""",
    ),
    (
        "limitation_central_halo_as_offset_compiles",
        True,
        "LIMITATION: indexing halos[halos[i].CentralHalo] compiles; the CentralHalo-is-not-an-offset rule needs a test, not the type system",
        """int probe(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {
  (void)ctx;
  for (int64_t i = 0; i < count; i++) halos[halos[i].CentralHalo].galaxy->StellarMass = 0.0f;
  return 0;
}
""",
    ),
)

# The generated header in the working tree may be built for a model that lacks
# the SHAM fields; probes reference only fields common to every model, and the
# one SHAM-shaped read uses a placeholder that we substitute at run time.
PLACEHOLDER = "ShamVpeakPlaceholder"


def detect_generated_model(property_defs: Path) -> tuple[bool, str]:
    text = property_defs.read_text(encoding="utf-8", errors="replace")
    has_sham = "ShamVpeak" in text
    return has_sham, "ShamVpeak" if has_sham else "StellarMass"


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--json", type=Path, default=None)
    args = parser.parse_args()

    chk = Checker("const_view")
    report: dict = {"cc": args.cc, "probes": []}
    cc = shutil.which(args.cc)
    if not cc:
        chk.error("setup.compiler", f"{args.cc!r} not found on PATH")
        return chk.finish()
    version = subprocess.run(
        [cc, "--version"], text=True, capture_output=True, check=False
    ).stdout.splitlines()
    report["cc_version"] = version[0] if version else "unknown"
    chk.note(f"compiler: {report['cc_version']}")

    property_defs = REPO_ROOT / "src" / "include" / "generated" / "property_defs.h"
    if not property_defs.is_file():
        chk.error(
            "setup.generated_header",
            f"{property_defs} missing; run `make generate` in the repo first (not done by this script)",
        )
        return chk.finish()
    has_sham, proxy_field = detect_generated_model(property_defs)
    report["generated_header_has_sham_fields"] = has_sham
    chk.note(
        f"generated property_defs.h {'includes' if has_sham else 'lacks'} SHAM fields; the scratch-sort probe reads galaxy->{proxy_field}"
    )
    if not has_sham:
        chk.skip(
            "sham_fields_in_generated_header",
            "working tree is generated for a model without ShamVpeak; probes use StellarMass instead",
        )

    flags = [cc, *BASE_FLAGS, "-Wcast-qual"] + [f"-I{REPO_ROOT / d}" for d in INCLUDE_DIRS]
    with tempfile.TemporaryDirectory(prefix="mimic-const-view-") as tmpdir:
        tmp = Path(tmpdir)
        for name, expect_compiles, description, body in PROBES:
            source = HEADER + body.replace(PLACEHOLDER, proxy_field)
            path = tmp / f"{name}.c"
            path.write_text(source, encoding="utf-8")
            run = subprocess.run(
                [*flags, str(path)], cwd=REPO_ROOT, text=True, capture_output=True, check=False
            )
            compiled = run.returncode == 0
            first_error = next((line for line in run.stderr.splitlines() if "error:" in line), "")
            report["probes"].append(
                {
                    "name": name,
                    "expect_compiles": expect_compiles,
                    "compiled": compiled,
                    "first_diagnostic": first_error.replace(str(tmp) + "/", ""),
                    "description": description,
                }
            )
            reason = (
                f"{description}; compiled={compiled}; {first_error.replace(str(tmp) + '/', '')}"
            )
            chk.check(name, compiled == expect_compiles, reason)
            if name == "negative_cast_away_const":
                # The project's own warning set (-Wall -Wextra -Wshadow -Wformat-security
                # -Wundef) lacks -Wcast-qual, so the same cast compiles under `make`.
                default_flags = [cc, *BASE_FLAGS] + [f"-I{REPO_ROOT / d}" for d in INCLUDE_DIRS]
                rerun = subprocess.run(
                    [*default_flags, str(path)],
                    cwd=REPO_ROOT,
                    text=True,
                    capture_output=True,
                    check=False,
                )
                report["probes"].append(
                    {
                        "name": "limitation_cast_away_const_compiles_under_project_flags",
                        "expect_compiles": True,
                        "compiled": rerun.returncode == 0,
                        "first_diagnostic": "",
                        "description": "LIMITATION: without -Wcast-qual (not in the project flags) the const-stripping cast compiles",
                    }
                )
                chk.check(
                    "limitation_cast_away_const_compiles_under_project_flags",
                    rerun.returncode == 0,
                    "expected the cast to compile under project-default flags (it is only -Wcast-qual that rejects it)",
                )
    report["summary"] = {
        "passed": len(chk.passed),
        "failed": len(chk.failed),
        "skipped": len(chk.skipped),
        "errors": len(chk.errored),
        "notes": chk.notes,
    }
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2), encoding="utf-8")
    return chk.finish()


if __name__ == "__main__":
    sys.exit(main())
