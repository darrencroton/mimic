# Mimic `src/` Systems Review (code-simplifier)

**Date**: 2026-06-10
**Scope**: Every file under `src/` (~13,600 lines of C/headers, plus metadata YAML and test-fixture Python). Generated files under `*/generated/` were read for context but are reviewed only through their generators' output shape — they must never be hand-edited.
**Method**: Holistic, system-by-system code-level review against `docs/VISION.md` principles. Goal: simplification, clarity, and maintainability opportunities with **no behavior change**, plus a small number of genuine bugs discovered along the way. All dead-code claims below were verified by repo-wide grep (src, models, tests, plot).
**Status**: IMPLEMENTED (2026-06-10). All eight batches of §5 were applied; see the addendum at the end of this document for deviations discovered during implementation.

---

## 1. System Map

Every file in `src/` belongs to exactly one of eight systems.

### S1. Execution Driver & Lifecycle
The program spine: startup, CLI, the file/tree loop, shutdown, and run provenance copying.

| File | Role |
|---|---|
| `src/core/main.c` | Entry point, CLI parsing, signal handling, file/tree loop, metadata snapshot |
| `src/core/init.c` | Units, snapshot list reading, lookback-time table (`Age`) |
| `src/core/allvars.c` | Global variable definitions |

### S2. Configuration & Parameter System
YAML run-file parsing into `MimicConfig`, plus the typed model-parameter access API used by modules.

| File | Role |
|---|---|
| `src/core/read_parameter_file.c` | libyaml DOM parsing, section parsers, validation, phase config |
| `src/core/module_registry.c` (lines 1070–1194) | `model_get_double/int/string` typed parameter getters |
| `src/module_system/parameter_helpers.h` | `LOAD_PARAM_*` / `VALIDATE_*` macros for module `init()` |

### S3. Tree Processing Engine (the science driver)
Depth-first traversal, progenitor gather, inheritance, galaxy storage, and output marshalling.

| File | Role |
|---|---|
| `src/core/build_model.c` | `build_halo_tree()`, gather step, scratch buffers, module-context setup |
| `src/core/inheritance.c` / `inheritance.h` | Format-neutral inheritance service (copy, orphan, new-halo rules) |
| `src/core/galaxy_pool.c` / `galaxy_pool.h` | Chunked per-tree `GalaxyData` pool |
| `src/core/output_buffer.c` / `output_buffer.h` | Driver-neutral workspace → output-buffer marshaller |
| `src/core/virial.c` | Virial mass/radius/velocity helpers |

### S4. Module System (registry, pipeline, events)
The physics-agnostic execution engine and its contracts.

| File | Role |
|---|---|
| `src/core/module_registry.c` / `.h` | Registry, pipeline build/validation, phase execution, event dispatch |
| `src/core/module_interface.h` | `struct Module`, `ModuleContext`, `ModuleEvent`, processing modes |
| `src/module_system/generated/module_init.c` | Generated registration (do not hand-edit) |
| `src/module_system/generated/event_contracts.h` | Generated producer/event IDs (do not hand-edit) |
| `src/module_system/template/template_module.c` + `template_module_info.yaml` | New-module template |
| `src/module_system/test_fixture/*` (`test_fixture.c`, `module_info.yaml`, `test_properties.yaml`, `_tests/*`) | Infrastructure test module + its tests |
| `src/module_system/test_event_producer{,_b}/*`, `test_event_consumer_{alpha,beta,gamma}/*` | Event-routing test modules |
| `src/module_system/physical_constants.h` | Shared physical constants |
| `src/module_system/output_helpers.h` | Metadata-referenced output conversion helpers |

### S5. Property / Metadata Generation Surface
The hand-written sources and consumption points of the code generator (the generator scripts themselves live outside `src/`).

| File | Role |
|---|---|
| `src/core/core_properties.yaml` | Core halo property metadata (source of truth) |
| `src/include/generated/property_defs.h` | Generated `Halo`/`GalaxyData`/`HaloOutput` structs + init/reset |
| `src/include/generated/property_test_helpers.h` | Generated test helpers |
| Generated `.inc` consumption sites | `main.c:196` (schema writer), `build_model.c:284` (payload), `output/util.c:69` (copy), `output/hdf5.c:79,89,974,1137` (HDF5 fields/metadata) |

### S6. Tree Input I/O
| File | Role |
|---|---|
| `src/io/tree/interface.c` / `interface.h` | Format dispatch, per-tree alloc/free, `myfread/myfwrite/myfseek` wrappers |
| `src/io/tree/binary.c` / `binary.h` | LHalo binary reader |
| `src/io/tree/hdf5.c` / `hdf5.h` | Genesis LHalo HDF5 reader |
| `src/io/util.c` / `util.h` | Endianness detection/swapping |

### S7. Output I/O & Run Products
| File | Role |
|---|---|
| `src/io/output/binary.c` / `binary.h` | Per-snapshot binary writer + header finalization |
| `src/io/output/hdf5.c` / `hdf5.h` | HDF5 tables, buffered writes, run metadata, master file |
| `src/io/output/util.c` / `util.h` | Shared `prepare_halo_for_output()` |
| `src/io/output/python_example.c` / `python_example.h` | Generated run-local Python example script |

### S8. Foundation Utilities & Shared Headers
| File | Role |
|---|---|
| `src/util/memory.c` / `memory.h` | Tracked categorized allocator, leak detection |
| `src/util/error.c` / `error.h` | Logging/error system, `*_LOG` macros |
| `src/util/numeric.c` / `numeric.h` | Epsilon comparisons, `safe_div` |
| `src/util/integration.c` / `integration.h` | Adaptive Simpson integration (GSL-style facade) |
| `src/util/io.c` / `io.h` | `copy_file`, `ensure_directory_exists` |
| `src/util/run_log.c` / `run_log.h` | Run banner, phase banners, color helpers |
| `src/util/version.c` / `version.h` | `version_info.json` provenance writer |
| `src/include/types.h`, `globals.h`, `config.h`, `constants.h`, `proto.h` | Shared types, globals, constants, prototypes |

---

## 2. Bugs Found During Review

These are correctness issues, not simplifications. They should be fixed first and each is small.

**B1 — `ERROR_LOG` + `assert()` is not a failure path in release builds.**
`build_model.c:494-497` (`process_halo_evolution`, no Type-0 central) and `inheritance.c:104-120` (`set_local_centrals`) log an error and then rely on `assert()`. If the binary is ever compiled with `-DNDEBUG`, the asserts vanish and execution continues with `centralgal == -1`, indexing `FoFWorkspace[-1]`. Replace each `ERROR_LOG` + `assert` pair with `FATAL_ERROR(...)` (which logs and exits unconditionally). Same pattern check: `main.c:434` `assert(!gotXCPU)` turns the graceful SIGXCPU design into an abort — handle the flag explicitly (log, finalize, exit) instead of asserting.

**B2 — HDF5 tree reader: unconditional out-of-bounds debug dump.**
`io/tree/hdf5.c:122-123` prints `InputTreeNHalos[i]` for `i < 20` regardless of `Ntrees`, reading past the array when a file has fewer than 20 trees. It also uses raw `printf` (here and at line 109), bypassing the logging system. Delete both `printf` blocks or convert to `DEBUG_LOG` guarded by `i < Ntrees`.

**B3 — Silent truncation of model parameters at 256.**
`read_parameter_file.c:971` iterates `pair < ... && idx < 256`. A run file with more than 256 `modules.parameters` entries silently drops the surplus, and the affected modules then fail with a misleading "parameter not found". Make overflow fatal, and replace the magic `256` (also hard-coded in `types.h:150`) with one named constant.

**B4 — Lenient numeric parsing for run-critical config.**
`get_int_value`/`get_double_value` (`read_parameter_file.c:317-356`) use `atoi`/`atof`: `first_file: abc` becomes 0 with no diagnostic. A strict variant (`get_strict_int_value`) already exists but is used only for `snapshot_list`. Route all numeric scalars (cosmology, units, `box_size`, `particle_mass`, file ranges, `SubSteps`, `max_tree_depth`) through strict parsers; add `get_strict_double_value`. This is exactly Vision Principle 7 ("invalid configuration should fail early").

**B5 — `modules.parameters` of wrong YAML type is non-fatal.**
`read_parameter_file.c:961-964`: if `parameters` is not a mapping it logs `ERROR_LOG` and `return`s — the run continues with zero model parameters and dies later inside the first module `init()` with a less helpful message. Should be `FATAL_ERROR`.

**B6 — GitHub commit URL malformed for SSH remotes.**
`version.c:151` advances `github_part += 10` past `"github.com"` but the SSH form needs to skip `"github.com:"` (11 chars), producing `https://github.com/:owner/repo/commit/<hash>`. The HTTPS branch (`+= 11`) is correct. One-character fix; better, see S8-4 which removes this code path entirely.

**B7 — `SubHalfMass` read with wrong datatype (verify).**
`io/tree/hdf5.c:239` reads `SubHalfMass` with `type_int = 0` (H5T_NATIVE_INT) and casts through `int`, but `RawHalo.SubHalfMass` is `float` (`types.h:32`) and the sibling float fields use `type_int = 1`. If the Genesis files store this as float, the value is garbage. Confirm against a real Genesis file; the field is currently unused downstream, which is why nothing fails.

**B8 — Master file links to output files that may not exist.**
`write_master_file()` (`output/hdf5.c:1204`) loops `FirstFile..LastFile` unconditionally, but the main loop skips missing tree files (`main.c:394-397`). For a sparse file range the master file gains external links to nonexistent files and then `H5Fopen` on the missing target (`hdf5.c:1248`) returns a negative id whose subsequent `H5Dopen`/`H5Aopen` failures are not checked. Track which filenrs were actually processed (or `stat` the target) before linking.

---

## 3. Per-System Review

### S1. Execution Driver & Lifecycle

What is good: the phase-banner structure makes runs readable; the metadata snapshot (run YAML + every referenced package file + `output_schema.json` + version JSON) is an excellent reproducibility feature; comments at the copy site (`main.c:549-557`) explain *why*, which is the right kind of comment.

**S1-1. `main()` is a 366-line function doing six jobs.** Argument parsing, output-path policy, the file loop, the tree loop, format-specific finalization, and metadata writing are interleaved. Extract, without behavior change: `parse_cli(argc, argv, &log_level)`, `process_file(filenr)`, `finalize_file(filenr)`, and `write_run_metadata(param_file)`. The `#ifdef HDF5 / OutputFormat` dispatch then collapses to one place per concern instead of four (`main.c:348-353, 402-418, 462-470, 475-498, 508-521`).

**S1-2. Output-path construction is quintuplicated.** The same two `snprintf` patterns (`"%s/%s_z%1.3f_%d"` binary, `"%s/%s_%03d.hdf5"` HDF5) appear in `main.c:405-417`, `io/tree/interface.c:111-141`, `io/output/binary.c:75-77`, and `io/output/hdf5.c:249, 1238`. Any change to the naming scheme currently requires five coordinated edits. Add two helpers in `io/output/util.c` — `output_path_binary(buf, n, filenr, snap_index)` and `output_path_hdf5(buf, n, filenr)` — and use them everywhere.

**S1-3. Output files are created in three different places.** `main.c:425-426` creates an empty marker file; `io/tree/interface.c:107-150` creates per-snapshot files inside *the tree-table loader* (a tree-input file should not be creating output files); `io/output/binary.c:80` reopens with `"wb+"`. Consolidate creation into the output writers (S7) and reduce `load_tree_table()` to actually loading the tree table. The marker-file behavior for `--skip` can live in the driver via the S1-2 helpers.

**S1-4. `bufz0` is a shared static path buffer with an `unlink` at exit.** `bye()` (`main.c:124-139`) unlinks whatever `bufz0` last held when `exitfail` is set. The buffer is serially reused for the input tree path and the output path (`main.c:392, 405-417`); a fatal error in the window where it holds the *input* path would delete nothing only because `unlink` on an open-for-read path still succeeds — i.e., it would delete the input tree file path string match. Replace with a dedicated `current_output_path` buffer set only when an output file is created, and clear it once the file is finalized.

**S1-5. `--skip` only checks the first snapshot's binary file.** `main.c:409-419` builds only the `ListOutputSnaps[0]` filename; a run interrupted while writing later snapshots is considered complete. If this is intended (cheap heuristic), say so in a comment; otherwise loop the check over `NOUT`.

**S1-6. `CORE_PROPERTIES_PATH` assumes the process CWD is the repo root.** `main.c:558` hard-codes `"src/core/core_properties.yaml"`; the metadata copy silently degrades to a warning when run from elsewhere. Either embed the file content at build time (it is already an input to `make generate`) or resolve it relative to the executable/an env override.

**S1-7. Dead startup code.** `init.c:64` calls `srand(time(NULL))` while three comments in the same files state the RNG was removed; nothing in `src/` calls `rand()`. Delete the call and the four "random generator removed" comments (`main.c:450, 532`, `init.c:61`, `allvars.c:93`, `globals.h:128`).

**S1-8. Stale process commentary.** Comments like "fix for issue 1.2.1" (`main.c:529`, `init.c:57`, `allvars.c:87`, `globals.h:123`), "(Phase 1)" sync notes (`init.c:74-76, 116, 214`), and `@file` headers naming old filenames (`init.c` says `core_init.c`; `allvars.c` says `core_allvars.c`; `virial.c` says `model_misc.c`; `memory.c` says `util_memory.c`; `io/tree/interface.c` says `io_tree.c`; `error.c` says `error_handling.c`; several others) are reviewer-noise from past refactors. Sweep them out; keep `@brief` content.

**S1-9. The `Age` 1-based-offset trick deserves one authoritative comment.** `init.c:66-69` sets `Age = Age_base + 1` so `Age[-1]` is the lookback time to `INITIAL_REDSHIFT`. This is load-bearing and surprising (e.g., `descendant.new_halo_dt` at `build_model.c:412` guards `current_snap > 0` precisely to avoid relying on it). Document the invariant where `Age` is declared in `globals.h`, or remove the offset and use an explicit `AgeToRecombination` scalar — the offset appears to have no remaining reader that actually indexes `-1`. Verify with a grep for `Age[` before changing.

### S2. Configuration & Parameter System

What is good: precedence (sim config first, run file overrides) is clearly documented at the load site (`read_parameter_file.c:117-125`); `reject_unknown_keys` and the compiled-model/simulation cross-checks (`:1020-1070`) are exactly the fail-fast posture the Vision asks for; phase-name validation (`add_substep_phase`) is thorough.

**S2-1. Validation asymmetry: `simulation` and `plotting` sections accept unknown keys.** `parse_output_section`, `parse_input_section`, and `parse_model_section` all call `reject_unknown_keys`; `parse_simulation_section` (`:523-611`) and `parse_plotting_section` (`:616-626`), including the `cosmology` and `units` sub-mappings, do not. A typo like `omega_mater:` is silently ignored and only caught indirectly if a downstream zero-check exists. Add the key tables for these sections (and for the sub-mappings).

**S2-2. The scalar-extraction pattern repeats ~25 times.** The idiom `node = get_mapping_value(...); if (node && (str = get_scalar_value(node))) strncpy(dest, str, MAX_STRING_LEN - 1);` dominates the file. A small descriptor table per section — `{key, kind(STRING/INT/DOUBLE/ENUM), dest, required}` — plus one `apply_section(doc, section, table, n)` walker would roughly halve the file, make `reject_unknown_keys` automatic (the table *is* the valid-key list), centralize strict numeric parsing (B4), and guarantee uniform null-termination. The HDF5 writer already proves the pattern works in this codebase (`ConfigParamDescriptor`, `output/hdf5.c:1006-1039`).

**S2-3. `strncpy` termination is inconsistent.** Most copies rely on `MimicConfig` being zero-initialized static storage; one site explicitly terminates (`:548`). With the table walker (S2-2) this becomes one correct implementation. If S2-2 is deferred, add a `copy_config_string()` helper now.

**S2-4. Duplicate YAML loader boilerplate.** `read_parameter_file()` (`:73-178`) and `parse_simulation_config_file()` (`:631-678`) duplicate open/parse/root-check/cleanup (~40 lines). Extract `load_yaml_document(fname, &parser, &document)` returning the root node, with one error path.

**S2-5. `resolve_config_path` repeats its own overflow check four times.** (`:268-312`) Each `snprintf` is followed by the identical truncation check. A tiny `checked_snprintf(buf, size, fmt, ...)` (or a single goto-style exit) reduces this to the actual three-step fallback logic, which is currently hard to see.

**S2-6. Model parameter getters re-parse strings on every call.** `model_get_double/int` (`module_registry.c:1135-1167`) do a linear name scan plus `strtod` per call. Current modules cache values in `init()`, so this is fine — but the contract is implicit. State in `module_registry.h` that getters are intended for `init()`-time use only (cheapest fix), so nobody calls them per-halo.

**S2-7. `parameter_helpers.h` hidden control flow.** The `LOAD_*`/`VALIDATE_*` macros `return -1` from the *enclosing function*. That is conventional for this kind of init-validation macro but worth one prominent line in the file header ("these macros return from the caller on failure") rather than only in examples.

### S3. Tree Processing Engine

What is good: this is the strongest-engineered system in the codebase. The gather → inherit → evolve → marshal split (`build_model.c` driver vs format-neutral `inheritance.c` / `output_buffer.c`) is clean and well-commented; the galaxy pool (`galaxy_pool.c`) is a textbook chunked arena with the ownership invariant documented exactly where it matters (`galaxy_pool.h:23-28`); the grow-to-high-water scratch idiom is consistently applied and explained.

**S3-1. `find_most_massive_progenitor` sentinel logic is needlessly opaque.** (`build_model.c:176-203`) `lenoccmax` doubles as a count and a `-1` sentinel meaning "first progenitor already occupied", inherited from SAGE. Equivalent clearer form: track `best_len`/`best_occupied_len`/`first_occupied` and short-circuit when the first progenitor is occupied. There is also a leftover `/* mother_halo = prog; */` (`:193`) and a `lenmax` that is computed but never used after the loop — `lenmax` can be deleted outright. Behavior-preservation matters here (SAGE parity); change only with the scientific tests green.

**S3-2. Duplicated FoF-walk counting.** `count_fof_subhalos` (`:310-320`) and `count_progenitor_galaxies` (`:230-240`) are fine, but `build_halo_tree` walks the FoF chain twice more (`:86-95` progenitor pass, `:113-141` gather pass). This is inherent to the two-phase HaloFlag algorithm — leave it, but the function-level comment should say the three walks are intentional (first: recurse progenitors of all members; second: count; third: gather), because the next reader will be tempted to merge them.

**S3-3. `join_progenitor_halos` copies three locals for no reason.** (`:385-415`) `virial_mass/radius/velocity` are read once into locals and immediately stored into `descendant`. Assign directly to the struct; the function shrinks and the descendant-population block becomes scannable.

**S3-4. `setup_module_context` re-derives `time_interval` that the descendant step already knows.** (`:433-464`) The `prev_snap` comes from `FoFWorkspace[centralgal].SnapNum`, which inheritance set; the same quantity (`dT`) was computed during the gather. Not wrong, but the comment should note that `SnapNum` here is intentionally the *progenitor* snapshot (it is only advanced to the current snapshot at marshal time, `output_buffer.c:47`) — this is the single most surprising invariant in the engine and is currently documented nowhere near this code.

**S3-5. `virial.c` commented-out Bolshoi line.** `get_virial_radius` (`virial.c:106`) starts with a commented-out alternative implementation. Per project style ("comments explain why"), replace with a one-line note that catalog Rvir is intentionally not used, or delete.

**S3-6. `process_halo_evolution` stamps `UniqueCentralGalaxyID` in a driver loop** (`:502-504`) while `CentralMvir` stamping lives in `build_halo_tree` (`:127-130`) — two different homes for the same kind of "structural per-group constant" stamping. Co-locating both (either both in the driver loop or both in the gather) would make the output contract easier to find. Pure organization; no behavior change.

### S4. Module System

What is good: validation is genuinely rigorous — processing-mode support, per-event subscription presence, producer-in-same-phase checks, emit-time declared-event checks — with error messages that tell the user the exact YAML fix. The event state machine (`emission_allowed`, producer-only v1, immediate dispatch preserving ordering) is carefully reasoned and documented. The dependency-enforcement API is a thoughtful response to user-named phases.

**S4-1. Hot-path module lookup by string.** `execute_phase` calls `find_module_by_name()` for every (galaxy × module) pair (`module_registry.c:875`) and `dispatch_events_range` for every (event × per-event module) pair (`:677`), each a linear scan with `strcmp`. With 17 SAGE modules, multiple substeps, and millions of FoF groups this is measurable work, and each site repeats the same "configured but not registered" exit block — a condition already guaranteed impossible by `module_system_init()`. Resolve once at init: add `struct Module *resolved;` to `PhaseModuleConfig`, populate it during `module_system_init()`, and delete the three lookup-and-exit blocks (`:839-843, 677-682, 875-879`). This is simultaneously the system's main simplification and its main performance win.

**S4-2. The "iterate all phases" boilerplate appears six times.** The pre/substep-loop/post triple is spelled out in `module_system_init` (pipeline build `:511-522`, mode validation `:543-559`, event validation `:565-581`), `free_phase_configuration` (`:951-993`), `module_system_enumerate_event_contracts` (`:1060-1067`), and again in `write_enabled_modules` (`output/hdf5.c:710-734`). Add one iterator: `void for_each_phase(void (*fn)(const char *name, struct PhaseModuleConfig *mods, int n, void *ud), void *ud)`. Each caller becomes a callback; ~120 lines collapse and a future fourth lifecycle phase needs one edit instead of six.

**S4-3. `processing_mode_to_string` exists twice.** `module_registry.c:220-231` and `output/hdf5.c:629-640` are identical. Declare it once in `module_interface.h` (next to the enum) and export it.

**S4-4. `module_precedes_in_phase` vs `module_mode_precedes_in_phase`.** (`:333-397`) Two near-identical index-scan functions differing only in whether mode participates in matching. Implement the mode-less one as a call to the mode-aware one with a wildcard (e.g., accept `PROCESSING_MODE_COUNT` as "any"), or share a static `find_index(phase, n, name, mode_or_any)` helper.

**S4-5. Inconsistent fatal-error style.** Registration and execution failures use `ERROR_LOG(...); exit(EXIT_FAILURE);` (`:127-148, 680, 708`), while the rest of the codebase uses `FATAL_ERROR` (which also logs at FATAL level and runs `myexit`). Standardize on `FATAL_ERROR`; the multi-line "Available modules:" listing can stay as preceding `ERROR_LOG` lines.

**S4-6. Misplaced doc comment.** The docstring for `module_system_init` sits at `:157-166` directly above `add_module_to_pipeline`, with a second `@brief` immediately after — readers attribute the wrong contract to the helper. Move it to `:504`.

**S4-7. `format_supported_modes` static buffer.** (`:240-250`) `strcat` into a static 128-byte buffer is safe today (max 3 modes ≈ 60 chars) but has no guard. Either add a size assert or build with `snprintf` accumulation. Cosmetic.

**S4-8. `module_emit_event` skips declared-event validation when the producer lookup fails.** (`:760-776`) `if (producer != NULL && producer->num_emitted_events > 0)` means a registry inconsistency degrades to no validation rather than an error. Since `current_producer_module_id != 0` was already checked, a NULL lookup is an internal invariant violation — make it fatal. One line.

**S4-9. Three byte-identical test consumers.** `test_event_consumer_{alpha,beta,gamma}` differ only in name strings. They must be distinct registered modules (routing is per-module), but the C can be one file generating three via a small macro, or one shared `static int consume(const char *tag, int *counter, ...)` helper. Low value; do only if touching them anyway.

**S4-10. `module_interface.h` example block drift risk.** The 45-line `@code` example (`:35-78`) shows a `Module` struct without the now-required `supported_processing_modes` fields, so copying it produces a module that fails registration-time expectations for directory modules. Either complete the example or point it at `template/template_module.c` (which is maintained).

### S5. Property / Metadata Generation Surface

What is good: this system delivers the Vision's central promise. The `.inc` injection points are each annotated with why they exist (`build_model.c:274-280` is exemplary: "cannot silently desync … this is the only place tree-index coupling touches halo init"). `output_schema.json` written by the producing executable (`main.c:186-200`) is the right answer to binary-format fragility.

**S5-1. Generated headers currently embed `MODEL=sage16` provenance** (`property_defs.h:8-11`, `module_init.c`, `event_contracts.h`), matching the recent package rename — consistent, nothing to do, but note for the fresh team: the *committed* generated files are a build product of one `(MODEL, SIMULATION)` pair; `make check-generated` is the guard.

**S5-2. The `i` variable contract between `.inc` files is invisible.** `calc_hdf5_props()` (`output/hdf5.c:63-97`) validates `i != HDF5_n_props` where `i` is declared inside one generated include and consumed after another. It works, but the host function should declare `int i;` itself with a comment naming the contract ("populated by hdf5_field_definitions.inc"), so the generated snippet doesn't own scope the host checks. Coordinate with the generator script.

**S5-3. `output_helpers.h` carries physics into shared infrastructure by design** — the file's own header explains the rationale well. One nit: `output_rvir_conditional`/`output_vvir_conditional` call `get_virial_radius/velocity(g->HaloNr)` which depend on `InputTreeHalos` being the *current tree's* array; that hidden coupling (output must run before `free_halos_and_tree()`) is worth one comment line, since the marshaller makes halos look self-contained.

### S6. Tree Input I/O

What is good: format dispatch is simple and explicit; per-tree lifecycle allocation/free comments cross-reference `globals.h` correctly.

**S6-1. `myfread`/`myfwrite`/`myfseek` are dead — and `myfwrite` is actively wasteful.** Repo-wide grep shows no callers in `src/`, `models/`, `tests/`, or `plot/`; the binary reader explicitly avoids them ("Use direct fread to avoid our problematic wrapper", `io/tree/binary.c:145`). `myfwrite` (`interface.c:414-445`) allocates, `memset`s (immediately overwritten by `memcpy`), copies, optionally swaps, writes, frees — per call. Delete all three functions, their prototypes (`proto.h:9-11`, `interface.h:49-68`), and then assess `set_file_endianness`/`get_file_endianness` (`interface.c:348-366`): their only remaining effect is a debug log in the binary loader. Either wire endianness handling into the actual `fread` path properly (if non-native files are a real requirement) or remove the half-mechanism; the current state implements byte-swapping that can never run.

**S6-2. `load_tree_table` does output work (see S1-3).** After moving file creation out, the `#ifdef HDF5` block in this function disappears entirely.

**S6-3. HDF5 reader hygiene.** Besides B2/B7: the local prototypes (`fill_metadata_names`, `read_attribute_int`, `read_dataset`, `hdf5.c:53-56`) should be `static`; `close_hdf5_file()` (`:269`) closes unconditionally — guard on a valid handle and reset it (the binary twin does this correctly); `if (hdf5_file <= 0)` (`:189`) should be `< 0` per hid_t semantics; allocation results from `mymalloc_cat` are then NULL-checked inconsistently (`:111` unchecked vs `:196` unchecked while buffer `calloc`s are checked — `mymalloc_cat` FATALs internally on failure, so *all* of these NULL checks, here and in `interface.c:95-99, 272-294` and `tree/binary.c:96-103`, are dead code; deleting them removes ~40 lines and a misleading implication that the allocator can return NULL).

**S6-4. Magic datatype codes.** `read_dataset(…, int32_t datatype, …)` uses 0/1/2 for int/float/llong with comment-only documentation (`hdf5.c:131-158, 217-239`). A tiny enum (`READ_AS_INT/FLOAT/LLONG`) at the top of the file makes the `READ_TREE_PROPERTY` table self-checking against B7-style mistakes.

### S7. Output I/O & Run Products

What is good: the cross-tree HDF5 write buffer with its rationale comment (`output/hdf5.c:1321-1331`) is excellent — measured reasoning, bounded memory, and the comment explains the O() change. The table-driven `store_run_properties` is the right pattern (and the model for S2-2). Metadata completeness (modules, contracts, parameters, redshifts, field schema, per-file self-containment) genuinely fulfills Vision Principle 6.

**S7-1. `output/hdf5.c` is 1,392 lines spanning four concerns.** Split mechanically into: `hdf5_table.c` (calc props, prep file, batch write, buffers, save/flush), `hdf5_metadata.c` (version/modules/contracts/parameters/redshifts/perfile/run-properties), and `hdf5_master.c` (`write_master_file`). No code changes, just placement — each new file is then reviewable in one sitting.

**S7-2. Compound-type construction is triplicated and self-duplicating.** `write_parameters_metadata`, `write_enabled_modules`, and `write_event_contracts` each build `memtype` and `filetype` with *identical* member layouts (`:516-533, 744-753, 891-903`) — when memory and file layouts match, one type passed to both `H5Dcreate` and `H5Dwrite` suffices. Additionally the create/write/describe/cleanup choreography (~50 lines each) differs only in row type and description text; one `write_compound_dataset(group, name, rowtype_builder, rows, nrows, description)` helper would collapse ~150 lines. Also reuse `copy_hdf5_string` in `fill_contract_cb` (`:824-837`), which hand-rolls the same strncpy/terminate four times.

**S7-3. Description-attribute boilerplate.** The "create scalar space, copy string type, set size, create attr, write, close ×3" block appears five times (`:375-389, 553-561, 604-616, 772-784, 919-931`) with sizes 64/256/128/512/512. One `write_description_attr(obj_id, const char *text)` ends it.

**S7-4. Dead `MINIMIZE_IO` block.** `write_hdf5_galsnap_data` (`:236-282`) references `ptr_galsnapdata`/`offset_galsnapdata`, which exist nowhere in the repo — the block cannot compile if the flag is ever defined. Delete it, along with unused `#define TRUE/FALSE` (`:38-39`).

**S7-5. `count_output_halos_by_snapshot` is dead.** (`output/util.c:30-48`, header `:29`) No callers anywhere. Delete (it also drags the `MAXSNAPS` global into its signature — see S8-1).

**S7-6. Binary writer placeholder-header path is confusing.** `save_halos` writes a zeroed header with a non-fatal `ERROR_LOG` saying "Will retry after output is complete" (`binary.c:100-104`); the retry *is* `finalize_halo_file` rewriting the header, but the message reads like a TODO. Reword ("header is rewritten by finalize_halo_file") or make the placeholder write fatal like every other write. Also `assert(save_fd[n])` at `finalize_halo_file` (`binary.c:160`) has the B1 release-build problem if `save_halos` was never called for a snapshot (zero-tree file): use a real check.

**S7-7. `asctime()` output embeds a newline** into the `RunEndTime` attribute (`hdf5.c:1108-1113`); use `strftime` like `version.c` does. Stale line-number references in the `write_master_file` preamble comment (`:1148-1156`, "lines 667-681") should be dropped — they are already wrong.

**S7-8. `python_example.c` is 280 lines of `PY()` line-printing.** Maintainable but unpleasant to edit (every Python edit needs C-string escaping, and the formatter can't help). Consider storing the two script variants as template files embedded at build time (e.g., `xxd -i` or a generated header) with a tiny `{{PLACEHOLDER}}` substitution pass. Low priority; the current form works and is tested by inspection.

### S8. Foundation Utilities & Shared Headers

What is good: the memory system's category tracking with thread-safety caveats documented up front; `numeric.h` has precise NaN-semantics documentation per function; the logging system's quiet/normal/verbose triage is well thought through.

**S8-1. The legacy global mirrors are the single largest coherence debt.** `config.h:16-32` says the `SYNC_CONFIG_*` macros are "temporary scaffolding … will be removed in Phase 3; DO NOT use in new code", yet the mirrors persist across `allvars.c:79-91` / `globals.h:114-126` and the engine still reads one: `ctx->redshift = ZZ[snap]` at `build_model.c:437` (everything else uses `MimicConfig.ZZ`). Plan: (1) switch the handful of remaining readers (`ZZ` ×1; audit `MAXSNAPS` in `output/util.h:29` which dies with S7-5; `ListOutputSnaps`/`NOUT`/unit globals appear reader-less) to `MimicConfig.*`; (2) delete the mirror definitions, externs, the `SYNC_CONFIG_*` macros, and the sync call sites (`init.c:117-128, 215-217`, `read_parameter_file.c:1123-1126`); (3) keep `Age`/`Age_base` (real state, not mirrors). This kills an entire class of "which copy is current?" bugs and ~60 lines.

**S8-2. Verified-dead globals and constants.** No reader anywhere in the repo: `NParam`, `ParamTag`, `ParamID`, `ParamAddr` (+ `MAXTAGS`), `TotHalos` (the Python test's `TotHalos` is a JSON key, not this symbol), `FirstHaloInSnap`, `HDF5Output`, `core_output_file`, `BoxSize` (the bare global; the config field is used); write-only: `HaloCounter`, `TreeID`, `FileNum` (set in `main.c:429, 447, 454`, never read). Constants `ALLOCPARAMETER`, `MAX_NODE_NAME_LEN`, `EPSILON_LARGE` are unused; `proto.h:32` declares `read_yaml_parameter_file` which has no definition. Delete all of the above. The memory-debug API (`set_memory_reporting`, `validate_memory_block`, `validate_all_memory`) is uncalled but is legitimate debug tooling — keep, but move the prototypes out of `proto.h` so `memory.h` is their single home (they are currently declared in both).

**S8-3. `integration.c` is a facade pretending to be GSL.** `integration_qag` ignores `epsabs`, `epsrel`, `limit`, `key`, and `workspace`, hard-codes tolerance 1e-10/depth 20 in `simpson_integrate`, and always returns success (`integration.c:116-141`, including an `if/else` where both branches `return 0`). Its only caller, `time_to_present` (`init.c:241-262`), consequently allocates a workspace that does nothing and passes `1/Hubble` as an "absolute tolerance" that is never read. Replace the whole API with `double integrate_adaptive_simpson(integrand_func_t f, void *params, double a, double b, double tol, double *abserr)`; delete the workspace type, the duplicate `INTEG_GAUSS*` constant definitions (defined in both `.c` and `.h`), and the misleading SAGE-parity tolerance comment (`init.c:252` claims 1e-9 relative tolerance is in effect; it is not). Numerically nothing changes — that is the point: the code should say what actually happens.

**S8-4. `version.c` has two version-info mechanisms; keep one.** Build-time `git_version.h` provides `GIT_COMMIT`/`GIT_BRANCH` (used by `run_log.c` and HDF5 metadata), while `version.c` checks differently-named `GIT_COMMIT_HASH`/`GIT_BRANCH_NAME` macros (with `"@…@"` template-substitution guards from an older build system) and falls back to `popen("git …")` — which reports whatever repo the *current working directory* is in, a provenance hazard. Use the `git_version.h` macros directly, delete `execute_command`-based git/branch/URL discovery (this also deletes bug B6), and consider whether the `popen`-based md5/sw_vers calls are worth keeping (an in-process MD5 of the param file would remove the last shell-out; the ~60-line `/etc/os-release` parser for cosmetic OS naming could shrink to plain `uname` fields).

**S8-5. `log_message` and `log_io_error` duplicate ~45 lines.** Stream selection, colour selection, message body, colour reset, newline fix-up, and flush logic are copy-pasted (`error.c:266-345` vs `:372-436`), and they have already drifted: `log_io_error` ignores `verbose_format` and always prints full context with a differently-formatted timestamp. Extract `static void vlog_common(level, FILE *out, const char *header, const char *fmt, va_list)` used by both; decide deliberately whether IO logs obey `verbose_format` (recommended: yes, for consistency).

**S8-6. `memory.c` minor duplication.** The high-watermark report block appears in both `mymalloc_cat` (`:227-234`) and `myrealloc_cat` (`:344-352`) — extract `update_high_watermark()`. The bare `mymalloc`/`myrealloc` compatibility wrappers have few remaining callers (e.g., `FoFWorkspace` growth uses `myrealloc`, `build_model.c:261`); migrating those to `_cat` and deleting the wrappers removes an API choice that currently exists only for history. `find_block_index`'s O(N) scan is acceptable post-galaxy-pool (block count is now O(arrays), not O(halos)) — keep, but its comment should state that assumption so a future per-object allocation pattern doesn't silently reintroduce quadratic frees.

**S8-7. `proto.h` is a residual grab-bag.** It mixes tree-driver, init, IO, memory, and virial prototypes, duplicating declarations that already live in proper headers (`memory.h`, `tree/interface.h`). Target state: each subsystem's header owns its prototypes; `proto.h` shrinks to the few genuinely cross-cutting driver functions (`build_halo_tree`, `join_progenitor_halos`, `process_halo_evolution`, `init`, `read_parameter_file`, virial helpers) or disappears. Mechanical, zero risk, large clarity gain for newcomers.

**S8-8. `numeric.c` epsilon semantics are absolute, not relative.** `safe_div` treats any |denominator| < 1e-10 as zero — correct for this domain (code units keep quantities near unity) but worth one header line stating the assumption, since `safe_div(x, tiny_but_real, 0.0)` silently returns the default. No code change.

---

## 4. Cross-Cutting Themes

1. **Finish the config migration (S8-1).** The codebase is 95% through a globals→`MimicConfig` transition; the remaining 5% (mirrors, sync macros, one live reader) costs more in reader confusion than the cleanup costs in effort.
2. **One fatal-error idiom.** Three styles coexist: `FATAL_ERROR`, `ERROR_LOG`+`exit`, `ERROR_LOG`+`assert`. The third is a release-build hazard (B1); the second loses the FATAL log level. Mandate `FATAL_ERROR` and add a line to AGENTS.md.
3. **Allocator NULL checks are dead weight.** `mymalloc_cat` FATALs internally; the ~15 `if (ptr == NULL) FATAL_ERROR` blocks after it (tree interface, readers, HDF5 writer) imply a contract that doesn't exist. Delete them all in one sweep.
4. **Fail-fast parsing has gaps precisely where the Vision demands it** (B3–B5, S2-1). The fixes are small and high-value for scientific users.
5. **Extract-the-third-copy rule.** The big duplication families — output paths (×5), phase iteration (×6), compound datasets (×3), YAML loading (×2), log formatting (×2) — each need one helper. Together they remove roughly 500–600 lines without touching behavior.
6. **Comments: this codebase over-documents the obvious and under-documents two real invariants** — the `Age+1` offset (S1-9) and "workspace `SnapNum` is the progenitor's until marshal" (S3-4). Trade fifty lines of boilerplate Doxygen for those two paragraphs.

---

## 5. Suggested Implementation Order

Each batch is independently shippable; run `make tests` (delegating unit/integration per AGENTS.md) plus `make tests-scientific` after each.

| Batch | Content | Risk | Approx. effort |
|---|---|---|---|
| 1. Bug fixes | B1–B6, B8 (+ verify B7 against a Genesis file) | Low — each is local | 0.5 day |
| 2. Dead-code sweep | S1-7, S6-1, S7-4, S7-5, S8-2, allocator NULL checks (theme 3), `lenmax` in S3-1 | Low — verified no readers | 0.5 day |
| 3. Globals retirement | S8-1 (switch `ZZ[snap]` reader, delete mirrors/sync macros), S8-7 (`proto.h` slimming) | Low-medium — mechanical but wide | 1 day |
| 4. Fail-fast config | B3–B5 hardening, S2-1 key tables, S2-4 YAML-loader helper, S2-5 | Low | 0.5–1 day |
| 5. Duplication helpers | S1-2 path helpers, S1-3 file-creation consolidation, S4-2 phase iterator, S4-3, S7-2/S7-3 HDF5 helpers, S8-5 log core, S8-6 | Medium — wide but mechanical; review diffs per helper | 2 days |
| 6. Module-system hot path | S4-1 resolved-pointer caching (+ S4-5 error-style unification) | Medium — touches execution engine; verify with scientific baseline tests | 0.5 day |
| 7. Structural splits | S1-1 `main()` extraction, S7-1 hdf5.c split, S8-3 integration API replacement, S8-4 version consolidation | Medium — placement-only refactors, behavior-neutral | 1.5 days |
| 8. Polish | S1-8 comment sweep, S3-3/S3-5, S4-6/S4-7/S4-10, S6-3/S6-4, S7-6/S7-7, invariant docs (S1-9, S3-4) | Low | 0.5 day |

Items deliberately deferred (revisit only with motivation): S3-1 sentinel rewrite (SAGE parity risk vs cosmetic gain), S4-9 test-consumer dedup, S7-8 Python-template externalization, S2-2 full table-driven parser (do after batch 4 proves the strict-parsing helpers; it subsumes S2-3).

---

## 6. What Not to Change

For a fresh team, these read as oddities but are deliberate and correct — do not "fix" them:

- **The galaxy pool returns uninitialised slots** (`galaxy_pool.h:23-28`); every caller fully overwrites. This is the documented invariant, not a bug.
- **Type 3 halos are dropped at marshal time with only a pointer clear** (`output_buffer.c:34-39`); the pool owns the memory.
- **Events dispatch immediately on emit** (`module_registry.c:806-807`) plus a safety re-dispatch after each producer (`:861`); the double mechanism preserves producer-side ordering and is intentional.
- **`MimicConfig` phase arrays mix `myfree` (arrays) with `free` (strdup'd names)** in `free_phase_configuration` — correct pairing with their allocators.
- **The run-persistent scratch buffers** (`ProgenitorScratch`, `OutputSegmentScratch`, the galaxy pool) intentionally survive across trees; the non-LIFO allocator makes this safe (`build_model.c:289-295`).
- **Binary output skips `--compress`**; compression is an HDF5-table feature only, as the CLI help implies.

---

## 7. Implementation Addendum (2026-06-10)

All eight batches were implemented and verified (unit 32/32, integration, and scientific-baseline suites all pass; both `USE-HDF5=yes` and `USE-HDF5=no` builds are clean with zero warnings). Deviations from the plan, found during implementation:

- **`TreeID` and `FileNum` are not dead** (§S8-2 was wrong): the SHAM module `sham_assign_stellar_mass.c` reads them to build unique IDs. They were kept, with a comment in `allvars.c` recording the dependency.
- **The bare unit globals had readers outside `src/`**: the generated `copy_to_output.inc` (via `output_convert:` expressions in the property YAML) and two SAGE/SHAM modules plus three module-local unit tests. All were migrated to `MimicConfig.*` — the property metadata now references `MimicConfig.Unit*` directly, which is the single-source-of-truth direction the Vision asks for.
- **`mymalloc`/`myrealloc` compatibility wrappers were kept**: unit tests exercise them directly.
- **`src/io/util.{c,h}` (endianness layer) became fully dead** after removing the unused `myfread`/`myfwrite`/`myfseek` wrappers and were archived to `archive/removed-src/io/`; `tests/unit/run_tests.sh` was updated accordingly.
- **B7 (`SubHalfMass` read as int)** was fixed to a float read, consistent with the struct field and sibling properties — but no Genesis-format HDF5 tree fixture exists in the repo, so this path is still unvalidated against real data.
- **The hdf5.c split** produced `metadata_hdf5.c` and `master_hdf5.c` (names chosen to keep matching the Makefile's `%hdf5.c` filter for non-HDF5 builds) with a private `hdf5_internal.h`.
- **S2-2 (full table-driven parser) and S3-1 (sentinel rewrite) were deferred** as planned; S4-9 (test-consumer dedup) and S7-8 (Python-template externalization) were also left as documented.
