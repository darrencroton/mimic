# Mimic Shin-Uchuu Cut-Vertical: Post-Plan Code Review and Simplification Pass

**Purpose:** The owner's whole-branch code review of `feature/shin-uchuu-cut-vertical` (HEAD `d918566d`, 20 commits over `main` at `e16cedac`, 47 files, +6,735/−40) after the plan `MIMIC-SHIN-UCHUU-CUT-VERTICAL-IMPLEMENTATION-PLAN.md` (since archived under `archive/dev-plans/`) closed, with a secondary simplification pass, and the assessment of every decision, defect and follow-up the PM recorded across the three runs. The end result is judged on its own merits: where the finished code shows a plan decision was unnecessary or wrong, the finding stands against the code, not against the implementer who followed the contract.

**Date:** 2026-10-20. **Reviewer:** Claude Fable 5.1 (this session), with three delegated read-only passes: mechanical validation (Sonnet 5.5), an independent converter correctness read (Opus 5.5) and a test-adequacy read (Sonnet 5.5). Every delegated finding cited below was re-verified in this session before it was kept.

**Inputs read in full:** the plan, `HANDOFF.md`, the two archived handoffs it names, the PM notes of all three runs (`.pm/runs/*/notes.md`), `docs/VISION.md`, `docs/STYLE-GUIDE.md`, every source and test file the branch adds or changes, and the one-hop-outward contracts (`horizontal_dataset.py`, `HORIZONTAL-HDF5-FORMAT.md`, `src/io/vertical/binary.c`).

---

## Executive summary

**The plan's goal is met and the production result stands.** `sage16` under the vertical driver on the 361-file converted dataset is bitwise identical to the cut horizontal run over 2,059,547,378 galaxies at ten snapshots; the converter, package, gate and documentation are on the branch; every mechanical gate passes at HEAD (format, docs, lint with no new findings, converter tests 1,621/0/0, `shin-uchuu-lhalo` unit 304/0/0 and scientific 47/0 with the one designed skip, build with zero warnings). Nothing found here changes a byte of the production dataset or of any output.

**Verdict: PASS WITH RISKS.** No P0 or P1. Nine P2s and a dozen P3s, nearly all in the converter, which is the one permanent tool the plan produced. The P2s fall into three groups:

1. **The converter is narrower than its manual says.** The assembler requires each forest's rows, concatenated from the highest snapshot down, to carry `HaloRankInForest` exactly 0..n−1. That is the Consistent-Trees ASCII route's convention, which the plan's Frozen Facts stated as if it were the format's. For the L-Halo and forests-HDF5 routes the rank is the source's within-tree row, which is not snapshot-descending, and the converter refuses such datasets by name (reproduced on the committed `mini-millennium-horizontal` `worked_graph` fixture). The check adds nothing to the output, because records are already placed by rank; a permutation check keeps Shin-Uchuu's bytes identical and makes the manual's claim ("any forest-blocked version 3 dataset") true. Fix it rather than narrow the claim.
2. **The converter's operational edges are rough.** The known read excess (one window per slab past every file's last forest, measured at about 6.8 GB per production file); no handling of SIGTERM or a parent-only interrupt (orphaned workers, finished files discarded); an uncaught non-`ConverterError` exits with status 1, which the tool defines as "verification failure"; and a hand-edited plan can wrap the int32 header silently. Each has a small, contained fix with a clear acceptance (the subset re-conversion is byte-identical at two window sizes).
3. **The `MostBoundID` link verification the plan mandated is a tautology.** By construction it cannot fail on an unpatched code path, costs a gather per link kind per batch, and is advertised in the manual, the Developer Guide and the CHANGELOG as independent verification. Remove it and the claim, or replace it with a check that uses data the mapping does not (`SourceHaloID − rank` constant per forest).

**Documentation is in good order** except for five known slips, all cheap: `docs/USER-GUIDE.md:416` still claims subset scale only; `AGENTS.md:73` omits the package; the `MallocLargeCache=0` qualifier on the 165/143 GB figures appears only in the package README; the package README's example recommends `--workers 8` while the manual says nothing about the conversion being disk-bound on a rotational disk (where 2 workers beat 4); and `sage16` has no `shin-uchuu-lhalo` plot profile, so 16 of 22 figures take default axes.

**The PM's record is sound.** All eleven decisions taken on the owner's behalf are ratified below; none needs reversing. The twenty-one plan defects are lessons for the next plan (the plan is archived unamended on merge), and the handful that encode durable facts are routed to the documents that should carry them. The recommended work is grouped into five packages at the end, ordered so that the converter fixes land first and the merge is not blocked by any of them.

---

## Authorization status

- Every slice ran under the PM with a drift audit (Sonnet 5.5, high) that passed, recorded in the run reports and `HANDOFF.md`; the procedure's two commits (`5dd6fcfe`, `0284bccc`) were owner-approved. This review is the post-plan quality review and performs no further drift audit.
- The lint skill was run differentially against `e16cedac` in this session: verdict pass, no new findings, all five languages covered.
- Code-health ran differentially against `e16cedac`: candidate verdict none; the two complexity hotspots it measures (`vertical/writer.py:645 convert`, cyclomatic 31; `tests/framework/parity_gate.py:1325 stage_dataset_provenance`, 35, +20) are addressed under simplification.

---

## Findings

Ordered by severity. Line numbers are at `d918566d`.

### P2

**1. `convert/mimic-convert/vertical/assemble.py:456-472` — the rank-order check refuses legal L-Halo-route and forests-HDF5-route version 3 datasets.**
The check requires the concatenation of a forest's slab blocks from the highest snapshot down to give `HaloRankInForest` exactly 0..n−1. `HORIZONTAL-HDF5-FORMAT.md:455` gives three conventions: for `consistent_trees_ascii` the rank is the reference vertical-driver order (snapshot-descending), but for `lhalo_binary` and `consistent_trees_hdf5` it is the source's original within-tree row index, which L-Halo trees do not order by snapshot. Reproduced on committed data: `ForestAssembler` over `simulations/mini-millennium-horizontal/_tests/data/worked_graph` (6 halos, `source_format lhalo_binary`) refuses with "forest 0: rank 3 found at position 2, where rank 2 is expected"; the sibling `forest_blocks` and `wide_slab` fixtures pass only because their rows happen to descend. The independent reviewer further ran the full mini-Millennium dataset: 788 of 3,432 trees are refused. The manual (`convert/mimic-convert/README.md:319-321`), the Developer Guide (`docs/DEVELOPER-GUIDE.md:1278`) and the CHANGELOG describe the tool as converting any forest-blocked version 3 dataset. The output does not depend on the order check: `_assemble` places every record by `position = offsets[forest] + rank` (`:473`). *Why P2, not P1:* the refusal is loud and names the forest; no wrong file can result, and the plan's declared input was the cut Shin-Uchuu dataset, where the convention holds. *Fix:* replace the order check with a permutation check (`0 <= rank < counts[forest]` and every position hit exactly once); prototyped by the independent reviewer against all 29,585 mini-Millennium forests with every link and payload field equal to the original `trees_063.*` records, and Shin-Uchuu bytes unchanged because placement is unchanged. Update the module docstring (`:15-19`), `README.md:339` and the two refusal tests at `test_vertical_assemble.py:296,321` (plant a duplicate or missing rank). Add the `worked_graph` fixture as a positive test.

**2. `convert/mimic-convert/vertical/assemble.py:161-166,341-370` — the read excess (the deferred brief).**
`_Slab.frontier` treats a slab as complete for `[first, stop)` only once a buffered row has `ForestIndex >= stop` or the slab is exhausted, so `_read_window` reads one whole window per slab past every file's last forest: 70 × `window_rows` × 132 B, 9.69 GB per file at the default window, measured at about 6.8 GB per file over 356 production files (the record's corrected figure). It rules out the larger window a seek-bound disk wants. *Fix (the brief's design 1, refined and prototyped byte-identical by the independent reviewer):* after the first-row search, find each slab's end row for `stop` by the same bisection (`_first_row(slab, stop, low=next_row)`), store it as `slab.end_row`, make `exhausted` and `_read_window` respect it, and let `frontier` drop its `stop` argument. Rows read on the mini-Millennium-horizontal dataset fall from 9,739,486 to exactly the halo total, 1,533,122, at a 65,536-row window. Cost: one extra bisection per slab per file (about 30 single-row reads). Acceptance as the brief states: re-convert the Stage R cut subset at the default window and at 4,194,304 rows and compare every SHA-256 with `$V/logs/sha256-subset-subset-w1.txt`; `make tests-converter`; a unit test asserting the highest row requested per slab (the test-adequacy read found none today; `test_the_per_forest_work_is_bounded` checks only that two windowed reads occur). Update the module docstring (`:7-11`, `:34-39`), the `memory_bound` docstring (`:114-117`, the bound stays a valid upper bound and needs no numeric change), the `forests()` Raises text (`:281-287`: the in-range monotonicity check no longer covers rows outside the range, which `forest_totals` covers at every `convert`), the manual's "reads up to one window per slab beyond" (`README.md:343`) and the CHANGELOG. The single-row-probe count in `test_vertical_assemble.py:478-497` doubles; classify probes by the column tuple, not by span length.

**3. `convert/mimic-convert/vertical/writer.py:617-642` — a parent-only interrupt or SIGTERM loses finished work and orphans workers.**
Reproduced by the independent reviewer on Python 3.14.6 with a spawn pool. (a) `KeyboardInterrupt` in the parent: `executor.shutdown(wait=True, cancel_futures=True)` blocks until every in-flight file finishes (up to about 30 minutes for a 100M-halo file), then the interrupt propagates and those completed results are never recorded; the rerun moves the correct files aside and rebuilds them. (b) SIGTERM to the parent: no handler, so `finally` never runs; the spawned workers finish, rename their files and then idle forever, and a rerun started meanwhile races them. The manifest itself stays correct throughout (`xb` partials, parent-only writes, SHA computed while writing), so this is wasted hours and spurious rebuilds, not corruption. The production stop that worked was SIGTERM to the workers (HANDOFF, "Related operational finding"); the manual documents no stop recipe. *Fix:* in `convert`, install a SIGTERM handler that raises in the parent; on the first interrupt stop submitting but drain `pending` and record every result; on a second, terminate the workers (`executor.terminate_workers()` on 3.14+, else `executor._processes`) and `shutdown(wait=False, cancel_futures=True)`. Enforce "run one `convert` per output directory at a time" (`write_vertical_trees.py:154`) with an `fcntl.flock` on `<output>/.convert.lock`. Document the stop recipe in the manual's resume paragraph (`README.md:351`) either way.

**4. `convert/mimic-convert/write_vertical_trees.py:237` — an uncaught exception exits with status 1, which the tool defines as "verification failure".**
`main` catches only `ConverterError` and `OSError`; any other exception is a traceback and Python's exit status 1, which `EXIT_VERIFICATION` also uses and which the manual (`README.md:353`) tells the operator means "that file stays `.partial`, every other file is written and recorded". Reachable: a plan with `"first_forest": 1.0` passes `_check_files` (`plan.py:305-330`, `1.0 == 1`) and `read_file_plan`, then raises `TypeError: slice indices must be integers` at `writer.py:704`. *Fix:* catch `Exception` in `main` and return `EXIT_REFUSED` naming the type; require plain `int` (not `bool`) for every entry field in `_check_files`.

**5. `convert/mimic-convert/vertical/writer.py:295` and `plan.py:305-330` — the int32 header cast wraps silently on a hand-edited plan (capped at P2: unreachable from `plan` output).**
`cut_files` caps every file at `INT32_MAX`, but `_check_files` does not re-check `n_halos` or `n_forests` against `INT32_MAX` or the recorded caps, and `convert`'s totals check (`writer.py:703-710`) accepts merged entries. `.astype("<i4")` then wraps (3,000,000,000 → −1,294,967,296); `_verify` catches it as a verification failure, but only after a 264 GB file has been written. *Fix:* refuse in `_check_files` (each entry ≤ `INT32_MAX` and ≤ the recorded caps) and assert before the cast in `_write_partial`.

**6. `convert/mimic-convert/vertical/assemble.py:477-478,494-507,604-648` — the `MostBoundID` link verification is a tautology and is advertised as independent verification.**
`_map_links` guarantees `forest[target] == owner`; `_assemble` has already required the owner's ranks to be exactly 0..n−1, so `mapped = rank[target]` is in `[0, counts[owner])` and the `astray` check is empty; `position` is a bijection, so `placed_most_bound[offsets[owner] + rank[target]]` is `most_bound[target]` identically. The plan foresaw this ("the `MostBoundID` verification exercised by a patched mapping ... since swapping ids in a valid source dataset changes nothing the invariants see") and mandated it anyway; the test at `test_vertical_assemble.py:382` needs a patch for the same reason. It costs five int64 gathers and compares per batch (part of `WORKING_BYTES_PER_ROW`), and the manual (`README.md:339`), `docs/DEVELOPER-GUIDE.md:1278` and the CHANGELOG present it as verification. The end result trumps the plan decision here. *Fix:* delete `_verify_links` and `placed_most_bound`, re-measure `WORKING_BYTES_PER_ROW`, and drop the claim from the three documents; if an independent check is wanted, read `SourceHaloID` (+8 B per row) and require `SourceHaloID − HaloRankInForest` constant within each forest, which checks ranks, and so every mapped link, against data the mapping never touches.

**7. `convert/mimic-convert/tests/test_write_vertical_trees.py` — three resume and verification branches that production relied on are untested.**
(a) A final file present, recorded, but with bytes that differ from its manifest SHA (`writer.py:389-393`, move aside and rebuild): only the "matches" branch (`test_an_interrupted_run_resumes`) and the orphan branch with `expected_sha256 is None` are tested; a truncated or bit-rotted file after an interrupted copy is the most credible production failure. (b) A manifest entry whose final file is missing (`writer.py:738-741`). (c) `_load_manifest`'s five named refusals (`writer.py:489-537`), of which only the `tree_name` mismatch is tested. (d) `_verify`'s header, `ntrees`, `totNHalos`, counts and size checks (`writer.py:337-359`): the one test flips the last byte, which the SHA always catches first, so a regression removing the size check would pass. *Fix:* four small cases; a truncated re-read covers (d).

**8. `convert/mimic-convert/tests/test_vertical_assemble.py` — no positive test of a non-adjacent link.**
The plan requires the companion target-snapshot columns to be honoured (`links_adjacent` need not be 1), but every positive fixture links to the adjacent snapshot; the mutated-companion refusals prove the column is read, not that gathers use it. A bug mapping with `owner − 1` instead of the companion would pass. *Fix:* one fixture with a gapped descendant link (the mini-Millennium-horizontal `three_snapshot_chain` fixture may serve).

**9. `tests/framework/parity_gate.py:581,610` — the dataset variants write directory scalars unquoted.**
`dataset_variant` and `vertical_dataset_variant` interpolate the path into YAML bare; a path containing `#` or a colon followed by a space breaks the variant. The verification step after the write catches it loudly, so a gate fails rather than runs the wrong dataset, but the failure names the wrong thing. *Fix:* `json.dumps(str(directory))` as a double-quoted YAML scalar in both helpers (the HANDOFF follow-up).

### P3

**10. `simulations/shin-uchuu-lhalo/README.md:52` and `convert/mimic-convert/README.md:337` — worker guidance contradicts the measurement.** The package README's example runs `--workers 8`; production found the conversion disk-bound on the rotational LaCie at about 120 MB/s, with 4 workers slower than 2, and the cold rehearsal workers at 6–9 % CPU. That is a measured tool property, not log content. State in the manual that on a rotational disk the conversion is seek-bound and one or two workers saturate it, that more workers help only on a page-cached or SSD source, and that a larger `--window-rows` becomes the lever once finding 2 lands; drop the `8` from the README example or qualify it.

**11. `docs/USER-GUIDE.md:416` — stale.** "Its evidence is at subset scale" predates the production identity; bring it to the CHANGELOG's claim (identity for `sage16` at production scale under `mpirun -np 4`; `halos-only` and `sage16` at subset scale; no `hod` or `sham`).

**12. `AGENTS.md:73` (and its `.claude/CLAUDE.md` symlink) — the vertical package list omits `shin-uchuu-lhalo`.**

**13. `CHANGELOG.md:14`, `.agents/skills/mimic-docs-and-writing/SKILL.md:74`, `.agents/skills/mimic-simulations-and-readers/SKILL.md:52` — the 165 GB / 143 GB figures lack the `MallocLargeCache=0` qualifier** that `simulations/shin-uchuu-lhalo/README.md:110` carries (the PM's tolerated P3 (e)). One parenthesis each.

**14. `models/sage16/plots/profiles/` — no `shin-uchuu-lhalo_plot_profile.yaml`.** `mimic-plot.py:335` looks up `<simulation_name>_plot_profile.yaml` under the model's profiles, so 16 of 22 `sage16` figures take default axes on this package; the production comparison worked around it with `plotting.profile`. The `-ascii` sibling is a copy of `shin-uchuu_plot_profile.yaml` with only `simulation:` changed (verified by `diff`); add the same one-file copy.

**15. `convert/mimic-convert/vertical/writer.py:389-393,738-741,796-805` — the manifest names a moved-aside file during a rebuild.** A recorded file whose SHA differs is moved aside and rebuilt while its entry stays in the manifest; if the parent dies mid-rebuild the manifest claims a complete file that is not on disk, and only a rerun selecting that file repairs it (`:738` drops absent-file entries for the current selection only). *Fix:* drop the entry and write the manifest before submitting such a job (re-set it on a `skipped` outcome), and sweep absent-file entries over every planned file.

**16. `convert/mimic-convert/vertical/writer.py:702,462` and `plan.py:362` — repeated whole-dataset passes.** Every `convert`, including `--only-files 5`, re-runs `forest_totals` (16 B per row, about 360 GB at production, about 20 minutes cold) and computes `dataset_identity` twice, each hashing the whole sidecar. Keep `forest_totals` (it is the only full-dataset monotonicity check, and the one finding 2 leans on); compute the identity once and pass it to both callers; consider caching the totals beside the plan (an int64 array of 8 B per forest, bound to the plan md5) so a restart does not pay the pass.

**17. `convert/mimic-convert/vertical/plan.py:293-302` — `write_file_plan` does not fsync** while `writer.write_json_atomic` does, and the two produce identical bytes. Move `write_json_atomic` to `plan.py` (writer imports plan) and use it for both.

**18. `convert/mimic-convert/vertical/writer.py:637-640` — files never submitted after a worker death are not named**, only counted in the summary; log their numbers.

**19. Endianness is unstated.** `record.py:33-39` and `writer.py:79` always write little-endian; `binary.c:77,101-115,213` reads host order with no marker. Correct on every supported host; say "little-endian" in the manual, the epilog and the `record.py` docstring.

**20. `simulations/shin-uchuu-lhalo/_tests/integration/test_fixture_halo_identity.py:186-190` — asserts `verdict == PASS` but not `matched_rows`.** A `compare` that passed on an empty dump would pass the test; assert `matched_rows == 13`.

**21. `scripts/check_generated.py` hint — "Run: make MODEL=sage16 generate"** omits the `SIMULATION` selector when the stale artefacts are the other package's (surfaced by the validation run; pre-existing, interacts with the new package).

**22. Test duplication (the owner's standing concern).** The record digest `ab04dcba…` is pinned at `test_vertical_record.py:123`, `test_vertical_plan.py:227` and `:279` plus the committed `file_plan.json`: keep the record-test pin, derive the other two. The 88-byte stride is typed in five places (`test_vertical_record.py`, `test_vertical_assemble.py:250`, `test_write_vertical_trees.py:192,256,285`): derive from `RECORD.itemsize`. `test_write_vertical_trees.py:155-158` re-types the writer's own buffer constants (`64 << 20`, `8 << 20`, `21 * 2`), a tautology that fails on any tuning: import them or keep only the `bound["total"]` assertion at `:159`. `HelpTests` pins the formula verbatim including `176`: assert the terms and the exit-code words, not the punctuation. `test_make_file_plan_refuses_a_bad_cap` duplicates `cut_files`' refusals without asserting the one thing that distinguishes it (no read happens): make it assert that, or drop it. Stale comment at `test_vertical_plan.py:137` ("its minimum 15" against an asserted 9).

---

## Simplification pass

Scope: the converter package, the CLI and the gate harness change, judged on the finished code with the production evidence as the behavioural baseline. Each item preserves every output byte; the ones that change a refusal message say so.

**Code the plan mandated that the result shows unnecessary (recommend removing):**
- `_verify_links` and `placed_most_bound` (finding 6), with the `WORKING_BYTES_PER_ROW` re-measurement and the three document claims.
- The rank-*order* check as a refusal (finding 1); the permutation check it becomes is both weaker as a constraint and stronger as a guarantee (every position hit once).
- `frontier(stop)`'s `last_forest >= stop` arm, dead once finding 2 lands.

**Structure:**
- `writer.convert` (cyclomatic 31, 183 lines): split into `_select_files`, `_check_plan_totals`, `_log_memory_bound` and the job loop; the memory-bound log statement alone is 21 lines of one `format` call.
- `parity_gate.stage_dataset_provenance` (35, +20): the 80-line manifest-proof block under `if pkg.vertical_dataset is not None` is a self-contained function (`_prove_inventory_from_manifest`) with its own docstring; the method then reads as the three routes it serves.
- `assemble._read_window`'s three-way `previous` branch (`:348-357`): give `_Slab` the range's `first` and start `last_forest = first`; `before = [slab.last_forest] + forest[:-1]` then covers every case and a first-window row below `first` is reported as an ordinary drop (message changes; the independent reviewer showed the `elif` branch is reachable only on a slab's first window, so this is coupling, not a gap).
- `plan.forest_totals` (`:102-130`): start `previous = -1` and drop both `previous is None` cases (negative values are still refused, by the drop message).
- Four near-identical positive-integer validators (`plan._require_cap`, `assemble._require_window_rows`, `writer._require_positive`, `horizontal_dataset._require_block_rows`): one helper with an optional upper bound.
- `writer._reread` (`:286-288`) is a pure alias of `_blocks`; `writer.py:467-472` repeats `plan._record_binding` (`:258-261`); `assemble.py:232-235`'s `if shape else` is redundant (`np.prod(()) == 1`).
- `writer._RangeTotals`: a two-method adapter so the assembler can slice `expected_totals[first:stop]` by `ForestIndex`; passing the file's totals plus `first` to `forests()` would remove the class.
- `record._halo_properties`'s `list(source)` copy and the `isinstance(halo_properties, (str, Path))` branches in three places could be one `load_halo_properties` returning `(entries, path_or_None, md5_or_None)`.

**Keep (doing real work):** `_Slab` as a class (its buffer-and-cursor invariant is the heart of the pass); `memory_bound` exposed at module and instance level (the CLI prints one, tests pin the other); `write_json_atomic`; the `BrokenProcessPool` handling in `_run_jobs`; `parse_file_selection`.

---

## The PM's record: decisions, defects, tolerated P3s and follow-ups

### PM decisions taken on the owner's behalf (eleven, across runs 1–3)

| # | Decision | Assessment |
|---|---|---|
| 1 | Slice 4 grant: `tests/integration/test_unique_galaxy_id_encoding.py` is NA unless the package's record equals mini-Millennium's | **Ratify.** The NA is a deterministic selection mismatch (the style guide's rule), the message names the differing field, and the test still runs on every L-Halo package that can read the fixture. |
| 2 | Slice 6 bounded per-leg RSS (sampled lower bounds, the gate step's `time -l` as the upper bound) | **Ratify.** The gate measures nothing and deletes its runs; the bounded statement is honest. If per-leg RSS is wanted later, it is a harness change of its own. |
| 3 | The read excess not treated as a Slice 6 stop | **Ratify.** Output correct and verified; a performance finding is not a contract defect. It is finding 2 here. |
| 4 | Estimates are not stops (361 files against 260–320; 270.6 GB spill against 170) | **Ratify.** |
| 5 | Slice 7's "simulation package list" read as the gate and reverse-direction paragraphs | **Ratify.** The guide holds no package list; the two paragraphs carry the content. |
| 6 | Slice 7's grant delivered by relaunch | **Ratify** (procedural; one empty attempt). Note for the PM skill: a grant should be recordable before launch. |
| 7 | Tolerated P3s instead of a further panel round (runs 1–2) | **Ratify**, and they are itemised below with a disposition each. |
| 8 | Criterion 2 ruling: every measured or derived figure traces to a log; decision times, session times and labelled estimates stay as labelled metadata | **Ratify.** The literal reading was unsatisfiable for an owner-operated procedure; the ruling is narrow and the independent Opus trace confirmed the record. Carry the amended wording (defect 20) into the next plan template. |
| 9 | "under `mpirun -np 4`" is part of the production claim everywhere it is stated | **Ratify.** It is the condition as measured; output identity does not depend on the task count (Decision 6), and the CHANGELOG says so in the same sentence. |
| 10 | Tolerated run-3 P3s (a)–(e), all in `docs/dev/` | **Ratify**, with (e) promoted to finding 13 because the figures are user-facing. |
| 11 | README "Memory": per-rank peaks removed as run-specific | **Ratify.** The general rule (a rank's peak follows the largest forest it holds) is the package property. |

### Plan defects (twenty-one)

The plan is revision 1, never amended, and is archived to `archive/dev-plans/` on merge, so no defect is fixed in the plan. Each is either a lesson for the next plan's template or a durable fact that belongs in a standing document. Disposition:

- **Lessons for the next plan (no action on the branch):** 1 (stale test counts: quote none, or quote the count at the commit), 2 (an unfireable refusal criterion), 5 (an inventory rule that breaks on one file), 7 (an md5 that depends on an argument's spelling), 8 and 9 (criteria the instrument cannot satisfy), 10 (a referent that does not exist), 11 and 12 (estimates stated as expectations), 13 (a surface omission), 17 (a step with no instance), 20 (an unsatisfiable literal criterion), 21 (no slot for the post-commit Reviewer trace).
- **Durable facts to carry into standing documents:** 3 (dense `SourceHaloID`: already in the manual), 4 (the super-forest's bound is ~86 GB at the default window, a small-forest file ~20 GB, and the printed line names the plan's largest forest: already in the manual and README), 14 (disk-bound at 2 workers on a rotational disk: finding 10), 15 (the comparator reads whole records, so one output on a rotational disk costs ~4 h: a candidate in the pathway's residuals), 16 (one LaCie-bound job at a time: a procedure discipline for the next procedure's template), 18 (`-np 4` fits 512 GiB when the rank choice is checked against the files' largest forests: the README already states the per-rank rule; keep `rank_memory.py` in `$P` as the instrument), 19 (SIGTERM, not SIGINT: finding 3).
- **Resource Profile rows (4, 14, 15, 18):** the next plan of this kind should take its rows from the acceptance record's measured table, not from the previous plan's estimates.

### Tolerated P3s (runs 1–3)

| Item | Disposition |
|---|---|
| Converter README `plan` paragraph "8 B per forest" vs the tool's ~17 B while deriving | Already corrected at HEAD (`README.md:335` states both). Closed. |
| Developer Guide's helper list omits `vertical_dataset_variant` | Still true at `docs/DEVELOPER-GUIDE.md:1259` ("`dynamic_variant`, `range_override_variant`, `dataset_variant`, `assert_horizontal_run_file_diff`"). Fix with WP3. |
| Pathway next-focus paragraph in planning tense | Superseded by the 2026-10-20 revision. Closed. |
| Validation skill's schema-test clause places the vertical package in a "horizontal package" sentence | Rewritten at HEAD (`SKILL.md:80` says `shin-uchuu-lhalo` ships the fixture test instead). Closed. |
| Slice 6 record: the write-verify re-read came from the page cache; the totals-pass bracket includes the prelude | Record-only; leave. |
| Slice 8 brief: Type 0/1 branch cited at `:240`, the `if` at `:239` | Brief-only; fix when the brief is next touched. |
| "three model packages" beside four `models/` directories | Defensible (`halos-only` is a model-free package); leave, or say "three physics model packages". |
| Run 3 (a): drop 20:40:45Z from the record's step 4 | Record-only; one-line edit with WP5. |
| Run 3 (b): session times | Covered by decision 8. Closed. |
| Run 3 (c): the pathway's focus box lists the comparator worktree's removal as pending while `git worktree list` shows only the main checkout | Mark done with WP5. |
| Run 3 (d): "`main` is v1.5" vs v1.5 plus `e16cedac` | Pathway-only; correct with WP5. |
| Run 3 (e): `MallocLargeCache=0` qualifier | Finding 13. |

### Recommended follow-ups from the handoffs

| Follow-up | Disposition |
|---|---|
| The converter's read excess | Finding 2; WP1. |
| SIGTERM stop for `convert` | Finding 3; WP1 (code) and WP3 (manual). |
| `sage16` plot profile for `shin-uchuu-lhalo` | Finding 14; WP3. |
| `AGENTS.md` package list | Finding 12; WP3. |
| `docs/USER-GUIDE.md:416` stale | Finding 11; WP3. |
| Quote the directory scalars in the two variants | Finding 9; WP4. |
| Per-window term in `memory_bound` | Fold into finding 2's docstring work: state the ~KB-per-window Python overhead as excluded (the manual already does); a term is not worth the formula's extra complexity. |
| NT `du -sk` on Lustre | Operational note for the next procedure's template; nothing on the branch. |
| Column-wise comparator reads | A performance candidate; add to the pathway's residuals with the measured 15,068 s / 291 GB. WP5. |
| Output-buffer seed from a measured ratio | Already in the pathway's residuals (116.27 GB of 164.8 GB). No action. |

---

## Contract defects

- Slice 2 criterion 2 required the snapshot-descending rank-order check on the strength of the Frozen Facts' statement that "the within-forest order is the vertical reader's order", which `HORIZONTAL-HDF5-FORMAT.md:455` scopes to the Consistent-Trees ASCII route. The implementer followed the contract; the coherent reading for a general-purpose tool is the permutation check (finding 1).
- Slice 2 criterion 4 required a `MostBoundID` verification that the plan itself noted cannot fail on valid input (finding 6).
- Slice 9 criterion 2 ("every number ... traces to a named log"): resolved by PM decision 8; recorded as plan defect 20.

## Open questions / assumptions

- Whether `write_vertical_trees.py` is meant to serve every version 3 route or only the ASCII-route cut dataset. The manual claims the general case; this review takes that as the intent and rates finding 1 accordingly. If the owner intends the narrower tool, finding 1 becomes a documentation fix (state the ASCII-route rank convention as a precondition and name the refusal), which is the weaker outcome.
- The independent reviewer's full-mini-Millennium reproduction (788 of 3,432 trees refused; 29,585 forests byte-equal after the permutation fix) ran against the machine-local `simulations/mini-millennium/snapshots` and the `mini-millennium-horizontal` dataset; this session reproduced the refusal on the committed `worked_graph` fixture only. The fix's acceptance should include the full-dataset comparison.
- `pyflakes` and `mypy` are not in `mimic_venv`; ruff covered the changed Python. Not a gap for this review.

## Coverage summary

- **Scope reviewed:** every file in `git diff e16cedac..d918566d` (47 files); the converter package and CLI line by line; the gate harness diff and its tests; the package metadata, fixtures, C and Python tests; the documentation diff; `HANDOFF.md`, both archived handoffs, the three PM notes files and the plan in full.
- **Requirements checked against:** the plan's nine owner decisions and the acceptance criteria of Slices 1–9; `docs/VISION.md` (principles 5, 6 and 7 are the ones this work touches: bounded memory, format-agnostic I/O with reproducible provenance, fast failure); `docs/STYLE-GUIDE.md`; the user-facing documentation convention.
- **Dimensions checked:** requirements fit, functional correctness, boundary conditions, state and lifetime, interfaces and contracts (one hop outward to `horizontal_dataset.py`, the format document and `binary.c`), concurrency (the spawn pool, signals), numerical validity (verbatim payload copy, int32 header, int64 links, endianness), performance (the read excess, repeated passes), security and robustness (path handling in `tree_name`, atomic writes), tests, observability (exit codes, refusal messages), portability (little-endian, macOS/Linux), maintainability, documentation.
- **Validation run in this session:** `make check-format` 0; `make check-docs` 0; `make MODEL=sage16 SIMULATION=shin-uchuu-lhalo validate-modules` 0, `check-generated` 0 after `generate`, build 0 with zero warnings; lint differential pass, no new findings; code-health differential, candidate verdict none; `make tests-converter` 1,621 passed, 0 failed, 0 skipped; `tests-unit summary` 304/0/0 (28 NA); `tests-scientific summary` 47/0, 1 designed skip, 1 pre-existing warning (`test_zero_values` on the fixture); default pair restored and `git status` clean afterwards. Logs: `archive/test-logs/review-2026-10-20/`.
- **Not run:** the integration tier (delegated runs were sequential and time-boxed; the PM ran it at both run-3 heads: 123/0); the gate on data; any production data. Finding 1 reproduced on committed data; findings 3 and 4 reproduced by the independent reviewer on synthetic data.

## Verdict

**PASS WITH RISKS.** The branch is mergeable as it stands: the production claim is evidenced, the user-facing documents are coherent, and nothing found changes an output byte. The risks are concentrated in the converter's generality and operational edges, and every one has a bounded fix with a byte-identity acceptance already available from the rehearsal. Land the converter work (WP1) before the next conversion with this tool; land the documentation slips (WP3) before or with the merge.

---

## Recommended work packages

Ordered. Each is independent of the others except where noted; none blocks the merge, and WP3 and WP5 are best done with it.

**WP1 — The converter (findings 1–6, 15–19; simplification).** One reviewed slice over `convert/mimic-convert/vertical/` and the CLI: the permutation check; the end-row bisection; signal handling and the directory lock; `Exception` → exit 2 and strict plan types; the int32 guard; removal of `_verify_links` (or its `SourceHaloID` replacement); identity computed once; `write_json_atomic` shared; the `convert` split. Acceptance: `make tests-converter`; the Stage R subset re-converted at the default window and at 4,194,304 rows byte-identical to `$V/logs/sha256-subset-subset-w1.txt`; the mini-Millennium-horizontal dataset converted and compared record for record with `trees_063.*`; optionally files 166 and 180 cold against the rehearsal's device-read figure.

**WP2 — Converter tests (findings 7, 8, 22).** The three resume/verification branches, a non-adjacent-link fixture, the read-excess row-bound assertion, `worked_graph` as a positive case; and the de-duplication of the literal pins. Can ride with WP1.

**WP3 — User-facing documents (findings 10–14, 19; the Developer Guide helper list).** `docs/USER-GUIDE.md:416`; `AGENTS.md:73`; the `MallocLargeCache=0` qualifier in three places; the worker and disk guidance in the manual and README; the stop recipe in the manual's resume paragraph; "little-endian"; the `sage16` plot profile; `docs/DEVELOPER-GUIDE.md:1259`. After WP1, the manual's claims about the `MostBoundID` verification, the read excess and the rank convention change too.

**WP4 — The gate harness (finding 9; simplification).** Quote the two variants' scalars; extract the manifest-proof block from `stage_dataset_provenance`. `tests/integration/test_parity_gate_helpers.py` is the acceptance.

**WP5 — Merge housekeeping.** Archive the plan to `archive/dev-plans/`; keep the acceptance record and the SAGE26 brief; the pathway's focus box and inventory (P3s (c) and (d), the comparator-I/O residual); the record's step-4 time (P3 (a)); `HANDOFF.md` rewritten so the post-plan brief is closed by WP1's commit, not carried forward again.

---

## Disposition (2026-10-20)

The owner accepted every finding and every work package the same day, with two decisions that settle the review's open questions: `write_vertical_trees.py` is a broad tool for any forest-blocked version 3 dataset, so finding 1's fix is the permutation check, not a narrower claim; and the SAGE26 brief was brought up to date in the same pass (re-pinned to SAGE26 `11b1ae85`, one citation moved, the production data locations, the snapshot-list requirement and two validation routes added, every converter citation anchored by name). The work was done by Sonnet 5.5 subagents under the Developer's gate, then reviewed by an independent external panel through the orchestrator (Codex `gpt-6.1-sol` and Claude Opus 5.5, both read-only at high effort, `code-review` embedded, staggered starts, one adjudication per round), each finding verified in the code before it was acted on.

**What landed.**
- **WP1, the converter.** The permutation check replaces the snapshot-order requirement (the `worked_graph`, `forest_blocks` and `wide_slab` fixtures of `mini-millennium-horizontal` convert, and the full mini-Millennium-horizontal dataset, 29,585 forests with `links_adjacent 0`, converts and matches the original `trees_063.*` tree for tree in all 14 common fields); each slab is read over the file's forest range only, found by a second bisection (rows read on that dataset fall to exactly its halo total); the `MostBoundID` verification and its claims are gone and `WORKING_BYTES_PER_ROW` re-measured at 160 B; `common.py` holds the shared validator and the durable JSON writer, the plan file is fsynced, `_RangeTotals`, `_reread` and the duplicated validators are gone, `convert` is split into named helpers; the dataset identity is computed once; `main` catches every exception, prints the traceback and exits 2; `_check_files` requires plain integers within the reader's limit and the recorded caps and the header is guarded before its int32 cast; the files are stated little-endian.
- **Signals, locks and the manifest** (the panel's rounds). A counting handler makes the first SIGINT or SIGTERM a drain wherever it lands, including during manifest writes and with one worker, and the second a termination; terminated workers are terminated, joined and if need be killed before the directory lock is released (`terminate_workers()` on 3.14 shuts the executor down without waiting, so it is not used); the submission loop checks the stop flag before each submission; `convert` holds `<output>/.convert.lock` and refuses a second run, degrading to one warning where the filesystem cannot lock; each worker holds `<name>.<k>.lock` for its whole job, with an inode recheck after locking and an unlink under the lock on a normal return, so a rerun refuses a file an orphan of a killed run is still writing and a finished run leaves only `.convert.lock`; the ledger writes every change to disk before memory and is reloaded from disk before the final absent-file sweep, so an incomplete run can never exit 0 and a run that completed every selected file exits 0 even if signalled. Finding 15's drop-before-submit mechanism was implemented and then reversed in favour of entries that persist while their file does.
- **WP2, tests.** Resume and verification branches, manifest refusals, a positive non-adjacent-link case (the gapped fixtures), the read-bound assertion, the committed byte-identity test (the `worked_ascii` fixture converted and compared with the committed `worked_ascii_lhalo` files at two windows; a missing fixture fails), signal and lock cases with real-process checks (first drain, second terminate, a process-group SIGINT, a SIGKILLed parent followed by a rerun), and the de-duplicated literal pins. `make tests-converter`: 1,655 passed, 0 failed, 0 skipped (1,621 at the reviewed commit).
- **WP3, documents.** The manual's record, memory, resume, locks-and-stopping, workers-and-disks and exit-code paragraphs; the Developer Guide's reverse-direction paragraph and helper list; the User Guide's production claim; the CHANGELOG's corrected converter entry and the `MallocLargeCache=0` qualifier there and in the two skills; `AGENTS.md`'s package list; the package README's worker example, disk sentence, 84 GB bound (the formula at 160 B per row) and profile bullet; `models/sage16/plots/profiles/shin-uchuu-lhalo_plot_profile.yaml`; `scripts/check_generated.py`'s hint names `SIMULATION` (finding 21).
- **WP4, the gate harness.** Both run-file variants write paths as double-quoted YAML scalars with `ensure_ascii=False` (a non-BMP character is in the helper test); the manifest inventory proof is `_prove_inventory_from_manifest`. The 13 helper cases pass.
- **WP5, merge housekeeping.** The plan and the owner's brief are archived under `archive/dev-plans/`, the root handoff under `archive/handoffs/`; the pathway's focus box, inventory, completed-work table and residuals are rewritten, with the owner's post-merge actions (the SAGE26 hand-off's two data copies, the data dispositions, the router, the release call); the acceptance record's one link to the plan is plain text; the review's own P3 (a) in the record and the pathway's (c) and (d) are closed by the rewrite.

**Deliberately not done.** Caching the forest totals beside the plan (finding 16, "consider"): it adds a second binding file for a 20-minute saving per restart; and a single `load_halo_properties` helper: three small branches are not worth the indirection. The comparator's whole-record reads and the output-buffer seed stay as the pathway's residuals.

**Panel.** Round 1: Codex FAIL (one P1 on orphaned workers outliving the directory lock, rated P2 here because no wrong file can be promoted, two P2), Claude PASS WITH RISKS (one P2 on `flock` refusal where a filesystem cannot lock, eight P3). Round 2: Codex FAIL (two P1: the unlink race and a forced stop exiting 0 with a file moved aside; two P2: reaping on Python 3.14 and the submission loop's cached flag), Claude PASS WITH RISKS (P3s only, the same reaping defect among them). Round 3: Codex FAIL (one P1, the unlink still outside the lock), Claude PASS WITH RISKS (one P3, the same point, with the simpler fix that was taken). Round 4, Codex only, on the lock functions: PASS. Every finding was verified in the code before it was acted on; none was accepted on the reviewer's word alone.

**Validation at the merge.** `./scripts/beautify.sh` changes nothing; `make check-format`, `make check-docs` and `make check-generated` pass; the lint differential against `e16cedac` reports no new finding; `make tests-converter` 1,655/0/0; `shin-uchuu-lhalo` unit, integration and scientific tiers pass with the gate's designed skip; the default pair's unit, integration and scientific tiers pass; no line outside `docs/dev/` references anything inside it.
