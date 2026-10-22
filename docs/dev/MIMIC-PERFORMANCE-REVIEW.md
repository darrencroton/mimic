# Mimic Performance Plan (Step 4, Tier A): Post-Run Code Review

**Purpose:** The owner's post-run review of `feature/performance-tier-a` (26 commits, `faa2228b..c9c6d0e0`): did the work meet the plan's goals, is the code correct and of the quality the project requires, and what should happen to the issues, decisions and follow-ups the PM recorded. Reviewed 2026-10-22 against the frozen plan ([`MIMIC-PERFORMANCE-IMPLEMENTATION-PLAN.md`](MIMIC-PERFORMANCE-IMPLEMENTATION-PLAN.md), revision 6), the acceptance record ([`MIMIC-PERFORMANCE-ACCEPTANCE.md`](MIMIC-PERFORMANCE-ACCEPTANCE.md)), the two PM run reports (`.git/pm/20261021T023658Z-d8663a/run-report.md`, `.git/pm/20261021T111420Z-d6f529/run-report.md`) and the root `HANDOFF.md`. Method: the `code-review` skill (risk-driven, line-anchored findings) with a secondary `code-simplifier` pass now that the end state, not the plan's pre-implementation choices, is what matters, and a differential `code-health` run against `main`. The long benchmarks were not repeated; the figures below are the record's, re-read against the code and the raw archives where that was cheap.

---

## Executive summary

**Verdict: PASS WITH RISKS. The branch is correct, achieves the plan's goals with margin, and is ready to merge once three stale documents are corrected.** No P0 or P1 finding. The runtime C changes (a level-first `DEBUG_LOG`, init-time per-phase dispatch plans, a shared output-buffer seed with the vertical zero-fill removed) are small, well-bounded, contract-preserving and byte-gated; the build and the full test suite pass at HEAD on this machine; the default binary runs both drivers and reports the recorded memory profile.

**Goals.** The plan promised about 10% (vertical) and 8% (horizontal) on mini-Millennium. The closing interleaved composite is 11.78% and 10.28% (n = 40 per build per driver, 46 and 62 standard errors), the full mini-Uchuu box 11.19% and 12.50%, and the Shin-Uchuu super-forest's vertical peak fell from 164.8 GB to 70.96 GB. Every one of the eight byte-gate legs passed at every code commit. Six build-flag sets were measured and none adopted, by the plan's own rule. The one planned slice that found nothing (Slice 5) closed as a documented no-go, correctly.

**What needs doing before or at merge (ordered):**

1. **P2, docs.** Three committed documents outside every slice's surface still state the removed five-fold vertical seed and a 165 GB requirement for `sage16` on the Shin-Uchuu super-forest: `simulations/shin-uchuu-lhalo/README.md:110`, `.agents/skills/mimic-simulations-and-readers/SKILL.md:52` and `.agents/skills/mimic-docs-and-writing/SKILL.md:74`. A user sizing a machine from the README would over-provision by 2.3× and be told a seed rule that no longer exists. One small docs commit; the figures to use are in Finding 1.
2. **P2, test-infrastructure robustness (follow-up slice, not a merge blocker).** `release_phase_plans()` and the pre-existing `free_phase_event_buffer()` free raw pointers that a unit test's `init_memory_system()` reset has forgotten, turning one failing assertion into a process-wide abort. Unreachable in `./mimic`. Finding 2 gives the fix and the gates it needs.
3. **P2, the Python gate (follow-up, not a merge blocker).** `assert_own_worktree` accepts a symlinked cache entry, and from there the gate runs `git checkout --detach`, `make clean` and an `output`-link repoint against whatever checkout the symlink names (Finding 3). Reachable only by planting that symlink, but the consequence is a user's build and branch state. The same follow-up should take the gate's smaller gaps: the per-ID comparison does not check row order inside a partition (the plan specified per-ID identity, and every claim is worded that way, but the Makefile help says "byte-identical"), created rows are uncompared in default comparator mode (latent until a model that creates records joins the legs), the flag stamp duplicates the Makefile's own mode markers, and the new helpers have no committed tests. Nothing found is a false-PASS path for the eight legs as the plan ran them.
4. **Process.** Archive the completed plan, acceptance record and reference document per the `docs/dev/` rule when the owner is ready, and take Decision 12 (the pathway's next step), which the plan deliberately left open.

The PM's delegated plan decisions D1 to D4 are each the natural reading of a defective clause and are endorsed below. The spectrum's remaining Tier A items were not taken for measured reasons that hold up.

---

## Authorization status

- **Drift audits.** Every code slice had a read-only drift audit (claude-sonnet-5-5) at its final commit, all PASS; the PM's seven floor facts were 7/7 at every acceptance, with the one Slice 4 round-2 incident (the auditor detached HEAD in the main checkout, disclosed, re-attached by PM, rated 0) changing no file, index or ref. This review takes the authorization check as done and does not re-audit surfaces.
- **Notes.** `git diff --stat main..HEAD`: 29 files, +2,610 −640. Working tree clean at `c9c6d0e0`; `main` is at `faa2228b`, the merge base. Nothing is pushed.

---

## Goal attainment against the plan

| Plan promise (Outcome and Limits) | Delivered | Evidence read |
|---|---|---|
| A reusable byte gate against any ancestor commit | `make tests-reference-identity REFERENCE_COMMIT=<hash>`, eight legs, fails never skips; green at every code commit | Makefile target; `tests/manual/test_reference_identity.py`; the record's per-slice gate lines |
| `DEBUG_LOG` free when debug output is off | Inline level test over exported state, no call and no argument evaluation on the disabled path; four-scenario unit test | `src/util/error.h:77-114`, `error.c`, `tests/unit/test_debug_log_gate.c` |
| Dispatch skips the empty galaxy loop and the no-pending safety dispatch, through per-mode lists if they measure better | Init-time plans committed by the measured rule (plan beats guards by 7.0 and 11.1 SE); no `processing_mode` test remains in `execute_phase` or `dispatch_events_range` | `src/core/module_registry.c:773-885,1647-1725`; record, Slice 4 |
| Vertical output buffer seeded like the horizontal one, not zero-filled | `output_buffer_seed_capacity()` shared `static inline`; `MAXHALOFAC` gone; FoF seed pinned and shown equal for every `int` | `src/core/output_buffer.h:64-96`, `src/io/vertical/interface.c:164-191`, `constants.h` |
| Whichever build flags proved identical and faster | None did on both drivers; default stays `-g -O2`; all six passed the byte gate | record, Slice 8; `make info` |
| About 10% V / 8% H expected | 11.78% / 10.28% interleaved; 11.19% / 12.50% on mini-Uchuu | record, Slice 9; `MIMIC-PERFORMANCE-REFERENCE.md` after-section |
| Super-forest memory measured | 70.96 GB against 164.8 GB; `C` 24.4 GB against 116.3 GB | record, Slice 9 |
| Limits stated, not overclaimed | Changelog "Not claimed" bullet scopes identity to the mini-Millennium gate, names macOS arm64 Clang only, states the GCC FMA caveat | `CHANGELOG.md:34` |

Two honest residuals in the measurement record are not defects: the product of the per-slice ratios (14.3% vertical) exceeds the single-session composite by 2.5 points (the Slice 6 pair carried one slow before-repeat; sessions drift), and the vertical mini-Uchuu HEAD repeats rose monotonically (1891.6, 1925.9, 1955.8 s) with the cause unestablished. Both are recorded as measured and the binding figure is the interleaved one, which is the right call.

---

## Findings

Severity follows the `code-review` skill: `P0`/`P1` need a stated reachability path; findings reachable only by constructing a state no caller produces cap at `P2`.

### 1. [P2] Stale production memory guidance in three committed documents

- `simulations/shin-uchuu-lhalo/README.md:110`: "peaked at about 165 GB of resident memory for `sage16` (164.8 GB) ... Of the `sage16` peak, 116.27 GB is the output buffer, seeded at five times the forest's halos (631,901,630 records)."
- `.agents/skills/mimic-simulations-and-readers/SKILL.md:52`: "needs about 165 GB of resident memory for `sage16` ... 116.27 GB of the `sage16` figure being the output buffer seeded at five times the forest's halos."
- `.agents/skills/mimic-docs-and-writing/SKILL.md:74`: instructs writers to "quote the memory the largest forest needs (about 165 GB for `sage16`, 143 GB for `halos-only` ...)".

**Why it matters.** The README is the package's user-facing sizing guidance and it is now wrong by a factor of 2.3 for `sage16`; the two skills will propagate the wrong figure into every future document an assistant writes. The seed rule it describes (`MAXHALOFAC`, 5×) no longer exists in the code, so a reader cannot reconcile the README with `src/io/vertical/interface.c`. The CHANGELOG's Shin-Uchuu line was correctly updated (`CHANGELOG.md:14`), so the repository now contradicts itself.

**Fix.** One docs commit, no plan needed. Restate with the Slice 9 measurement: `sage16` on `shin-uchuu-lhalo` file 0 with `MallocLargeCache=0` peaks at 70.96 GB (70,955,646,976 B), wall 165.4 s, output buffer `C` 132,699,342 records (24.4 GB, the forest's halos plus 5%), `P` 121,907,647 unchanged; say the `halos-only` 143 GB figure was measured before the seed change and not re-measured. In the writing skill, replace the 165 GB instruction with the measured figure and the same `halos-only` caveat. Run `make check-docs`.

### 2. [P2, capped: unreachable in `./mimic`] Plan and event-buffer release do not survive a memory-system reset

- `src/core/module_registry.c:810-816` (`release_phase_plans`), `:940-941` (its call at the entry of `module_system_init()`), and the pre-existing `:1036-1044` (`free_phase_event_buffer`, called from `module_system_cleanup()`), both of which `myfree()` a pointer held in file-static state.

**Mechanism.** The unit tiers call `init_memory_system(0)` per test case (for example `tests/unit/test_event_buffer_growth.c:148,212,278`), which `calloc`s a fresh tracking table and forgets every earlier block. If a case fails an assertion before its `module_system_cleanup()` (every `TEST_ASSERT` returns early), the plan table and `phase_event_state.events` keep pointers the next table does not know. The next case's `module_system_init()` then calls `release_phase_plans()` → `myfree(stale)` → `FATAL_ERROR("Attempting to free untracked pointer")` (`src/util/memory.c:395`), and even without plans the next `ensure_phase_event_capacity()` would `myrealloc_cat` the stale event buffer and hit the same fatal. One red assertion becomes a binary-wide abort that loses every later result in that test binary.

**Reachability.** Only through a failing unit test of that shape; `./mimic` initialises the memory system once and never resets it, and on the passing path every case cleans up. The accepted record verified no leak report changed (14 vs 14). So this is a diagnosability hazard in the test tier, not a product defect, and the plan made "no reset with live plans" an explicit precondition (following the event buffer's precedent). The hazard is nevertheless real, pre-dates the branch for the event buffer, and the branch doubled its surface.

**Fix direction** (the plan is complete, so the "committed code equals measured code" rule no longer binds; the owner may still want the gates for the record). In `module_registry.c` only: have both release helpers free a pointer only when `validate_memory_block()` (`src/util/memory.c:593`) reports it tracked, and otherwise drop it with a `WARNING_LOG` naming what was abandoned. That is a linear scan over the block table, run once per init and once per cleanup, so it costs nothing on the hot path. Gates: `make tests-reference-identity REFERENCE_COMMIT=5daa24ee` (minutes) and `make tests`; a wall comparison is not scientifically necessary for a path that runs twice per process, but an interleaved pair is cheap if the record wants it. Two natural companions in the same file and gates: name the phase in the no-plan `FATAL_ERROR` (Finding 6) and the contract-neutral `workspace_callback_count()` hoist (Simplification 3).

### 3. [P2] The Python identity gate: a planted symlink under the cache root reaches destructive operations on another checkout

- `tests/framework/parity_gate.py:1127-1145` (`assert_own_worktree`), `:1181` (`git checkout --detach`), `:1163-1186` (`head_worktree`), `:746-755` (`point_output_at`); `tests/manual/test_reference_identity.py:265-282`, `:402`.

**Mechanism** (the PM's Slice 1 residual, confirmed and found wider than recorded). `assert_own_worktree` accepts a path whose `--show-toplevel` equals `path.resolve()` and whose common directory is the main repository's `.git`. A `head__<model>__<simulation>` entry under `output/snapshot-global-identity/worktrees/` that is a symlink to the main checkout, or to any other linked worktree a user has, passes that test. The gate then runs `git checkout --detach` in that checkout (detaching its branch), `make clean` there when the flag stamp is absent (deleting the user's `build/` and `mimic`), and `point_output_at` unlinks that checkout's `output` link and repoints it into the gate's archive, which is where `WORKTREE_ROOT` itself lives. `cached_worktree` (`:1095-1099`) has no ownership check at all.

**Reachability.** Only by someone placing that symlink, so `P2` by the skill's rule; but the consequence when reached is the loss of a user's build products and a detached branch in their working checkout, and the Slice 4 incident shows that a tool detaching the main checkout does happen in practice. **Fix:** refuse `path.is_symlink()`; require `git rev-parse --absolute-git-dir` to differ from the common directory; require the path to appear in `git worktree list --porcelain` exactly as given, under `WORKTREE_ROOT.resolve()`; and call the same check from `cached_worktree`. Fold in the three Slice 1 round-3 `P3`s (docstring wording, untracked check before checkout, `-z` splitting of `ls-files` output).

**Also in the gate, from the independent Python review** (each verified against the code by this reviewer where marked):

- **[P3, verified] Row order inside a partition is not compared.** `compare_hdf5_file` skips the byte comparison for `Galaxies` datasets (`parity_gate.py:1407`, "compared per ID") and `compare_galaxies` pairs records by `UniqueGalaxyID`. That is exactly what the plan specified (Decision 5: "per-`UniqueGalaxyID` byte identity"), and every claim in the acceptance record and changelog is worded that way; but the Makefile help text ("Require byte-identical galaxy output", `Makefile:509`) overstates it. Both sides of a leg use the same driver and layout, so requiring `canonical_bytes(left[()]) == canonical_bytes(right[()])` on `Galaxies` as well costs nothing and would catch a marshalling-order change. Recommended: add the strict check behind a parameter the battery leaves off, and leave the help text as it is; otherwise reword the help text.
- **[P3, latent] Created rows (`UniqueGalaxyID < 0`) are not compared in default comparator mode** (`parity_gate.py:1456`); the comparator notes them and `captured` discards the note when there are no failures. Harmless for `halos-only` and `sage16`, which create no records, but a false-PASS path the moment `MODELS` gains `hod`. Pass the comparator's `--compare-created` matching, which is documented for same-driver pairs, and count only compared rows.
- **[P3] Two concurrent runs corrupt each other.** The HEAD worktree, its flag stamp and its `output` link are shared by every invocation and nothing locks them; two gates in flight, or a gate and the battery (which shares `WORKTREE_ROOT` and the `<commit12>__<model>__<simulation>` key shape), can build one run's binary with the other's flags and still record the first's flags in evidence. The project's standing rule is never to run two suites in parallel, so this is a precondition, not a defect; an `fcntl.flock` on `WORKTREE_ROOT/.lock` that refuses rather than waits would make it enforced.
- **[P3, verified] The flag stamp duplicates what the Makefile already guarantees.** `build/.last_compile_mode` records `CC` and `EXTRA_CFLAGS` and `build/.last_exec_mode` records `EXTRA_LDFLAGS` (`Makefile:347-382`, both `FORCE` prerequisites), so a flag change already rebuilds every object and relinks. The gate's own `FLAGS_STAMP` plus `make clean` (`test_reference_identity.py:265-282`) is therefore redundant, and it assumes the flags were applied rather than checking. Simpler and stronger: drop the stamp and after each build assert that the two marker files carry the requested `EXTRA_CFLAGS=`/`EXTRA_LDFLAGS=` lines (empty for the reference), and record both files in `evidence.json`. That also closes the unrecorded `CC`/`USE-HDF5`/`USE-MPI` provenance residual for free.
- **[P3] The battery still lets a caller's `EXTRA_CFLAGS` into its builds.** The stripping lives only in the gate's `build_env` (`test_reference_identity.py:245-256`); the shared `worktree_env` (`parity_gate.py:1062-1071`) strips `MAKEFLAGS` only. Moving the stripping into `worktree_env` is a one-line battery behaviour change the plan's non-goal protected only during the run.
- **[P3, verified] `point_output_at` leaves every worktree's `output` pointing into the last leg's archived run directory** (`parity_gate.py:746-755`), so a later manual `./mimic` in that worktree writes into archived evidence, and `link.unlink()` raises on a real `output/` directory, so a worktree where someone created one by hand fails every later run until fixed by hand. Repoint the link to a scratch directory at the end of `execute`, and handle a real directory with a clear error.
- **[P3] No committed tests cover the new helpers.** `tests/integration/test_parity_gate_helpers.py` is unchanged on the branch; `compare_outputs` is exercised only by the battery's mutation self-checks with the battery's hooks installed, and `head_worktree`, `assert_own_worktree` and the stamp logic are untested. The blocker is structural: `git()` binds `cwd=REPO_ROOT` at definition (`parity_gate.py:1019`), so these helpers can only operate on the real repository. Give them a `repo: Path = REPO_ROOT` parameter and the tests can `git init` a throwaway repository: identical synthetic runs pass, one flipped byte errors, a missing file or empty directory errors, a symlinked or `.git`-less worktree directory is refused.
- **[P3] Creation properties are not compared** (chunking, compression, fill value), which is acceptable for a galaxy-identity claim and worth one sentence in the docstring.

The battery refactor was checked function by function by the independent reviewer and preserves behaviour (the `RUNTIME_PATHS` tuple and `_tests/` exemption, `resolve_reference`'s order, the metadata comparison order, `OUTPUT_BASENAME` at every construction, `cached_worktree` unchanged apart from logging); `ruff --select F` and `py_compile` are clean on all four files. The legs are deterministic, every empty, missing or malformed output is an error, a leg is PASS only with no error, `check_provenance` ties each run to its commit, and subprocess calls use argument lists.

### 4. [P3] A dead accessor after Slice 3

- `src/util/error.c:214` `is_debug_log_rate_limiting_enabled()` has no caller left in `src/`, `models/` or `tests/` (the macro reads `mimic_debug_log_rate_limiting` directly). Keep it only if it is considered public API; otherwise remove it and its declaration in `error.h`. No behaviour change either way.

### 5. [P3] The benchmark script's other JSON string fields are still unescaped

- `scripts/benchmark_mimic.sh:700-707`: `json_escape` now covers `extra_cflags` and `extra_ldflags`, but `build_flags` (`MAKE_FLAGS`), `runtime_flags` (`MIMIC_FLAGS`) and `mimic_executable` are interpolated raw. A `MIMIC_FLAGS` value with a double quote produces unparsable JSON exactly as the fixed field did. Apply the same helper to the three fields; verify with `python3 -c 'import json,sys; json.load(open(sys.argv[1]))'` on an emitted file. Known residual from Slice 8, recorded here so it is not lost.

### 6. [P3] The no-plan `FATAL_ERROR` does not name the phase

- `src/core/module_registry.c:1653-1657`. `num_modules > 0` is guaranteed at that point, so `phase_config[0].module_name` is a safe, cheap identifier of the phase; today the message gives only a count. Fold into Finding 2's follow-up.

### 7. [P3] The `ProcessedHalos` allocation comment is longer than the style guide prefers

- `src/io/vertical/interface.c:182-189`: eight lines of inline comment carrying the load-bearing "no zero is read" argument. The content is right and should stay somewhere; the style guide prefers one or two focused lines inline with the longer argument at function level or in the Developer Guide, which already carries it (`docs/DEVELOPER-GUIDE.md:217`). Optional.

---

## Contract defects and the PM's delegated decisions

The frozen contract had four clauses the PM resolved under delegated authority. Each resolution is assessed here against the code and the record.

| Decision | The defective clause | PM's ruling | Assessment |
|---|---|---|---|
| **D1** (Slice 1, criterion 4) | "comparator ... in default mode requiring exit 0" while the same criterion says the comparison "runs ... once" and the moved helpers call the comparator in-process | Zero comparator failures through the unchanged functions satisfy it | **Endorsed.** `compare_galaxies()` (`tests/framework/parity_gate.py:1419`) calls the comparator's functions directly; there is no process to have an exit status. The recommended amendment ("with no comparator failure") is the honest wording. |
| **D2** (Slice 1, criterion 5 vs Design) | "a clean rebuild" versus "rebuilt incrementally on every run" | Clean when the flag stamp differs from the last build or is absent, incremental otherwise | **Endorsed.** This is the only reading under which a flag change cannot reuse stale objects and an unchanged HEAD build stays fast. Finding 3 covers whether the stamp logic is tested. |
| **D3** (Slice 1, "nothing is removed") | A flag change runs `make clean` in a HEAD worktree | Nothing user-owned is removed; a HEAD worktree's regenerable build products may be | **Endorsed.** The docs were qualified to match (`tests/README.md`, the validation skill row). |
| **D4** (Slice 6, criterion 6) | "< 0.5% to `__bzero` (the FoF-workspace zero-fill remains)" cannot be met inside a contract that keeps a 0.97% memset | The bound applies to the zero-fill the slice removes; total `__bzero` is recorded before and after | **Endorsed, and the root cause is in the plan's own frozen fact.** The plan attributed `__bzero` 2.14% with "`interface.c` pays 2.12 points" and treated that as the output-buffer memset alone; both memsets live in `interface.c`, so the fact conflated them. The amendment ("the profile attributes no `__bzero` to the `ProcessedHalos` allocation; total `__bzero` is recorded before and after") is right. |

**The plan defect that stopped the first run** (Slice 4, revision 5's per-call compaction regressing by 4.6 to 6.2%) was resolved by revision 6's measured two-candidate rule. That was the right resolution: the measured cause (1.07 to 1.17 galaxies per `execute_phase` call on every dataset, so a per-call pass is never amortised) is now in the plan's frozen facts, the committed init-time plan beat the guards by 7.0 and 11.1 SE, and the "committed code equals measured patch" discipline was enforced by PM comparison. The one tolerance the PM accepted (the no-plan `FATAL_ERROR` now precedes the empty-workspace return, so an uninitialised phase fails even with nothing to process) is stricter, not looser, and no caller in the tree hits it.

**Plan amendments (HANDOFF open item 5).** The plan is complete, so D1 to D4's recommended amendments matter only if the plan is revised or reused as a template. Recommendation: do not edit the frozen plan (its hash binds two closed PM runs); if the plan is archived, leave the acceptance record's recorded rulings as the authority. One amendment worth carrying into any future plan's measurement protocol, which Decision 14 already states: launch placement (repository root, binaries by absolute path) and interleaving.

---

## The PM's recorded issues, incidents and follow-ups: dispositions

From `HANDOFF.md` ("Residuals and follow-ups", "PM lessons") and the two run reports.

| Item | Source | Disposition |
|---|---|---|
| Stale super-forest memory statements in three out-of-surface files | Slice 6 fable P2, Slice 9 finding (3) | **Fix now** (Finding 1). |
| Phase-plan and event-buffer release not tolerant of a memory-system reset | Slice 4 fable P2 | **Follow-up slice**, small (Finding 2). |
| Name the phase in the no-plan `FATAL_ERROR` | Slice 4 fable P3 | **Fold into the same follow-up** (Finding 6). |
| Hoist `workspace_callback_count()` out of PASS 1's per-module loop (~1%) | Slice 5 follow-up | **Fold into the same follow-up**, with the care noted in Simplification 3. |
| Harden `assert_own_worktree` (require a linked worktree, refuse the main root); fold in three P3s | Slice 1 review-9 P2, review-8 P3s | **Follow-up, confirmed and widened** (Finding 3): the reachable consequences include `make clean` and an `output`-link repoint on the aliased checkout. |
| Committed tests for `head_worktree` and the flag-stamp clean logic | Slice 1 fable P2 | **Follow-up** (Finding 3): first give the worktree helpers a `repo` parameter so they can be tested on a throwaway repository; replace the stamp with an assertion on the Makefile's mode markers. |
| Designed interleaved comparison of `-O3` and `-fno-math-errno -O3` | Slice 8 follow-up | **Optional future measurement.** Both sets were within noise on one driver; the PM's refusal to re-roll until one crossed 2 SE was correct. If attempted, it is a one-day measurement with the existing gate and `s4-interleave.py`-style harness; the GCC `-ffp-contract=fast` caveat means any adoption needs a Linux identity check too. |
| Benchmark JSON's other string fields unescaped | Slice 8 residual | Finding 5. |
| Vertical mini-Uchuu HEAD repeats rose 1891.6 → 1955.8 s; horizontal peak RSS 55 to 75 GB with identical accounted terms | Slice 9 residual | **Accept as recorded.** Neither affects a claim the changelog makes; the RSS variance is the macOS allocator (the record says so). A later research-scale re-profile would settle both. |
| `CC`, `USE-HDF5`, `USE-MPI` reach both gate builds symmetrically, unrecorded | Slice 1 drift audit | **Accept; small improvement available.** Symmetry protects identity. Recording the three in the gate log (one line) would close the provenance gap at no risk; fold into the Finding 3 follow-up. |
| The gate's own logic files are outside `RUNTIME_PATHS`, so an uncommitted edit to them is not refused | Slice 1 fable residual | **Accept by design.** The gate compares two builds whose sources are committed; the gate's Python is the instrument, and refusing to run with an edited instrument would block developing it. Worth one sentence in the test's docstring. |
| `cached_worktree` / `assert_clean_checkout` check tracked edits only; `Tee` duplicated | Slice 1 residuals | The battery's helpers are protected by commit identity (accept); `Tee` is Simplification 5. |
| Supervised `TMPDIR` inside the repository fails two `test_parity_gate_helpers` cases | Slice 3 PM lesson | **Environmental; accept.** Worth one line in `tests/README.md` so the next supervised run does not re-diagnose it. |
| Reviewer ran `git checkout --detach` in the main checkout | Slice 4 incident | **Process.** No state was lost. The PM's lesson (check `git branch --show-current` after every review) is the right control; a stronger one is to give read-only reviewers a detached scratch worktree rather than the main checkout. |
| Token-file path exposed in a `ps` listing (not the token) | Slice 3 PM lesson | **Process.** Within the toolkit's stated same-user limit; the lesson is recorded. |
| `cp -i` / `ls`→`eza` aliases hung Developer and PM pipelines overnight | PM lessons | **Process.** Already fixed in the PM's practice (`command ls`, background tasks). The owner may set `DISABLE_AUTOUPDATER=1` for unattended runs, as the HANDOFF suggests. |
| 306 GB runaway Shin-Uchuu output from a mis-edited run file | First-run PM lesson | **Process, resolved.** Slice 9's run printed and checked the edited keys before starting, which is the right control and should be the standing rule for scripted run-file edits. |
| About 217 GB of run scratch in the macOS Trash | HANDOFF open item 7 | **Owner action:** empty the Trash when convenient; `git worktree list` already shows only the main checkout. |

---

## Simplification pass (end state, behaviour-preserving)

The `code-simplifier` skill was applied as a read-only pass; no edit was made. The C changes are already lean and the recommendations below are small.

1. **`src/util/error.c:214` dead accessor** (Finding 4). Remove `is_debug_log_rate_limiting_enabled()` and its declaration, or keep it as API and say so in `error.h`.
2. **`src/io/vertical/interface.c:165-168` unreachable `FATAL_ERROR`.** `InputTreeNHalos` is `int *` (`src/include/globals.h:137`), so `output_buffer_seed_capacity()` cannot return 0 for it; the message about `int64_t` overflow is dead. No change justified: the contract asked both callers to treat the overflow return uniformly, and the branch costs nothing. Noted so nobody tries to test it.
3. **`src/core/module_registry.c:1664` and `:1670`, `workspace_callback_count()` once for the event view and again per full-halo module.** The hoist the PM recorded (~1% by profile) is contract-neutral only if the count is recomputed after every `commit_created_records()` that appended rows, because `ws->count` can change between modules. The simplest correct form is one computation before PASS 1 and one after each commit, which keeps the "re-read per callback" wording of the contract true.
4. **`src/core/module_registry.c:773-885` plan construction.** Three `collect_phase_mode()` calls into one allocation is the plainest correct structure; a mode-indexed array of `(pointer, count)` pairs would save a few lines and read worse. No change justified. The `MAX_PHASE_PLANS` static table is bounded by the parser (`src/core/read_parameter_file.c:1322`), so its own `FATAL_ERROR` is defensive; keep.
5. **`Tee` duplicated** (`tests/manual/test_reference_identity.py:549`, `tests/manual/test_snapshot_disabled_identity.py:1110`): identical 23-line classes. Move one copy into `tests/framework/parity_gate.py` and import it from both tests. Behaviour-preserving; a one-line battery edit, which the plan's non-goal protected only for the run's duration.
6. **`src/util/error.h:96-114` macro nesting.** The two-level `if` inside `do { } while (0)` is the minimal shape that keeps the per-site statics scoped to the armed branch; collapsing it would move the statics out of the branch. No change justified.
7. **`scripts/benchmark_mimic.sh` JSON emission.** The heredoc-with-interpolation design is the root cause of Finding 5; a future cleanup could emit the result JSON through `python3 -c 'json.dump(...)'` once, escaping everything. Out of scope here.
8. **Python gate, from the independent pass** (all behaviour-preserving): the two manual tests' `main()` functions are nearly identical and could become one `run_manual_gate(stages, title, log_path)` in the framework beside `Tee`; `cached_worktree` and `head_worktree` repeat the create-when-missing branch (`parity_gate.py:1095-1099`, `:1163-1167`); `ReferenceIdentity.build` tests `tree == "reference"` twice in a row (`test_reference_identity.py:380-391`); `untracked_runtime_files` has one caller and can be inlined; `execute` can read its provenance attributes through the existing `decoded()` helper (`parity_gate.py:646`) and build `prefixes` as one set literal; and `FLAGS_STAMP` can go once the Makefile markers are asserted instead (Finding 3).

---

## Code-health (differential against `main`, `health.py analyze --base main`)

- **Coverage.** C, Python: composition, duplication, cyclomatic (Lizard 1.23.0 and the Python AST) and dependencies all covered; Make and Shell have no cyclomatic or dependency coverage, so the Makefile and `benchmark_mimic.sh` changes are measured for size and duplication only. One symbolic link not followed (`.github/copilot-instructions.md`).
- **Measured facts attributable to the branch.** `src/core/module_registry.c` 1,322 → 1,386 code lines (+64); `execute_phase` CCN 17 → 16 and `dispatch_events_range` 14 → 12 (the two hot functions lost complexity); four new helpers at CCN 6/4/3/2, file sum 351 → 363, as the record states. `src/core/horizontal_driver.c` −15 lines (the seed helper moved out). `tests/framework/parity_gate.py` 1,745 → 2,207 code lines (+462) with three functions new to it at CCN 16 (`compare_hdf5_file`, `:1367`), 13 (`compare_attributes`, `:1327`) and 11 (`compare_metadata_file`, `:1466`); `tests/manual/test_snapshot_disabled_identity.py` −294 lines. Those three are the battery's comparison functions relocated, not new logic: their complexity moved files. Duplication candidates are all in `tests/manual/test_reference_identity.py` and are the `Tee` class (23 lines, Simplification 5) plus four import and idiom blocks of 7 to 9 lines shared with the battery, which are not worth a helper.
- **Interpretation.** The structural cost of the branch is one read-only startup table in the dispatch engine and a larger shared gate module. Both are proportionate to what they bought (4.5% on the hot path at 17 to 22 SE; one comparison implementation instead of two). No new dependency edge or cycle.

---

## Validation run for this review

Run at HEAD `c9c6d0e0` on 2026-10-22 by a delegated agent, sequentially, with logs under `archive/test-logs/review-c9c6d0e0/` (machine-local). Every command exited 0.

| Command | Result |
|---|---|
| `make -j` (default `sage16` / `mini-millennium`) | exit 0, no `warning:` line |
| `make check-generated`, `make check-format`, `make check-docs`, `make validate-modules` | exit 0 each (format: C and Python clean) |
| `make tests summary`, then `make tests` for the full log | exit 0 both; unit 603 PASS, 27 NA; integration 257 PASS, 12 NA; scientific 46 PASS plus the long-standing `WARN test_zero_values`; no FAIL, ERROR or SKIP |
| `grep -c "Memory leak detected"` on the unit tier | 14, the count the record gives at `before_head` and at every code commit |
| `tests/unit/build/test_debug_log_gate.test` | 4 of 4 pass |
| `./mimic models/sage16/input/sage16_mini-millennium.yaml -q` | exit 0; peak RSS 0.035 GB, `C` 15,869, `P` 14,648, `G` 15,525; no `Growing` line |
| `./mimic models/sage16/input/sage16_mini-millennium-horizontal.yaml -q` (after `make SIMULATION=mini-millennium-horizontal`) | exit 0; peak RSS 0.183 GB, `C` 39,694, `P` 37,425, `R` at most 3, retention pool 0.071 GB; no `Growing` line |
| Final `make` (default pair restored), `git status --short` | exit 0; only this review document is untracked, no tracked file changed |

These match the record's Slice 6 and Slice 9 figures for both drivers (`C` 15,869 / 39,694, `P` 14,648 / 37,425). The horizontal run's first attempt against the default binary failed at startup with the expected selector mismatch (`Run file selects simulation.name='mini-millennium-horizontal' but this executable was built with SIMULATION=mini-millennium`), which is the MODEL/SIMULATION invariant working as documented, not a defect.

Differential code-health: `python3 ~/.claude/skills/code-health/scripts/health.py analyze --base main --json`, exit 0 (the summary is in the section above; the JSON is in this session's scratchpad).

---

## Coverage summary

- **Scope reviewed:** every file in `git diff main..HEAD` (29 files): the runtime C (`module_registry.{c,h}`, `error.{c,h}`, `output_buffer.h`, `constants.h`, `interface.c`, `horizontal_driver.c`), the Makefile and benchmark script, the new and changed tests (`test_debug_log_gate.c`, `test_horizontal_retention_budget.c`, the Python gate, the refactored battery, the shared helpers), the user-facing docs, the five skills and the five `docs/dev/` records. The Python gate was reviewed independently by a delegated reviewer (Finding 3 and the Python section) and the C by this reviewer.
- **Requirements checked against:** plan revision 6's Outcome and Limits, each slice's acceptance criteria and non-goals, the dispatch contract (`docs/DEVELOPER-GUIDE.md:515-521`, `module_registry.h:169-210`), VISION Principles 1, 4, 5 and 7, and the Style Guide.
- **Dimensions checked:** correctness and contract fit, lifetime and ownership (plan table, event buffer, output buffer), boundary conditions (empty phases, empty workspaces, `nhalos` 0 and above `MAX_HALO_ARRAY_SIZE`, odd halo counts for the FoF seed), numerical and reproducibility claims (byte gate scope, the GCC FMA caveat), tests, observability (the FATAL messages), portability (the header-inline seed for the converter's standalone link), maintainability and documentation.
- **Validation run:** build, `check-generated`, `check-format`, `check-docs`, `validate-modules`, `make tests summary`, the DEBUG_LOG gate test, both default runs and differential code-health (see the section above). **Not run:** the byte gate, the converter and distributed suites, any wall-time or profile measurement, the research-scale runs. Lint was run differentially by the PM at every slice (verdict pass); this review did not repeat it.

---

## Open questions and assumptions

- The review takes the record's measurements as true where it could not cheaply re-derive them (every wall and profile figure). Where it could (the FoF-seed equality, the bounded readers of `ProcessedHalos`, the dispatch order, the no-plan paths, the memory-reset mechanism), it did, and found the record accurate.
- Decision 12 (whether the pathway's next step is the emulator, the coupled-rate spike or thread-per-forest) is the owner's and is not taken here. The evidence the plan gathered for its recommendation (near-linear `mpirun` scaling to 16 ranks) was not re-measured and is the record's.

## Verdict

**PASS WITH RISKS.** No `P0`/`P1`. Merge is recommended after Finding 1's docs commit; Findings 2, 3, 5 and 6 are one short follow-up each or together, none blocking.
