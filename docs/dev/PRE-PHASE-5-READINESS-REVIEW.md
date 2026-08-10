# Pre-Phase-5 Readiness Review

**Status:** IMPLEMENTED 2026-08-10 — F1, F2 and F3 all landed and validated; the simplification pass deliberately changed nothing. Externally reviewed by a two-model panel before implementation and re-reviewed after. No P0/P1 at any stage. See [Implementation Record](#implementation-record). Ephemeral working material — fold into the pathway and archive once Phase 5 planning consumes it.
**Date:** 2026-08-10
**Reviewer:** Independent pass, commissioned after `docs/dev/PHASE-4B-REVIEW-AND-PRE-PHASE-5-WORK.md` was marked IMPLEMENTED.
**Question this report answers:** Is the repository at `feature/ctrees-snapshot-reader` (= `main`, `69590cc6`) ready for Phase 5 planning to begin from `MIMIC-DUAL-DRIVER-PLAN.md`'s Phase 5 section?

---

## Summary

**Verdict: PASS WITH RISKS. One one-line code fix and one document correction stand between this repository and Phase 5 planning.**

The Phase 4b snapshot reader and the five pre-Phase-5 work commits are high-quality, well-tested, and internally consistent. This pass found **no P0 or P1 defect**: nothing mishandles valid input, no memory or lifetime error, no numerical or byte-order hazard, no build or portability regression, and no untested behaviour that valid data can reach. The one code finding (F2) is a *missing* rejection of invalid input, not a mistake in handling valid input. Full validation is green (see [Validation Performed](#validation-performed)).

Two P2 findings and one P3 remain, and only one of them is about code:

- **F1 (P2, documentation integrity)** — the five pre-Phase-5 commits shifted line numbers in four files that the dual-driver plan's Phase 5 section cites. **Four** references now point at unrelated content, two more have drifted, and one quoted fact is stale — all inside the exact document the pathway names as "the sole input a Phase 5 implementation plan needs". Every one of them resolved correctly when the plan text was finalised at `6365d882`. This is self-inflicted by the work that was supposed to leave Phase 5 ready, and W7 (the documentation-corrections batch) did not re-check it after the code landed.
- **F2 (P2, validation gap)** — `HaloRankInForest` has no per-value lower bound at `open_run`, while its sibling identity column `ForestIndex` does. A negative rank passes validation and would silently produce a wrong `UniqueGalaxyID` under the Phase 5 encoder. A one-token fix plus the one regression case that pins the branch.
- **F3 (P3, coverage)** — the two new `make tests` steps (`tests-converter`, `check-snapshot-fixture`) are not mirrored in CI, so W5/W6 gate local runs only.

Total remedial effort: well under an hour. **Recommendation: fix F1 and F2 before Phase 5 planning starts; F3 is optional and already on the record as a follow-up.**

The simplification pass found the reader to be **already at its right size**. Four micro-cleanups were considered; the two external reviewers split on them (Codex: drop all four; OpenCode: three are worth doing), which is itself the verdict — genuinely marginal changes do not belong in a pre-Phase-5 gate. Of four larger consolidations, three are **recommended against** outright and one is **deferred to Phase 5**, with reasons recorded so none is rediscovered later. **The actionable output of the simplification pass is: change nothing before Phase 5.**

---

## Scope and Method

**Reviewed:** the full `b4d9b240..HEAD` range — 22 commits, 104 files, ~7.3k insertions — with direct attention to:

- the reader core: `src/io/snapshot/reader.h`, `interface.c`, `registry.c`, `read_snapshot_hdf5.c` (1,177 lines, read in full);
- the configuration wiring diff: `src/core/read_parameter_file.c`, `src/include/types.h`, `src/util/error.h`;
- the five pre-Phase-5 commits `77ab8462` (W1), `c7d25df2` (W2/W6), `166fbfa4` (W4/W5), `07aad915` (W3), `69590cc6` (W7);
- the fixture package, both Python tools, the fixture unit test, `test_parameter_parsing.c`, and `test_processing_order.py`;
- `Makefile`, `.github/workflows/ci.yml`;
- `docs/VISION.md`, `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`, `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md`, `docs/dev/SNAPSHOT-HDF5-FORMAT.md`, and the prior review report.

**Checked against:** the frozen format spec (`SNAPSHOT-HDF5-FORMAT.md`, `format_version = 1`), the dual-driver plan's Phase 4b scope and Phase 5 inputs, and `docs/VISION.md` principles 1, 5, 6 and 7.

**Dimensions covered:** correctness; boundary and invalid input; memory lifetime, ownership and leak paths; interfaces and data contracts; numerical validity (exact `scale_factor` comparison, overflow-safe bound algebra, byte-order handling, float comparison in the conformance checker); error handling and diagnosability; test adequacy and the specific risk that the W3 rewrites became tautological or silently skipping; build and portability (`USE-HDF5=no`, worktree builds, CI parity); documentation accuracy; and a simplification pass over the reader and the new tooling.

**Independent corroboration — pre-panel.** F1 was first cross-checked by a separate read-only pass that re-derived every `file:line` citation and checkable factual claim in the Phase 5 section (~45 of them) from the repository without seeing this report's conclusions. It independently found the same four hard breaks and the same stale quote, rated the two `read_parameter_file.c` entries as still-acceptable (which is why they are "soft drift" above rather than breaks), and surfaced the `util.c:63` off-by-four. It confirmed the remaining ~40 citations land exactly, including the load-bearing ones Phase 5 is built on: the three `TREE_MUL_FAC` tree-reader sites plus the one diagnostic-only site, `snapshot_reader_open_run()` having no `src/` caller, and both `TotHalosPerSnap` `H5T_NATIVE_INT` sites.

**External panel (2026-08-10).** Two independent read-only delegates via the orchestrator, artifacts under `.orchestrator/runs/delegates-20260810-175424-82746/`:

- **Codex `gpt-5.6-sol`** (high effort, read-only sandbox) — full review. Returned CHALLENGE with 5 issues, no P0/P1, and **no additional material implementation defect beyond F2**. Every factual challenge was independently re-verified against the repository before acceptance; all were correct, and all are folded into this revision (see below).
- **OpenCode `opencode-go/hy3`** (high effort, plan-agent edit denial) — focused review of F2 and the simplification set after three full-scope attempts failed. Confirmed F2 as "worth-doing… not defensive bloat" from independent evidence, and split from Codex on the four cleanups.

**Panel-driven corrections to this report, all verified before acceptance:** the commit count (22, not 17 — the earlier figure was copied from the prior report, which predates the five pre-Phase-5 commits); "five link domains" → five link *fields* across three domains (`read_snapshot_hdf5.c:741-765`); F1's "resolved exactly at `6365d882`" softened, since `read_parameter_file.c:963-965` covered only `PartMass` even then; F2's regression test upgraded from optional to **required**; the four simplifications demoted from "worth doing" to "leave them"; the false "a truncated snapshot list always aborts" claim withdrawn; and the summary no longer says the code is ready without qualification, since F2 is a code finding.

**Panel limitations, stated rather than hidden:** neither delegate ran tests (read-only mode; both treated the green validation record below as evidence). OpenCode's pass was narrowed to F2 and the simplification judgement, so F1's individual line corrections and F3 carry Codex's verification plus this report's own, not OpenCode's. Three earlier OpenCode attempts and one Codex attempt failed for environment reasons — a pending CLI update, a macOS permission prompt blocking Codex's command host, and an OpenCode tool-call schema error — and produced no reviewable output; none is counted as a review. The failed Codex attempt correctly reported every item UNCHECKED rather than reviewing from the report text alone.

**Not re-done:** authorization/drift audit (performed under `project-manager` Mode B against the frozen `MIMIC-SNAPSHOT-READER-PLAN.md`), and the prior report's own findings, which were verified as fixed rather than re-litigated.

---

## Findings

### F1 — [P2] Citations in the Phase 5 plan section were broken by the pre-Phase-5 work itself

`docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md:50` states that the dual-driver plan's Phase 5 section "is now the sole input a Phase 5 implementation plan needs". The prior review verified every citation in that section and found it accurate — but it did so *before* the five pre-Phase-5 commits landed, and those commits edited four of the cited files above the cited lines. W7's documentation batch corrected other documents and did not re-verify this one.

All but one of the references below resolved at `6365d882`, the commit that finalised the Phase 5 plan text (the exception is noted in the soft-drift table). They are separated by severity, because they are not equally bad and treating them as one bucket would overstate the finding.

**Hard breaks — the cited lines now hold unrelated content (4):**

| Plan line | Citation | Content at `HEAD` | Correct value | Broken by |
|---|---|---|---|---|
| 199 | `test_unit_snapshot_reader_open.c:1071-1072` | an unrelated NULL-hook-abort test | **`:1221-1222`** | `166fbfa4` (+142 lines above) |
| 196 | `read_snapshot_hdf5.c:981-990` | the header cross-file consistency checks | **`:996-1006`** | `166fbfa4` (+15 `_Static_assert` lines above) |
| 216 | `Makefile:779` | a comment inside the new `tests-converter` block | **`:810`** (`define RUN_PYTHON_TIER`) | `c7d25df2` (+~32 lines above) |
| 216 | `Makefile:795` | the new `check-snapshot-fixture:` target | **`:826`** (`tests-scientific:`) | `c7d25df2` |

Two of these carry more than cosmetic cost. Recorded input 3 (plan line 199) tells a Phase 5 implementer that removing the two identity fields from `halo_properties.yaml` "will otherwise fail to compile" the fixture unit test *at those lines*; an implementer who checks `:1071-1072`, finds an unrelated test, and concludes the coupling is gone would ship a broken slice. And plan line 216 uses `Makefile:779`/`:795` as the evidence that `make tests-scientific` builds one pair at a time — the premise for requiring a bespoke eight-run gate harness. That evidence no longer points at the target.

**Soft drift — the range still overlaps the right code, but no longer starts on it (2):**

| Plan line | Citation | State at `HEAD` | Tightened value |
|---|---|---|---|
| 185 | `read_parameter_file.c:583-602` | now spans a blank line, `unit_label_h_convention()` (584-589), and the first two-thirds of `convert_unit_scalar()` (591-602 of 591-611) | **`:591-611`** |
| 185 | `read_parameter_file.c:963-965` | now the `box_size` node lookup; the `BoxSize` assignment sits one line past the range at `:966` and `PartMass` nine past at `:972-974` | **`:964-974`** |

These two still land a reader in the right function and would not mislead anyone; they are listed for completeness and to be fixed in the same edit, not as independent defects. Note one nuance on the second, found by the external panel: even at `6365d882` the range `:963-965` covered only the `PartMass` assignment, while `BoxSize` sat at `:956-958` — so the plan's citation never quite matched its own "`BoxSize` and `PartMass`" wording. The widened `:964-974` correction covers both.

**Two further items in the same document, same class:**

- **Stale factual assertion.** The Definition of Done's allowed-delta 3 (`MIMIC-DUAL-DRIVER-PLAN.md:252`) states in the present tense that `src/core/core_properties.yaml:79` "describes the field as `creation_halonr + 10^9 * forestnr_global`". W1 fixed that on this branch; the file now reads `creation_halonr + 10^9 * (forestnr_global + 1)`. The paragraph's own scope note already anticipates this ("Phase 5 should find the `+ 1` already correct"), so the *instruction* survives — but the quoted text no longer matches the codebase and should be restated as done.
- **Pre-existing off-by-four**, not caused by this work: recorded input 1 cites `src/io/output/util.c:63` as `prepare_halo_for_output`'s "single entry point"; line 63 is a doc-comment line and the function begins at **`:67`**. `util.c` was not touched by this branch, so this is older drift — worth correcting while the same paragraph is open.

**Fix.** Six line-number corrections, one tense correction, and one pre-existing off-by-four in `MIMIC-DUAL-DRIVER-PLAN.md`, using the values above. No code change.

**Why it is P2 and not P3.** The prior review rated a single stale `run_tests.sh` reference as a finding (its finding 8) and set the standard explicitly: Phase 5 planning may begin only when "the plan Phase 5 will be written from contains no known-stale reference". By that standard this is a regression against the report's own Definition of Ready, in the load-bearing document, introduced after the report was written.

### F2 — [P2] `HaloRankInForest` is scanned with no lower bound, so a negative rank passes `open_run`

`src/io/snapshot/read_snapshot_hdf5.c:999-1000` scans the rank column with bounds `(INT64_MIN, INT64_MAX)`:

```c
const int64_t file_max_rank = snapshot_h5_scan_i64_max(file, path, "HaloRankInForest",
                                                       header.n_halos, INT64_MIN, INT64_MAX);
```

Its sibling identity column is bounded per value four lines later (`:1004-1005`, `[0, n_forests_total - 1]`). The only other constraint on rank is the run-scoped equality check at `:1026` — `measured_max_halo_rank == max_halo_rank_in_forest` — which constrains the **maximum** only. A file containing, say, `HaloRankInForest = -5` at one halo therefore passes validation completely, provided some other halo still carries the declared maximum.

**Why it matters.** Frozen-spec invariant 4 requires `HaloRankInForest` to be dense over `[0, forest halo count)`, and the Galaxy Identity Encoding section makes it a direct term of `UniqueGalaxyID = HaloRankInForest + multiplier × (ForestIndex + 1)`. A negative rank yields an ID below its forest's base — silently wrong identity, which is precisely the class of failure the reader's open-time validation exists to prevent and precisely what Phase 5's cross-format identity gate is built to detect. Today the gate would catch it as a mysterious scientific divergence rather than the validator catching it as a named corrupt-input abort.

**Reachability.** A malformed or non-conforming input file — the reader's entire stated job is to reject those and abort (`SNAPSHOT-HDF5-FORMAT.md`: "Producers and consumers abort on violation; nothing repairs"). Not reachable from Mimic's own converter, which asserts the invariant. Rated P2 rather than P1 because it requires a third-party or corrupted producer and causes wrong data rather than a crash.

**Fix — and one thing not to do.** Change the lower bound only:

```c
snapshot_h5_scan_i64_max(file, path, "HaloRankInForest", header.n_halos, 0, INT64_MAX);
```

Do **not** also tighten the upper bound to `max_halo_rank_in_forest`. That looks like the symmetric completion of the fix, but it breaks a passing test — verified directly against the committed fixture, whose true ranks are `{0,1,2,3,4,5,6}` with a declared `max_halo_rank_in_forest` of 6:

- `corrupt_max_rank_too_small` (`test_unit_snapshot_reader_open.c:786`) stamps `max_halo_rank_in_forest = 2` on every file, and the case at `:865-866` asserts the needle `"declares max_halo_rank_in_forest 2 but the measured maximum"`. With an upper bound of `max_halo_rank_in_forest`, the per-value scan would abort first at `snapshot_003.h5` (rank 6, outside `[0, 2]`) with a *different* message, and the needle would never appear.
- Its sibling `corrupt_max_rank_too_large` (`:782`, header 9, case at `:863-864`) is unaffected, since 6 lies inside `[0, 9]` — which is exactly why the asymmetry is easy to miss by inspecting only one of the pair.

The lower bound alone is free: no existing corrupt case sets a negative rank, so no test changes and no diagnostic moves.

**The regression case is required, not optional.** A matching corrupt case (`set_i64_element(path, "HaloRankInForest", 0, -1)`, needle `"'/halos/HaloRankInForest' is -1 at halo 0"`) is a five-line addition mirroring the negative-`ForestIndex` case added in `166fbfa4` (`test_unit_snapshot_reader_open.c:797-801`, table entry `:871-872`). An earlier revision of this report called it optional; the external panel argued that down and is right. Neither existing rank case would catch a regression of the new bound — `corrupt_max_rank_too_small` and `corrupt_max_rank_too_large` both exercise the *measured-maximum* comparison, which a negative value leaves untouched. This new case is the only test that would pin the branch, which is exactly the standard W4 applied when it closed the same gap on the `ForestIndex` side.

### F3 — [P3] `tests-converter` and `check-snapshot-fixture` gate local runs only, not CI

`c7d25df2` wired both new checks into the `make tests` aggregate (`Makefile:751` and `:753`, with the targets themselves at `:783` and `:795`), which closes W5 item 5 and W6 for a developer running the full suite. But `.github/workflows/ci.yml` invokes the tiers individually (`make tests-unit`, `tests-integration`, `tests-scientific` at `:55`, `:59`, `:70`) and never `make tests`, so neither new check runs on any pull request. The committed snapshot fixture and the 327-test converter suite — which Phase 5's identity gate depends on for correctness — remain ungated on merge.

This is already recorded as a follow-up in the prior report ("mirror `tests-converter` and `check-snapshot-fixture` in CI so they gate merges"). It is restated here only because W5/W6 are listed as complete in the Definition of Ready while half their value is missing, and the fix is two steps in `ci.yml`. Note the venv coupling: `CONVERTER_PYTHON` prefers `mimic_venv` unconditionally, so a CI step must ensure the venv exists or accept the system interpreter.

---

## Simplification Pass

The reader is already at its right size: 1,177 lines for a format that validates an object set, twelve typed header attributes, sixteen typed datasets, three data invariants, and five link fields across three link domains. The contract tables carry their weight, the comments state constraints the code cannot show rather than restating it, and the diagnostics are bounded by design.

**Conclusion of this pass: change nothing before Phase 5.** Four micro-cleanups were considered and are recorded below. None is a defect, none affects behaviour, and **none should be done as pre-Phase-5 work**.

**The panel split on them, which is itself the answer.** Codex judged all four not worth doing; OpenCode judged 1, 3 and 4 worth doing and agreed only on dropping 2. Two high-effort reviewers reaching opposite conclusions on the same four items is the signature of changes that are genuinely marginal — and marginal cleanups do not belong in a gate whose purpose is to make Phase 5 safe to start. Each saves two to six lines in a validated reader whose byte-level output identity is the next phase's acceptance criterion, so each would have to be justified against a standalone commit plus a binary-record comparison. That trade does not clear *now*.

Adjudication: leave all four. If someone opens `read_snapshot_hdf5.c` or the fixture tool for an unrelated reason later, item 4 has the strongest independent case — OpenCode's objection is not cosmetic but a maintainability hazard (the load-bearing "manifest last" property is encoded as a stable-sort boolean key a future editor could silently break), and Codex's counter is only that the docstring names it. Items 1 and 3 are pure taste; item 2 both reviewers reject.

**The four, with the argument on each side:**

1. **Drop the alias pair.** `read_snapshot_hdf5.c:69-70` defines `SNAPSHOT_HDF5_EMPTY_N_FORESTS`/`SNAPSHOT_HDF5_EMPTY_MAX_RANK` as exact aliases of `SNAPSHOT_EMPTY_N_FORESTS`/`SNAPSHOT_EMPTY_MAX_RANK` from `reader.h`, used four times in this file, while `interface.c:108-109` already uses the shared names directly.
   *Codex (against):* the local names document the format-to-interface contract mapping at the point of use, for two lines. *OpenCode (for):* they are exact aliases and `interface.c` already uses the shared names, so they buy only a naming hop. **Split — taste; leave it.**
2. **Fold the duplicated `/halos/<name>` construction.** `:530-533` and `:634-637` are the same four lines — `snprintf` into `char full_name[MAX_STRING_LEN]` plus the identical overflow check — in `snapshot_h5_open_scan()` and `snapshot_h5_read_column()`.
   *Both reviewers against:* two call sites is below the threshold where a helper pays for its indirection, and both sites keep their own `full_name` buffer for later error text, so the helper would be net churn. **Agreed — do not do this one.**
3. **Pass the path directly to the attribute-iteration callback.** `struct snapshot_h5_attr_scan` (`:253-256`) wraps a single `const char *path` to satisfy `H5Aiterate2`'s `void *op_data`.
   *Codex (against):* the typed wrapper preserves `const` intent across the `void *` boundary and is the natural extension point for a second field. *OpenCode (for, "lowest value of the four"):* a one-member struct touched at three points is a real if small abstraction to remove. **Split — taste; leave it.**
4. **Make the fixture installer's ordering explicit.** `create_snapshot_fixture.py:664` encodes the load-bearing "manifest last" property as a boolean sort key: `sorted(staged_dir.iterdir(), key=lambda p: p.name == manifest_name)`.
   *Codex (against):* the invariant is already named in the docstring (`:629-640`), so the trick is documented rather than hidden. *OpenCode (for):* it is a correctness-critical trick a future editor can break silently — the strongest of the four, and the only one whose argument is a failure mode rather than taste. **Split — the best candidate if this file is opened later; still not pre-Phase-5 work.**

**Larger consolidations — three refused outright, one deferred. Reasons recorded so these are not rediscovered as "obvious" cleanups:**

- **Do not unify `snapshot_h5_validate_object_set()` and `snapshot_h5_validate_halo_datasets()`.** They share a ~20-line "enumerate group links, reject names outside an expected set" skeleton, but their diagnostics differ meaningfully (root objects name the two required groups; dataset members name the count and the format version), and the dataset version continues into dtype/rank/shape checking. A shared helper would need enough parameterisation to cost more than the duplication saves, in a validator whose output is a frozen contract.
- **Do not unify the two block-scan loops.** `snapshot_h5_scan_snapnum()` and `snapshot_h5_scan_i64_max()` share the offset/remaining/count arithmetic but read different types into different static buffers and do different per-element work. Unifying requires a callback per element or a type switch in the inner loop — more indirection on the one path that is genuinely hot at open time.
- **Do not add `_Static_assert(NDIM == 3, …)`** to match the five link-width asserts. Those guard against a *package* widening a field via `halo_properties.yaml`, which is a real and reachable edit; `NDIM` is a universal constant in `constants.h` that no package can change. The assert would add a line and pin nothing that can move.
- **Defer the "snapshot list shorter than the dataset" question to Phase 5** — downgraded from "recommended against" after both reviewers objected, on two separate grounds.
  *The rationale was wrong.* An earlier revision claimed such a truncation *always* aborts loudly at `load_slab`, because the final configured snapshot's `Descendant` links fall outside the empty next-snapshot domain (`snapshot_h5_link_limit()`, `:790-805`). Both reviewers independently showed that is false: `Descendant == -1` is explicitly accepted (`:840-842`, `allow_null_link = 1`), so a truncated prefix whose last loaded slab holds only null descendants passes silently. A real truncation usually aborts, since most halos below the final snapshot have genuine descendants — but "guaranteed" and "unmissable" were wrong and are withdrawn.
  *The framing was wrong too.* OpenCode's point stands: an oblique downstream abort for a misconfiguration is the same inconsistency F2 objects to, so "recommended against" overstates the case. The honest position is **defer, not refuse** — the frozen spec's reader obligations (`SNAPSHOT-HDF5-FORMAT.md:132-136`) do not require this check, and `load_slab` has no caller until Phase 5 wires it, so Phase 5 is where the question should be decided with a real driver in hand. It is *not* pre-Phase-5 work either way.

---

## What Was Checked and Found Sound

Recorded so the panel and the Phase 5 planner know these dimensions were exercised, not assumed.

- **W1 (identity formula) is complete and correct.** All eight locations now carry `(forestnr_global + 1)`: the YAML source, three committed schema sidecars, and — verified by reading the strings inside the binary artifacts, not by a source grep — the `FieldMetadata` tables embedded in both tracked HDF5 baselines. No tracked copy of the old formula survives outside documents that quote it deliberately.
- **W3 did not weaken the tests it made package-agnostic.** `expected_default_target_file_size()` (`test_parameter_parsing.c:1034-1065`) is an *independent* textual re-derivation of the parser's precedence, not a read-back of the parser's own result, so the assertion is not tautological. `write_simulation_output_fixture()`'s substituted target moved from 4096 MB to 5000 MB specifically so a value that failed to apply is now distinguishable from the global fallback — a strictly stronger assertion than before.
- **The new integration skips cannot silently disable coverage.** `effective_input_setting()` (`test_processing_order.py:95-115`) guards both `TestSkipped` raises on a specific, checkable package property, and returns `None` (i.e. does not skip) under the default pair, so both tests run there. The skips fire only under the fixture pair, where the package's own configuration contradicts the test's premise.
- **The abort-path claims hold.** A snapshot-ordered configuration still stops at exactly one place. `store_run_properties()` — which holds the unguarded `MimicConfig.reader->name` at `metadata_hdf5.c:582` — is reached only from the master-file write *after* `run_processing_driver()` (`main.c:413`), which `FATAL_ERROR`s first (`tree_driver.c:531-532`). No unguarded dereference is reachable before the abort. `myexit()` calls `exit()` directly with no leak report, so `open_run`'s `halo_counts` allocation leaking on a validation abort is inert, not a diagnostic false positive.
- **The numerical and byte-order handling is right.** Reads go through native memory types with HDF5 performing conversion, dtypes are matched by class/size/signedness rather than `H5Tequal` against a native type, the identity bound is expressed as a division so it cannot overflow while being checked, and `dims[1]` is compared in `hsize_t` so a second dimension of `2^32 + 3` cannot truncate to 3. `snapshot_h5_validate_halo_datasets()` checks rank *before* filling a two-element `dims[]`, so the extent read cannot overflow.
- **The Makefile worktree fix is coherent.** The rewritten prerequisite (`Makefile:328`, resolving `GIT_DIR` at `:310`) leaves no `.git` literal anywhere in the file; the `$(wildcard $(GIT_DIR)/…)` form collapses to no prerequisites for an exported tarball, where the recipe's existing `|| echo 'unknown'` fallback already degrades gracefully.

---

## Validation Performed

Run against `HEAD` (`69590cc6`), working tree clean before and after; raw logs captured per step.

| Check | Result |
|---|---|
| `make generate` + `make check-generated` (default pair) | exit 0 — 6/6 checks pass, **no generated-code drift** |
| `make -j8` (default pair) | exit 0 — **zero warnings** under `-Wall -Wextra -Wshadow -Wformat-security -Wundef` |
| `make tests summary` (default pair) | unit 43/43, integration and scientific tiers all pass; converter suite and fixture conformance both ran; final banner `ALL TESTS AND CHECKS PASSED` |
| Fixture pair: `generate`, build, `tests/unit/run_tests.sh` (`halos-only` / `micro-uchuu-snapshot`) | exit 0 — 23/23 pass, no leak diagnostic |
| `make check-format`, `make check-docs` | exit 0, exit 0 — 152 files unchanged; all internal links/anchors resolve |
| `make USE-HDF5=no -j8` | exit 0, zero warnings; link line correctly omits every `*hdf5.o` and `-lhdf5*` |
| Default build restored, `git status --short` | clean; no tracked file modified |

Only non-pass markers were the two pre-existing expected skips (`test_unknown_module_error` — process isolation; `test_with_infall_module` — module absent from `halos-only`) and one pre-existing `WARN test_zero_values`.

**Evidence caveat, stated rather than papered over:** the `make tests summary` step's exit code was inferred from the recipe rather than captured. The inference is sound and checkable — `Makefile:772-775` exits 1 if and only if `build/.test_failures` exists, and the observed run printed the `else`-branch success banner — but it is an inference, not a recorded `$?`.

**Not run:** the opt-in real-data reader test against the full 50-snapshot micro-Uchuu dataset (requires the external volume); MPI builds; Linux/GCC (macOS Clang only).

---

## Recommendation / Next Action

Ordered, and all small:

**F1 and F2 together gate Phase 5 planning; nothing else here does.** That is also the external panel's conclusion: *"Once F1 is corrected and F2 lands with the negative-`HaloRankInForest` regression case, the repository is ready for Phase 5 planning from the plan's Phase 5 section."*

1. **Fix F1** — correct the four hard-broken references, the two drifted ranges, the stale allowed-delta-3 quote, and the pre-existing `util.c:63` off-by-four in `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md`, using the values in the tables above. Gating, because that section is the planner's sole input.
2. **Fix F2** — change one bound constant at `read_snapshot_hdf5.c:1000`, **and** add the negative-rank corrupt case that pins the branch. Do not tighten the upper bound. Gating, because it is the one code defect and the branch is otherwise untested.
3. **Change nothing for simplification.** All four candidates were considered and left; see the Simplification Pass.
4. **Optionally close F3** by adding both checks to `ci.yml`. Not gating.
5. Re-run the fixture-pair unit tier and `make check-docs`; then Phase 5 planning may begin.

**On the prior report's Definition of Ready:** every clause is satisfied except "W7's corrections are in (so the plan Phase 5 will be written from contains no known-stale reference)" — which F1 shows is now false again, through no fault of W7's execution. Fixing F1 restores it.

---

## Risks / Unknowns

- **Confirmed:** `snapshot_h5_fill_halos()` (`read_snapshot_hdf5.c:719-721`) allocates two staging buffers sized `8 × n_halos` and `8 × n_halos × NDIM` on top of the slab itself — 32 bytes per halo of transient overhead, so ≈10 GB at the projected 315M-halo Shin-Uchuu slab, against that slab's own ≈32.8 GB. Harmless at micro-Uchuu scale and correctly outside Phase 5's scope. It is not among item 6's *explicit* figures (104/176/176/264 B and the 32.8 GB second-slab number), though item 6's fallback trigger does tell the Shin-Uchuu step to recount "output and HDF5 buffers", which arguably covers it. Flagged so the recount treats it as a named term rather than rediscovering a 10 GB transient under time pressure.
- **Assumption:** that the panel-verified claims in the prior report remain true for the parts of the diff this pass did not independently re-derive (chiefly the converter suite internals and the fixture regeneration byte-identity, both of which were re-run rather than re-read during that report's implementation).
- **Unverified:** Linux/GCC compilation of the five new `_Static_assert`s, which are the codebase's first and rely on the compilers' default C11+ mode with no `-std=` pinned. Recorded as an accepted residual risk in the prior report; safe for the supported targets, but macOS Clang is the only compiler this pass exercised.
- **Not a risk, recorded to prevent re-litigation:** the `ProcessingOrderConfigured` file-scope static, the `GIT_DIR` path-with-spaces limitation, the `expect_fatal` exactly-16383-byte false positive, and `CONVERTER_PYTHON`'s unconditional venv preference were all reviewed and are all correctly characterised as accepted residual risks in the prior report. None warrants action now.

---

## Implementation Record

All three findings implemented 2026-08-10, on `feature/ctrees-snapshot-reader`, after the external panel round that produced this report's final form.

**F1 — eight corrections in `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md`.** The four hard breaks, the two drifted ranges, the stale allowed-delta-3 quote (restated as done, naming commit `77ab8462`), and the pre-existing `util.c:63` → `:67`.

**A second-order defect the post-implementation panel caught, worth recording because the class will recur.** F1's replacement line numbers were computed *before* F2 edited the same two files, and the two changes were implemented in parallel. F2's one added comment line pushed the `ForestIndex` literal down, and its eight added test lines pushed the identity checks down, so two freshly-corrected citations were stale again the moment they landed: `read_snapshot_hdf5.c:996-1005` → **`:996-1006`**, and `test_unit_snapshot_reader_open.c:1213-1214` → **`:1221-1222`**. Both corrected. **Lesson for Phase 5, which will edit cited files constantly: line-number citations must be re-derived after the last edit to the cited file, never before.** A mechanical sweep of all 124 line-bearing citations across the five changed dev documents now confirms none is out of range.

**F2 — `src/io/snapshot/read_snapshot_hdf5.c` and the fixture unit test.** The rank scan's lower bound is now `0`; the upper bound remains `INT64_MAX` deliberately, with a one-line comment stating why (the run-scoped measured-maximum check owns that end). One corrupt-fixture case (`corrupt_halo_rank_negative`, snapshot 4, needle `"'/halos/HaloRankInForest' is -1 at halo 0"`) pins the branch. **Negative control performed:** the needle was deliberately broken, the case was shown to fail with the reader's real diagnostic, and the needle was then restored byte-for-byte and the tier shown green again — so the case is proven to exercise the branch rather than pass vacuously.

**F3 — `.github/workflows/ci.yml`.** Two steps, `make tests-converter` and `make check-snapshot-fixture`, placed after module validation and before the test tiers. CI needs no venv: `CONVERTER_PYTHON` falls back to `python3`, and `requirements.txt` supplies numpy, h5py, PyYAML and pandas.

**Simplification pass — nothing changed**, as adjudicated. None of the four micro-cleanups and none of the four larger consolidations was implemented.

### Documentation currency sweep (added 2026-08-10, same session)

A separate check of `docs/dev/` for staleness ahead of Phase 5 found four gaps, all fixed:

- **The pathway did not record this review at all.** `MIMIC-DEVELOPMENT-PATHWAY.md` gained rows for this report and the Phase 4b report in its Active Plans table, plus a summary of F1–F3 under sequence item 4.
- **`MIMIC-SNAPSHOT-READER-PLAN.md` contradicted a settled Phase 5 decision in two places** (`:57` and its Deferred list), both promising a `populate_halo_payload_from_snapshot.inc` sibling that `MIMIC-DUAL-DRIVER-PLAN.md:155` records as superseded — there is one shared view-based populator. Both now record the supersession. This mattered because the pathway names *both* documents as Phase 5 inputs.
- **`MIMIC-SNAPSHOT-READER-PLAN.md:200` cited `read_parameter_file.c:932-936`**, now unrelated code; corrected to `:972-974`. Same defect class as F1, in the other Phase 5 input.
- **A Phase 5 decision was delegated to a plan that never received it.** `MIMIC-DUAL-DRIVER-PLAN.md:181` assigns the snapshot-driver memory-projection fallback trigger to `SHIN-UCHUU-CONVERSION-PLAN.md`; that plan had no trace of it. It is now recorded there under *Relation to the Dual-Driver Plan*, including the 85%-of-RAM threshold, the `{Len, NextProgenitor}` projection, the measured struct sizes, and the reader's 32-bytes-per-halo transient staging buffers this report flagged — so the production recount has every term named.

### Validation at implementation

Default pair (`sage16`/`mini-millennium`) unless stated; literal captured exit codes.

| Check | Result |
|---|---|
| `make generate`, `make check-generated` | EXIT=0 — no drift |
| `make -j8` | EXIT=0, **0 warnings** |
| `make tests summary` | **EXIT=0** — unit 43/43, integration and scientific tiers pass, converter suite and fixture conformance both run, banner `ALL TESTS AND CHECKS PASSED` |
| Fixture pair `generate` + build + `tests/unit/run_tests.sh` | EXIT=0, 0 warnings, all unit tests pass |
| `make tests-converter` standalone (as CI invokes it) | EXIT=0 — `Ran 327 tests / OK` |
| `make check-snapshot-fixture` standalone | EXIT=0 — `conformance: PASS` |
| `./scripts/beautify.sh`, `make check-format`, `make check-docs` | EXIT=0, EXIT=0, EXIT=0 |
| All 124 line-bearing citations in the five changed dev docs | 0 out of range |

Unchanged from the pre-implementation run: the only non-pass markers are the two pre-existing expected skips and one pre-existing `WARN test_zero_values`. **Not re-run:** the opt-in real-data reader test (needs the external volume); MPI; Linux/GCC.
