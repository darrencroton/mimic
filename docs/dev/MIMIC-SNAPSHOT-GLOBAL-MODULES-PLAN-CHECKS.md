# Snapshot-Global Modules Plan: Executable Contract Check Evidence

**Purpose:** Record what the planning-only checks under [snapshot-global-checks/](snapshot-global-checks/README.md) proved about the frozen plan [MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md](MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md) at revision 5 on 2026-10-01, with exact commands, counts, limitations and the remaining observations worth acting on before Project Manager (PM) initialization.

**Revision 6 (2026-10-01):** the plan was re-baselined to `50d2ca7ad0dc80464d145e325ff69b77a891208c` after the horizontal micro-Uchuu package rename; see [Re-baseline review](#re-baseline-review-revision-6-2026-10-01) at the end. Everything above that section is unchanged evidence from revision 5 at `717cf3ed`, under that commit's package names.

**Status:** Planning evidence only. Nothing here implements, mocks or exercises the proposed driver integration. The planning checks concern the plan text, installed PM tooling, C types and specified arithmetic. The separate baseline preflight below exercises existing code only. The checks are pre-implementation evidence: they exercise named contract properties and demonstrate that selected faulty variants are detected, not that a feature works, and a passing phrase check shows only that the named sentences survived, never that the contract has no other gap. All production code was left untouched.

## Environment at the pre-commit check

| Item | Value |
|---|---|
| Repository HEAD | `717cf3ed5647eb85d2426f44f7dcda7ecd695103` (the plan's planning baseline) |
| Plan bytes checked | working tree, SHA-256 `d88ba2425e67170429ba88e2ec4d7f5b276c6c17c591be5a207160cb0052b421` (revision 5; revision 4 was `f464f1d8…`) |
| Plan vs committed baseline | modified in the working tree; PM's `plan_digest` hashes the plan file's actual bytes, so this digest is what PM would freeze if `init` ran on these bytes, and it changes with any further edit |
| Drift from baseline | committed 0, staged 0, unstaged 2, untracked 13 paths, all inside the planning surface (plan, pathway, two records, checker directory); nothing outside it |
| PM library | `pm_lib` imported from `/Users/dcroton/Documents/AI/skills/project-manager/scripts` (resolves through a symlink to `/Users/dcroton/Documents/AI/repos/ai-agent-coder/skills/project-manager/scripts/pm_lib`) |
| Skills root for prompt rendering | `/Users/dcroton/Documents/AI/skills` (`drift-audit`, `code-review`, `lint/scripts/lint.py` all present) |
| Python | 3.14.6 (`python3`; PM's surface matcher needs 3.13+ for `PurePosixPath.full_match`) |
| C compiler | Apple clang 21.0.0 (clang-2100.3.34.2) via `cc` |
| Generated header in tree | `src/include/generated/property_defs.h` generated for `sage16` (no `ShamVpeak` field) |
| libm measured by the oracle | the platform libm reached through Python's `math` module, the same library a C build on this machine links |

## Commands

All commands run from the repository root. The runner executes the three suites sequentially, captures each suite's stdout to `docs/dev/snapshot-global-checks/results/<suite>.log`, its JSON report to `results/<suite>.json`, and derives counts from `MIMIC_RESULT:` markers only.

```bash
python3 docs/dev/snapshot-global-checks/run_checks.py
# individually:
python3 docs/dev/snapshot-global-checks/check_plan_contract.py --json results/plan_contract.json
python3 docs/dev/snapshot-global-checks/c_const_view_probes.py --json results/const_view.json
python3 docs/dev/snapshot-global-checks/sham_rank_oracle.py --json results/sham_rank.json
python3 /Users/dcroton/Documents/AI/skills/project-manager/scripts/pm.py check-plan \
  --plan docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md --repo .
python3 scripts/check_docs.py
```

Demonstration that the checker fails on a corrupted plan (the `lost_oracle_criterion` variant produced by applying that `plan_mutations.py` entry to the plan and written under the ignored `results/` directory with a non-Markdown name so `check_docs.py` does not scan its relative links; nothing is written beside the plan):

```bash
python3 docs/dev/snapshot-global-checks/check_plan_contract.py \
  --plan docs/dev/snapshot-global-checks/results/lost_oracle_variant/corrupt-plan.txt
# exit 1; 637 pass, 3 FAIL (the three Slice 3 phrases on the deleted line are lost from both
# seats) plus 1 ERROR because the mutation catalogue assumes the frozen plan text and the
# lost_oracle_criterion mutation itself no longer applies to the variant. (Revision 5 re-run.)
```

## Results

Final run (`run_checks.py`) on the revision 5 plan bytes, total elapsed under 5 s (the revision 4 run gave 634 / 18 / 88, 740 in total; revision 5 added seven critical phrases and one oracle test):

| Suite | Pass | Fail | Skip | Error | Exit |
|---|---|---|---|---|---|
| `plan_contract` | 641 | 0 | 0 | 0 | 0 |
| `const_view` | 18 | 0 | 1 | 0 | 0 |
| `sham_rank` | 89 | 0 | 0 | 0 | 0 |
| **Total** | **748** | **0** | **1** | **0** | **0** |

PM's own `check-plan` with repository context: `5 slice(s); approval-gated: Slice 1, Slice 2, Slice 3, Slice 4`, no errors, no warnings. `scripts/check_docs.py`: internal links and anchors resolve, no unresolved markers.

The one skip is `const_view.sham_fields_in_generated_header`: the working tree's generated header is for `sage16`, so the scratch-sort probe reads `galaxy->StellarMass` instead of `ShamVpeak`. It is a coverage gap, not a pass.

### `plan_contract` breakdown (641 checks)

| Category | Checks | What passed |
|---|---|---|
| setup | 1 | `pm_lib` imported from the installed skills root |
| drift | 13 | synthetic path sets classify as intended: the planning surface passes; runtime sources, tests, scripts, skills, other `docs/` files, other `docs/dev/` plans, a plan-name prefix look-alike, checker-directory look-alikes, generated and build outputs, `.pm/` bookkeeping and a bare directory name are all reported as drift; a mixed set reports only its outside paths; the empty set reports nothing |
| baseline | 3 | Git answered; HEAD descends from the planning baseline (here it equals it); no committed, staged, unstaged or untracked path lies outside the planning surface |
| parse | 13 | 5 slices, numbered 1–5 contiguously, all 7 `REQUIRED_SECTIONS` present and no foreign sections; Slice 5's body does not absorb "Next Chat Prompts" |
| check_plan | 3 | `plan_check_report(plan, repo)` returns 0 errors and 0 warnings; approval-gated set is exactly Slices 1–4 |
| risk | 16 | `approval_needed` is True for 1–4 and False for 5; `independent_audit_required` True for all; `plan_risk` elevated for all; Slice 5's risky-surfaces line is correctly *not* read as the literal `none` |
| guard | 2 | every audit flag is an explicit yes/no; every slice derives elevated risk |
| eligibility | 11 | Slices 1–4 blocked without a recorded approval and eligible with one; Slice 5 eligible without; `next_slice` picks 1 first and 5 after 1–4 are accepted |
| surface | 274 | 73 authorized entries (18/24/5/8/18) all pass `authorized_entry_error`, none duplicated, none a plain directory; 67 witness paths authorized; 54 slice-specific plus 120 everywhere-unauthorized rejections; the plan file is never authorized by any slice; the annotated `Makefile` entries in Slices 1, 3 and 4 normalize to `Makefile`; `unauthorized_files` isolates exactly the plan file from each witness set |
| prompts | 136 | for every slice, Developer and both Reviewer prompts (`drift-audit`, `code-review`) contain the five shared sections verbatim; Developer also carries Validation Plan and Rollback Path; no unresolved `{field}`; the lint command renders with the baseline commit; skill bundles are embedded from the real skills root; all 63 acceptance checkboxes (14/15/21/7/6) and the recommended model/effort line reach both seats; every slice has a "Required evidence" checkbox |
| critical_phrase | 75 | every phrase in `fixtures/critical_phrases.txt` (13/20/24/13/5 per slice) reaches both seats verbatim, including the revision 2 and 3 clauses: dual-mode `test_fixture`, harness `post_snapshot` key, no new fixture data, the expanded log form, the log-space bound and its measured `exp(log(100000.0))` value, the `FLT_MAX` peak bound, the subnormal bit pattern, the exact oracle mapping, the malformed-string list, `tests/manual/` and `tests/data/`; and the seven revision 5 clauses: the two explicit-selector `test_snapshot_phase.py` invocations, the Slice 4 arrival of the wrapping target, the never-rebuilt shared executable, the key-absent baseline leg, both feature variants comparing to that baseline, and byte-equal `Parameters`/`FieldMetadata`/`EnabledModules`/`EventContracts`/`Redshifts`, and no two-rank case in the auto-discovered test |
| model_effort | 5 | each receipt's `Recommended Developer: <model>; effort: <level>` matches the profile table and uses an allowed model/effort |
| anchor | 22 | all `path:start–end` citations in "Repository Evidence" contain the identifiers they describe, including the revision 2 and 3 additions (Makefile SOURCES block, discovery glob, test-registry glob, harness phase writer, generator and `check_generated` hashes, `VALIDATE_RANGE_EXCLUSIVE`, `parse_double_strict`, `module_emit_event`, `execute_phase`'s early return, `module_registry_add`'s null check, reserved keys, unknown-key rejection, repo-relative config, `get_virial_mass`) |
| identifier | 21 | every symbol the plan names by name resolves, including `LOAD_PARAM_DOUBLE_INTERNAL`, `VALIDATE_RANGE_INCLUSIVE`, `ShamMpeak`, `Mvir` and the two fixture box sizes (`100.0` and `62.5` Mpc/h) |
| new_path_absent | 10 | plan-named new files and directories, including `tests/manual/`, do not exist at the baseline |
| removed_envelope_absent | 2 | `tests/data/snapshot_global` and `tests/integration/test_snapshot_disabled_identity.py`, removed from the surface by revision 2, do not exist either |
| mutant | 34 | every corrupted variant behaved as expected (table below) |

### Corrupted plan variants (34, all in temporary storage)

| Variant | PM `check-plan` result | Compensating guard |
|---|---|---|
| Rollback Path section removed (Slice 2) | error: missing required sections | – |
| Acceptance Criteria heading kept, body blank (Slice 3) | error: missing required sections | – |
| absolute path, `./` prefix, unwrapped annotation, backslash, `..` segment in a surface | one error each, naming the entry | – |
| `**/*`, `requirements.txt`, `LICENSE`, existing directory without `/` | warning each (not an error) | – |
| file list written flush-left (Slice 1) | error: surface has no files (silent narrowing to nothing) | – |
| approval `yes, after review`, `not yet`, line deleted | error each; `approval_needed` parses to None | – |
| approval `Yes.` | silent; parses to True (tolerated by design) | – |
| audit flag blank (Slice 5) | silent; `independent_audit_required` becomes False | `explicit_audit_flags` fails |
| audit `no` and risky `none` (Slice 5) | silent; `plan_risk` becomes `standard` | `all_slices_elevated` fails |
| Slice 5 renumbered to 4 | error: duplicate slice numbers | – |
| `## Slice 6:` inside a fence (Slice 5) | 5 errors: fenced heading, and Slice 5 truncated so it loses Rollback Path | – |
| `## Slice 6 - Extra work`, `### Slice 6: Nested` | error: malformed slice heading | – |
| unclosed code fence | error | – |
| `## Slice Batches` section | warning: batches ignored in Mode B | – |
| oracle mapping line deleted (Slice 3) | silent | `critical_phrases` fails |
| expanded log-form Outputs line deleted (Slice 3) | silent | `critical_phrases` fails |
| log-space bound rewritten as `M_r > 100000` (Slice 3) | silent | `critical_phrases` fails |
| `<= FLT_MAX` before the cast dropped (Slice 3) | silent | `critical_phrases` fails |
| `module_emit_event` rejection line deleted (Slice 2) | silent | `critical_phrases` fails |
| harness `post_snapshot` key line deleted (Slice 2) | silent | `critical_phrases` fails |
| "two float ULPs" changed to "two percent" | silent | `critical_phrases` fails |
| `tests/manual/…` renamed to `tests/integration/…` (Slice 4) | silent | `critical_phrases` fails |
| Slice 4 receipt changed to Opus/low | silent | `model_effort_consistency` fails |
| literal `{braces}` in acceptance text | silent; braces survive rendering into both seats | – |

Sixteen variants produce PM errors, five produce warnings only, thirteen are invisible to PM. Of the thirteen, two are deliberate tolerance and eleven are caught by the guards in `check_plan_contract.py`; those guards are the only mechanical protection against lost or weakened binding text.

### `const_view` (18 pass, 1 skip)

Compiled with `cc -std=c11 -Wall -Wextra -Wshadow -Wformat-security -Wundef -Werror -fsyntax-only -Wcast-qual` against the real `module_interface.h`, `types.h` and generated `property_defs.h`; `struct SnapshotContext` is defined locally with the four fields Slice 1 lists.

- **Compile (5):** galaxy write through `const struct Halo *`; zero count with a NULL population; reading halo fields and the typed context; sorting a private scratch array with checked byte sizing; an `int64_t` count loop.
- **Rejected (10):** writing `Type`, `CentralHalo` or `galaxy`; swapping two entries; `qsort` and `memcpy` on the borrowed view; writing a context field; `module_emit_event` given a `SnapshotContext`; handing the const view to the existing FoF `process` signature; casting const away (with `-Wcast-qual`).
- **Limitations that compile (3):** storing the borrowed pointer in a static; indexing `halos[halos[i].CentralHalo]`; the const-stripping cast under the project's default flags, which lack `-Wcast-qual`. The plan states these three patterns are review obligations (Slice 1 criterion, repeated as a manual check in Slices 1 and 3); no flag is added.

### `sham_rank` (89 pass)

The oracle implements the revision 3 prescription: the expanded logarithmic form `ln M_r = ln M0 - [ln(r + 0.5) - 3 ln BoxSize - ln n0] / alpha` with the bracket evaluated first, the log-space upper bound `ln M_r <= log(100000.0)` before exponentiation, then the float rounding checked for finite, nonzero and `<= 100000`, and the updated double `ShamMpeak` bounded by `FLT_MAX` before its cast. Revision 5 aligned its peak validation with the plan's "consumed values" rule: inherited `ShamVpeak`/`ShamMpeak` are validated for every Type, the current `Vmax`/`Mvir` only for Type 0/1, so a Type 2 entry's unused current proxies are neither read nor validated (the revision 4 oracle validated them for all Types, which the plan does not require and which `make_orphan`'s zeroed `Mvir` shows is not the module's input).

- **Exact oracle.** The rational oracle (`Fraction`) for `alpha=1`, `n0*V=1`, `M0=8` with proxies `(80,200), (7,100), (42,100)` yields `16, 16/3, 16/5` at ranks 0, 1, 2 for IDs 80, 7, 42; the float32 rounding of those values equals the reference implementation's output.
- **Behaviour tests (29)** all pass on the reference implementation: the revision 5 regression `type2_unused_current_proxies_not_validated` (a Type 2 entry with `Vmax = NaN`, `Mvir = inf` and valid inherited peaks 300/1 succeeds, its inherited peaks are unchanged, and it takes rank 0 with mass 16 over a Type 0 at 200 with 16/3; the revision 4 oracle rejected this population), the nineteen from revision 1 (empty population, all-ineligible population without rank evaluation, Type 2 inclusion with inherited peaks, Type 2 peak preservation, zero-peak ineligibility, reset of the seven assigned fields with `ShamOrphanAge` untouched, a higher proxy elsewhere pushing every lower rank down, 128 shuffled populations, bit-identical repeats over 1000 entries, range failure rather than clipping, float32 underflow failure, malformed-but-ineligible NaN failing the snapshot, negative proxy failing, duplicate/zero/negative IDs failing, 2-ULP agreement over 4000 ranks) plus nine revision 3 edge tests: the tiny box (`BoxSize = 1e-105`) succeeds within 2 ULP of the Decimal reference, the exact endpoint is accepted and stores `100000.0f`, `n0 = 0.5000000000005` is rejected, the huge box (`BoxSize = 5.6e102`) fails with `M0 = 1, n0 = 1e3, alpha = 0.1` and succeeds with `M0 = 1e-30, alpha = 10`, the subnormal example stores bits `0x000012DA`, the underflow example fails, `Mvir = FLT_MAX` is accepted and stored exactly, `Mvir = 1e39` fails.
- **Measured platform facts.** `log(100000.0) = 11.512925464970229`; `exp(log(100000.0)) = 100000.00000000001`, which exceeds `100000` and rounds to `100000.0f`; the exact endpoint's `ln M_r` is bit-identical to `log(100000.0)`; one double ULP of the bound is `1.78e-15`, a mass band of `1.78e-10` at `100000`, against a float32 spacing of `0.0078125` there. Roundoff-band example: `n0 = 0.5·(1 + 2^-52)` gives `ln M_r - log(100000) = 0.0` and is accepted although the exact mass exceeds `100000` by `2.22e-11`; its stored float is `100000.0f`, the same as the endpoint's.
- **Mutation kill matrix:** all 19 mutants are detected. Wrong tie order (descending ID) is killed by 3 tests including the hand oracle and the dedicated tie test; `r+1` by 17 and `r+0` by 23; positional assignment by 13 including shuffle invariance; Type 2 peak updates by 11 including the new unused-proxy regression; float32 evaluation by 9; clipping by 6; materialising `n_r` first by exactly the tiny-box test; comparing after `exp` by exactly the endpoint test; the unchecked `ShamMpeak` cast by exactly the `Mvir = 1e39` test; flushing a subnormal to zero by exactly the pinned-bits test; silently dropping a malformed ineligible entry by two tests (the NaN-peak test and the new regression, whose non-finite `Vmax` the dropping mutant also discards); inventing fallback IDs by 3.
- **Parameter corners:** 1080 combinations (5 mass scales including the subnormal `5e-324`, 3 densities, 3 slopes, 8 box sizes from `2e-108` to `5.6e102`, 3 population sizes). `ln M_r` is finite on every corner and every rank. 765 corners fail a snapshot by specification (mass above `100000` or float underflow), 759 of them at rank 0. On 105 corners the superseded `n_r`-first form fails an in-range mass that the expanded form accepts: the old evaluation overflows intermediates on some admitted tiny and huge volumes. The specified boundaries are enforced as written (inclusive/exclusive as the plan states; NaN and a `BoxSize` whose cube overflows are rejected).
- **Malformed parameter strings** under the plan's parse-then-`isfinite`-then-range model: `nan`, `inf`, `infinity`, `1e400`, `1,0`, `abc`, `"1e5 "` (trailing space), the empty string and `0` are rejected; `1e5` and `100000` are accepted with identical double bits.
- **Decimal sweep:** 10000 random samples over the whole admitted domain (`M0` in `[1e-40, 1e5]`, `n0` in `[1e-12, 1e3]`, `alpha` in `[0.1, 10]`, `BoxSize` in `[2e-108, 5e102]`, ranks below one million): `ln M_r` finite on all, 3806 accepted, 3009 failed the log-space bound, 3185 rounded to float zero; the worst float32 distance of an accepted value from the 60-digit reference is 0 ULP. The pow-form sweep (2000 samples on ordinary boxes, 1995 finite) also agrees to 0 ULP.
- **Scale:** a 100 000-entry population assigns exactly its eligible Type 0/1 members, leaves peak-less Type 2 entries at zero, and the assigned masses are non-increasing by rank.

## Contract observations

The executable checks are not a completeness proof. Independent reviews found additional contract defects; revision 4 resolves the findings recorded in the review record. The revision 3 changes closed the earlier actionable items (the example parameters are frozen, subnormals are specified, the const-cast limitation is stated as a review obligation, the witness fixtures match the surface, the checker fixtures are inside the planning surface). What remains is awareness and process, ordered by how much it could cost an execution run.

1. **Two Slice 1 rules are documentation-only, not type-enforced (awareness).** Retaining the borrowed pointer and treating `CentralHalo` as a snapshot offset both compile; so does an explicit cast away from `const` under the project warning set. The plan makes all three explicit review obligations in every code review of a snapshot callback; no runtime assertion or flag enforces them.
2. **PM cannot see lost or weakened binding text (process).** `check-plan` validates structure only. Thirteen of the 34 variants pass PM silently; the `critical_phrases.txt`, model/effort and audit-flag guards here catch eleven and the other two are deliberate tolerance. Run `run_checks.py` after any plan edit and before PM initialization, and keep the phrase list in step with the plan; the fixtures now live inside the planning surface and are maintained with it.
3. **A blank audit flag silently disarms the audit gate (process).** PM's `independent_audit_required` fails closed to *off*, and `check-plan` reports nothing. The current plan has explicit `yes` on all five slices; the guard here fails if that changes.
4. **Slice 5's risky-surfaces line is not the literal `none` (harmless).** PM records "none newly implemented; documentation of already accepted APIs and measured outcomes" as a risky surface touched. The derived risk is elevated either way because the audit flag is `yes`.
5. **Twelve authorized entries do not exist at baseline and are not annotated as new (informational).** Examples: `tests/unit/test_snapshot_module_contract.c` (Slices 1–2), `models/sham/modules/sham_global_rank/` (Slice 3), `docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-ACCEPTANCE.md` (Slice 4). PM handles them correctly (new file paths and `/`-terminated envelopes match at runtime); the annotation is only for human readers.
6. **The Reviewer never sees Validation Plan or Rollback Path (by PM design, mitigated).** The plan's own statement that "Required test evidence in Acceptance Criteria binds both seats" holds: every slice carries a "Required evidence" checkbox and it reaches both seats, and the revision 3 numerical edge battery is an Acceptance Criteria checkbox for the same reason.
7. **A fenced `## Slice N:` example anywhere in the plan truncates the preceding slice (hazard).** `parse_plan` reads headings before masking fences; the variant lost Slice 5's Rollback Path and `check-plan` reported it. The current plan's fenced blocks contain no slice-like headings.
8. **The pre-commit check used uncommitted planning bytes (process).** PM's `init` refuses a tree that is dirty outside `.pm/` and freezes the run to `plan_digest`, the SHA-256 of the plan file's bytes at that moment. The drift check here confirms the only differences from the baseline are the planning surface, so a planning-only commit is what the plan's run preparation asks for; the user explicitly authorized that planning-artifact commit on 2026-10-01. Re-run the checker after it lands so the recorded digest is the one PM will freeze.
9. **Mass-boundary arithmetic (awareness).** Acceptance follows the frozen double computation. The recorded one-ULP spacing and nearby example are platform measurements, not a universal bound on accumulated log-expression error. Independent mass checks allow two float ULPs; same-build repeats and permutations are bitwise exact.
10. **Revision 5 closed the final review's Slice 2 and Slice 4 wording gaps (resolved).** Slice 2's Acceptance Criteria named an "explicit v2/v3 target" that its surface (no `Makefile`) cannot provide and described the two-rank MPI check as a test that "builds with MPI explicitly", which inside an auto-discovered tier would replace the tier's shared `mimic` (`USE-MPI` changes `CC`/`CFLAGS` but not `BUILD_DIR` or `$(EXEC)`). Slice 4 read as if the baseline leg also ran with an explicit empty list, which the baseline commit rejects as an unknown key. The plan now names the two explicit-selector invocations, moves the MPI evidence to an explicit command in an isolated candidate copy or worktree, states the key-absent baseline with both feature variants compared to it, and lists the exact permitted provenance differences; six phrase guards pin those clauses. The oracle is aligned as described under `sham_rank`.

## Limitations

- No driver integration exists at the baseline and none was mocked; nothing here says the post-snapshot phase, dispatch, lifecycle, provenance or inheritance behaviour would work. The C probes are `-fsyntax-only` compiles against headers; they do not link or run.
- The generated header in the tree is for `sage16`, so probes never touched `ShamVpeak`, `ShamMpeak` or the other SHAM fields; `make generate` was not run because that would change tree state outside the planning surface.
- The SHAM oracle is an independent Python re-implementation of the prescription, not a test of any C module. It exercises selected domain corners and detects the listed deliberately faulty variants; it says nothing about a C implementation's memory handling, scratch scaling or a Linux libm. The `exp(log(100000.0))` measurement and the 0-ULP results are this platform's; the `compare_after_exp` mutant is skipped, not failed, on a platform where the overshoot does not reproduce.
- The parameter-string model uses Python's `float`, which agrees with `strtod` for the strings the plan names but not for hexadecimal literals (which `strtod` accepts) or for `ERANGE` on subnormal results (which macOS and glibc `strtod` report and Python does not). The plan forbids relying on the latter.
- The `Mvir > FLT_MAX` case is unreachable through the readers from a real catalog (the horizontal mass is `float` on disk, and `Len × PartMass` would need `PartMass` above `1.6e29` internal units); it is a contract closure exercised only with a constructed entry.
- The drift check is conservative and pre-implementation only: it treats any path outside the five planning-surface entries as drift, including paths PM itself ignores, and it is expected to fail once a slice lands.
- The PM library and prompt templates are imported from outside the repository. If they change, these checks measure the new behaviour; the skill bundles embedded into the Reviewer prompt were checked for presence, not content.
- Witness and unauthorized paths are plausible file names chosen for this exercise, not the files a Developer will actually create.
- The mutation catalogue is written against the frozen plan text; running `check_plan_contract.py --plan` on a variant reports catalogue entries that no longer apply as ERROR, which is the correct outcome but not a defect in the variant.
- The delegated session could not invoke the venv formatters. The primary subsequently ran the venv Black and isort successfully over all six checker files; one file required Black changes. The whole-repository format check is recorded separately below.
- The baseline preflight below exercises existing builds and fixture runs; it does not run the full test tiers, MPI execution or real-data identity gates. Production input availability was checked by path inventory only.


## Baseline execution preflight (2026-10-01)

The primary delegated existing-code runs to a fresh local test agent in a detached checkout of `717cf3ed5647eb85d2426f44f7dcda7ecd695103`. Tracked-file hashes and `git diff HEAD` were unchanged afterward. All 30 commands (six `info`, six `generate`, six builds, twelve runs) exited zero. Every requested snapshot was present, populated input epochs produced nonempty output, all floating output fields were finite, and every master recorded the requested `TimestepScheme`. No compiler or runtime warnings/errors appeared in the final matrix. Shards omit that scheme attribute under the existing contract.

| Model | Committed fixture route | Fixed rows | Dynamic rows | Snapshots per run |
|---|---|---:|---:|---:|
| halos-only | mini-millennium vertical | 307518 | 307518 | 64 |
| halos-only | micro-uchuu horizontal v2 | 13 | 13 | 6 |
| halos-only | mini-millennium horizontal v3 | 6 | 6 | 5 |
| sage16 | mini-millennium vertical | 173205 | 173197 | 64 |
| sage16 | micro-uchuu horizontal v2 | 11 | 11 | 6 |
| sage16 | mini-millennium horizontal v3 | 4 | 4 | 5 |

The fixed/dynamic SAGE counts are separate references; no equality between timestep schemes is claimed, and individual merger/disruption causes were not traced. The first exploratory run used the older `test_physics_binary.yaml`, which omits metal enrichment despite its description. It was superseded by the matrix above using the shipped full pipeline; no baseline configuration was edited.

To reproduce, start each YAML from `models/sage16/input/sage16_mini-millennium.yaml`. Preserve its entire module mapping and parameters for SAGE, including metal enrichment. For halos-only replace modules with `{phases: {}, parameters: {}}`. Select the matching model and one of these simulation configurations: `tests/data/test_simulation.yaml`, `simulations/micro-uchuu-horizontal/_tests/input/test_simulation.yaml`, or `simulations/mini-millennium-horizontal/_tests/input/test_simulation.yaml`. These select, respectively, the committed vertical trees, v2 `data/generic/`, and v3 `data/worked_graph/`; do not override input paths. Use fresh scratch YAML/output directories, HDF5, `snapshot_list: []`, `SubSteps: 10`, `MaxDynamicSubsteps: 200`, and the selected `TimestepScheme`. Unset inherited model/simulation/test-build/MPI selector variables. For each pair run `make MODEL=<model> SIMULATION=<simulation> USE-HDF5=yes info`, then `generate`, then `make -j4 ... all`, followed by both `./mimic <run.yaml>` legs. Count shard rows without double-counting master links and scan every floating field.

The separate SHAM preflight passed production `generate`, `validate-modules`, `lint-parameters`, and `check-generated` with `MODEL=sham SIMULATION=micro-uchuu-horizontal TEST_BUILD=no USE-HDF5=yes`, followed by `TEST_BUILD=yes generate validate-build mimic`. Its legacy SHAM smoke run preserved the mapping and parameters from `models/sham/input/sham_mini-millennium.yaml`, selected the v2 generic fixture, used fixed timestepping and all HDF5 snapshots, and produced finite output with counts `[0, 1, 1, 1, 5, 5]`. The generated SHAM/test properties and linked module symbols were verified.

Applying production validators to TEST_BUILD output initially failed: `validate-modules` rejected fixture-only `TestDummyProperty`, and `check-generated` reported different property hashes. This is explained by `scripts/validate_modules.py:206` and `scripts/check_generated.py:43`, which load production property inputs, while `scripts/generate_properties.py:1918` adds test properties. The passing sequence follows the existing Makefile test workflow; no failure was hidden or validator weakened.

All six real-data gates' required source and horizontal files, run/config YAMLs and snapshot lists exist under `/Volumes/Internal/data/`. This inventory checked 50 snapshot files for each micro-Uchuu route and mini-Uchuu, and 64 for mini-Millennium/Millennium, plus `forests.h5` and each gate's source files. Millennium and mini-Uchuu remain files 0–15 samples. HDF5 tools, `mpicc`, `mpirun`, Git, Make, C compiler and the shared Python dependencies are available. The existing output scratch location had approximately 750 GiB free, above the gates' 10–100 GiB thresholds. This is availability evidence, not conversion-provenance or parity acceptance; recheck availability before execution.

Operational logs are archived at `archive/snapshot-global-planning/2026-10-01/baseline-preflight/.planning-probes/`: `matrix-20260930T133509Z/results.json`, `sham-build-20260930T135115Z/results.json`, and their command logs. The reproduction recipe and measured results above stand independently of those local archive paths.


## Final mechanical checks

Differential lint passed across all 15 changed files (Ruff, Markdown and spelling; project Black/isort owns Python layout, so `ruff-format` was excluded). Scoped Black/isort passed. The full unmodified `./scripts/beautify.sh` passed on an isolated complete candidate source snapshot: 858 tracked paths plus the 15 planning overlays, zero changes among 871 source paths. `make check-docs` and `git diff --check` passed. The main-checkout `make check-format` exited 2 only because isort followed `obsidian-inbox` to an unrelated external `compare_leaderboards.py`; C and Black passed, and no external file was changed. This failure remains recorded rather than relabelled as a whole-tree pass.

Revision 5 (delegated bounded session, 2026-10-01): `run_checks.py` exit 0 with the table above; PM `check-plan` with repository context reported `5 slice(s); approval-gated: Slice 1, Slice 2, Slice 3, Slice 4` with no errors or warnings; `scripts/check_docs.py` exit 0; `git diff --check` clean. The session's permission profile blocked `mimic_venv/bin/black` and `isort` and the system interpreter has neither module, so the edited `sham_rank_oracle.py` was checked by hand against the 100-column Black style (its longest new line is 99 columns) and was subsequently confirmed by the primary's Black/isort run (six files unchanged). No build, test tier, `check-format` or gate ran.

The primary independently reproduced all 747 passes and the one declared header skip on revision 5. Differential lint again reported no new findings; docs and whitespace checks passed. The local test agent refreshed all 15 overlays and repeated the full unmodified beautifier on the complete candidate snapshot: exit 0, zero changes across 871 source paths. Its differential style audit against `docs/STYLE-GUIDE.md` found no material issue in naming, comments/docstrings, documentation ownership/provenance, errors, fixture metadata, structured markers or generated-file handling. The relevant skill sweep found no newly stale guidance: no runtime or production-test contract changed, so future behavior is not documented as implemented.

The final two-clause clarification (after review 13) removes the optional in-tier MPI case and explicitly includes `EventContracts` and `Redshifts` in byte-equality checks. The primary replay passed 748 checks (641 / 18 / 89), with the same declared skip and no failures/errors. The lost-oracle demonstration rejected the corrupted plan with 637 passes, three failures and one error. A subsequent presence clarification requires identical dataset presence and byte equality where present; its replay also passed 748 checks with the same skip and no failures/errors. The environment table names those final bytes.

After the final reviews, scratch checkouts, raw delegate/probe/formatter logs and ad-hoc checker helpers were archived under `archive/snapshot-global-planning/2026-10-01/`. The detached baseline checkout was moved with `git worktree move`; no files were deleted. The reusable scripts, fixtures and latest ignored results remain beside the plan. Raw paths embedded in archived launch manifests retain their original values as historical provenance.


## Re-baseline review (revision 6, 2026-10-01)

The plan was baselined at `717cf3ed5647eb85d2426f44f7dcda7ecd695103`. Four commits then landed outside the planning surface: `da816c8d` (four sage16 horizontal run files), `fadabb6d` (the horizontal micro-Uchuu packages renamed after their source format), `4aee080b` (the plan and its records following the rename) and `50d2ca7a` (a Shin-Uchuu README correction). The planning checker's drift check `baseline.no_drift_outside_planning_surface` failed from `da816c8d` onward, as designed. This section records the review that decided whether the plan's premises still hold, and the evidence repeated at the new baseline. The sections above are unchanged evidence from `717cf3ed` and keep the package names of that commit.

### Name map

| Name at `717cf3ed` | Name at `50d2ca7a` | Meaning |
|---|---|---|
| `micro-uchuu-horizontal` | `micro-uchuu-ascii-horizontal` | version 2, Consistent-Trees ASCII source, the plan's v2 fixture package |
| `micro-uchuu-lhalo-horizontal` | `micro-uchuu-horizontal` | version 3, L-Halo binary source |
| `micro-uchuu-hdf5-horizontal`, `mini-millennium-horizontal`, `millennium-horizontal`, `mini-uchuu-horizontal`, `shin-uchuu` | unchanged | |

The string `micro-uchuu-horizontal` therefore means a different package before and after the rename. In the narrated preflights above it is the version 2 package; everywhere in the plan it is the current name.

### Drift review

| Question | Finding |
|---|---|
| Did runtime behaviour change? | No. From `717cf3ed` to `50d2ca7a`, `scripts/`, `tests/framework` and the model modules do not differ; `src/` and the tests differ only by comments in four files (`src/io/horizontal/read_horizontal_hdf5.c`, `src/io/vertical/read_ctrees_ascii.c`, `tests/integration/test_processing_order.py`, `tests/unit/test_horizontal_v3_reader.c`); the `Makefile` differs by one comment and one path (`check-horizontal-fixture`); and the simulation packages' `halo_properties.yaml`, `simulation_info.yaml` and `test_simulation.yaml` changed only in comments and package-relative paths. |
| Is the v2 fixture the same data? | Yes. Every committed v2 fixture data file (HDF5 snapshots, `forests.h5`, `a_list`) moved at 100% blob identity under the new name. The two `fixture_manifest.json` files differ only in their `generator` path and `_tests/data/generic/regenerate.sh` only in its generator path. |
| Do the old v2 and L-Halo run files match the new ones? | Yes, apart from the header comment, `simulation.name` and `output_directory` (checked for `halos-only` and `sage16`). |
| Do the new sage16 horizontal run files touch any criterion? | No. No test or script globs `models/*/input`; the parity gates choose their models explicitly (`halos-only` for the four packages that gained sage16 files); Slice 4 derives from `sage16_mini-millennium.yaml`, which is unchanged. |
| Do the plan's cited line anchors hold? | Yes: the anchor checks pass at the working tree. |
| What did not hold? | Slice 4 named `717cf3ed` as the pre-feature reference commit. That commit predates the rename: the baseline worktree has no `micro-uchuu-ascii-horizontal` package, the compiled simulation name must equal the run file's `simulation.name`, and the recorded `SimulationName` and `SimulationDir` attributes and the copied run YAML would differ between the reference and feature legs for the v2 fixture, which the plan's list of permitted differences does not allow. Revision 6 therefore moves the planning baseline, and with it the Slice 4 reference commit, to `50d2ca7ad0dc80464d145e325ff69b77a891208c`, which carries the names the feature tree uses. |
| Was recorded evidence rewritten? | The rename commit rewrote the narrated preflights in this record and the review record to the new names, which made them describe paths that do not exist at `717cf3ed`. Revision 6 restores both records to their `c51e3623` text and adds this section. |

### Evidence repeated at `50d2ca7a`

A fresh local test agent ran the following in a detached worktree of `50d2ca7a`, sequentially, with the selectors unset between pairs and without any real-data `snapshots/` link. Every command exited zero. The primary re-counted the log markers and two of the four v2 legs from the written output.

| Check | Result |
|---|---|
| `make check-docs`, `make check-horizontal-fixture` | pass |
| `make MODEL=sage16 SIMULATION=mini-millennium generate validate-modules lint-parameters check-generated`, default build | pass; no compiler warnings |
| `make tests summary` (default pair) | all pass; unit 498 PASS, 10 SKIP; integration 217 PASS, 4 SKIP; scientific 19 PASS, 1 WARN; total 734 PASS, 14 SKIP, 1 WARN, 0 FAIL, 0 ERROR |
| `make MODEL=sage16 SIMULATION=mini-millennium tests-horizontal-v3` | `PASS: tests-horizontal-v3 (4 C tests, 2 Python tests, no unexpected skips)` |

The 14 skips are the expected default-pair skips: nine v3-fixture unit tests (the fixture schema matches only `mini-millennium-horizontal`), `test_unknown_module_error` (needs process isolation), and four `test_horizontal_*` integration tests (the selected package is not horizontal). The warning is the scientific tier's `test_zero_values` (three fields with zero values), a soft check.

The four v2 fixture legs use the same recipe as above with `simulation.name: micro-uchuu-ascii-horizontal` and `simulation.config` set to `simulations/micro-uchuu-ascii-horizontal/_tests/input/test_simulation.yaml`:

| Model | Scheme | Exit | Snapshots | Rows (per snapshot) | Recorded at `717cf3ed` | Float fields scanned / non-finite |
|---|---|---:|---:|---|---:|---|
| halos-only | fixed | 0 | 6 | 13 (0,1,1,1,5,5) | 13 | 84 / 0 |
| halos-only | dynamic | 0 | 6 | 13 (0,1,1,1,5,5) | 13 | 84 / 0 |
| sage16 | fixed | 0 | 6 | 11 (0,1,1,1,5,3) | 11 | 216 / 0 |
| sage16 | dynamic | 0 | 6 | 11 (0,1,1,1,5,3) | 11 | 216 / 0 |

Every master recorded the requested `TimestepScheme`. The SHAM preflight repeated under `MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal USE-HDF5=yes`: `TEST_BUILD=no generate validate-modules lint-parameters check-generated` passed, `TEST_BUILD=yes generate validate-build mimic` built without warnings, and the legacy SHAM smoke run on the v2 fixture (fixed timestepping, all HDF5 snapshots) exited zero with per-snapshot counts `[0, 1, 1, 1, 5, 5]` and no non-finite value, equal to the count recorded at `717cf3ed`.

The raw logs, run YAMLs, outputs and the worktree itself are archived under `archive/snapshot-global-planning/2026-10-01/rebaseline-50d2ca7a/` (`evidence/` and `worktree/`).

### Planning checks on revision 6

`run_checks.py` on the revision 6 bytes gives `plan_contract` 641 pass, `const_view` 18 pass and 1 skip (the same generated-header coverage gap as above), `sham_rank` 89 pass: 748 pass, 1 skip, 0 fail, 0 error, with `baseline.no_drift_outside_planning_surface` passing against `50d2ca7a` because only planning-surface files differ. PM's own `check-plan` with repository context reports `5 slice(s); approval-gated: Slice 1, Slice 2, Slice 3, Slice 4` with no errors. The plan digest PM would freeze is the SHA-256 recorded in the revision 6 commit message, since this record cannot carry the digest of a file it is committed beside without changing it.

### Limits

- The other eight legs of the 12-leg matrix (`mini-millennium` vertical and `mini-millennium-horizontal` v3, both models, both schemes) were not repeated: their selectors did not change, and the runtime tree differs from `717cf3ed` only by the comments listed above. The default tiers and `tests-horizontal-v3` above exercise those packages' readers and drivers, but the eight legs' row counts were not re-measured.
- No real-data identity gate was run. They read machine-local datasets and run inside Slice 4, as the plan says. The `snapshots/` links of the three micro-Uchuu horizontal packages resolve (51 entries each) and, read from the snapshot headers, carry the expected formats: `micro-uchuu-ascii-horizontal` version 2, `micro-uchuu-horizontal` version 3 from `lhalo_binary`, `micro-uchuu-hdf5-horizontal` version 3 from `consistent_trees_hdf5`. Each gate's own dataset-provenance stage remains the authority.
- The `mimic` binary and generated selectors in the main checkout were not touched by these runs.
