# General Horizontal Runtime — Full-Plan Code Review

**Purpose:** Independent post-run code review of the completed [`MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md`](MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md) (commits `87aa9a86..cd5bbde4`, 26 commits, 147 files, +19,874/−1,048 lines), with emphasis on whether the plan's goals were met, code correctness and quality, a behaviour-preserving simplification pass, and a disposition for every item in the run's follow-up register (F1–F25) and the PM's run notes. Section 0 is the entry point for the follow-up work.

**Status:** Review complete 2026-09-29 at `cd5bbde4`; revised the same day after an independent claim-check and value-versus-complexity triage of every item (§2). Nothing was changed by the review itself; every recommendation is for the follow-up passes in §0. **Verdict: PASS WITH RISKS** — no P0 or P1 finding; 8 P2 findings, all bounded; the plan's acceptance gate and every route gate were re-run and hold. Both owner decisions the review needed (F18, F1) are now recorded in §0.3.

---

## 0. Work plan — the entry point for the follow-up session

This section is written so a fresh session can execute the follow-up without re-reading the review: an orchestrating agent (Fable, holding this document and the cited files in context) delegates each group below to a lower-capability subagent, checks the diff against the group's proof, and commits per group. Finding numbers refer to §4; "S6.n" to §6; "Fn" to the follow-up register in §7.

**Status (2026-09-29): Pass 1 is implemented, panel-reviewed and committed — see [§0.5](#05-pass-1-record-2026-09-29). Pass 2 (§0.2) and the two recorded owner decisions (§0.3) remain; the root `HANDOFF.md` is the entry point for that work.**

**Ground rules for every group** (from `AGENTS.md` and the `mimic-change-control` skill): same `MODEL=`/`SIMULATION=` for `generate`, `validate-modules`, tests and `make`; never run two test suites concurrently; never hand-edit `*/generated/`; never weaken a failing test or widen a tolerance; run `./scripts/beautify.sh` then `make check-format`, `make check-docs` and `git diff --check` before every commit; ask before committing; commit messages list every changed file with its reason. The v2 gate (`make MODEL=halos-only SIMULATION=micro-uchuu-horizontal tests-scientific`, ~7 min) and the acceptance gate (`make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-scientific`, ~1 min plus builds) build worktrees at the committed HEAD, so run them **after** committing a group that touches the reader, the driver, a harness or a package.

### 0.1 Pass 1 — Easy (one session; three groups, each one subagent and one commit)

Each item is a text/metadata edit, a one-function change, or one added test case, doable from this document alone with the existing suites as proof. Do the groups in order; 1C last because 1B's tests are what it wires into CI.

**Group 1A — text, comments and metadata (no behaviour change).** Proof: `make check-docs`, `make check-format`, `make tests-converter` (the converter tests assert the runtime text), `git diff --check`.

| Item | File(s) | Edit |
|---|---|---|
| 6 (F21) | `scripts/convert/convert_trees.py:349-351, :356-357` | Replace the two false driver statements printed by `_print_topology()` (called at `:370`, `:630`, `:655`) with the bound wording of item 13 and the admission facts of item 1; update `scripts/convert/tests/test_cli.py` if it asserts the text |
| 13 (F24) | `docs/dev/HORIZONTAL-HDF5-FORMAT.md:524`, `scripts/convert/report.py:333-338` | "retain more than two" → "may retain more than two, at most the longest descendant span plus one" |
| 14 (F16) | `docs/VISION.md:90`, `src/core/horizontal_driver.c:1596-1601` | Ceiling "bounds the retention pool's admitted payload only" (not "rather than letting the run exhaust memory"); same clause in the one-time warning |
| 12 (F22, F25) | 20 sites: five `simulations/*-horizontal/README.md:3`; five `halo_properties.yaml` headers (`mini-millennium-horizontal:10`, others `:9`); `src/io/horizontal/reader.h:19`; `read_horizontal_hdf5.c:12,197`; `scripts/convert/convert_trees.py:5`, `hdf5_writer_v3.py:4`, `transpose.py:11`, `validate_v3.py:4`, `validate.py:1722` (diagnostic text), `profiles/README.md:91`; `scripts/convert/tests/test_hdf5_writer.py:776` | Point at `HORIZONTAL-HDF5-FORMAT.md#version-3`; READMEs stop saying "the draft frozen in" |
| 7 | `simulations/mini-millennium-horizontal/simulation_info.yaml:17` | `last_file: 7` (the "metadata only" comment at `:11` already exists) |
| 8 (F5) | `docs/dev/HORIZONTAL-HDF5-FORMAT.md:367-374`; `scripts/generate_properties.py:1000-1007` error text | State that `double` catalog fields are declarable by the format but not yet generatable by Mimic's readers; name the two places an implementation must touch (`src/io/vertical/hdf5.c:51` read modes; the horizontal fill path at `read_horizontal_hdf5.c:1592`) |
| 18 | `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md:186`; plan `:24`, `:28-37` | Pathway row describes the live `HANDOFF.md` as this plan's; plan Outcome points at the pathway's "Named follow-ups" and lists F1, F3, F5, F14, F18, F19 in one sentence; add `Outcome` to the plan TOC |
| 22 (D1) | plan `:125` | Append "(v2 checks unchanged; the version-rejection messages now name both supported versions and mixed-version files)" |
| 23 | `src/core/horizontal_driver.c:338-341` | Message names both causes: "the input's chain names a generation its own DescendantSnapshot did not keep alive, or a generation was released before its horizon" |
| 26 (F15) | `src/core/main.c:471` | Comment: pools are created lazily and recycled through a spare stack, harvested at teardown |
| 37 | acceptance `:79`, `:260` | Append "(done in `73149ca6`)" |
| S6.6 | `src/core/horizontal_driver.c:1137-1138` | "the seed never adds proportional headroom past `MAX_HALO_ARRAY_SIZE`" (the code adds `MIN_HALO_ARRAY_GROWTH`; do not change the code) |
| S6.10 | `tests/unit/test_index_width.c:47` | One comment: `IS_SIGNED_64` probes width and sign rather than using `_Generic` because `struct Halo.HaloNr` is `long long`, which is not `int64_t` on Linux/glibc |
| PM rows | this document §7 | Already corrected here; nothing to edit elsewhere |

**Group 1B — small code fixes and test additions.** Proof: `make tests summary` (default pair, delegated); `make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-unit`; `make check-generated` for the default pair and for `SIMULATION=mini-millennium-horizontal`; then, after commit, the acceptance gate and the v2 gate (item 33 touches the vertical path; its proof is the default-pair scientific baseline inside `make tests` plus the v2 gate's Stage 8).

| Item | File(s) | Edit and test |
|---|---|---|
| 27 (F17) | `tests/unit/test_horizontal_retention_budget.c:158-165` | `set_role()` gets a `matched` flag and `abort()`s when no role matched. Do this **before** item 10 |
| 10 (F17) | `tests/unit/test_horizontal_retention_budget.c` | Two fake-reader cases with cleanup assertions: `load_slab` returning `nhalos + 1` (count-mismatch FATAL, driver `:1175-1179`); a v2 slab with `Descendant >= 0` in the last snapshot (horizon-beyond-run FATAL, `:1193-1197`). Each asserts the failure path released every generation (pattern: the existing `test_failure_at_load_…`) |
| 9 (F9) | `simulations/mini-millennium-horizontal/_tests/unit/test_unit_horizontal_retention.c` | One seed with a `NextProgenitor` whose target is **earlier** than its owner's snapshot, e.g. D(3) ← P0(2) → P1(0) → P2(1): assert count 3, gather order P0, P1, P2 with `source_time` from each own snapshot, and main-branch selection. Mirror it in `_tests/data/source/generate_sources.py` for `test_gap_retention.py` only if the converter can emit that chain order |
| 4 (F6) | `src/io/horizontal/read_horizontal_hdf5.c:2270-2281` | When `horizontal_h5_peek_format_version()` fails at snapshot 0 and `H5Lexists(file, "schema") > 0`, FATAL naming the `/header` link type or attribute fault before dispatching to the v2 path; add the soft-linked-`/header` and int64-`format_version` cases to `OPEN_CASES` in `tests/unit/test_horizontal_v3_reader.c`. v2 files are untouched (a v2 file has no `/schema`) |
| 19 | `tests/unit/test_horizontal_v3_reader.c` `OPEN_CASES` | One negative case each: `ForestIndex` outside `[0, n_forests_total)`; measured max `ForestIndex ≠ n_forests_total − 1`; `HaloRankInForest < 0`; empty-dataset sentinel with halos present; `n_forests_total = 0` with halos; a `/schema` member that is a dataset; a schema attribute with the right count but a wrong name; an ASCII (non-UTF-8) vlen schema attribute; `column_mapping_sha256` of the wrong fixed length; a non-NUL-padded header string; a missing snapshot file. Each must fail with the message the reader already emits at the cited lines in item 19 |
| 20 (F4) | `scripts/generate_properties.py:507`; `tests/integration/test_unit_contract_generation.py` | Default `units` once at `catalog_by_name` construction: `{**prop, "source": …, "units": prop.get("units", "dimensionless")}`; drop the now-redundant `.get("units", "dimensionless")` in `generate_catalog_field_metadata_inc` (`:1054-1062`) and the other generators; add a test with a unit-less extra field. Proof also: `make check-generated` on every package must be unchanged |
| 24 (F10) | `src/include/types.h:257`; `src/core/horizontal_driver.c:1553`; `simulations/mini-millennium-horizontal/_tests/unit/test_unit_horizontal_retention.c:152`; `simulations/micro-uchuu-horizontal/_tests/unit/test_unit_horizontal_driver_gather.c:187` | Remove the never-read `HorizontalGatherContext.snapshot_count` |
| 33 | `src/core/build_model.c:278-301` | `if (required > MAX_HALO_ARRAY_SIZE) FATAL_ERROR(...)` before the growth loop, same message as the existing fatal; vertical-only, so the default-pair baselines are the proof |

**Group 1C — CI coverage for v3 (item 3, F3).** Proof: the new target passes locally under both `MODEL=halos-only` and `MODEL=sage16`; CI green on the branch.

- `Makefile`: a `tests-horizontal-v3` target that runs `make MODEL=halos-only SIMULATION=mini-millennium-horizontal generate` and the build, then **only** the three fixture-backed C tests (`tests/unit/test_horizontal_v3_reader`, `tests/unit/test_horizontal_retention_budget`, and the package's `test_unit_horizontal_retention`, which Group 1B item 9 extends) and the two package Python tests (`simulations/mini-millennium-horizontal/_tests/integration/test_gap_retention.py`, `test_schema_conformance.py`), fails if any test other than the intentional `test_int_link_package_rejects_v3` reports SKIP (that one skips by design under the v3 package), and regenerates the caller's pair on every path including a failed build. `tests/unit/run_tests.sh` already accepts a list of test basenames (`:217-221`), so no selector is needed.
- `.github/workflows/ci.yml`: a second job invoking it (1–4 min, dominated by the unit compile). Do **not** run the whole `tests-integration` tier under a horizontal package — 39–42 core tests fail there by design (acceptance §13).
- `tests/README.md` and `.agents/skills/mimic-validation-and-qa/SKILL.md`: one line each naming the target.
- Restore the default pair at the end of the target so a developer's tree is left as found.

### 0.2 Pass 2 — Complex (design first, then delegate; one session, one commit per item)

These need the orchestrator's judgement before a subagent implements: a cross-file seam, a framework helper, a spec change, or a policy. Ordering: 2 → 1 → F1 → 16 → 17.

| Item | Design decision the orchestrator makes | Implementation and proof |
|---|---|---|
| 2 (F14) | Publish the slab row width from the reader: `int64_t slab_row_bytes` on `struct HorizontalRunInfo` (`reader.h:38-45`), filled by `open_run` from the same `sizeof` terms `load_slab` allocates (`read_horizontal_hdf5.c:2609-2627`); the driver's `horizontal_generation_footprint()`/`horizontal_retained_resident_bytes()` consume it and `horizontal_slab_row_bytes()` is deleted. The existing fixture-gated test (`test_horizontal_retention_budget.c:1128`, `test_slab_width_matches_the_real_v3_reader`) is itself a manual sum, so add an **independent** allocation check: the `MEM_TREES` category delta across one real `load_slab` (via the allocator's category totals) must equal `nhalos × slab_row_bytes` for the v3 fixture and for the micro-Uchuu v2 fixture | Touches `reader.h`, the driver, two tests, `tests/unit/test_stubs.c`. Proof: Group 1B suites, then the acceptance gate and the v2 gate |
| 1 | Admission policy for slabs the output path cannot emit. Recommended: in `horizontal_require_generation_fits()`, refuse before allocation when `nhalos > INT_MAX` **and** the snapshot is a requested output snapshot (the `TotHalosPerSnap` int bound, `src/io/output/util.c:65`), and `WARNING_LOG` at admission when `nhalos > MAX_HALO_ARRAY_SIZE` (the marshaller cap, `output_buffer.c:50-54`; the FoF `ngal` narrowing, `halo_evolution.c:157`), naming the bound and chunked slab streaming. Correct the acquire comment at `:1150-1157`. The existing `test_wide_slab_is_sized_before_load` (`:1117`) must be adjusted to a non-output snapshot or to expect the refusal, and the converter's wide-output sentence in `_print_topology()` (`scripts/convert/convert_trees.py`) must change in the same commit, since it now says the driver does not refuse a slab by width | Proof: `test_horizontal_retention_budget` plus the Group 1B suites; no gate impact (no real dataset reaches the bound) |
| F1 (decided, §0.3) | Spec, writer, validator, fixtures | `HORIZONTAL-HDF5-FORMAT.md` Version 3 storage-layout paragraph (`:500`) and its Errata table: chunk rows MUST NOT exceed 65,536; a producer SHOULD write `max(1, min(n_rows, 65536))`; record the owner ruling that this is producer layout, not consumer-visible structure, so no version bump. `hdf5_writer_v3.py:226-232, :261` write that shape; the validator at `:541-542` checks `1 ≤ chunk_rows ≤ 65536`; `scripts/convert/tests/test_hdf5_writer.py` chunk assertions updated; then run `tests/data/horizontal_v3/regenerate.sh` and `simulations/mini-millennium-horizontal/_tests/data/regenerate.sh`, confirm `du -sh` drops from ~130 MB to well under 1 MB, and re-run Group 1C's target and `make tests-converter`. Version 2's text (`:146`) stays frozen; the five real datasets keep their 65,536-row chunks and remain conformant |
| 16 (F19) + 5 + 35 | One helper `tests/framework/parity_gate.py`: a frozen `GatePackage` dataclass (vertical/horizontal package, a_list, evidence label, file range, `override_vertical_range`, expected `source_format`, `column_mapping_sha256`, `links_adjacent`, halo/forest/gapped-link/max-span counts, required free bytes, `models`, `schemes`) and a `ParityGate` class that captures HEAD once, exposes `stage_preconditions / stage_dataset_provenance / stage_run_files / stage_builds / leg(model, scheme) / stage_leg_verdicts / run()`, wraps worktrees in a context manager with `timeout=` on every subprocess and git stderr preserved, runs the comparator **from the worktree copy**, keeps explicit aborted-leg markers, and asserts `RunProperties/TimestepScheme == scheme` on both runs of every leg (item 5). Sibling `tests/framework/schema_conformance.py` with a `SchemaPackage` dataclass; the Slice 6 copy's WARN-on-missing-`catalog_field_metadata.inc` becomes FAIL like the four Slice 7 copies. Per-package files shrink to ≈25 and ≈10 lines. Port the v2 micro-Uchuu gate last, keeping its Stage 8 and adding the provenance stage, comparator re-check and first-match `TimestepScheme` guard it lacks | Proof: every package gate re-run on its dataset (five v3 gates one after another, then the v2 gate); the Slice 6 gate's four legs and the v2 gate's Stage 8 must be unchanged. Then correct `HANDOFF.md` F19 (the "recorded TimestepScheme cross-check" only exists from this commit on) |
| 17 (F20), reduced | Do **not** rewrite historical plans or acceptance records, and do not add a spec-table parser. Only: one structured constant in `scripts/convert/runtime_routes.py` from which `RUNTIME_NOTICE`, the argparse description and `_V3_STANDING_LIMITATIONS` are rendered (three strings from one place), and the *live* guides, skills and `scripts/convert/README.md` reduced to a pointer at `HORIZONTAL-HDF5-FORMAT.md#v3-runtime-support`, which stays the single source of truth. The two sampled packages keep their own "files 0–15" statements | Proof: `make tests-converter`, `make check-docs` |

### 0.3 Owner decisions (recorded 2026-09-29)

- **F18 — `test_physical_ranges`.** Decided: (B)+(A). Make the test read every partition (today the vertical packages pass only because it samples partition 0 of 8 for mini-Millennium and 1 of 16 for the sampled Millennium and mini-Uchuu routes; `tests/scientific/test_scientific.py:148-166`), then set the declared `Spin` ranges per package and the core `deltaMvir` range from a **scientifically justified bound with margin**, never from the sample maximum: the recorded extremes are −37.39…43.35 (mini-Millennium), −143.9…149.5 (Millennium 0–15), −165.6…228.1 and one `deltaMvir` at −4.77×10⁴ (mini-Uchuu 0–15), so "±43" or "±228" would still exclude recorded values. Cite acceptance §6/§13 in the YAML comment. Keep the check a failure. `range` has no consumer beyond `property_ranges.json` and this test, so widening has no hidden effect. This is Pass 2 work (it touches a core range and a shared test) and needs the whole-simulation extremes or a physical argument before the numbers are chosen.
- **F1 — fixture size.** Decided: option (a) as specified in §0.2, with the "≤ 65,536, SHOULD `max(1, min(n_rows, 65536))`" wording and an Errata ruling rather than a version bump, because the rule governs producer layout that the spec already forbids consumers from depending on (`:147`, `:503`) and the C reader makes no chunk assumption. The claim-check delegate rated this DROP on cost grounds; the owner's decision stands, and the "≤" form is what keeps the cost small (no existing dataset becomes non-conformant; production output for snapshots ≥ 65,536 rows is byte-identical).

### 0.4 Dropped after triage (recorded, not scheduled)

Kept here so a later reader knows they were considered. Findings 11, 21, 25, 28, 29, 30, 31, 32, 34, 36 and simplifications S6.1–5, 7–9, 11 — one line each in §4 and §6 with the reason.

### 0.5 Pass 1 record (2026-09-29)

Implemented as planned in §0.1, one Sonnet-class subagent per group under the orchestrator's diff review, then validated, panel-reviewed (§2; the panel's findings — the target's restore-on-failure gap, the pre-teardown assertion, and four P3s — were fixed before commit) and committed on `converter-generalisation-mode-b`. Item 9's optional converter-fixture mirror was not done: the unit test pins the driver behaviour directly, and whether the converter can emit that chain order was not established. Every group's proof passed with zero compiler warnings; the default `make tests summary`, the new `make tests-horizontal-v3`, `check-generated` on six package pairs, `check-format`, `check-docs` and `git diff --check` were green on the final tree, and the acceptance gate and the v2 gate were re-run after the commits.

| Group | Commit | What landed |
|---|---|---|
| 1A | see `git log` (subject "Correct runtime-status text, stale pointers and comments (review 1A)") | Items 6, 7, 8, 12 (20 sites), 13, 14, 18, 22, 23, 26, 37, S6.6, S6.10 and the `test_links.py` raw string — text, comments and metadata only; no converter test needed an assertion change |
| 1B | subject "Harden v3 reader diagnostics, driver tests and units default (review 1B)"; this commit also carries the 1A comment and message edits in the three files both groups touched (`read_horizontal_hdf5.c`, `horizontal_driver.c`, `generate_properties.py`) | Item 4: `horizontal_h5_peek_format_version()` reports which check failed and `open_run` names it for a file carrying `/schema` (v2 files never enter the branch). Item 19: thirteen new `OPEN_CASES` (each mutation-checked to fail on a wrong fragment). Item 27: `set_role()` aborts on an unknown role. Item 10: two fork-based refusal tests that assert the failure path's per-generation `Released snapshot K` lines and the reader's `Closed horizontal run` line, both logged after the work is done. Item 9: `test_next_progenitor_into_an_earlier_snapshot`. Item 24: `HorizontalGatherContext.snapshot_count` removed. Item 33: up-front over-cap guard in `ensure_fof_workspace_capacity()`. Item 20: `units` defaulted once at `catalog_by_name` construction, with a regression test that reproduces `Unknown unit label 'None'` against the old generator; generated output byte-unchanged for every package |
| 1C | subject "Run the v3 fixture battery in CI through tests-horizontal-v3 (review 1C)" | `tests-horizontal-v3` builds `halos-only`/`mini-millennium-horizontal`, runs exactly the three fixture-backed C tests and the two package Python tests, fails on any exit or any skip other than the by-design `test_int_link_package_rejects_v3`, then regenerates the caller's pair; a second CI job runs it; `tests/README.md` and the validation skill name it |

Findings closed by Pass 1: 3, 4, 6, 7, 8 (documented), 9, 10, 12, 13, 14, 18, 19, 20, 22, 23, 24, 26, 27, 33, 37, S6.6, S6.10. Remaining for Pass 2: 1, 2, 5 (with 16), 16, 17 and the owner-decided F1 and F18.

---

## 1. Executive summary

**The plan met its goal.** Mimic's horizontal reader and driver consume horizontal-HDF5 version 3 — gapped links, int64 indices, per-file `/schema` — and the acceptance gate holds on re-run: on the real complete mini-Millennium dataset (1,533,122 halos, 29,291 gapped descendant links) horizontal v3 output is bitwise identical per `UniqueGalaxyID` to the vertical `lhalo_binary` reader on all four `{halos-only, sage16} × {fixed, dynamic}` legs. The four Slice 7 route gates and the v2 micro-Uchuu gate (including its Stage 8 check against pre-plan output) are recorded green. Every vertical baseline is bitwise unchanged, and the v2 reader path takes exactly the v2 checks it took before.

**The code is sound.** Four independent reviewers read every changed line of the reader, the driver, the int64 seam and the packages/gates/docs; the driver and the highest-complexity reader function were read again by the lead reviewer; an independent claim-check then verified every item against the source. Findings that would have been P0/P1 — a leaked or double-freed retained generation, a horizon computed early, a chain resolved into a released generation, a silent int64→int narrowing, a cross-file HDF5 handle leak on a non-fatal path, a v2 behaviour change — were each looked for specifically and not found. Every allocation of a retained generation traces to exactly one release on both the success path and the `atexit` failure path. The build is warning-free under `-Wall -Wextra -Wshadow -Wformat-security -Wundef`; `make tests summary`, `check-generated`, `validate-modules`, `check-format`, `check-docs` and `git diff --check` all pass; the v3 unit battery passes under `SIMULATION=mini-millennium-horizontal`.

**What survives triage** (8 P2, all bounded; the rest P3 or dropped):

1. **Evidence that only runs by hand** — the 74-case v3 reader/retention battery, every v3 package test and every gate skip in default CI and nothing notices (3); no fixture pins a `NextProgenitor` that points to an earlier snapshot than its owner, which real data has 2,980 of (9); ~11 reader invariants lack a negative case (19).
2. **One accounting seam copied three times** — the slab row width lives in the reader, the driver and a test, and even the fixture-gated "drift guard" is another manual sum rather than a measured allocation (2). Separately, a slab above the output path's int32 bounds is admitted without a ceiling and swept before it can fail — reachable only by a dataset no one has, so P2 (1).
3. **Record and text that overstate** — the handoff says the v3 gates cross-check the recorded `RunProperties/TimestepScheme`; no harness does, though the dynamic legs remain sound on the input side (5). `VISION.md:90` promises the ceiling prevents memory exhaustion; it bounds admitted payload only (14). The converter still prints "the driver cannot carry state across a gap" (6, P3), twenty comments and metadata headers cite the demoted V3 draft (12, P3), and `mini-millennium-horizontal` records `LastFile = 0` for an eight-file dataset (7, P3).
4. **Duplication the plan accepted** — six harnesses of which ~6,700 lines are copies, with the hardening applied unevenly (16). The route-list duplication (17) is real but reduced to a small change: historical records keep their contemporaneous text.

**Both owner decisions are made** (§0.3): F18 becomes a Pass 2 item with justified ranges; F1 becomes a Pass 2 item under option (a).

---

## 2. Authorization status and review provenance

- **Drift audits:** performed per slice under Mode B by an independent drift auditor (`adacs/qwen`, with `hy3` earlier), recorded in each run's `run-report.md`; the PM's assessments accepted every slice. Two plan amendments (`aedded2f`, `3d52320f`) were owner-approved and are recorded in the plan. This review did not re-audit authorization and treats the accepted surface as the contract.
- **Review:** lead reviewer Claude Fable 5.1 with four delegated read-only reviewers (same model) by area (§3), each instructed to read whole diffs and cite `path:line`; every headline claim was re-verified by the lead against the source before acceptance.
- **Claim-check and triage (2026-09-29):** one read-only delegate, Codex CLI `gpt-6-astra` at high reasoning effort in the mechanical read-only sandbox with `approval_policy=never`, launched through the orchestrator's `delegate_jobs.py` (run `.orchestrator/runs/delegates-20260929-155441-5050`, label `01-codex-claim-check-review`, session `01a0ebba…`). It was asked only to verify each item against the cited lines and to judge value against complexity, not to review the code again. It returned a verdict for all 37 findings, 11 simplifications and every disposition row, with evidence for each ADJUST/REMOVE; its output satisfied the requested section contract. The lead reviewer checked the corrections it proposed against the source (the `_print_topology` call sites, the wide-admission and real-reader tests, `USER-GUIDE.md:449`, `memory.c:201`, `generate_properties.py:507`, the driver comment at `:1137-1138`, the `regenerate.sh` shebangs, `ci.yml:12,26`) and adopted them; where this document departs from the delegate's triage (items 24 and F1) it says so.
- **Independent panel on Pass 1 (2026-09-29):** two read-only delegates through the same launcher — Codex CLI `gpt-6-sol` at medium effort (read-only sandbox) and OpenCode `opencode-go/qwen3.8-flash` at xhigh effort (plan agent) — reviewed the uncommitted Pass 1 diff in parallel against §0.1 and §4 (run `.orchestrator/runs/panel-20260929-170437-60375`). Both returned PASS WITH RISKS with no correctness finding; both found the same P2 (the `tests-horizontal-v3` target did not regenerate the caller's pair when its own build step failed), Codex found that the two new refusal tests asserted a line logged before teardown, and the P3s were two redundant generate lines in the target, a leftover `units` fallback in the generator, the untracked review document, and two inaccuracies in this document's Group 1C text. Every finding was verified against the source and fixed before commit; the post-teardown evidence the tests now assert is the per-generation `Released snapshot K` line and the reader's `Closed horizontal run` line.
- **Contract defects noted** (§5): the D1 record's "v2 checks and messages unchanged" is not literally true of the messages; the plan's TOC omits its own Outcome section.

## 3. Scope, method and validation

**Scope.** Every non-binary file in `git diff 87aa9a86..HEAD` (147 files), split by risk:

| Area | Files | Reviewer emphasis |
|---|---|---|
| v3 reader | `src/io/horizontal/read_horizontal_hdf5.c` (+1,518), `reader.h`, `tests/unit/test_horizontal_v3_reader.c` (1,691), fixture provenance | spec conformance, HDF5 resource handling, untrusted input, HDF5 1.10 API, negative-test map |
| Driver and memory | `src/core/horizontal_driver.c` (1,641), `galaxy_pool.*`, `types.h`, `proto.h`, `read_parameter_file.c`, `run_profile.*`, four C tests, `test_gap_retention.py`, `test_processing_order.py` | lifetime trace, horizon/release, progenitor resolution, gap-spanning `dT`, int64 arithmetic, ceiling honesty |
| int64 seam | `build_model.c`, `halo_evolution.c`, `inheritance.*`, `output_buffer.*`, `virial.c`, vertical `hdf5.c`, eight sage16 files, `generate_properties.py`, `check_generated.py`, generated headers, seam tests | survivor audit of every `int` index, format specifiers, `RawHalo` ABI, generator type rules |
| Packages, gates, converter, docs | five `simulations/*-horizontal/`, six gate harnesses, five schema tests, `scripts/convert/` diff, format spec, guides, skills, READMEs | metadata diff vs vertical packages, gate fail-not-skip and pinning, claims audit, duplication measurement |

**Structural evidence.** `code-health` (Lizard 1.23.0 for cyclomatic; stdlib lexical analysis for composition and duplication) differential against `87aa9a86`: `open_run_horizontal_hdf5()` measured cyclomatic 58 (+23), `horizontal_h5_v3_read_schema()` 23 and `horizontal_h5_v3_link_invalid()` 22 new; the reader grew by 1,026 code lines; new 4–6-way exact duplicate blocks of 56–114 lines across the Slice 7 harnesses and schema tests. No new include cycle. The metric artifact lives in the review session's scratch space, not the repository; the linear shape of `open_run` was confirmed by reading it (§6).

**Validation run for this review** (sequential; logs under `archive/test-logs/review-20260929/`, gitignored):

| Step | Result |
|---|---|
| `make info`, `make -j8` (default pair) | clean; **0 compiler warnings** |
| `make check-generated`, `make validate-modules` | exit 0 |
| `make tests summary` (default pair) | `ALL TESTS AND CHECKS PASSED`; 0 FAIL/ERROR; 14 SKIP (nine v3 reader tests: "matches only SIMULATION=mini-millennium-horizontal"; four horizontal-driver tests: package not horizontal; one needs process isolation) |
| `make check-format`, `make check-docs`, `git diff --check` | exit 0 |
| `MODEL=halos-only SIMULATION=mini-millennium-horizontal generate validate-modules check-generated` | exit 0; `snapshots` symlink resolves |
| same pair, `tests-unit` | `ALL UNIT TESTS PASSED`, 0 warnings; the v3 reader and retention tests ran (3 unrelated SKIPs) |
| same pair, `tests-scientific` (the acceptance gate) | all four legs PASS; `all 4 parity legs PASS`; tier exits 2 only on `test_physical_ranges` (F18) |
| restore default build; `git status --short` | empty |

Not re-run here: the four Slice 7 gates and the v2 micro-Uchuu gate (7 min); their `6cd0c448`/`7900080c` records in the acceptance document were read and are internally consistent with the package READMEs.

---

## 4. Findings

Ordered by severity; numbering is stable across revisions so §0 can refer to it. Line numbers are at `cd5bbde4`. No P0. No P1: the two items proposed at P1 by a reviewer (5 and 6) are rated below because neither causes a failure of that severity.

### P2

1. **`src/core/horizontal_driver.c:1150-1157`, `:1033-1075` — a slab wider than the output path's int32 bounds is admitted without a ceiling and swept before it can fail.** The acquire comment says "the only int32 bound on a slab is the format's own". The bounds that remain are on *emitted records and FoF workspaces*, not on raw slab width: `output_increment_halo_counters_checked()` FATALs when `TotHalosPerSnap` (an `int`) reaches `INT_MAX` for an output snapshot (`src/io/output/util.c:65`); the marshaller refuses growth past `MAX_HALO_ARRAY_SIZE` = 10⁹ (`src/core/output_buffer.c:38-54`; for a slab already past it the seed is `nhalos + 1000`, driver `:941-944`, so more than 1,000 carried orphans FATAL mid-marshal); `process_halo_evolution()` narrows a FoF group's `ngal` to `int` (`halo_evolution.c:157`). All are loud and none is silent, and failure is likely rather than certain above 10⁹ rows — but with no `retention_memory_ceiling_mb` such a snapshot is sized, loaded and swept for hours first. *Reachability:* needs a v3 dataset with a snapshot above 10⁹ rows; none exists (full Uchuu was never converted), which keeps this at P2. *Fix:* the qualified admission policy in §0.2 (refuse output snapshots above `INT_MAX`; warn above `MAX_HALO_ARRAY_SIZE`), the comment corrected, and `test_wide_slab_is_sized_before_load` (`tests/unit/test_horizontal_retention_budget.c:1117`) adjusted — a policy change, so it is Pass 2 rather than a blanket cap.

2. **`src/core/horizontal_driver.c:929-935`, `tests/unit/test_horizontal_retention_budget.c:299-305`, `src/io/horizontal/read_horizontal_hdf5.c:2609-2627` — the slab row width is hand-copied three times, and no test measures a real allocation (F14).** The reader allocates `sizeof(struct RawHalo)` + 2×int64 + (v3) 3×int32 + int64 per row; the driver restates it; the ungated test restates it with the same arithmetic. The fixture-gated `test_slab_width_matches_the_real_v3_reader` (`:1128`) is also a manual reconstruction of bytes, and it skips under the default build (finding 3). A fourth reader-owned array, or a widened target column, would leave the ceiling and resident-bytes accounting silently wrong while every test still passed; the v2 width is never checked against the real reader at all. *Fix:* §0.2 item 2 — the reader publishes the width on `struct HorizontalRunInfo`, the driver consumes it, and an independent check compares the allocator's `MEM_TREES` delta across a real `load_slab` for both the v3 and the v2 fixture.

3. **`.github/workflows/ci.yml:36-67`, `tests/unit/test_horizontal_v3_reader.c:111-117,1042`, `test_horizontal_retention_budget.c:1052`, `Makefile:759,800-801` — nothing in the repository runs the v3 battery or any v3 package test; default CI reports them as SKIP (F3).** Every v3 reader case but `test_int_link_package_rejects_v3` (58 open-time, 16 load-time) and the retention width test skip unless `MIMIC_COMPILED_SIMULATION == "mini-millennium-horizontal"`; `make tests` passes the configured `SIMULATION`, whose default is `mini-millennium`; `check-horizontal-fixture` checks only the micro-Uchuu v2 fixture; package `_tests/` are registered only for the selected package. The battery passed when this review selected the package by hand, so the code is fine — the evidence just does not run unattended, and fixture conformance alone cannot replace it. *Fix:* §0.1 Group 1C; the target must reject any skip other than the intentional int-link one.

5. **`HANDOFF.md:159` — the "cross-check the dynamic legs against recorded `RunProperties/TimestepScheme`" hardening is recorded as applied to the v3 gates; no harness implements it.** Mimic writes the attribute (`src/io/output/metadata_hdf5.c:664-668`), but in all six `test_cross_format_identity.py` files every `TimestepScheme` mention is the `dynamic_variant` writer or its first-match guard (e.g. `mini-millennium…:815-828`), and `PREFLIGHT_ATTRS` (`:134-141`) reads six other attributes only. The acceptance record's "timestep scheme pinned" (`:3`) is supported by that input-side guard and stands; the handoff's claim of an output-side cross-check does not. *Why P2, not P1:* the dynamic legs remain sound — the harness guarantees exactly one `TimestepScheme: dynamic` key, Mimic rejects unknown keys, and the mini-Millennium `sage16` legs show the scheme's physical effect (36,530 vs 36,515 galaxies at snapshot 63). *Fix:* the output-side assertion in every leg, delivered by the §0.2 harness helper; then correct HANDOFF F19.

9. **`simulations/mini-millennium-horizontal/_tests/unit/test_unit_horizontal_retention.c` — no fixture pins a `NextProgenitor` whose target snapshot is earlier than its owner's (F9).** The worked graph A(0)→B(1) and the three-snapshot chain P0(0)→P1(1)→P2(2) are later-than-owner (`:184`, `:342-344`); `test_same_snapshot_next_progenitor` (`:418`) is same-snapshot. The driver path that resolves `target_snap < owner->snapnum` (`horizontal_next_progenitor`, `:375-392`) is exercised only by the real-data gates (2,980 such links in mini-Millennium). A scientifically relevant ordering protected by one synthetic case. *Fix:* §0.1 Group 1B.

14. **`docs/VISION.md:90`, `src/core/horizontal_driver.c:1596-1601` — the ceiling is overclaimed (F16).** VISION: "an optional run-file ceiling refuses a generation before allocation rather than letting the run exhaust memory" and "The bound depends on the widest slab and the longest gap". Per `:1023-1076` and the one-time warning, the ceiling bounds admitted payload only; in-sweep output-buffer and pool growth, the driver's workspace and scratch buffers (`:254-301`), the reader's run tables and process RSS are outside it. `USER-GUIDE.md:445-449` and `DEVELOPER-GUIDE.md:1072` state this correctly, so the vision document is the one place a reader planning memory would be misled. *Fix:* one clause in each (§0.1 Group 1A).

16. **Six parity harnesses (8,705 lines, ~6,700 identical by line diff) and five schema tests carry the plan's hardening unevenly (F19).** Measured against `mini-uchuu-horizontal`'s harness: `micro-uchuu-lhalo` 1,430/1,439 identical lines, `micro-uchuu-hdf5` 1,428, `millennium` 1,428, `mini-millennium` 1,068/1,296, the v2 `micro-uchuu` gate 653/1,653. The four Slice 7 copies differ only in one parameter block (`:86-127`). The Slice 6 harness lacks Slice 7's horizontal-side file-range check and forest count, and its schema test returns WARN on a missing or foreign `catalog_field_metadata.inc` (`mini-millennium…/test_schema_conformance.py:146-150,226-234`) where the four Slice 7 copies FAIL (`7900080c`, not back-ported). The v2 gate lacks the dataset-provenance stage, the comparator re-check entirely, the `TimestepScheme` first-match guard, the recorded `TreeName/FirstFile/LastFile/SimulationDir` cross-check and a leg-verdict stage (`abort_on_failure=True`, `:1643-1647`, so later legs go unreported). Worktrees are created at a captured commit, but the comparator is imported from the working tree with a byte-equality check rather than run from the worktree copy. No subprocess carries `timeout=`; a `git worktree add` failure drops git's stderr; `SystemExit(128+signum)` escapes `run_test_suite` (`tests/framework/runner.py:84-88`) so an interrupt emits no marker. All six fail-not-skip correctly, none uses `cp` or `shell=True`, and the comparator is unchanged since `87aa9a86`. *Fix:* §0.2 item 16.

19. **`tests/unit/test_horizontal_v3_reader.c` — open-time invariants the reader enforces without a negative case.** `ForestIndex` outside `[0, n_forests_total)` and `n_forests_total` disagreeing with the measured maximum (`read_horizontal_hdf5.c:2463-2467`, `:2491-2497`); `HaloRankInForest < 0`; the empty-dataset sentinel with halos present and `n_forests_total = 0` with halos (`:2417-2426`); a `/schema` member that is a dataset (`:1250`); a schema attribute with the right count but a wrong name (`:1112`); an ASCII vlen schema attribute (`:1126`); `column_mapping_sha256` of the wrong fixed length (`:1001-1002`); a non-NUL-padded or non-printable header string (`:1023-1036`); a missing snapshot file (`:2254-2258`). (The soft-linked `/header` case belongs to finding 4; "an a_list shorter than the file set" is not an invariant — the reader enumerates only the configured snapshots, `:2231,2250`.) The battery's coverage is otherwise thorough: every `OPEN_CASES` (`:1290-1401`) and `LINK_CASES` (`:1524-1561`) entry maps to a spec invariant. *Fix:* §0.1 Group 1B.

### P3 (scheduled in §0)

4. `src/io/horizontal/read_horizontal_hdf5.c:2270-2281`, `:818-850` — a v3 dataset whose snapshot 0 has a malformed `format_version` carrier (soft-linked `/header`; attribute absent, non-scalar or int64) is diagnosed as a v2 object-set error, because the failed peek defaults the dataset to version 2 (F6, generalised). The input still fails; only the diagnostic misleads. A v2 file with a soft `/header` was accepted before and still is, so the fix stays on the v3-detect side (§0.1 Group 1B).

6. `scripts/convert/convert_trees.py:349-351`, `:356-357` — `_print_topology()` (called at `:370`, `:630`, `:655`) still prints "the current horizontal driver cannot carry state across a gap" (false since Slice 4; not in the follow-up register) and "the current horizontal driver refuses such a slab" above `INT32_MAX` (F21; false — only the int64 byte-count overflow and the optional ceiling refuse). Both share stdout with the corrected `RUNTIME_NOTICE`. Text only (§0.1 Group 1A), worded consistently with findings 1 and 13.

7. `simulations/mini-millennium-horizontal/simulation_info.yaml:17` — `last_file: 0` for a dataset converted from files 0–7, so every output records `RunProperties/LastFile = 0` (`metadata_hdf5.c:562-563`); the Slice 7 packages carry their converted ranges and their gates assert both sides record it, the Slice 6 gate checks the vertical side only (`:921-953`). Provenance only; no output value depends on it (§0.1 Group 1A, and the horizontal-side assertion via §0.2 item 16).

8. `scripts/generate_properties.py:1000-1007` — a package cannot declare a `double` catalog field although `HORIZONTAL-HDF5-FORMAT.md:367-374` says every `/schema` type is package-declarable (F5). `_read_type_for_catalog()` raises for `double`, and the generator runs for every package; the vertical HDF5 reader has no double read mode (`src/io/vertical/hdf5.c:51`) and the horizontal fill path (`read_horizontal_hdf5.c:1592`) has no double memory-type mapping either. No committed package declares `double`. Document the limitation now (§0.1 Group 1A); implement only when a package needs it.

10. `src/core/horizontal_driver.c:1175-1179`, `:1193-1197` — two defensive FATALs are unreachable through the real reader (both figures come from `SNAP.halo_counts`; the reader's `target_snap >= SNAP.snapshot_count` check at `read_horizontal_hdf5.c:2046` preempts the horizon abort) and untested (F17). Both release correctly by inspection. Two fake-reader cases with cleanup assertions (§0.1 Group 1B, after 27).

12. Twenty comments and metadata headers cite the demoted `HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md` as the contract (F22, F25): the sites listed in §0.1 Group 1A. The stub says it specifies nothing (`V3-DRAFT.md:5`) but its section map keeps every pointer functional, so this is P3; metadata headers matter because metadata is structural source code.

13. `docs/dev/HORIZONTAL-HDF5-FORMAT.md:524`, `scripts/convert/report.py:333-338` — "more than two generations" stated as a certainty (F24); the true statement is a bound of the longest descendant span plus one (`horizontal_generation_horizon()`, `:844-860`). The two skills were corrected in `cd5bbde4`; these two were not (§0.1 Group 1A).

17. The evidenced-route list is repeated in roughly thirty places (F20): `convert_trees.py` (`:129-138`, `:791-794`, `:44-53`), `report.py:310-325`, the converter tests, the spec's V3 Runtime Support table (`:506-522`), the guides, the pathway, the plan, the acceptance record, five skills, `scripts/convert/README.md:14-22`, the sampled packages' READMEs and YAMLs, three vertical READMEs. Historical plans and acceptance records must keep their contemporaneous text, and a spec-table parser would add maintenance, so the fix is reduced to the converter's three strings from one constant plus pointers in the live documents (§0.2 item 17).

18. `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md:186` still describes the root `HANDOFF.md` as the converter-generalisation run's; the plan's Outcome (`:24`) defers every open follow-up to that gitignored file (`.gitignore:71`; naming it is allowed by `STYLE-GUIDE.md:207`, but the committed record then holds none of F1–F25); the plan TOC (`:28-37`) omits `## Outcome` (§0.1 Group 1A).

20. `scripts/generate_properties.py:1054-1062` — `generate_catalog_field_metadata_inc()` raises `Unknown unit label 'None'` for a catalog field without `units` although `normalize_catalog_contract()` accepts it at `:505-507` (F4). Loud and latent (all fifteen packages declare `units`). Default `units` once where `catalog_by_name` is built (`:507`) and add the regression case (§0.1 Group 1B).

22. `src/io/horizontal/read_horizontal_hdf5.c:2272-2287` — the message a v2 dataset gets for an unsupported or mixed version changed wording (§5, D1): before, "supports only version 2" for every file; now "supports only versions 2 and 3" at snapshot 0 and "snapshot 0 declares version N; a dataset never mixes format versions" later. The v2 test pins only `'format_version' is 99` (`micro-uchuu-horizontal/_tests/unit/test_unit_horizontal_reader_open.c:1062`). Checks unchanged, messages better; record it (§0.1 Group 1A).

23. `src/core/horizontal_driver.c:338-341` — the "released before its retention horizon" abort attributes a malformed input to a driver bug: the reader checks `NextProgenitorSnapshot < owner's DescendantSnapshot` (`read_horizontal_hdf5.c:2086-2092`) but cannot check that a chain member's own `DescendantSnapshot` equals the owner's, so such a file aborts here with the wrong explanation. Reword only (§0.1 Group 1A); any topology enforcement is separate design work.

24. `src/include/types.h:257` `HorizontalGatherContext.snapshot_count` — written at `horizontal_driver.c:1553` and in two test files, never read (F10). The claim-check rated removal low value; it is kept in Group 1B because the same commit already touches both test files and a dead field in a public struct misleads the next reader.

26. `src/core/main.c:471` — "reports its own two pools" is stale (F15): pools are created lazily and recycled through a spare stack (`horizontal_driver.c:1109-1122`), harvested at teardown (`:1323-1335`). Retention is reported by the profile's term R (`run_profile.c:138-144`), the right home; fix the comment only (§0.1 Group 1A).

27. `tests/unit/test_horizontal_retention_budget.c:158-165` — `set_role()` silently no-ops on an unknown role (F17). Not vacuous today (all eight roles are provided by mini-Millennium, and the byte figures and lifecycle lines asserted do not depend on `Len`/`HaloMass`/`SnapNum`), but a `matched` flag with `abort()` prevents future vacuous coverage (§0.1 Group 1B, first).

33. `src/core/build_model.c:278-301` — `ensure_fof_workspace_capacity()` reallocs to `MAX_HALO_ARRAY_SIZE` (10⁹ × 184 B) before it can report an over-cap request; an up-front guard gives the same outcome without the futile allocation (§0.1 Group 1B).

35. Harness robustness: no `timeout=` on any subprocess, git stderr dropped on `worktree add` failure, no marker on an interrupt (see 16). Builds are sequential and the range-override rewrite preserves line count, so the earlier "concurrent build" and "latent IndexError" notes are withdrawn. Folded into §0.2 item 16.

37. Acceptance `:79`, `:260` — "the plan schedules its update for Slice 8" is accurate as a record; append "(done in `73149ca6`)" (§0.1 Group 1A).

### Dropped after triage

- 11 (F12): with no ceiling, an allocator failure carries no retention context — `mymalloc_cat` reports MB only (`memory.c:201`) and the driver's size line is VERBOSE. Real, but the `--verbose` line and the failure handler already exist; extra failure-state bookkeeping is low value.
- 21 (F23): `sham_assign_stellar_mass.c:87` `(uint64_t)(uint32_t)halo->HaloNr` aliases for `HaloNr ≥ 2³²` on the `UniqueGalaxyID == 0` fallback path only. The Slice 1 widening changed no existing seed (identical bits below 2³²). The proposed `(uint64_t)HaloNr` is bit-identical only for non-negative values; a negative `HaloNr` reaching this path would change, so the fix is not proven safe without more work, and no gated route is affected. Recorded residual.
- 25 (F17): `HORIZONTAL_BYTES_PER_GB` duplicates `run_profile.c:18`'s private constant; exporting it couples two files to remove one harmless literal.
- 28 (F13): the 12 B allocator-rounding undercount for odd `nhalos` is already disclosed at `USER-GUIDE.md:449`.
- 29: a FATAL inside the `atexit` teardown would nest `exit()` (C11 7.22.4.4); the state is detached first (`:1379`) so no double free can follow, `bye()` has the same property, pre-existing; documentation would not resolve it and redesign is out of proportion.
- 30: `horizontal_h5_member_count()` (`:1264`) names the bare member rather than `/schema/<name>` in a rare HDF5 failure; the file and member are already identified.
- 31: a false `links_adjacent = 1` is caught at the first gapped slab load (`:2061-2067`), spec-conformant and already documented as a load-time check.
- 32: pre-existing unchecked `(int)` narrowings in the Consistent-Trees vertical readers (`read_ctrees_ascii.c:708`, `read_ctrees_hdf5.c:1893`, `ctrees_utils.c:737`); vertical-only, unreachable for int-linked packages, outside this plan's scope.
- 34: withdrawn — `tests/data/horizontal_v3/regenerate.sh` and the package `regenerate.sh` are `#!/bin/bash` scripts run non-interactively, where aliases are not expanded, so the handoff's interactive `cp -i` hazard does not apply to them. Pinning the converter commit in a header comment is optional policy, not a defect.
- 36: `HORIZONTAL-HDF5-FORMAT.md:9` vs `:172` banner wording; clear in context.

---

## 5. Contract defects

- **D1 (Slice 2), plan `:111`, `:125`.** R0-1(a) says "v2's validation path byte-for-byte unchanged" and the PM resolved D1 as "v2 checks and messages unchanged". The checks are unchanged; the version-rejection messages are not (finding 22). The coherent reading, and the one the implementation took, is "v2's validation *checks* unchanged: every v2 file is still accepted or rejected exactly as before, with more informative version messages". Record that wording (§0.1 Group 1A); do not revert the messages.
- **D2 (Slice 4)** — "released exactly when its horizon has been processed" vs "retains exactly two generations" — was reworded in `3d52320f` to "never retains more than two generations"; the implementation matches (adjacent input: horizon = N+1, so at most two live; `horizontal_driver.c:844-860`, `:1487-1489`).
- **Plan `:28-37`** — the TOC does not list the Outcome section added in `cd5bbde4` (cosmetic; §0.1 Group 1A).
- None rates as a finding against the implementers.

---

## 6. Simplification pass (behaviour-preserving)

Each kept item changes how the code says what it does, never what it does, and names the check that proves it. The end result trumps pre-implementation decisions, but the triage bar was value against risk, and most candidates did not clear it.

**Kept (scheduled in §0.1 Group 1A):**

- **S6.6** `src/core/horizontal_driver.c:1137-1138` — the acquire comment says "the seed never enlarges a slab already past `MAX_HALO_ARRAY_SIZE`" while `horizontal_output_seed_capacity()` (`:941-944`) adds `MIN_HALO_ARRAY_GROWTH` for such a slab. Correct the comment only; dropping the increment would change behaviour (and finding 1 may make the branch unreachable).
- **S6.10** `tests/unit/test_index_width.c:47` — one comment recording why `IS_SIGNED_64` probes width and sign rather than using `_Generic`: `struct Halo.HaloNr` is `long long`, which is not `int64_t` on Linux/glibc, so a "consistency" rewrite would break there.

**Considered and dropped:**

- S6.1 Extracting the per-file loop body of `open_run_horizontal_hdf5()` (`:2220-2525`, measured cyclomatic 58): the function is a linear validation sequence — a validator's honest shape — and extraction alone buys little.
- S6.2 Merging the v2 and v3 `*_validate_object_set`/`*_reject_unknown_attr` pairs (`:464-515`/`:906-950`, `:446-461`/`:953-973`): the v3 versions also add helper diagnostics and string-attribute validation, so the merge is larger than "one hard-link call and one name" and exposes the frozen v2 path to regression for ~60 lines.
- S6.3 Hoisting `desc_snaps` and removing the double evaluation of `horizontal_h5_v3_link_invalid()` (`:2117-2169`): the first diagnostic must be preserved per field and then overall, so the simple "copy on `count == 0`" is incorrect; minimal payoff.
- S6.4 Replacing `horizontal_h5_fill_identity()`'s v2-table lookup with literals (`:1720-1728`): the lookup is shared and its invariant documented; churn.
- S6.5 Extracting `run_horizontal_driver()`'s loop body, turning the derived live-slab count into a FATAL and removing two of the three `horizontal_clear_output_globals()` calls: the failure-path clearing at `:1266` matters when a FATAL inside output writing bypasses the normal clear, and a new FATAL changes behaviour; the extraction alone is not worth a driver diff.
- S6.7 Duplicate of finding 16.
- S6.8 `role_kinds.get(role, "") if role else ""` (`:1062`) is harmless; the units defaulting belongs to finding 20 as a behaviour fix, not a cleanup.
- S6.9 A generic grow-to-`required` helper for the four scratch growers: four short typed helpers do not justify an abstraction.
- S6.11 Trimming comments at `read_horizontal_hdf5.c:1404`, `:2595-2596`, `:2617-2619` and the thirteen `prepare_output_dir()` assertions in `test_horizontal_retention_budget.c`: the cited comments explain width and ownership decisions rather than restating code.

---

## 7. Follow-up register and PM notes — disposition

Every item from `HANDOFF.md` §5 and the PM's run notes, with the review's verdict after triage. "Pass 1/2" refers to §0.

| Item | Verdict | Severity | Disposition |
|---|---|---|---|
| F1/F11 fixture size | Confirmed and corrected: the writer uses fixed `(65536,)`/`(65536, 3)` chunks with **incremental** allocation (`ALLOC_TIME_INCR`, not early), so the first row written to any dataset materialises a full 65,536-row chunk; ~130 MB of checkout for a few hundred rows of fixture (`hdf5_writer_v3.py:226-232, :261`); the proposed `min(n, 65536)` violates spec `:500` and validator `:541-542` as written | P3 | **Decided by the owner: option (a)**, §0.3 and §0.2 — "≤ 65,536 / SHOULD `max(1, min(n_rows, 65536))`", Errata ruling, regenerate fixtures. Pass 2 |
| F2 v3 little-endian check | Confirmed absent; reads go through native memory types so a big-endian file is read correctly; the spec's consumer minimum does not list it | — | Won't-fix |
| F3 no CI target | Confirmed (finding 3); the intentional `test_int_link_package_rejects_v3` skip under the v3 package must be allowed while every other v3 case must execute | P2 | Pass 1 Group 1C |
| F4 unit-less catalog field | Confirmed (finding 20; normalisation at `:507`) | P3 | Pass 1 Group 1B |
| F5 `double` catalog field | Confirmed (finding 8); an implementation must also add the horizontal memory-type mapping at `read_horizontal_hdf5.c:1592` | P3 | Document now (Pass 1 Group 1A); implement when needed |
| F6 soft-linked `/header` | Confirmed and generalised (finding 4); diagnostic only | P3 | Pass 1 Group 1B |
| F7 consumer minimum | The reader enforces exactly the spec's consumer-minimum paragraph (`:486`) plus its "at open" bullet (`:490`), each ticked to a line in §8; `Len ≥ 0`, `SourceHaloID` order/uniqueness, identity density and chain topology are producer duties at `:496` | — | Won't-fix; the trust boundary is the spec's and is documented (Residual risk 4) |
| F8 gather has no cycle guard | Confirmed; safe because `count` walks the same immutable chain immediately before and sizes the scratch (`:575`, `:584-585`); the vertical originals have no guard either; documented at `:492-493` | — | Won't-fix; the invariant is the protection |
| F9 earlier-snapshot `NextProgenitor` fixture | Confirmed missing (finding 9) | P2 | Pass 1 Group 1B |
| F10 dead `snapshot_count` | Confirmed (finding 24; two test files, not three) | P3 | Pass 1 Group 1B (kept despite the claim-check's DROP; same files are open) |
| F12 no context on allocator failure | Confirmed (dropped item 11; `memory.c:201` reports MB only) | P3 | Dropped |
| F13 12 B rounding | Confirmed; already disclosed at `USER-GUIDE.md:449` | — | Dropped |
| F14 row-width duplication | Confirmed, worse than stated (finding 2): even the real-reader test is a manual sum | P2 | Pass 2 item 2 |
| F15 memory brief / stale comment | Comment confirmed (finding 26); the brief is the wrong home | P3 | Pass 1 Group 1A (comment only) |
| F16 ceiling scope wording | Confirmed (finding 14), rated with it | P2 | Pass 1 Group 1A |
| F17 untested FATALs, GB constant, `set_role` | Tests and the fail-fast fixture kept (findings 10, 27); the constant export dropped (25) | P3 | Pass 1 Group 1B |
| F18 `test_physical_ranges` | Confirmed: mini-Millennium samples 1 partition of 8, the sampled Millennium/mini-Uchuu routes 1 of 16 (`test_scientific.py:148-166`); both micro-Uchuu packages pass the whole tier; `range` has no consumer beyond `property_ranges.json` and this test | — | **Decided by the owner: (B)+(A)** with scientifically justified ranges with margin, §0.3. Pass 2 |
| F19 harness duplication | Confirmed and measured (finding 16) | P2 | Pass 2 item 16 |
| F20 route-list duplication | Confirmed (finding 17); reduced scope | P3 | Pass 2 item 17 |
| F21 stale `_print_topology` | Confirmed, plus an unregistered sibling at `:349-351`; three call sites (finding 6) | P3 | Pass 1 Group 1A |
| F22 stale draft citations | Confirmed at the four sites and sixteen more — 20 in all (finding 12) | P3 | Pass 1 Group 1A |
| F23 SHAM seed cast | Confirmed; no interaction with Slice 1; the proposed fix is not proven bit-identical for negative values (dropped item 21) | P3 | Recorded residual |
| F24 "more than two" certainty | Confirmed at `report.py:336` and spec `:524` (finding 13) | P3 | Pass 1 Group 1A |
| F25 converter docstring | Confirmed (`convert_trees.py:5`), within finding 12 | P3 | Pass 1 Group 1A |

**PM run-notes items** (`.git/pm/<run>/notes.md`, the three runs of this plan) not already in the register:

| Note | Assessment |
|---|---|
| Settled rulings: producer duties (F7), endianness (F2), CI target (F3), `double` (F5), unit-less metadata table (F4) | Dispositions above; none reopened |
| HDF5 1.10 API rule (no `H5O_info2_t`, `*3`, `H5Literate2`, tokens) | Verified by inspection of every HDF5 call in the reader: `H5Lget_info`/`H5L_info_t`, `H5Oopen`/`H5Iget_type`, `H5Aiterate2`, `H5Aexists_by_name`, `H5Tis_variable_str`, `H5free_memory` — spellings present in 1.10; no numbered-version names. CI (`ci.yml:12,26`) installs an unpinned `libhdf5-dev` on `ubuntu-latest`, so it tests whatever that ships, not 1.10 specifically |
| Later-slice trap: converter CI test rejects `converter_columns` in `models/*/input/*{millennium,uchuu}*.yaml` | Honoured: no run file names a profile (acceptance §15) |
| Later-slice trap: `DEVELOPER-GUIDE.md:983` still describes int links and the INT_MAX refusal | Fixed in Slice 8: `:981` now describes `long long` links, `int64_t` accessors, the removed refusal and the checked narrowing |
| Slice 8: do not claim `sage16` on Slice 7 routes; full Uchuu not runnable | Verified across guides, spec table, converter README, `RUNTIME_NOTICE` and skills — no overclaim. The remaining wording corrections are findings 6 (converter topology text), 13 (retention certainty) and 14 (`VISION.md:90` ceiling) |
| Shell hazard `cp -i` | Recorded correctly for the Developer's interactive shell; it does not apply to the non-interactive `regenerate.sh` scripts (dropped item 34), and no harness uses `cp` |
| Evidence methods: Stage 8 re-anchored to `aedded2f`, `source_md5` exempted | The v2 gate diff since `87aa9a86` touches only `BASELINE_COMMIT` and the `.source_md5` exemption — verified |
| Formatter is black + isort, not ruff | `make check-format` passed in this review's validation run; the `code-health` run surfaced a pre-existing `SyntaxWarning` (invalid `\(` escape) at `scripts/convert/tests/test_links.py:1030` — a raw string fixes it (P3, outside the plan; add to Group 1A if convenient) |
| Residual: SHAM cast | F23 above |
| Reviewer notes (handoff §6) | Not a code matter; the model-performance record stands as written and was not evaluated here |

**PM end-of-run documentation pass** (`cd5bbde4`): all nine items landed — mini-Millennium README `:59` and `:12`; `halo_properties.yaml:7-8` pointer; schema-test docstring `:26-29`; acceptance title and `:3`; `DEVELOPER-GUIDE.md:1069`; spec `:261`; converter README `:22`; both skills' "gapped holds more" wording; plan status and Outcome. The same certainty the skills lost survives in spec `:524` and `report.py:336` (finding 13). `git diff 87aa9a86..HEAD -- docs/dev/HORIZONTAL-HDF5-FORMAT.md` is 342 insertions and exactly one deletion (the "Non-normative pointer" paragraph), so version 2's text is unchanged as D5 required.

---

## 8. Coverage summary

- **Scope reviewed:** all 147 changed files; the reader, driver and `open_run` read in full; the seam files' every changed line; all five packages diffed against their vertical counterparts (only justified differences: `long long` links, dropped padding fields and `source:` keys, description text, and finding 7; **no `range:` line differs**, so every declared range mirrors the vertical package, including mini-Uchuu `Spin` after `7900080c`; cosmology, box, particle mass and a_lists byte-identical); all six harnesses and five schema tests; the converter diff (text-only, `runnable_by_current_mimic` stays a boolean with the same key); the spec, guides, skills, READMEs and the pathway. Every item then claim-checked independently (§2).
- **Requirements checked against:** the plan's Preservation rules 1–7 (no synthetic halo; no gap removal; inheritance time from the source's own `SnapNum` — verified at `output_buffer.c:60`, `halo_evolution.c:107-109`, `inheritance.c:168`, `horizontal_driver.c:512`; chain order as stored; identity from `ForestIndex`/`HaloRankInForest` only, `:534-548`; v2 unchanged; abort-never-repair), each slice's acceptance criteria, Gate R0's recorded options, and the spec's consumer-minimum paragraph (every bullet ticked to a line: version `:2272-2281`; header consistency `:2326-2398`; `/schema` agreement `:1491-1539`; scale factor `:2342`; identity bounds `:2510`; physical values `:2354-2362`; link ranges and biconditionals `:2020-2098`).
- **Dimensions checked:** requirements fit, functional correctness, boundary and invalid input, lifetime and resources (full allocation→release trace on both paths), interfaces and contracts, numerical/scientific validity (bitwise gates; no tolerance anywhere), error handling and observability, tests, performance and memory (whole-slab retention cost recorded, not re-measured), portability (HDF5 1.10 spellings; `long long` vs `int64_t` on glibc), maintainability, documentation and claims. Concurrency: not applicable (no MPI on the horizontal path; the driver is single-threaded).
- **Validation run / not run:** §3. Not run: the Slice 7 gates and the v2 gate; no sanitizer or valgrind pass (the Slice 4 record reports a leak-free real run; the allocator's own leak report is the standing check).
- **Lint:** `make check-format` and `check-docs` pass; no separate `lint` skill verdict was supplied, so mechanical hygiene beyond the formatter is not evidenced here.

## 9. Open questions and assumptions

- v2 bitwise identity was reasoned from the code (pool recycling cannot change bytes: every slot is fully overwritten before read, pointers are never emitted, join/marshal order is unchanged; the one v2 lifecycle change is that a no-descendant generation is now released after its own output rather than after N+1's sweep — memory timing only) and relied on for the Stage 8 record at `6cd0c448`; it was not re-run here.
- Whether `save_halos_hdf5()` handles `NumProcessedHalos > INT_MAX` before the counter FATAL was not audited (moot once finding 1's admission policy lands).
- For F18, whether a package can override a core property's `range` was not verified; if not, the `deltaMvir` widening is global.
- Whether any galaxy on the SHAM fallback path can carry a negative `HaloNr` was not established (dropped item 21).

## 10. Verdict

**PASS WITH RISKS.** The plan's goals are met and evidenced; the code is correct on every path the review could reach; the surviving risks are bounded and named — v3 evidence that runs only by hand (3, 9, 19), one triplicated accounting seam and one qualified admission gap (2, 1), a record and a vision sentence that overstate (5, 14), and harness duplication with uneven hardening (16). Pass 1 (§0.1) closes every P3 and the test-coverage P2s in one session; Pass 2 (§0.2) closes the rest and both owner decisions.

---

## Documentation Directory

- [MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md](MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md): the plan this review covers
- [MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md](MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md): the acceptance evidence
- [HORIZONTAL-HDF5-FORMAT.md](HORIZONTAL-HDF5-FORMAT.md): the normative format, version 3 section
- [MIMIC-CONVERTER-GENERALISATION-CODE-REVIEW.md](MIMIC-CONVERTER-GENERALISATION-CODE-REVIEW.md): the preceding plan's review, same format
- [MIMIC-DEVELOPMENT-PATHWAY.md](MIMIC-DEVELOPMENT-PATHWAY.md): where the follow-ups should be indexed
