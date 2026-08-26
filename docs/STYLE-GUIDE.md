# Mimic Style Guide

**Purpose**: Define the human style conventions that keep Mimic readable, reviewable, and consistent across releases.

This guide complements [VISION.md](VISION.md), [DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md), and the formatter configuration. It should preserve what Mimic already does well, make mixed or weak conventions explicit, and avoid style churn that does not improve maintainability.

---

## Guiding Principle

Mimic style exists to support the architecture: a physics-agnostic core, runtime-configurable modules, generated structural metadata, reproducible outputs, and fast failure. Prefer code and documentation that make ownership, assumptions, units, dependencies, and validation boundaries obvious.

Prefer the surrounding file's style when it is clear, professional, and consistent with this guide. If local style is confusing, stale, over-commented, under-documented, or inconsistent with nearby packages, flag it during the sweep and clean it up deliberately rather than preserving it by default.

---

## Style Dimensions

A project style guide can cover many dimensions. For Mimic, keep the list broad and practical:

- Formatting: indentation, line length, include/import ordering, and whitespace.
- Naming: files, functions, variables, constants, properties, modules, tests, and generated identifiers.
- Comments: what deserves explanation, how long comments should be, and where API documentation belongs.
- Documentation: README structure, module documentation, user-facing guides, and generated-system explanations.
- Architecture boundaries: core versus model packages, simulation packages, generated files, tests, and plotting.
- Error handling and logging: when to fail, warn, log verbosely, or skip.
- Metadata and configuration: YAML schema style, descriptions, units, dependencies, and validation.
- Testing: test names, result markers, skip behavior, fixture ownership, and output discipline.
- Scientific clarity: units, reference data, provenance, numerical tolerances, and parity notes.
- Repository hygiene: generated code, archived outputs, local data, and sweep/commit discipline.

Mimic should formalise the dimensions that affect readability, correctness, and contributor expectations. Leave purely mechanical layout to tools.

---

## Tool-Owned Style

The formatter is authoritative for mechanical C and Python formatting.

- Run `./scripts/beautify.sh` before committing changes.
- C uses `.clang-format`: LLVM base style, 100-column limit, include order preserved.
- Python uses Black and isort via `pyproject.toml`: 100-column limit, Black import profile.
- Do not hand-format around the formatter.
- Do not edit generated C or headers under `generated/` directories. Change the metadata or generator instead, then run `make generate`.

The style guide should not duplicate every formatter rule. If a formatting question is answered by clang-format, Black, or isort, use the tool.

---

## Repository Boundaries

Keep changes in the package that owns the behavior.

- `src/core/` owns execution, configuration parsing, tree processing, workspace lifecycle, module dispatch, inheritance, and output buffering.
- `src/io/` owns input readers and output writers.
- `src/module_system/` owns framework helpers, templates, constants, and module-system test fixtures.
- `models/<model>/` owns model-local physics modules, model properties, model parameters, shared helpers, plot figures, and model-local tests.
- `simulations/<simulation>/` owns catalog halo properties, simulation metadata, snapshot lists, tree fixtures, and simulation-local tests.
- `plot/mimic-plot/` owns plotting, schema readers, plotting profiles, and plot validation helpers.
- `tests/` owns cross-package framework, unit, integration, and scientific tests.

Do not put model-specific physics in the core. Do not make a simulation package depend on a model package. Cross-model experiments should become a new model package with reconciled properties, parameters, dependencies, tests, and plots.

---

## Naming

Use existing Mimic naming where it is already established and works well. Treat inconsistent or unclear local naming as a sweep finding, not as precedent.

### Files and Directories

- C source/header files use `snake_case.c` and `snake_case.h`.
- Python files use `snake_case.py`.
- Runtime module directories use the module name, usually `snake_case` with a model prefix when useful, such as `sage_apply_cooling`.
- Module-owned tests live under the module's `_tests/` directory.
- Cross-module tests live under `models/<model>/modules/_tests/`.
- Markdown guides use uppercase descriptive names when they are top-level project documents, such as `VISION.md` and `DEVELOPER-GUIDE.md`.

### C

- Functions and local variables use `snake_case`.
- Struct types use `struct Name` rather than typedef aliases for project-owned structs.
- Enums and macros use uppercase names where they represent constants, modes, or logging/test helpers, such as `PROCESSING_MODE_BY_GALAXY`, `ERROR_LOG`, and `TEST_ASSERT`.
- Generated property fields keep their metadata names. Do not rename properties merely to fit C naming style.
- Runtime module lifecycle functions use `<module_name>_init`, `<module_name>_process`, and `<module_name>_cleanup`.

### Python

- Functions and variables use `snake_case`.
- Constants use uppercase names.
- Test functions use `test_<behavior_being_validated>`.
- Prefer `Path` for repository paths in new code unless existing local code is built around strings.

### Metadata

- Module names are lowercase `snake_case`.
- Processing mode strings use the established configuration names: `process_full_halo`, `process_per_event`, and `process_by_galaxy`.
- Property names follow the existing scientific/output schema names, often `PascalCase` or legacy SAGE-compatible names such as `Mvir`, `dT`, and `ColdGas`. Preserve these names because they are part of the generated API and output schema.
- Parameter names follow the model package convention and should match the user-facing run YAML exactly.

---

## Comments

Comments should explain intent, contracts, ownership, units, scientific assumptions, edge cases, or non-obvious implementation choices. Avoid comments that restate the next line of code.

### C Comments

- Public headers, important source files, framework APIs, and test framework utilities should use Doxygen-style `/** ... */` comments for file headers and public contracts.
- Static helper functions may have a short Doxygen comment when the helper encodes a meaningful algorithm or contract.
- Use short local comments for invariants, parity notes, sentinel handling, and failure cleanup.
- Both `/* ... */` and `//` are already used in Mimic. Preserve the local file's style for short comments when it is readable and consistent; clean up files where mixed comment style makes the code harder to scan.
- Prefer one or two focused lines for inline comments. Move longer explanations to a function-level comment, module README, or developer guide section.
- Comment why generated-code boundaries exist; do not document generated field lists by hand unless the list is small and stable.

Good comment subjects:

- `SAGE parity:` notes where behavior intentionally matches legacy SAGE.
- Unit conversions and h-convention assumptions.
- Sentinel values, valid zeroes, and physical range assumptions.
- Processing order requirements that are also enforced in `init()`.
- Ownership/lifetime rules for allocations, workspaces, and output buffers.
- Why a warning is non-fatal.

Poor comment subjects:

- Repeating a function name in prose.
- Explaining obvious assignments.
- Long historical notes that belong in docs or commit history.
- TODOs without an owner, condition, or intended resolution.

### Python Docstrings

- Public scripts and modules should start with a module docstring that explains purpose and command-line usage when relevant.
- Public helpers should have concise docstrings when their behavior, return value, or error contract is not obvious.
- Tests should document the behavior being validated when the setup is non-trivial.
- Prefer plain docstrings over heavy framework-specific markup. Include `Args:` and `Returns:` when they clarify the contract.

---

## C Style Beyond Formatting

Keep C code explicit and straightforward.

- Include headers in the order expected by the local file. `.clang-format` preserves include order, so choose order intentionally.
- Keep module C files organised into helpers, lifecycle functions, and local state. Section banners are acceptable in module files that already use them.
- Validate processing mode expectations at the top of module `process()` functions, for example `ngal == 1` for `process_by_galaxy`.
- Treat `struct ModuleContext *ctx` fields as read-only.
- Use the project logging macros instead of raw `printf` in runtime code, except in tests and deliberate CLI output.
- Use `mymalloc_cat`, `myrealloc_cat`, and `myfree` for tracked project allocations.
- Use explicit memory categories that match the owning subsystem.
- Prefer early returns for invalid/no-op cases when they keep physics paths clear.
- Avoid hidden global state in modules unless it is loaded during `init()` and cleaned up in `cleanup()`.
- Keep scientific constants in existing constants headers or model-local shared helpers, not scattered through modules.

When a function must encode a scientific prescription, separate the mechanical plumbing from the physics calculation where that improves reviewability.

---

## Python Style Beyond Formatting

Python code should be script-friendly, testable, and explicit about repository paths.

- Keep command-line parsing in `main()` or script-level orchestration functions.
- Keep reusable logic in helpers that can be tested without invoking a whole run.
- Use `REPO_ROOT`/discovery helpers rather than assuming the current working directory.
- Use structured helpers for repeated test and validation patterns.
- Raise or return explicit failures rather than relying on natural-language output parsing.
- Keep plotting functions returning `(plot_path, skip_message)` where that is the local convention.
- Prefer concise validation messages that include the missing field, parameter, path, or command context.

Python scripts may print user-facing progress. Tests and validators should still produce structured result markers where the test framework expects them.

---

## Metadata and YAML

Metadata is structural source code. Treat it with the same care as C and Python.

- Keep YAML keys in the order used by nearby entries.
- Provide a `description` that is meaningful to users inspecting metadata or output files.
- Include units for physical quantities. Use `dimensionless` when no unit applies.
- Document non-obvious `range`, `sentinels`, `init_repeat`, `output_convert`, and transport-buffer behavior with short inline comments.
- Declare module dependencies completely in `module_info.yaml`: properties, parameters, tests, docs, supported processing modes, and event contracts where applicable.
- Do not duplicate generated structural lists in prose unless the list is small and stable.
- Prefer explicit empty lists such as `parameters: []` when the schema expects a list.
- Keep model-local and simulation-local metadata inside their package.

For property metadata, preserve established scientific names and output schema compatibility unless a migration is intentionally planned.

---

## Documentation

Documentation should help users and developers understand the active contract, not mirror generated code exhaustively.

- Start major project documents with a short purpose statement.
- Link to `VISION.md` for architectural rationale rather than restating it in full.
- Keep user-facing docs focused on commands, configuration, output interpretation, and troubleshooting.
- Keep developer docs focused on ownership boundaries, extension points, contracts, and validation.
- Module READMEs should document processing contract, ordering constraints, properties, parameters, and relevant scientific notes.
- Simulation READMEs should document data provenance, units, snapshot lists, fixtures, and maintenance notes.
- Markdown prose should not be manually hard-wrapped — write full paragraphs as single long lines; editors and rendered views soft-wrap automatically.
- Code blocks and YAML/shell examples within Markdown follow the 100-character guideline; keep them readable without horizontal scrolling.
- Apply these rules to all `.md` files: project docs, READMEs, and skill files.

**Never cite a gitignored or machine-local document as the source of a technical fact.** A committed comment, test, or document that says "see X" must resolve for anyone who clones the repository. Operational working files — a root `HANDOFF.md`, anything under `archive/` — are legitimate to *name* as the place work is tracked, but a fact they carry must be stated where it is used and evidenced against committed code. Prefer `path.c:line` to a prose pointer; a shorthand tag whose key lives only in an untracked file is the failure mode this rule exists to stop.

When code, metadata, and docs disagree, fix the source of truth first. Usually that means metadata or code, then generated artifacts, then documentation.

---

## Logging and Errors

Mimic should fail early on invalid configuration, invalid metadata, impossible processing contracts, corrupt input, and unsafe ownership assumptions.

- Use `FATAL_ERROR` for unrecoverable runtime conditions where continuing would corrupt results or hide invalid state.
- Use `ERROR_LOG` and return failure from module lifecycle/process functions when the caller can abort cleanly.
- Use `WARNING_LOG` for unusual but recoverable conditions that users should see.
- Use `INFO_LOG` for major lifecycle or run milestones.
- Use `VERBOSE_LOG` for diagnostic detail that should only appear with `--verbose` or `--debug`.
- Use I/O-specific logging macros for file format and filesystem operations.
- Error messages should include the relevant file, module, property, parameter, mode, or path.
- Do not weaken errors into warnings merely to get a test or run to pass.

Skips are allowed only when a test or feature is genuinely unavailable in the current configuration. A skipped test should state the reason.

---

## Tests

Tests are part of the style contract because they define how failures are surfaced.

- New C tests should use the framework macros in `tests/framework/test_framework.h`.
- New Python tests should use the result marker helpers from `tests/framework/markers.py` or the higher-level framework helpers that emit them.
- Every test case should emit a structured `MIMIC_RESULT:` marker through the framework.
- C unit tests commonly follow `SETUP`, `EXECUTE`, `VALIDATE`, `CLEANUP` comment sections; use that structure when it improves scanability.
- Test names should describe behavior, not implementation details.
- Use `TEST_SKIP_WITH` or `TestSkipped` for deliberate configuration skips.
- Keep module-owned tests near the module; keep cross-module pipeline behavior in the model package `_tests/` or top-level `tests/` as appropriate.
- Capture long test output to logs under `archive/test-logs/` when running manually.
- Never simplify failing tests to make them pass.

Scientific tests should state the physical or parity contract they protect, including tolerances and reference data where applicable.

---

## Generated Code

Generated files are outputs, not editing targets.

- Do not hand-edit files under any `generated/` directory.
- Change property YAML, module metadata, or generator scripts instead.
- Run `make generate` after metadata or generator changes.
- Run `make check-generated` before considering generation-sensitive work complete.
- Generated code should be reviewed through its source metadata and generator changes, not by tweaking generated output.

This rule keeps structural truth in one place and matches Mimic's metadata-first architecture.

---

## Codebase Sweep Checklist

Use this checklist for any wider consistency passes. Prefer focused edits, but do not skip real cleanup just because the existing file already has a poor local pattern.

1. Run `./scripts/beautify.sh` and treat formatter output as mechanical.
2. Find files with missing or misleading file/module docstrings.
3. Check C public headers and framework APIs for clear Doxygen-style contracts.
4. Check module `process()` functions for explicit processing-mode expectations.
5. Check comments for stale statements, duplicated code descriptions, and unexplained scientific assumptions.
6. Check module READMEs against `module_info.yaml` dependencies, parameters, tests, and docs.
7. Check YAML descriptions, units, sentinels, and ranges for user-facing clarity.
8. Check logging and error messages for actionable context.
9. Check tests for structured result markers and meaningful skip reasons.
10. Check that no generated files were hand-edited.
11. Run the narrow relevant tests first, then broader suites according to the testing strategy in `AGENTS.md`.

Do not perform global renames, comment delimiter rewrites, or documentation reshuffles unless they fix real inconsistency, weak local style, or user/developer confusion.

---

## Review Standard

A style cleanup is successful when it makes the codebase easier to review without changing scientific behavior. The preferred outcome is a targeted set of edits that align inconsistent or sub-par files with Mimic's strongest existing conventions.

When in doubt, choose the convention that is already dominant in the nearest well-maintained package and that best supports the contracts in `VISION.md`.
