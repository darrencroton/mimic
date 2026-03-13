# Review: Task 2 — Processing-Mode Metadata Audit (Standalone Module Conversion)

Date: 2026-03-13
Reviewer: Claude Code (claude-sonnet-4-6)
Working tree head: `11be9a3158947eb0ae3171e18edb12b27d5b6308`
Status: **PASS WITH RISKS** — two P2 items should be resolved before merge

Requirement source: `docs/workplan-immediate-merger-followup-2026-03-12.md`, Task 2

---

## Background

The immediate merger handler bug (`sage_handle_mergers_immediate` advertising all
three processing modes when it should only support `process_full_halo`) was traced
to the generator's standalone `.c` module default: any module without a
`module_info.yaml` received all three modes automatically. The sign-off work
(completed 2026-03-12) fixed that specific module.

Task 2 extends the fix to the full remaining module fleet. The delivery:

- Converted all remaining standalone `.c` modules in `src/modules/` to directory
  modules with `module_info.yaml` declaring only the modes each module actually
  supports.
- Migrated all per-module tests from the central `src/modules/_tests/` registry
  into each module's own `_tests/` subdirectory.
- Added a new shared integration test
  (`src/modules/_tests/test_integration_processing_mode_contracts.py`) that
  validates startup rejection for three representative mode-contract groups.
- Updated the generator to enforce that directory modules must declare
  `supported_processing_modes` explicitly.
- Cleaned up `src/modules/_tests/module_info.yaml` to remove per-module test
  registrations (now owned by each module).
- Updated `src/modules/_tests/README.md` and `docs/DEVELOPER-GUIDE.md` to
  reflect the new directory-module-first convention.

### Module conversions and mode declarations

All 17 runtime modules are now directory modules. The declared modes after
conversion, cross-checked against runtime `ngal` assertions in C source:

| Module | Declared mode(s) | Runtime `ngal` contract | Match |
|---|---|---|---|
| `sage_add_cooling` | `process_by_galaxy` | `ngal != 1` → error | ✓ |
| `sage_add_infall` | `process_full_halo` | loops `for i in 0..ngal` | ✓ |
| `sage_calculate_infall` | `process_full_halo` | loops `for i in 0..ngal` | ✓ |
| `sage_calculate_merger_timescale` | `process_full_halo` | loops `for i in 0..ngal` | ✓ |
| `sage_calculate_star_formation` | `process_by_galaxy` | `ngal != 1` → error | ✓ |
| `sage_calculate_supernova_feedback` | `process_by_galaxy` | `ngal != 1` → error | ✓ |
| `sage_clear_disk_instability_triggers` | `process_by_galaxy` | `ngal != 1` → error (in archive source; not re-checked post-conversion) | ✓ |
| `sage_collisional_starburst` | `process_by_galaxy, process_per_event` | dual branch on `ctx->processing_mode`, each arm checks `ngal == 1` | ✓ |
| `sage_disk_instability` | `process_by_galaxy` | `ngal != 1` → error | ✓ |
| `sage_quasar_mode` | `process_by_galaxy, process_per_event` | dual branch on `ctx->processing_mode`, each arm checks `ngal == 1` | ✓ |
| `sage_radio_mode_heating` | `process_by_galaxy` | `ngal != 1` → error | ✓ |
| `sage_reincorporation` | `process_full_halo` | `ngal <= 0` → error; iterates on full set | ✓ |
| `sage_reionization` | `process_full_halo` | loops `for i in 0..ngal` | ✓ |
| `sage_satellite_stripping` | `process_full_halo` | loops `for i in 0..ngal` | ✓ |
| `sage_update_disk_radius` | `process_full_halo` | loops `for i in 0..ngal` | ✓ |
| `sage_update_star_formation_supernova` | `process_by_galaxy` | `ngal != 1` → error | ✓ |
| `sage_handle_mergers_immediate` | `process_full_halo` | full-halo FOF walk (prior sign-off) | ✓ |

---

## Findings

### 1. [P2] `scripts/generate_module_registry.py:213` — Standalone discovery silently inherits the 3-mode default with no deprecation signal

**What the code does.**
`create_standalone_module_metadata()` (lines 159–176) still generates fallback
metadata for any `.c` file discovered directly in `src/modules/`, granting all
three modes:

```python
"supported_processing_modes": [
    "process_full_halo",
    "process_per_event",
    "process_by_galaxy",
],
```

When a standalone module is discovered, the generator prints an informational
line:

```python
print(f"  Discovered standalone module: {module_name}")
```

`validate_processing_mode_declarations()` (line 284) explicitly skips standalone
modules (`pattern == "standalone"`), so `make validate-modules` will not flag the
problem.

**Why it matters.**
This is exactly the root-cause pattern that was fixed. If a developer creates a
new physics module as a bare `src/modules/my_module.c` — plausible when working
quickly or following older examples — it will silently receive all three modes
and pass all validation checks. The informational print is easy to miss in a full
`make generate` run.

**Evidence path.**
`generate_module_registry.py:159–176` (default metadata) →
`generate_module_registry.py:280–285` (validation skip) →
`generate_module_registry.py:213` (informational-only print).
Contrast with: `generate_module_registry.py:287–314` (strict error for directory
modules with missing or invalid mode declarations).

**Fix direction.**
Elevate the standalone discovery print to a visible warning, ideally using the
existing `print_warning()` helper (which writes to stderr with a `WARNING:`
prefix). The warning should state that standalone modules inherit broad mode
support and should be converted to directory modules. A stronger option is to
make `--strict` mode fail validation when any standalone module is discovered.

---

### 2. [P2] `src/modules/_tests/test_integration_processing_mode_contracts.py:37–76` — `process_per_event` is never tested as the *invalid* mode

**What the test covers.**
Three negative configuration tests:

| Test | Module under test | Configured with | Should reject |
|---|---|---|---|
| `test_full_halo_only_module_rejects_process_by_galaxy` | `sage_add_infall` (`process_full_halo`) | `process_by_galaxy` | ✓ |
| `test_by_galaxy_only_module_rejects_process_full_halo` | `sage_add_cooling` (`process_by_galaxy`) | `process_full_halo` | ✓ |
| `test_dual_mode_module_rejects_process_full_halo` | `sage_quasar_mode` (`[by_galaxy, per_event]`) | `process_full_halo` | ✓ |

**What is missing.**
In none of these tests is `process_per_event` the invalid mode being rejected.
There is no test that a `process_full_halo`-only module (e.g. `sage_add_infall`)
or a `process_by_galaxy`-only module (e.g. `sage_add_cooling`) correctly rejects
a `process_per_event` configuration.

This matters because `process_per_event` is the specific mode that caused the
original merger-pathway bug. The event-path regression that brought in Task 2
was exactly a module accepting `process_per_event` when it should not. Leaving
this mode untested as an invalid case leaves the most historically sensitive
rejection path unverified.

The existing `sage_handle_mergers_immediate` test
(`test_integration_sage_merger_event_consumers.py:144`) tests rejection of
`process_by_galaxy` for a full-halo module, which also does not cover the
`process_per_event` case.

**Fix direction.**
Add one test to `test_integration_processing_mode_contracts.py`:

```python
def test_full_halo_only_module_rejects_process_per_event():
    assert_invalid_mode_rejected(
        output_name="mode_contract_full_halo_rejects_per_event",
        phase_config={
            "pre_timestep": [],
            "phase_1": [("sage_add_infall", "process_per_event")],
            "phase_2": [],
            "post_timestep": [],
        },
        expected_mode="process_per_event",
        expected_supported="process_full_halo",
    )
```

A second test for a `process_by_galaxy`-only module rejecting `process_per_event`
would complete the coverage matrix, but the above is the minimum.

---

### 3. [P3] `src/modules/sage_clear_disk_instability_triggers/module_info.yaml` — Converted module has zero test coverage

The module was promoted from a standalone `.c` file (3-mode default) to a
directory module declaring `process_by_galaxy`. Its `_tests/` directory is empty
and `module_info.yaml` registers `unit: []`, `integration: []`.

This is intentional: `sage_clear_disk_instability_triggers` is a trivial
single-field reset with no physics logic, and the `process_by_galaxy` contract
class is already covered by `test_by_galaxy_only_module_rejects_process_full_halo`
using `sage_add_cooling`. The mode tightening is structurally sound.

The gap to note is that if the module were ever extended with real logic, the
absence of a test scaffold would not be obvious. A comment in `module_info.yaml`
acknowledging the intentional omission would prevent future confusion.

---

### 4. [P3] `Makefile:459,504` — `test-integration` and `test-scientific` no longer guarantee an HDF5-capable binary

**Before:**

```makefile
test-integration:
    @if [ ! -f "$(EXEC)" ]; then \
        $(MAKE) clean; \
        $(MAKE) USE-HDF5=yes; \
    fi
```

**After:**

```makefile
test-integration: generate validate-build $(EXEC)
```

The new prerequisite approach is cleaner Make practice, and HDF5 is the default
(`USE-HDF5 := yes`, Makefile line 98). However, if a developer has a stale
non-HDF5 binary in place (e.g. after running `make USE-HDF5=no`), Make will
consider `$(EXEC)` up-to-date and run tests against it without warning. The old
code also did not protect against a stale binary, so the regression risk is low —
this is noted for completeness.

---

## Acceptance Criteria Assessment

From `docs/workplan-immediate-merger-followup-2026-03-12.md`, Task 2:

| Criterion | Status |
|---|---|
| Every runtime module advertises only the modes it actually supports | ✓ All 17 modules verified |
| Startup validation rejects invalid processing-mode configurations before execution | ✓ Confirmed via `module_registry.c:256–266` and 3 new integration tests |
| No duplicate or fragmented test registration introduced during cleanup | ✓ Central registry cleaned; module-owned `module_info.yaml` files are the sole registration point |
| `make validate-modules`, `make test-unit`, `make test-integration` stay green | Not verified — test suite was not executed during this review |

The workplan acceptance criterion "Add negative configuration tests where the
supported-mode contract is easy to regress and high-value to protect" is
substantially met. The gap (Finding 2) is that `process_per_event` as the invalid
mode is absent, which is the historically highest-risk rejection path.

---

## Recommended Actions Before Merge

1. **[Required]** Address Finding 2: add `test_full_halo_only_module_rejects_process_per_event`
   to `test_integration_processing_mode_contracts.py`.

2. **[Required]** Address Finding 1: elevate standalone module discovery to a
   `print_warning()` call (or equivalent) in `generate_module_registry.py`.

3. **[Recommended]** Run the full test suite (`make validate-modules`,
   `make test-unit`, `make test-integration`) on the current working tree and
   confirm all green. The workplan claims this was done on the sign-off tree but
   the Task 2 file movements are significant enough to warrant re-verification.

4. **[Optional]** Add a comment to
   `sage_clear_disk_instability_triggers/module_info.yaml` acknowledging the
   intentional absence of tests.

---

## Scope Reviewed

- All 52 files in the working-tree diff (modified tracked files + all new
  untracked module directories)
- `src/core/module_registry.c:221–269` — runtime mode validation and error
  message format (cross-checked against test string assertions)
- `scripts/generate_module_registry.py` — discovery, validation, and fallback
  logic
- `scripts/generate_test_registry.py` — test discovery chain for module-owned
  tests
- C source runtime `ngal` contracts for all 17 modules (spot-checked via grep)
- `src/modules/_archive/` isolation (confirmed excluded by `_`-prefix filter)
