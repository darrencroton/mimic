#!/usr/bin/env python3
"""
Snapshot Module Schema and Generated-Registration Tests

Validates the metadata and generated-code side of the typed snapshot callback
contract (process_snapshot / PROCESSING_MODE_SNAPSHOT):

  - scripts/module_modes.py mirrors the C mode table and enum order
  - the registry generator and the metadata validator accept and reject the same
    supported_processing_modes lists (unknown, duplicate and empty lists fail both)
  - standalone fallback modules keep exactly the three FoF modes
  - generated registration declares and binds only the callbacks a module's modes
    advertise; the generated code is compiled with the project warning set and its
    callbacks are invoked through the registered structs
  - the borrowed const view permits galaxy writes, rejects halo writes at compile
    time, and lets the three forbidden-by-contract patterns compile silently
  - generation freshness covers the descriptor file's path and bytes
  - snapshot-only metadata cannot declare events; dual-mode FoF events stay valid
  - the snapshot fixtures register only in test builds

Real callback invocations of the fixtures and family-aware registration in the
C registry are covered by tests/unit/test_snapshot_module_contract.c.

Usage:
    MODEL=sage16 SIMULATION=mini-millennium mimic_venv/bin/python \\
        tests/integration/test_snapshot_module_schema.py
"""

import contextlib
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))
sys.path.insert(0, str(REPO_ROOT / "tests"))

import check_generated  # noqa: E402
import generate_module_registry as generator  # noqa: E402
import module_modes  # noqa: E402
import validate_modules as validator  # noqa: E402
from framework import run_test_suite  # noqa: E402

CC = os.environ.get("CC", "cc")

# Scratch files live under the ignored build/ tree: the generator and the freshness
# hashes report repository-relative paths, so their inputs must sit inside the repo.
SCRATCH_ROOT = REPO_ROOT / "build" / "test_snapshot_module_schema"
INCLUDE_FLAGS = [
    f"-I{REPO_ROOT / path}"
    for path in ("", "src", "src/include", "src/core", "src/io", "src/util", "src/module_system")
]

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def project_warning_flags():
    """Return the warning flags of the Makefile's CFLAGS (the project warning set)."""
    makefile = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
    match = re.search(r"^CFLAGS = (.+)$", makefile, re.MULTILINE)
    assert match, "Makefile CFLAGS line not found"
    flags = [flag for flag in shlex.split(match.group(1)) if flag.startswith("-W")]
    assert "-Wall" in flags and "-Wextra" in flags, f"unexpected warning set: {flags}"
    return flags


def compile_c(source, workdir, link=False):
    """Compile C source under the project warning set with -Werror.

    Returns:
        (returncode, compiler output, executable path or None)
    """
    src = Path(workdir) / "case.c"
    src.write_text(source, encoding="utf-8")
    out = Path(workdir) / ("case" if link else "case.o")
    cmd = [CC, *project_warning_flags(), "-Werror", *INCLUDE_FLAGS]
    cmd += [str(src), "-o", str(out)] if link else ["-c", str(src), "-o", str(out)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr, out if link else None


@contextlib.contextmanager
def scratch_dir():
    """Yield a fresh scratch directory under SCRATCH_ROOT, removed afterwards."""
    SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
    path = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
    try:
        yield path
    finally:
        shutil.rmtree(path, ignore_errors=True)


def require_generated_headers():
    """Fail with an actionable message when property code has not been generated."""
    header = REPO_ROOT / "src" / "include" / "generated" / "property_defs.h"
    assert header.exists(), f"{header.relative_to(REPO_ROOT)} missing; run 'make generate' first"


def generator_mode_errors(modes):
    """Run the generator's mode-list validation on one synthetic module."""
    return generator.validate_processing_modes(
        [{"name": "synthetic", "supported_processing_modes": modes}]
    )


def validator_mode_errors(modes):
    """Run the validator's mode-list validation on one synthetic module."""
    results = validator.ValidationResults()
    module = {"name": "synthetic", "supported_processing_modes": modes}
    validator.validate_supported_processing_modes(module, "synthetic", results)
    return [str(error) for error in results.errors]


def synthetic_module(name, modes):
    """Return a minimal runtime module dict as the generator sees it."""
    return {
        "name": name,
        "supported_processing_modes": modes,
        "dependencies": {"properties": [], "parameters": []},
        "_pattern": "directory",
    }


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_descriptors_mirror_c_mode_table():
    """scripts/module_modes.py matches enum ProcessingMode and the C descriptor table."""
    interface = (REPO_ROOT / "src" / "core" / "module_interface.h").read_text(encoding="utf-8")
    enum_body = re.search(r"enum ProcessingMode \{(.*?)\};", interface, re.DOTALL).group(1)
    enumerators = re.findall(r"^\s*(PROCESSING_MODE_[A-Z_]+)", enum_body, re.MULTILINE)
    assert enumerators[-1] == "PROCESSING_MODE_COUNT", enumerators
    assert [m.enum for m in module_modes.PROCESSING_MODES] == enumerators[:-1], enumerators

    registry = (REPO_ROOT / "src" / "core" / "module_registry.c").read_text(encoding="utf-8")
    rows = re.findall(
        r"\{(PROCESSING_MODE_[A-Z_]+), \"([a-z_]+)\", MODULE_CALLBACK_FAMILY_([A-Z]+)\}", registry
    )
    expected = [(m.enum, m.name, m.family.upper()) for m in module_modes.PROCESSING_MODES]
    assert rows == expected, f"C table {rows} != Python descriptors {expected}"

    fields = [family.field for family in module_modes.CALLBACK_FAMILIES]
    assert fields == ["process", "process_snapshot"], fields
    assert (
        "int (*process_snapshot)(const struct SnapshotContext *ctx, const struct Halo *halos,\n"
        "                          int64_t count);" in interface
    )
    assert "int (*process)(struct ModuleContext *ctx, struct Halo *halos, int ngal);" in interface
    print(f"  ✓ {len(rows)} modes agree between C and Python, in enum order")


def test_generator_and_validator_agree_on_mode_lists():
    """Both tools accept the same lists and reject unknown, duplicate and empty ones."""
    valid = [
        ["process_snapshot"],
        ["process_by_galaxy"],
        ["process_full_halo", "process_snapshot"],
        ["process_by_galaxy", "process_per_event", "process_full_halo", "process_snapshot"],
    ]
    invalid = [
        [],
        None,
        "process_snapshot",
        ["process_global"],
        ["process_snapshot", "process_snapshot"],
        ["process_full_halo", "process_full_halo", "process_snapshot"],
        ["process_snapshot", 3],
    ]
    for modes in valid:
        gen, val = generator_mode_errors(modes), validator_mode_errors(modes)
        assert not gen and not val, f"{modes} should be valid: generator={gen} validator={val}"
    for modes in invalid:
        gen, val = generator_mode_errors(modes), validator_mode_errors(modes)
        assert gen and val, f"{modes} should fail both: generator={gen} validator={val}"
    print(f"  ✓ {len(valid)} valid and {len(invalid)} invalid lists agree")


def test_standalone_fallback_keeps_three_modes():
    """Standalone fallback metadata advertises exactly the original three FoF modes."""
    fof = ["process_full_halo", "process_per_event", "process_by_galaxy"]
    metadata = generator.create_standalone_module_metadata(Path("models/x/modules/demo.c"))
    assert metadata["supported_processing_modes"] == fof, metadata
    assert list(module_modes.STANDALONE_FALLBACK_MODES) == fof
    families = module_modes.callback_families(metadata["supported_processing_modes"])
    assert [family.key for family in families] == [module_modes.FAMILY_FOF], families
    validator_source = (REPO_ROOT / "scripts" / "validate_modules.py").read_text(encoding="utf-8")
    assert '"supported_processing_modes": list(STANDALONE_FALLBACK_MODES)' in validator_source
    print("  ✓ standalone modules keep [process_full_halo, process_per_event, process_by_galaxy]")


def test_unknown_mode_fails_closed_in_generation():
    """Generation helpers raise on an unknown mode instead of dropping or defaulting it."""
    for helper in (module_modes.enum_for_mode, lambda m: module_modes.callback_families([m])):
        try:
            helper("process_global")
        except KeyError:
            continue
        raise AssertionError("unknown mode was accepted by a generation helper")
    print("  ✓ unknown modes raise KeyError")


def test_generated_registration_binds_only_advertised_callbacks():
    """Generated registration compiles cleanly and wires each family's callback.

    The generator writes module_init.c for snapshot-only, FoF-only and dual-mode
    synthetic modules. A harness defines their callbacks and module_registry_add(),
    includes the generated file, registers, then checks every struct pointer and
    invokes each bound callback.
    """
    require_generated_headers()
    modules = [
        synthetic_module("demo_snapshot_only", ["process_snapshot"]),
        synthetic_module("demo_fof_only", ["process_by_galaxy", "process_full_halo"]),
        synthetic_module("demo_dual", ["process_full_halo", "process_snapshot"]),
    ]
    event_info = {"producer_ids": {}, "emits": {}, "consumes": {}}
    with scratch_dir() as tmp:
        init_c = tmp / "module_init.c"
        assert generator.generate_module_init_c(modules, event_info, "0" * 32, init_c)
        text = init_c.read_text(encoding="utf-8")

        assert "extern int demo_snapshot_only_process_snapshot(" in text
        assert "extern int demo_snapshot_only_process(" not in text
        assert "extern int demo_fof_only_process(" in text
        assert "extern int demo_fof_only_process_snapshot(" not in text
        assert "extern int demo_dual_process(" in text
        assert "extern int demo_dual_process_snapshot(" in text
        for name in ("demo_snapshot_only", "demo_fof_only", "demo_dual"):
            assert f"extern int {name}_init(void);" in text
            assert f"extern int {name}_cleanup(void);" in text

        harness = f"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "module_registry.h"

static const struct Module *registered[8];
static int num_registered = 0;
void module_registry_add(struct Module *module) {{ registered[num_registered++] = module; }}

static int calls = 0;
#define LIFECYCLE(name)                                                                \\
  int name##_init(void) {{ return 0; }}                                              \\
  int name##_cleanup(void) {{ return 0; }}
LIFECYCLE(demo_snapshot_only)
LIFECYCLE(demo_fof_only)
LIFECYCLE(demo_dual)
int demo_fof_only_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {{
  (void)ctx; (void)halos; calls += 1; return ngal == 2 ? 0 : 1;
}}
int demo_dual_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {{
  (void)ctx; (void)halos; calls += 10; return ngal == 2 ? 0 : 1;
}}
int demo_snapshot_only_process_snapshot(const struct SnapshotContext *ctx,
                                        const struct Halo *halos, int64_t count) {{
  (void)halos; calls += 100; return (ctx->snapshot_number == 4 && count == 2) ? 0 : 1;
}}
int demo_dual_process_snapshot(const struct SnapshotContext *ctx, const struct Halo *halos,
                               int64_t count) {{
  (void)halos; calls += 1000; return (ctx->snapshot_number == 4 && count == 2) ? 0 : 1;
}}

#include "{init_c}"

static const struct Module *find(const char *name) {{
  for (int i = 0; i < num_registered; i++)
    if (strcmp(registered[i]->name, name) == 0) return registered[i];
  return NULL;
}}

int main(void) {{
  register_all_modules();
  const struct Module *snap = find("demo_snapshot_only");
  const struct Module *fof = find("demo_fof_only");
  const struct Module *dual = find("demo_dual");
  if (num_registered != 3 || !snap || !fof || !dual) return 10;
  if (snap->process != NULL || snap->process_snapshot == NULL) return 11;
  if (fof->process == NULL || fof->process_snapshot != NULL) return 12;
  if (dual->process == NULL || dual->process_snapshot == NULL) return 13;
  if (!snap->init || !snap->cleanup || !fof->init || !fof->cleanup) return 14;
  if (snap->num_supported_modes != 1 || snap->supported_processing_modes[0] !=
      PROCESSING_MODE_SNAPSHOT) return 15;

  struct Halo halos[2];
  memset(halos, 0, sizeof(halos));
  struct ModuleContext fctx;
  memset(&fctx, 0, sizeof(fctx));
  struct SnapshotContext sctx = {{.snapshot_number = 4, .redshift = 0.0, .time = 0.0,
                                  .params = NULL}};
  if (fof->process(&fctx, halos, 2) || dual->process(&fctx, halos, 2)) return 16;
  if (snap->process_snapshot(&sctx, halos, 2) || dual->process_snapshot(&sctx, halos, 2))
    return 17;
  if (calls != 1111) return 18;
  printf("REGISTRATION_OK\\n");
  return 0;
}}
"""
        rc, output, exe = compile_c(harness, tmp, link=True)
        assert rc == 0, f"generated registration failed to compile cleanly:\n{output}"
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        registered_ok = run.returncode == 0 and "REGISTRATION_OK" in run.stdout
        assert registered_ok, f"harness exit {run.returncode}: {run.stdout}{run.stderr}"
    print("  ✓ snapshot-only: .process = NULL; FoF-only: .process_snapshot = NULL; dual: both")


def test_const_view_compile_contract():
    """The shallow const view: galaxy writes compile, halo writes do not, and the three
    forbidden-by-contract patterns compile silently under the project warning set."""
    require_generated_headers()
    flags = project_warning_flags()
    assert "-Wcast-qual" not in flags, f"project warning set unexpectedly has -Wcast-qual: {flags}"

    prologue = """
#include <stdint.h>
#include <string.h>
#include "module_interface.h"
"""
    callback = (
        "int cb(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count) {\n"
        "  (void)ctx;\n  if (count <= 0) return 0;\n  BODY\n  return 0;\n}\n"
    )
    compiles_cleanly = {
        "galaxy write": "for (int64_t i = 0; i < count; i++) {\n"
        "    struct GalaxyData *g = halos[i].galaxy;\n"
        "    memset(g, 0, sizeof(*g));\n  }",
        "cast away const (forbidden)": "((struct Halo *)halos)[0].Type = 1;",
        "retain pointer (forbidden)": "static const struct Halo *kept;\n  kept = halos;\n"
        "  (void)kept;",
        "index by CentralHalo (forbidden)": "memset(halos[halos[0].CentralHalo].galaxy, 0, 1);",
    }
    rejected = {
        "halo field write": "halos[0].Type = 1;",
        "galaxy pointer write": "halos[0].galaxy = NULL;",
    }
    with scratch_dir() as tmp:
        for label, body in compiles_cleanly.items():
            rc, output, _ = compile_c(prologue + callback.replace("BODY", body), tmp)
            assert rc == 0, f"{label} should compile without warnings:\n{output}"
        for label, body in rejected.items():
            rc, output, _ = compile_c(prologue + callback.replace("BODY", body), tmp)
            assert rc != 0, f"{label} through the const view should not compile"
            assert "read-only" in output or "const" in output, output
    print("  ✓ galaxy write compiles; halo/pointer writes rejected; forbidden patterns silent")


def test_descriptor_freshness_inputs():
    """Both freshness hashes cover module_modes.py's path and bytes, and a changed
    descriptor makes check_generated report the module output stale."""
    original_gen = generator.MODULE_MODES_PY
    original_chk = check_generated.MODULE_MODES_PY
    assert original_gen == original_chk == REPO_ROOT / "scripts" / "module_modes.py"

    modules = generator.discover_modules()
    assert generator.compute_metadata_hash(modules) == (
        check_generated.compute_module_metadata_hash()
    ), "generator and check_generated disagree on the module-generation hash"

    makefile = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
    stamp_rule = re.search(r"^\$\(MODULE_STAMP\):(.*)$", makefile, re.MULTILINE).group(1)
    assert "scripts/module_modes.py" in stamp_rule.split(), stamp_rule

    with scratch_dir() as scratch:
        same_bytes = scratch / "module_modes.py"
        shutil.copyfile(original_gen, same_bytes)

        def hashes(path):
            generator.MODULE_MODES_PY = path
            check_generated.MODULE_MODES_PY = path
            try:
                return generator.compute_metadata_hash(modules), (
                    check_generated.compute_module_metadata_hash()
                )
            finally:
                generator.MODULE_MODES_PY = original_gen
                check_generated.MODULE_MODES_PY = original_chk

        base_gen, base_chk = hashes(original_gen)
        moved_gen, moved_chk = hashes(same_bytes)
        assert moved_gen != base_gen and moved_chk != base_chk, "descriptor path is not hashed"

        same_bytes.write_text(original_gen.read_text(encoding="utf-8") + "\n# edit\n")
        edited_gen, edited_chk = hashes(same_bytes)
        assert edited_gen != moved_gen and edited_chk != moved_chk, "descriptor bytes not hashed"
        assert edited_gen == edited_chk, "generator and check_generated diverge after an edit"

        generated = scratch / "event_contracts.h"
        generated.write_text(f"/* AUTO-GENERATED CODE\n * Source MD5: {base_chk}\n */\n")
        assert check_generated.check_hashes([generated], base_chk, "module metadata")
        stale_accepted = check_generated.check_hashes([generated], edited_chk, "module metadata")
        assert not stale_accepted, "check_generated accepted output from before the descriptor edit"
    print("  ✓ descriptor path and bytes feed both hashes; stale output is detected")


def test_snapshot_metadata_cannot_declare_events():
    """Snapshot-only event declarations fail the required-FoF-mode checks; dual-mode
    modules keep their valid FoF event declarations."""
    snapshot_producer = {
        "name": "snap_producer",
        "supported_processing_modes": ["process_snapshot"],
        "events": {"emits": [{"name": "ev", "description": "x"}]},
    }
    snapshot_consumer = {
        "name": "snap_consumer",
        "supported_processing_modes": ["process_snapshot"],
        "events": {"consumes": [{"producer": "dual_producer", "event": "ev"}]},
    }
    dual_producer = {
        "name": "dual_producer",
        "supported_processing_modes": ["process_full_halo", "process_snapshot"],
        "events": {"emits": [{"name": "ev", "description": "x"}]},
    }
    dual_consumer = {
        "name": "dual_consumer",
        "supported_processing_modes": ["process_per_event", "process_snapshot"],
        "events": {"consumes": [{"producer": "dual_producer", "event": "ev"}]},
    }

    def errors_for(modules):
        info, collect_errors = generator.collect_event_info(modules)
        return collect_errors + generator.validate_event_declarations(modules, info)

    errors = errors_for([snapshot_producer])
    assert any("only valid for modules that support process_full_halo" in e for e in errors)
    errors = errors_for([dual_producer, snapshot_consumer])
    assert any("only valid for modules that support process_per_event" in e for e in errors)
    errors = errors_for([dual_producer, dual_consumer])
    assert errors == [], f"dual-mode FoF events should stay valid: {errors}"
    print("  ✓ snapshot-only emits/consumes rejected; dual-mode FoF events accepted")


def test_fixtures_register_only_in_test_builds():
    """Generated registration includes the snapshot fixtures only for test builds."""
    env = {k: v for k, v in os.environ.items() if k != "MIMIC_TEST_BUILD"}
    env.setdefault("MODEL", "sage16")
    env.setdefault("SIMULATION", "mini-millennium")
    script = str(REPO_ROOT / "scripts" / "generate_module_registry.py")

    production = subprocess.run(
        [sys.executable, script, "--dry-run"], capture_output=True, text=True, env=env
    )
    assert production.returncode == 0, production.stderr
    for fixture in ("test_fixture", "test_snapshot_fixture", "test_event_producer"):
        assert f"{fixture}_module" not in production.stdout, f"{fixture} in production registry"

    test_env = dict(env, MIMIC_TEST_BUILD="1")
    test_build = subprocess.run(
        [sys.executable, script, "--dry-run"], capture_output=True, text=True, env=test_env
    )
    assert test_build.returncode == 0, test_build.stderr
    out = test_build.stdout
    assert (
        ".process = NULL,\n    .process_snapshot = test_snapshot_fixture_process_snapshot," in out
    )
    assert (
        ".process = test_fixture_process,\n    .process_snapshot = test_fixture_process_snapshot,"
        in out
    )
    assert ".process = test_event_producer_process,\n    .process_snapshot = NULL," in out
    assert "module_registry_add(&test_snapshot_fixture_module);" in out

    makefile = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
    test_block = re.search(r"ifeq \(\$\(TEST_BUILD\),yes\)\n(SOURCES \+=.*?)endif", makefile, re.S)
    assert test_block, "TEST_BUILD fixture SOURCES block not found in Makefile"
    for source in ("test_fixture/test_fixture.c", "test_snapshot_fixture/test_snapshot_fixture.c"):
        assert source in test_block.group(1), f"{source} missing from TEST_BUILD SOURCES"
    print("  ✓ fixtures registered only for MIMIC_TEST_BUILD; Makefile lists both fixtures")


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_descriptors_mirror_c_mode_table,
            test_generator_and_validator_agree_on_mode_lists,
            test_standalone_fallback_keeps_three_modes,
            test_unknown_mode_fails_closed_in_generation,
            test_generated_registration_binds_only_advertised_callbacks,
            test_const_view_compile_contract,
            test_descriptor_freshness_inputs,
            test_snapshot_metadata_cannot_declare_events,
            test_fixtures_register_only_in_test_builds,
        ],
        "Snapshot Module Schema (test_snapshot_module_schema.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
